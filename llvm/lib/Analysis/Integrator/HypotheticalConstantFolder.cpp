// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass uses some heuristics to figure out loops that might be worth peeling.
// Basically this is simplistic SCCP plus some use of MemDep to find out how many instructions
// from the loop body would likely get evaluated if we peeled an iterations.
// We also consider the possibility of concurrently peeling a group of nested loops.
// The hope is that the information provided is both more informative and quicker to obtain than just speculatively
// peeling and throwing a round of -std-compile-opt at the result.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "hypotheticalconstantfolder"

#include "llvm/Analysis/HypotheticalConstantFolder.h"

#include "llvm/Pass.h"
#include "llvm/Instructions.h"
#include "llvm/BasicBlock.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ProfileInfo.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include <string>
#include <algorithm>

#define LPDEBUG(x) DEBUG(dbgs() << dbgind() << x)

using namespace llvm;

bool blockIsDead(BasicBlock* BB, const SmallSet<std::pair<BasicBlock*, BasicBlock*>, 4>& ignoreEdges) {

  for(pred_iterator PI = pred_begin(BB), PE = pred_end(BB); PI != PE; PI++) {
      
    if(!ignoreEdges.count(std::make_pair(*PI, BB)))
      return false;

  }

  return true;

}

static std::string ind(int i) {

  char* arr = (char*)alloca(i+1);
  for(int j = 0; j < i; j++)
    arr[j] = ' ';
  arr[i] = '\0';
  return std::string(arr);

}

std::string HypotheticalConstantFolder::dbgind() {

  return ind(debugIndent);

}

void HypotheticalConstantFolder::realGetRemoveBlockPredBenefit(BasicBlock* BB, BasicBlock* BBPred) {

  LPDEBUG("Getting benefit due elimination of predecessor " << BBPred->getName() << " from BB " << BB->getName() << "\n");

  eliminatedEdges.push_back(std::make_pair(BBPred, BB));

  if(outBlocks.count(BB)) {
    LPDEBUG(BB->getName() << " not under consideration" << "\n");
    return;
  }

  ignoreEdges.insert(std::make_pair(BBPred, BB));

  if(blockIsDead(BB, ignoreEdges)) {
    // This BB is dead! Kill its instructions, then remove it as a predecessor to all successor blocks and see if that helps anything.
    LPDEBUG("Block is dead!\n");
    for(BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; BI++) {
      if(!isa<PHINode>(BI)) {
	DenseMap<Value*, Constant*>::iterator it = constInstructions.find(BI);
	if(it != constInstructions.end()) {
	  LPDEBUG("Dead instruction " << *BI << " had already been constant folded\n");
	}
	else {
	  LPDEBUG("Instruction " << *BI << " eliminated\n");
	  eliminatedInstructions.push_back(BI);
	}
      }
    }
    for(succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {
      getRemoveBlockPredBenefit(*SI, BB);
    }
  }
  else {
    // See if any of our PHI nodes are now effectively constant
    for(BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E && isa<PHINode>(*I); ++I) {
      PHINode* PN = cast<PHINode>(I);
      getPHINodeBenefit(PN);
    }
  }

}

void HypotheticalConstantFolder::getRemoveBlockPredBenefit(BasicBlock* BB, BasicBlock* BBPred) {

  debugIndent += 2;
  realGetRemoveBlockPredBenefit(BB, BBPred);
  debugIndent -= 2;

}

void HypotheticalConstantFolder::getPHINodeBenefit(PHINode* PN) {

  LPDEBUG("Checking if PHI " << *PN << " is now constant" << "\n");

  if(constInstructions.find(PN) != constInstructions.end()) {
    LPDEBUG("Already constant");
    return;
  }

  BasicBlock* BB = PN->getParent();
  Constant* constValue = 0;

  for(pred_iterator PI = pred_begin(BB), PE = pred_end(BB); PI != PE; ++PI) {

    if(ignoreEdges.count(std::make_pair(*PI, BB)))
      continue;

    Value* predValue = PN->getIncomingValueForBlock(*PI);
    Constant* predConst = dyn_cast<Constant>(predValue);
    Instruction* predInst;
    if(!predConst) {
      if((predInst = dyn_cast<Instruction>(predValue))) {
	DenseMap<Value*, Constant*>::iterator it = constInstructions.find(predInst);
	if(it != constInstructions.end())
	  predConst = it->second;
      }
    }
    if(!predConst) {
      constValue = 0;
      break;
    }
    if(!constValue)
      constValue = predConst;
    else if(predConst != constValue) {
      constValue = 0;
      break;
    }
    // else this predecessor matches the others.
  }
  if(constValue) {
    LPDEBUG("Constant at " << *constValue << "\n");
    getConstantBenefit(PN, constValue);
  }
  else {
    LPDEBUG("Not constant\n");
    return;
  }

}

void HypotheticalConstantFolder::realGetConstantBenefit(Value* ArgV, Constant* ArgC) {

  Instruction* ArgI = dyn_cast<Instruction>(ArgV);

  if(ArgI && outBlocks.count(ArgI->getParent())) { 
    LPDEBUG(*ArgI << " not under consideration, ignoring\n");
    return;
  }

  DenseMap<Value*, Constant*>::iterator it = constInstructions.find(ArgV);
  if(it != constInstructions.end()) { // Have we already rendered this instruction constant?
    LPDEBUG(*ArgV << " already constant\n");
    return;
  }
 
  constInstructions[ArgV] = ArgC;
  if(ArgI && !isa<PHINode>(ArgI)) {
    if (!(ArgI->mayHaveSideEffects() || isa<AllocaInst>(ArgI))) { // A particular side-effect
      eliminatedInstructions.push_back(ArgI);
    }
    else {
      LPDEBUG("Not eliminating instruction due to side-effects\n");
    }
  }

  // A null value means we know the result will be constant, but we're not sure what.

  if(ArgC)
    LPDEBUG("Getting benefit due to value " << *ArgV << " having constant value " << *ArgC << "\n");
  else
    LPDEBUG("Getting benefit due to value " << *ArgV << " having an unknown constant value" << "\n");

  for (Value::use_iterator UI = ArgV->use_begin(), E = ArgV->use_end(); UI != E;++UI){

    Instruction* I;
    if(!(I = dyn_cast<Instruction>(*UI))) {
      LPDEBUG("Instruction has a non-instruction user: " << *UI << "\n");
      continue;
    }

    if(blockIsDead(I->getParent(), ignoreEdges)) {
      LPDEBUG("User instruction " << *I << " already eliminated (in dead block)\n");
      continue;
    }

    LPDEBUG("Considering user instruction " << *I << "\n");

    if (isa<BranchInst>(I) || isa<SwitchInst>(I)) {
      // Both Branches and Switches have one potentially non-const arg which we now know is constant.
      // The mechanism used by InlineCosts.cpp here emphasises code size. I try to look for
      // time instead, by searching for PHIs that will be made constant.
      if(ArgC) {
	BasicBlock* target;
	if(BranchInst* BI = dyn_cast<BranchInst>(I)) {
	  // This ought to be a boolean.
	  if((cast<ConstantInt>(ArgC))->isZero())
	    target = BI->getSuccessor(1);
	  else
	    target = BI->getSuccessor(0);
	}
	else {
	  SwitchInst* SI = cast<SwitchInst>(I);
	  unsigned targetidx = SI->findCaseValue(cast<ConstantInt>(ArgC));
	  target = SI->getSuccessor(targetidx);
	}
	if(target) {
	  // We know where the instruction is going -- remove this block as a predecessor for its other targets.
	  LPDEBUG("Branch or switch instruction given known target: " << target->getName() << "\n");
	  TerminatorInst* TI = cast<TerminatorInst>(I);
	  const unsigned NumSucc = TI->getNumSuccessors();
	  for (unsigned I = 0; I != NumSucc; ++I) {
	    BasicBlock* otherTarget = TI->getSuccessor(I);
	    if(otherTarget != target)
	      getRemoveBlockPredBenefit(otherTarget, TI->getParent());
	  }
	}
	else {
	  // We couldn't be sure which block the branch will go to, but its target will be constant.
	  // Give a static bonus to indicate that more advanced analysis might be able to eliminate the branch.
	  LPDEBUG("Promoted conditional to unconditional branch to unknown target\n");
	}

      }
      else {
	// We couldn't be sure where the branch will go because we only know the operand is constant, not its value.
	// We usually don't know because this is the return value of a call, or the result of a load. Give a small bonus
	// as the call might be inlined or similar.
	LPDEBUG("Unknown constant in branch or switch\n");
      }
      eliminatedInstructions.push_back(I);
    }
    else {
      // An ordinary instruction. Give bonuses or penalties for particularly fruitful or difficult instructions,
      // then count the benefits of that instruction becoming constant.
      if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
	LPDEBUG("Constant call argument\n");
      }

      // Try to calculate a constant value resulting from this instruction. Only possible if
      // this instruction is simple (e.g. arithmetic) and its arguments have known values, or don't matter.

      PHINode* PN;
      if((PN = dyn_cast<PHINode>(I))) {
	// PHI nodes are special because of their BB arguments, and the special-case "constant folding" that affects them
	getPHINodeBenefit(PN);
      }
      else {

	SmallVector<Constant*, 4> instOperands;

	bool someArgumentUnknownConstant = false;

	// This isn't as good as it could be, because the constant-folding library wants an array of constants,
	// whereas we might have somethig like 1 && x, which could fold but x is not a Constant*. Could work around this,
	// don't at the moment.
	for(unsigned i = 0; i < I->getNumOperands(); i++) {
	  Value* op = I->getOperand(i);
	  Constant* C;
	  Instruction* OperandI;
	  if((C = dyn_cast<Constant>(op)))
	    instOperands.push_back(C);
	  else if((OperandI = dyn_cast<Instruction>(op))) {
	    DenseMap<Value*, Constant*>::iterator it = constInstructions.find(OperandI);
	    if(it != constInstructions.end()) {
	      instOperands.push_back(it->second);
	      if(!it->second)
		someArgumentUnknownConstant = true;
	    }
	    else {
	      LPDEBUG("Not constant folding yet due to non-constant argument " << *OperandI << "\n");
	      break;
	    }
	  }
	  else {
	    LPDEBUG((*ArgV) << " has a non-instruction, non-constant argument: " << (*op) << "\n");
	    if(isa<CastInst>(I) || isa<GetElementPtrInst>(I)) {
	      
	    }
	    break;
	  }
	}

	if(instOperands.size() == I->getNumOperands()) {
	  Constant* newConst = 0;
	  if(!someArgumentUnknownConstant) {
	    if (const CmpInst *CI = dyn_cast<CmpInst>(I))
	      newConst = ConstantFoldCompareInstOperands(CI->getPredicate(), instOperands[0], instOperands[1], this->TD);
	    else if(isa<LoadInst>(I))
	      newConst = ConstantFoldLoadFromConstPtr(instOperands[0], this->TD);
	    else
	      newConst = ConstantFoldInstOperands(I->getOpcode(), I->getType(), instOperands.data(), I->getNumOperands(), this->TD);
	  }

	  if(newConst) {
	    LPDEBUG("User " << *I << " now constant at " << *newConst << "\n");
	  }
	  else {
	    if(I->mayReadFromMemory() || I->mayHaveSideEffects()) {
	      LPDEBUG("User " << *I << " may read or write global state; not propagating\n");
	      continue;
	    }
	    else if(someArgumentUnknownConstant) {
	      LPDEBUG("User " << *I << " will have an unknown constant value too\n");
	    } 
	    else {
	      LPDEBUG("User " << *I << " has all-constant arguments, but couldn't be constant folded" << "\n");
	    }
	  }
	  getConstantBenefit(I, newConst);
	}

      }

    }
  }

}

void HypotheticalConstantFolder::getConstantBenefit(Value* ArgV, Constant* ArgC) {

  debugIndent += 2;
  realGetConstantBenefit(ArgV, ArgC);
  debugIndent -= 2;

}

bool HypotheticalConstantFolder::tryForwardLoad(LoadInst* LI, const MemDepResult& Res) {

  if(StoreInst* SI = dyn_cast<StoreInst>(Res.getInst())) {
    if(Constant* SC = dyn_cast<Constant>(SI->getOperand(0))) {

      LPDEBUG(*LI << " defined by " << *SI << "\n");
      eliminatedInstructions.push_back(LI);
      getConstantBenefit(LI, SC);
      return true;

    }
    else {
      LPDEBUG(*LI << " is defined by " << *SI << " with a non-constant operand\n");
    }
  }
  else if(LoadInst* DefLI = dyn_cast<LoadInst>(Res.getInst())) {
		
    DenseMap<Value*, Constant*>::iterator DefLIIt = constInstructions.find(DefLI);
    if(DefLIIt != constInstructions.end()) {
		  
      LPDEBUG(*LI << " defined by " << *(DefLIIt->second) << "\n");
      eliminatedInstructions.push_back(LI);
      getConstantBenefit(LI, DefLIIt->second);
      return true;

    }

  }
  else {
    LPDEBUG(*LI << " is defined by " << *(Res.getInst()) << " which is not a simple store\n");
  }

  return false;

}

void HypotheticalConstantFolder::getBenefit(const SmallVector<Value*, 4>& roots) {

  for(SmallVector<Value*, 4>::const_iterator RI = roots.begin(), RE = roots.end(); RI != RE; RI++) {

    getConstantBenefit(*RI, constInstructions[*RI]);

  }

  bool anyStoreForwardingBenefits = true;

  while(anyStoreForwardingBenefits) {

    LPDEBUG("Considering store-to-load forwards...\n");
    anyStoreForwardingBenefits = false;

    MemoryDependenceAnalyser MD;
    MD.init(AA);

    for(Function::iterator FI = F->begin(), FE = F->end(); FI != FE; FI++) {

      BasicBlock* BB = FI;

      if(outBlocks.count(BB))
	continue;

      if(blockIsDead(BB, ignoreEdges))
	continue;

      for(BasicBlock::iterator II = BB->begin(), IE = BB->end(); II != IE; II++) {

	if(LoadInst* LI = dyn_cast<LoadInst>(II)) {

	  DenseMap<Value*, Constant*>::iterator it = constInstructions.find(LI);

	  if(it != constInstructions.end()) {
	    LPDEBUG("Ignoring " << *LI << " because it's already constant\n");
	    continue;
	  }

	  MemDepResult Res = MD.getDependency(LI, constInstructions, ignoreEdges);

	  if(Res.isClobber()) {
	    LPDEBUG(*LI << " is locally clobbered by " << Res.getInst() << "\n");
	    continue;
	  }
	  else if(Res.isDef()) {
	    anyStoreForwardingBenefits |= tryForwardLoad(LI, Res);
	  }
	  else { // Nonlocal
	      
	    Value* LPointer = LI->getOperand(0);

	    if(Instruction* LPointerI = dyn_cast<Instruction>(LPointer)) {
	      DenseMap<Value*, Constant*>::iterator it = constInstructions.find(LPointerI);
	      if(it != constInstructions.end())
		LPointer = it->second;
	    }

	    SmallVector<NonLocalDepResult, 4> NLResults;

	    MD.getNonLocalPointerDependency(LPointer, true, BB, NLResults, constInstructions, ignoreEdges);

	    assert(NLResults.size() > 0);

	    const MemDepResult* TheResult = 0;
	      
	    for(unsigned int i = 0; i < NLResults.size(); i++) {
		
	      const MemDepResult& Res = NLResults[i].getResult();
	      if(Res.isNonLocal())
		continue;
	      else if(Res.isClobber()) {
		LPDEBUG(*LI << " is nonlocally clobbered by " << *(Res.getInst()) << "\n");
		break;
	      }
	      else {
		if(TheResult) {
		  LPDEBUG(*LI << " depends on multiple instructions, ignoring\n");
		  TheResult = 0;
		  break;
		}
		else {
		  TheResult = &Res;
		}
	      }
		
	    }

	    if(TheResult)
	      anyStoreForwardingBenefits |= tryForwardLoad(LI, *TheResult);

	  }
	    
	}

      }

    }

    if(anyStoreForwardingBenefits) {
      LPDEBUG("At least one load was made constant; trying again\n");
    }
    else {
      LPDEBUG("No loads were made constant\n");
    }

  }
  
}
