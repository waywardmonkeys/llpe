//===- Shadows.cpp --------------------------------------------------------===//
//
// The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// Implement guts of instruction and block shadow structures, as well as utility routines for generating them
// from a function or block.

#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LLPE.h"

using namespace llvm;

void llvm::createTopOrderingFrom(BasicBlock* BB, std::vector<BasicBlock*>& Result, SmallSet<BasicBlock*, 8>& Visited, LoopInfo* LI, const Loop* MyL) {

  const Loop* BBL = LI ? LI->getLoopFor(BB) : 0;
  
  // Drifted out of scope?
  if(MyL != BBL && ((!BBL) || (BBL->contains(MyL))))
    return;

  auto insertres = Visited.insert(BB);
  if(!insertres.second)
    return;

  // Follow loop exiting edges if any
  if(MyL != BBL) {

    SmallVector<BasicBlock*, 4> ExitBlocks;
    BBL->getExitBlocks(ExitBlocks);
    for(SmallVector<BasicBlock*, 4>::iterator it = ExitBlocks.begin(), it2 = ExitBlocks.end(); it != it2; ++it) {
      
      createTopOrderingFrom(*it, Result, Visited, LI, MyL);
      
    }

  }

  // Explore all successors within this loop:
  for(succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {

    createTopOrderingFrom(*SI, Result, Visited, LI, BBL);
    
  }

  Result.push_back(BB);

}

static void ignoreChildLoops(SmallSet<BasicBlock*, 1>& headers, const Loop* L) {

  headers.insert(L->getHeader());
  for(Loop::iterator it = L->begin(), itend = L->end(); it != itend; ++it)
    ignoreChildLoops(headers, *it);
  
}

ShadowLoopInvar* LLPEAnalysisPass::getLoopInfo(ShadowFunctionInvar* FInfo,
							DenseMap<BasicBlock*, uint32_t>& BBIndices, 
							const Loop* L,
							DominatorTree* DT,
							ShadowLoopInvar* ParentLoop) {
  
  release_assert(L->isLoopSimplifyForm() && L->isLCSSAForm(*DT) && "Don't forget to run loopsimplify and lcssa first!");

  ShadowLoopInvar* LInfo = new ShadowLoopInvar();

  LInfo->headerIdx = BBIndices[L->getHeader()];
  LInfo->preheaderIdx = BBIndices[L->getLoopPreheader()];
  LInfo->latchIdx = BBIndices[L->getLoopLatch()];
  LInfo->nBlocks = L->getBlocks().size();
  LInfo->parent = ParentLoop;

  // If we're supposed to ignore this loop and all children, register them now so that applyIgnoreLoops
  // does the right thing.

  BasicBlock* HBB = L->getHeader();
  Function* LF = HBB->getParent();

  if(shouldIgnoreLoopChildren(LF, HBB))
    ignoreChildLoops(ignoreLoops[LF], L);

  LInfo->optimisticEdge = std::make_pair(0xffffffff, 0xffffffff);

  for(uint32_t i = LInfo->headerIdx, ilim = LInfo->headerIdx + L->getNumBlocks(); i != ilim; ++i) {

    // TODO: Fix or discard outerScope.
    // Note these will be overwritten if the block is also within a child loop.
    FInfo->BBs[i].outerScope = applyIgnoreLoops(LInfo, LF, FInfo);
    FInfo->BBs[i].naturalScope = LInfo;

    BasicBlock* OptEdgeSink = getOptimisticEdge(LF, FInfo->BBs[i].BB);
    if(OptEdgeSink) {
      release_assert(LInfo->optimisticEdge.first == 0xffffffff && "Only one optimistic edge allowed per loop");
      LInfo->optimisticEdge = std::make_pair(i, BBIndices[OptEdgeSink]);
    }

  }

  LInfo->alwaysIterate = shouldAlwaysIterate(LF, HBB);

  {
    SmallVector<BasicBlock*, 4> temp;
    L->getExitingBlocks(temp);
    {
      LInfo->exitingBlocks.reserve(temp.size());
      for(unsigned i = 0; i < temp.size(); ++i)
	LInfo->exitingBlocks.push_back(BBIndices[temp[i]]);
    }

    temp.clear();
    L->getExitBlocks(temp);
    {
      LInfo->exitBlocks.reserve(temp.size());
      for(unsigned i = 0; i < temp.size(); ++i)
	LInfo->exitBlocks.push_back(BBIndices[temp[i]]);
    }
  }

  {
    SmallVector<std::pair<const BasicBlock*, const BasicBlock*>, 4> exitEdges;
    L->getExitEdges(exitEdges);
    LInfo->exitEdges.reserve(exitEdges.size());
    for(unsigned i = 0; i < exitEdges.size(); ++i)
      LInfo->exitEdges.push_back(std::make_pair(BBIndices[const_cast<BasicBlock*>(exitEdges[i].first)], BBIndices[const_cast<BasicBlock*>(exitEdges[i].second)]));
  }

  for(Loop::iterator it = L->begin(), itend = L->end(); it != itend; ++it) {

    ShadowLoopInvar* child = getLoopInfo(FInfo, BBIndices, *it, DT, LInfo);
    LInfo->childLoops.push_back(child);

  }

  return LInfo;

}

void LLPEAnalysisPass::initShadowGlobals(Module& M, uint32_t extraSlots) {

  uint32_t i = 0;
  uint32_t nGlobals = std::distance(M.global_begin(), M.global_end());
  // extraSlots are reserved for new globals we know will be introduced between now and specialisation start.
  nGlobals += extraSlots;
  shadowGlobals = new ShadowGV[nGlobals];

  // Assign them all numbers before computing initialisers, because the initialiser can
  // reference another global, and getValPB will then lookup in shadowGlobalsIdx.

  for(Module::global_iterator it = M.global_begin(), itend = M.global_end(); it != itend; ++it, ++i) {

    shadowGlobals[i].G = it;
    shadowGlobalsIdx[it] = i;

  }

  i = 0;
  for(Module::global_iterator it = M.global_begin(), itend = M.global_end(); it != itend; ++it, ++i) {

    if(it->isConstant()) {
      shadowGlobals[i].storeSize = GlobalAA->getTypeStoreSize(shadowGlobals[i].G->getType());
      continue;
    }

    shadowGlobals[i].allocIdx = (int32_t)heap.size();
    
    heap.push_back(AllocData());
    AllocData& AD = heap.back();
    AD.allocIdx = heap.size() - 1;
    AD.storeSize = GlobalAA->getTypeStoreSize(it->getType()->getElementType());
    AD.isCommitted = true;
    AD.allocValue = ShadowValue(&(shadowGlobals[i]));
    AD.allocType = shadowGlobals[i].G->getType();

    //errs() << "Init store for " << *it << " -> ";
    //printPB(errs(), *Init);
    //errs() << "\n";

    shadowGlobals[i].storeSize = AD.storeSize;

  }

}

const GlobalValue* llvm::getUnderlyingGlobal(const GlobalValue* V) {

  if(const GlobalAlias* GA = dyn_cast<GlobalAlias>(V)) {
    const GlobalValue* Aliasee = dyn_cast_or_null<GlobalValue>(GA->getAliasee());
    if(!Aliasee)
      return 0;
    else
      return getUnderlyingGlobal(Aliasee);
  }
  return V;

}

static const GlobalVariable* getGlobalVar(const Value* V) {

  const GlobalValue* GV = dyn_cast<GlobalValue>(V);
  if(!GV)
    return 0;

  return dyn_cast<GlobalVariable>(getUnderlyingGlobal(GV));

}

ShadowFunctionInvar* LLPEAnalysisPass::getFunctionInvarInfo(Function& F) {

  DenseMap<Function*, ShadowFunctionInvar*>::iterator findit = functionInfo.find(&F);
  if(findit != functionInfo.end())
    return findit->second;

  // Beware! This LoopInfo instance and whatever Loop objects come from it are only alive until
  // the next call to getAnalysis. Therefore the ShadowLoopInvar objects we make here
  // must mirror all information we're interested in from the Loops.
  LoopInfo* LI = &getAnalysis<LoopInfo>(F);

  ShadowFunctionInvar* RetInfoP = new ShadowFunctionInvar();
  functionInfo[&F] = RetInfoP;
  ShadowFunctionInvar& RetInfo = *RetInfoP;

  std::vector<BasicBlock*> TopOrderedBlocks;
  SmallSet<BasicBlock*, 8> VisitedBlocks;

  createTopOrderingFrom(&F.getEntryBlock(), TopOrderedBlocks, VisitedBlocks, LI, /* loop = */ 0);

  std::reverse(TopOrderedBlocks.begin(), TopOrderedBlocks.end());

  // Assign indices to each BB and instruction (IIndices is useful since otherwise we have to walk
  // the instruction list to get from an instruction to its index)

  DenseMap<BasicBlock*, uint32_t> BBIndices;
  DenseMap<Instruction*, uint32_t> IIndices;

  for(uint32_t i = 0; i < TopOrderedBlocks.size(); ++i) {

    BasicBlock* BB = TopOrderedBlocks[i];

    BBIndices[BB] = i;
    
    uint32_t j;
    BasicBlock::iterator it, endit;
    for(j = 0, it = BB->begin(), endit = BB->end(); it != endit; ++it, ++j) {

      IIndices[it] = j;

    }

  }

  ShadowBBInvar* FShadowBlocks = new ShadowBBInvar[TopOrderedBlocks.size()];

  for(uint32_t i = 0; i < TopOrderedBlocks.size(); ++i) {

    BasicBlock* BB = TopOrderedBlocks[i];
    ShadowBBInvar& SBB = FShadowBlocks[i];
    
    SBB.F = &RetInfo;
    SBB.idx = i;
    SBB.BB = BB;
    // True loop scope will be computed later, but by default...
    SBB.outerScope = 0;
    SBB.naturalScope = 0;

    const Loop* BBScope =  LI->getLoopFor(BB);

    // Find successor block indices:

    succ_iterator SI = succ_begin(BB), SE = succ_end(BB);
    uint32_t succSize = std::distance(SI, SE);
    SBB.succIdxs = ImmutableArray<uint32_t>(new uint32_t[succSize], succSize);

    for(uint32_t j = 0; SI != SE; ++SI, ++j) {

      SBB.succIdxs[j] = BBIndices[*SI];

    }

    // Find predecessor block indices:

    pred_iterator PI = pred_begin(BB), PE = pred_end(BB);
    uint32_t predSize = std::distance(PI, PE);
    SBB.predIdxs = ImmutableArray<uint32_t>(new uint32_t[predSize], predSize);
    
    for(uint32_t j = 0; PI != PE; ++PI, ++j) {

      SBB.predIdxs[j] = BBIndices[*PI];
      
      if(SBB.predIdxs[j] > i) {

	if((!BBScope) || SBB.BB != BBScope->getHeader()) {

	  errs() << "Warning: block " << SBB.BB->getName() << " in " << F.getName() << " has predecessor " << (*PI)->getName() << " that comes after it topologically, but this is not a loop header. The program is not in well-nested natural loop form.\n";

	}

      }

    }

    // Find instruction def/use indices:
    ShadowInstructionInvar* insts = new ShadowInstructionInvar[BB->size()];

    BasicBlock::iterator BI = BB->begin(), BE = BB->end();
    for(uint32_t j = 0; BI != BE; ++BI, ++j) {

      Instruction* I = BI;
      ShadowInstructionInvar& SI = insts[j];

      SI.idx = j;
      SI.parent = &SBB;
      SI.I = I;
      
      // Get operands indices:
      uint32_t NumOperands;
      ShadowInstIdx* operandIdxs;
      if(PHINode* PN = dyn_cast<PHINode>(I)) {

	NumOperands = PN->getNumIncomingValues();
	operandIdxs = new ShadowInstIdx[NumOperands];
	uint32_t* incomingBBs = new uint32_t[NumOperands];

	for(unsigned k = 0, kend = PN->getNumIncomingValues(); k != kend; ++k) {

	  if(Instruction* OpI = dyn_cast<Instruction>(PN->getIncomingValue(k)))
	    operandIdxs[k] = ShadowInstIdx(BBIndices[OpI->getParent()], IIndices[OpI]);
	  else if(GlobalVariable* OpGV = const_cast<GlobalVariable*>(getGlobalVar(PN->getIncomingValue(k))))
	    operandIdxs[k] = ShadowInstIdx(INVALID_BLOCK_IDX, getShadowGlobalIndex(OpGV));
	  else
	    operandIdxs[k] = ShadowInstIdx();
	  incomingBBs[k] = BBIndices[PN->getIncomingBlock(k)];

	}

	SI.operandBBs = ImmutableArray<uint32_t>(incomingBBs, NumOperands);

      }
      else {

	NumOperands = I->getNumOperands();
	operandIdxs = new ShadowInstIdx[NumOperands];

	for(unsigned k = 0, kend = I->getNumOperands(); k != kend; ++k) {
	  
	  if(Instruction* OpI = dyn_cast<Instruction>(I->getOperand(k)))
	    operandIdxs[k] = ShadowInstIdx(BBIndices[OpI->getParent()], IIndices[OpI]);
	  else if(GlobalVariable* OpGV = const_cast<GlobalVariable*>(getGlobalVar(I->getOperand(k))))
	    operandIdxs[k] = ShadowInstIdx(INVALID_BLOCK_IDX, getShadowGlobalIndex(OpGV));
	  else if(BasicBlock* OpBB = dyn_cast<BasicBlock>(I->getOperand(k)))
	    operandIdxs[k] = ShadowInstIdx(BBIndices[OpBB], INVALID_INSTRUCTION_IDX);
	  else
	    operandIdxs[k] = ShadowInstIdx();

	}

      }

      SI.operandIdxs = ImmutableArray<ShadowInstIdx>(operandIdxs, NumOperands);

      // Get user indices:
      unsigned nUsers = std::distance(I->use_begin(), I->use_end());

      ShadowInstIdx* userIdxs = new ShadowInstIdx[nUsers];

      Instruction::use_iterator UI;
      unsigned k;
      for(k = 0, UI = I->use_begin(); k != nUsers; ++k, ++UI) {

	if(Instruction* UserI = dyn_cast<Instruction>(UI->getUser())) {

	  userIdxs[k] = ShadowInstIdx(BBIndices[UserI->getParent()], IIndices[UserI]);

	}
	else {

	  userIdxs[k] = ShadowInstIdx();
	  
	}

      }

      SI.userIdxs = ImmutableArray<ShadowInstIdx>(userIdxs, nUsers);

    }

    SBB.insts = ImmutableArray<ShadowInstructionInvar>(insts, BB->size());

  }

  RetInfo.BBs = ImmutableArray<ShadowBBInvar>(FShadowBlocks, TopOrderedBlocks.size());

  // Get user info for arguments:

  ShadowArgInvar* Args = new ShadowArgInvar[F.arg_size()];

  Function::arg_iterator AI = F.arg_begin();
  uint32_t i = 0;
  for(; i != F.arg_size(); ++i, ++AI) {

    Argument* A = AI;
    ShadowArgInvar& SArg = Args[i];
    SArg.A = A;
      
    unsigned j = 0;
    Argument::use_iterator UI = A->use_begin(), UE = A->use_end();

    uint32_t nUsers = std::distance(UI, UE);
    ShadowInstIdx* Users = new ShadowInstIdx[nUsers];

    for(; UI != UE; ++UI, ++j) {

      Value* UsedV = *UI;
      if(Instruction* UsedI = dyn_cast<Instruction>(UsedV)) {

	Users[j] = ShadowInstIdx(BBIndices[UsedI->getParent()], IIndices[UsedI]);

      }
      else {

	Users[j] = ShadowInstIdx();

      }

    }

    SArg.userIdxs = ImmutableArray<ShadowInstIdx>(Users, nUsers);

  }

  RetInfo.Args = ImmutableArray<ShadowArgInvar>(Args, F.arg_size());

  // Populate map from loop headers to header index. Due to the topological sort,
  // all loops consist of that block + L->getBlocks().size() further, contiguous blocks,
  // making is-in-loop easy to compute.

  DominatorTree* thisDT = DTs[&F];

  for(LoopInfo::iterator it = LI->begin(), it2 = LI->end(); it != it2; ++it) {
    ShadowLoopInvar* newL = getLoopInfo(&RetInfo, BBIndices, *it, thisDT, 0);
    RetInfo.TopLevelLoops.push_back(newL);
  }

  // Count alloca instructions at the start of the function; this will control how
  // large the std::vector that represents the frame will be initialised.
  RetInfo.frameSize = 0;
  for(BasicBlock::iterator it = F.getEntryBlock().begin(), itend = F.getEntryBlock().end(); it != itend && isa<AllocaInst>(it); ++it)
    ++RetInfo.frameSize;

  // "&& RootIA" checks whether we're inside the initial context creation, in which case we should
  // allocate a frame whether or not main can ever allocate to avoid the frame index underflowing
  // in some circumstances.
  if((!RetInfo.frameSize) && RootIA) {

    // Magic value indicating the function will never alloca anything and we can skip all frame processing.
    RetInfo.frameSize = -1;

    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E && RetInfo.frameSize == -1; ++I) {
      
      if(isa<AllocaInst>(*I))
	RetInfo.frameSize = 0;

    }
      

  }

  return RetInfoP;

}

// Prepare the context-specific data structures, tying them to known invariant information.
// For an inline attempt, create a BB array 

void InlineAttempt::prepareShadows() {

  invarInfo = pass->getFunctionInvarInfo(F);
  nBBs = F.size();
  release_assert(nBBs == invarInfo->BBs.size() && "Function contains unreachable blocks, run simplifycfg first!");
  BBs = new ShadowBB*[nBBs];
  for(uint32_t i = 0; i < nBBs; ++i)
    BBs[i] = 0;
  BBsOffset = 0;

  uint32_t shadowsSize;
  if(isPathCondition || !Callers.size())
    shadowsSize = F.arg_size();
  else
    shadowsSize = Callers[0]->getNumArgOperands();

  ShadowArg* argShadows = new ShadowArg[shadowsSize];
  this->argShadows = ImmutableArray<ShadowArg>(argShadows, shadowsSize);
  uint32_t i = 0;
  for(; i != F.arg_size(); ++i) {

    argShadows[i].invar = &(invarInfo->Args[i]);
    argShadows[i].IA = this;
    argShadows[i].dieStatus = 0;
    argShadows[i].patchInst = 0;
    argShadows[i].committedVal = 0;
    
  }

  for(; i != shadowsSize; ++i) {

    argShadows[i].invar = 0;
    argShadows[i].IA = this;
    argShadows[i].dieStatus = 0;
    argShadows[i].patchInst = 0;
    argShadows[i].committedVal = 0;    

  }

}

void PeelIteration::prepareShadows() {

  invarInfo = pass->getFunctionInvarInfo(F);
  nBBs = L->nBlocks;
  BBs = new ShadowBB*[nBBs];
  for(uint32_t i = 0; i < nBBs; ++i)
    BBs[i] = 0;
  BBsOffset = parentPA->L->headerIdx;

}

ShadowBB* IntegrationAttempt::getOrCreateBB(uint32_t i) {

  if(ShadowBB* BB = getBB(i))
    return BB;
  return createBB(i);

}

ShadowBB* IntegrationAttempt::getOrCreateBB(ShadowBBInvar* BBI) {

  bool inScope;
  if(ShadowBB* BB = getBB(*BBI, &inScope))
    return BB;
  release_assert(inScope && "getOrCreateBB in wrong scope");
  return createBB(BBI);

}

ShadowBBInvar* IntegrationAttempt::getBBInvar(uint32_t idx) const {

  return &(invarInfo->BBs[idx]);

}

ShadowBB* IntegrationAttempt::getUniqueBBRising(ShadowBBInvar* BBI) {

  if(BBI->naturalScope == L)
    return getBB(*BBI);

  if(PeelAttempt* LPA = getPeelAttempt(immediateChildLoop(L, BBI->naturalScope))) {

    if(LPA->isTerminated() && LPA->Iterations.back()->isOnlyExitingIteration()) {

      return LPA->Iterations.back()->getUniqueBBRising(BBI);

    }

  }

  return 0;

}

ShadowBB* IntegrationAttempt::createBB(uint32_t blockIdx) {

  release_assert((!BBs[blockIdx - BBsOffset]) && "Creating block for the second time");
  ShadowBB* newBB = new ShadowBB();
  newBB->invar = &(invarInfo->BBs[blockIdx]);
  newBB->succsAlive = new bool[newBB->invar->succIdxs.size()];
  for(unsigned i = 0, ilim = newBB->invar->succIdxs.size(); i != ilim; ++i)
    newBB->succsAlive[i] = false;
  newBB->status = BBSTATUS_UNKNOWN;
  newBB->IA = this;

  ShadowInstruction* insts = new ShadowInstruction[newBB->invar->insts.size()];
  for(uint32_t i = 0, ilim = newBB->invar->insts.size(); i != ilim; ++i) {
    insts[i].invar = &(newBB->invar->insts[i]);
    insts[i].parent = newBB;
    insts[i].dieStatus = 0;
    insts[i].isThreadLocal = TLS_MUSTCHECK;
    insts[i].needsRuntimeCheck = RUNTIME_CHECK_NONE;
    insts[i].typeSpecificData = 0;
  }
  newBB->insts = ImmutableArray<ShadowInstruction>(insts, newBB->invar->insts.size());
  newBB->useSpecialVarargMerge = false;
  newBB->localStore = 0;

  BBs[blockIdx - BBsOffset] = newBB;
  return newBB;

}

ShadowBB* IntegrationAttempt::createBB(ShadowBBInvar* BBI) {

  return createBB(BBI->idx);

}

ShadowInstructionInvar* IntegrationAttempt::getInstInvar(uint32_t blockidx, uint32_t instidx) {

  return &(invarInfo->BBs[blockidx].insts[instidx]);

}

ShadowInstruction* InlineAttempt::getInstFalling(ShadowBBInvar* BB, uint32_t instIdx) {

  release_assert((!BB->outerScope) && "Out of scope in getInstFalling");
  ShadowBB* LocalBB = getBB(*BB);
  if(!LocalBB)
    return 0;
  return &(LocalBB->insts[instIdx]);

}

ShadowInstruction* PeelIteration::getInstFalling(ShadowBBInvar* BB, uint32_t instIdx) {

  if(BB->outerScope == L) {

    ShadowBB* LocalBB = getBB(*BB);
    if(!LocalBB)
      return 0;
    return &(LocalBB->insts[instIdx]);
    
  }
  else {
    
    return parent->getInstFalling(BB, instIdx);
    
  }

}

ShadowInstruction* IntegrationAttempt::getInst(uint32_t blockIdx, uint32_t instIdx) {

  bool inScope;
  ShadowBB* OpBB = getBB(blockIdx, &inScope);

  if(!inScope) {

    // Access to parent context.
    ShadowBBInvar* OpBBI = &(invarInfo->BBs[blockIdx]);
    return getInstFalling(OpBBI, instIdx);

  }
  else if(!OpBB) {
    
    return 0;

  }
  else {

    return &(OpBB->insts[instIdx]);

  }

}

ShadowInstruction* IntegrationAttempt::getInst(ShadowInstructionInvar* SII) {

  return getInst(SII->parent->idx, SII->idx);

}

ShadowValue ShadowValue::getInt(Type* CIT, uint64_t CIVal) {

  if(CIT->isIntegerTy(8))
    return ShadowValue::getInt8((uint8_t)CIVal);
  else if(CIT->isIntegerTy(16))
    return ShadowValue::getInt16((uint16_t)CIVal);
  else if(CIT->isIntegerTy(32))
    return ShadowValue::getInt32((uint32_t)CIVal);
  else if(CIT->isIntegerTy(64))
    return ShadowValue::getInt64(CIVal);
  else
    return ShadowValue(ConstantInt::get(CIT, CIVal));

}

// Get the ShadowValue for this instruction's operand.
// For most kinds of ShadowValue they're just passed through,
// but for ShadowInstructions we must make sure if the operand is
// a loop invariant then we find the right version of the SI.
// Note that due to LCSSA form operands are always in the same context or a parent,
// except for exit PHI operands, which are special cased in HCF's
// getPHINodeValue function.
ShadowValue ShadowInstruction::getOperand(uint32_t i) {

  ShadowInstIdx& SII = invar->operandIdxs[i];
  uint32_t blockOpIdx = SII.blockIdx;
  if(blockOpIdx == INVALID_BLOCK_IDX) {
    Value* ArgV = invar->I->getOperand(i);
    if(SII.instIdx != INVALID_INSTRUCTION_IDX) {
      return ShadowValue(&(parent->IA->pass->shadowGlobals[SII.instIdx]));
    }
    else if(Argument* A = dyn_cast<Argument>(ArgV)) {
      return ShadowValue(&(parent->IA->getFunctionRoot()->argShadows[A->getArgNo()]));
    }
    else if(ConstantInt* CI = dyn_cast<ConstantInt>(ArgV)) {
      return ShadowValue::getInt(CI->getType(), CI->getLimitedValue());
    }
    else {
      return ShadowValue(ArgV);
    }
  }
  else if(SII.instIdx == INVALID_INSTRUCTION_IDX) {

    // BasicBlock operand, only encountered on this path with Invoke instructions.
    return ShadowValue();

  }
  else {
    ShadowInstruction* OpInst = parent->IA->getInst(blockOpIdx, SII.instIdx);
    if(OpInst)
      return ShadowValue(OpInst);
    else
      return ShadowValue();
  }

}


ShadowInstruction* ShadowInstruction::getUser(uint32_t i) {

  ShadowInstIdx& SII = invar->userIdxs[i];
  return &(parent->IA->BBs[SII.blockIdx]->insts[SII.instIdx]);

}

void IntegrationAttempt::copyLoopExitingDeadEdges(PeelAttempt* LPA) {

  const std::vector<std::pair<uint32_t, uint32_t> >& EE = LPA->L->exitEdges;

  for(uint32_t i = 0; i < EE.size(); ++i) {

    std::pair<uint32_t, uint32_t> E = EE[i];
    if(ShadowBB* BB = getOrCreateBB(E.first)) {

      bool dead = edgeIsDeadRising(*BB->invar, *getBBInvar(E.second), /* ignoreThisScope = */ true);
      
      for(uint32_t j = 0; j < BB->invar->succIdxs.size(); ++j) {
	if(BB->invar->succIdxs[j] == E.second)
	  BB->succsAlive[j] = !dead;
      }
      
    }

  }

}

bool llvm::blockAssumedToExecute(ShadowBB* BB) {

  return BB->status != BBSTATUS_UNKNOWN;

}

bool llvm::blockCertainlyExecutes(ShadowBB* BB) {

  return BB->status == BBSTATUS_CERTAIN;

}

bool AllocData::isAvailable() {

  if(isCommitted)
    return !!committedVal;
  else
    return allocValue.getCtx()->allAncestorsEnabled();

}

bool FDGlobalState::isAvailable() {

  if(isCommitted)
    return !!CommittedVal;
  else if(isFifo)
    return false;
  else
    return SI->parent->IA->allAncestorsEnabled();

}

bool ShadowValue::objectAvailable() const {

  switch(t) {
  case SHADOWVAL_OTHER: 
    {
      if(Function* F = dyn_cast<Function>(u.V))
	return !GlobalIHP->specialLocations.count(F);
      else
	return true;
    }
  case SHADOWVAL_GV:
  case SHADOWVAL_ARG:
    return true;
  case SHADOWVAL_INST:
    if(u.I->parent->IA->getFunctionRoot()->isPathCondition)
      return false;
    if(!u.I->parent->IA->allAncestorsEnabled())
      return false;
    return true;
  case SHADOWVAL_PTRIDX:
    // Stack-allocated members are necessarily available from any context
    // that can conceivably reach them.
    if(u.PtrOrFd.frame != -1)
      return true;
    else {
      AllocData* AD = getAllocData((OrdinaryLocalStore*)0);
      return AD->isAvailable();
    }
  case SHADOWVAL_FDIDX:
  case SHADOWVAL_FDIDX64:
    return GlobalIHP->fds[getFd()].isAvailable();
  default:
    release_assert(0 && "Bad SV type in objectAvailableFrom");
    llvm_unreachable("Bad SV type in objectAvailableFrom");
  }
  
}

BasicBlock* ShadowBB::getCommittedBreakBlockAt(uint32_t idx) {

  for(uint32_t i = 0, ilim = committedBlocks.size(); i != ilim; ++i) {

    CommittedBlock& Block = committedBlocks[i];
    if(Block.startIndex <= idx) {

      if(i + 1 == ilim || committedBlocks[i + 1].startIndex > idx)
	return Block.breakBlock;

    }

  }

  release_assert("Failed to find block index");
  return 0;

}

ShadowBB* PeelIteration::getUniqueExitingBlock2(ShadowBBInvar* BBI, const ShadowLoopInvar* exitLoop, bool& bail) {

  PeelAttempt* LPA;

  // Defer to child loop iteration?

  if(BBI->naturalScope != L && 
     (LPA = getPeelAttempt(immediateChildLoop(L, BBI->naturalScope))) && 
     LPA->isTerminated()) {

    return LPA->Iterations.back()->getUniqueExitingBlock2(BBI, exitLoop, bail);

  }

  // Find a unique exiting edge if there is one.
  ShadowBB* ExitingBB = getBB(*BBI);
  if(!ExitingBB)
    return 0;

  uint32_t exitingEdges = 0;

  for(uint32_t i = 0, ilim = BBI->succIdxs.size(); i != ilim && exitingEdges < 2; ++i) {

    ShadowBBInvar* ExitedBBI = getBBInvar(BBI->succIdxs[i]);
    if(ExitingBB->succsAlive[i] && 
       ((!ExitedBBI->naturalScope) || !exitLoop->contains(ExitedBBI->naturalScope))) {

      ++exitingEdges;

    }

  }

  if(exitingEdges == 0)
    return 0;
  else if(exitingEdges == 1)
    return ExitingBB;
  else {
    bail = true;
    return 0;
  }

}

ShadowBB* PeelIteration::getUniqueExitingBlock() {

  ShadowBB* uniqueBlock = 0;

  for(std::vector<uint32_t>::const_iterator it = parentPA->L->exitingBlocks.begin(),
	itend = parentPA->L->exitingBlocks.end(); it != itend; ++it) {

    ShadowBBInvar* ExitingBBI = getBBInvar(*it);
    bool bail = false;
    ShadowBB* ExitingBB = getUniqueExitingBlock2(ExitingBBI, L, bail);
    if(bail)
      return 0;
    else if(ExitingBB) {
      if(uniqueBlock)
	return 0;
      else
	uniqueBlock = ExitingBB;
    }

  }

  return uniqueBlock;
  
}

bool ShadowInstruction::readsMemoryDirectly() {

  if(isCopyInst())
    return true;

  switch(invar->I->getOpcode()) {
  case Instruction::Load:
  case Instruction::AtomicCmpXchg:
  case Instruction::AtomicRMW:
    return true;
  default:
    return false;
  }

}

bool ShadowInstruction::hasOrderingConstraint() {

  switch(invar->I->getOpcode()) {

  case Instruction::Load:
    return !cast_inst<LoadInst>(this)->isUnordered();
  case Instruction::Store:
    return !cast_inst<StoreInst>(this)->isUnordered();
  case Instruction::AtomicRMW:
    return cast_inst<AtomicRMWInst>(this)->getOrdering() > Unordered;
  case Instruction::AtomicCmpXchg: 
    {
      auto cmpx = cast_inst<AtomicCmpXchgInst>(this);
      return cmpx->getSuccessOrdering() > Unordered || cmpx->getFailureOrdering() > Unordered;
    }
  case Instruction::Fence:
    return true;
  default:
    break;

  }

  return false;

}
