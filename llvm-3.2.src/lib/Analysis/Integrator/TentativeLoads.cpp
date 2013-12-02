// A mini-analysis that spots tentative loads and memcpy instructions.
// These are loads whose incoming dataflow (a) crosses a /yield point/, a point where we must assume
// that another thread got a chance to run and messed with our state, (b) is not dominated
// by other loads or stores that will check the incoming state / overwrite it with known state,
// and (c) is not known to be thread-local regardless.

// The main phase has already taken care of part (c) for us by setting ShadowInstruction::u.load.isThreadLocal
// when the load was known to be from a thread-private object. We will set the same flag wherever
// it's clear that checking this load would be redundant.

#include "llvm/Analysis/HypotheticalConstantFolder.h"

#include "llvm/Function.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"

using namespace llvm;

static uint32_t TLProgressN = 0;
const uint32_t TLProgressLimit = 1000;

static void TLProgress() {

  TLProgressN++;
  if(TLProgressN == TLProgressLimit) {

    errs() << ".";
    TLProgressN = 0;

  }

}

static TLMapTy::Allocator TLMapAllocator;
static TLMapTy TLEmptyMap(TLMapAllocator);
TLMapPointer llvm::TLEmptyMapPtr(&TLEmptyMap);

TLLocalStore* TLMapPointer::getMapForBlock(ShadowBB* BB) {

  return BB->tlStore;

}

TLMapPointer TLMapPointer::getReadableCopy() {

  TLMapTy* newMap = new TLMapTy(TLMapAllocator);
  for(TLMapTy::iterator it = M->begin(), itend = M->end(); it != itend; ++it)
    newMap->insert(it.start(), it.stop(), *it);

  return TLMapPointer(newMap);

}

void TLMapPointer::dropReference() {

  delete M;
  M = 0;

}

void TLMapPointer::mergeStores(TLMapPointer* mergeFrom, TLMapPointer* mergeTo, uint64_t ASize, TLMerger* Visitor) {

  // Intersect the sets per byte. The values are just booleans, so overwriting without erasing is fine.

  SmallVector<std::pair<uint64_t, uint64_t>, 4> keepRanges;

  for(TLMapTy::iterator it = mergeFrom->M->begin(), itend = mergeFrom->M->end();
      it != itend; ++it) {

    for(TLMapTy::iterator toit = mergeTo->M->find(it.start()), toitend = mergeTo->M->end();
	toit != toitend && toit.start() < it.stop(); ++toit) {

      uint64_t keepStart = std::max(toit.start(), it.start());
      uint64_t keepStop = std::min(toit.stop(), it.stop());
      keepRanges.push_back(std::make_pair(keepStart, keepStop));

    }

  }

  mergeTo->M->clear();
  for(SmallVector<std::pair<uint64_t, uint64_t>, 4>::iterator it = keepRanges.begin(),
	itend = keepRanges.end(); it != itend; ++it) {

    mergeTo->M->insert(it->first, it->second, true);

  }

}

TLMapPointer* ShadowBB::getWritableTLStore(ShadowValue O) {

  tlStore = tlStore->getWritableFrameList();
  bool isNewStore;
  TLMapPointer* ret = tlStore->getOrCreateStoreFor(O, &isNewStore);

  if(isNewStore)
    ret->M = new TLMapTy(TLMapAllocator);

  return ret;

}

static void markAllObjectsTentative(ShadowBB* BB) {

  BB->tlStore = BB->tlStore->getEmptyMap();
  BB->tlStore->allOthersClobbered = true;
  BB->IA->yieldState = BARRIER_HERE;

}

static void markGoodBytes(ShadowValue GoodPtr, uint64_t Len, bool contextEnabled, ShadowBB* BB, uint64_t Offset = 0) {

  // ignoreUntil indicates we're within a disabled context. The loads and stores here will
  // be committed unmodified, in particular without checks that their results are as expected,
  // and so they do not make any subsequent check redundant.
  // Stores in disabled contexts can't count either, because of the situation:
  // disabled {
  //   call void thread_yield();
  //   %0 = load %x;
  //   store %0, %y;
  // }
  // %1 = load %y
  
  // Here the load %y must be checked, because the load %x cannot be checked.

  if(!contextEnabled)
    return;

  // If allOthersClobbered is false then no object is tentative.
  if(!BB->tlStore->allOthersClobbered)
    return;

  std::pair<ValSetType, ImprovedVal> PtrTarget;
  if(!tryGetUniqueIV(GoodPtr, PtrTarget))
    return;

  if(PtrTarget.first != ValSetTypePB)
    return;

  if(PtrTarget.second.V.isGV() &&  PtrTarget.second.V.u.GV->G->isConstant())
    return;

  SmallVector<std::pair<uint64_t, uint64_t>, 1> addRanges;

  TLMapPointer* store = BB->tlStore->getReadableStoreFor(PtrTarget.second.V);
  uint64_t start = PtrTarget.second.Offset + Offset;
  uint64_t stop = PtrTarget.second.Offset + Offset + Len;

  if(!store) {
   
    addRanges.push_back(std::make_pair(start, stop));

  }
  else {

    TLMapTy::iterator it = store->M->find(start), itend = store->M->end();

    if(it == itend || it.start() >= stop) {

      addRanges.push_back(std::make_pair(start, stop));

    }
    else {

      // Gap at left?

      if(it.start() > start)
	addRanges.push_back(std::make_pair(start, it.start()));

      for(; it != itend && it.start() < stop; ++it) {
    
	// Gap to the right of this extent?
	if(it.stop() < stop) {

	  TLMapTy::iterator nextit = it;
	  ++nextit;

	  uint64_t gapend;
	  if(nextit == itend)
	    gapend = stop;
	  else
	    gapend = std::min(stop, nextit.start());

	  if(it.stop() != gapend)
	    addRanges.push_back(std::make_pair(it.stop(), gapend));

	}

      }

    }

  }
  
  if(!addRanges.empty()) {

    TLMapPointer* writeStore = BB->getWritableTLStore(PtrTarget.second.V);
    for(SmallVector<std::pair<uint64_t, uint64_t>, 1>::iterator it = addRanges.begin(),
	  itend = addRanges.end(); it != itend; ++it) {

      writeStore->M->insert(it->first, it->second, true);

    }

  }

}

static void walkPathCondition(PathConditionTypes Ty, PathCondition& Cond, bool contextEnabled, ShadowBB* BB) {

  ShadowValue CondSV = BB->IA->getFunctionRoot()->getPathConditionSV(Cond);
  uint64_t Len = 0;
  switch(Ty) {
  case PathConditionTypeIntmem:
    Len = GlobalAA->getTypeStoreSize(Cond.val->getType());
    break;
  case PathConditionTypeString:
    Len = cast<ConstantDataArray>(Cond.val)->getNumElements();
    break;
  case PathConditionTypeInt:
  case PathConditionTypeFptrmem:
    release_assert(0 && "Bad path condition type");
    llvm_unreachable();
  }

  markGoodBytes(CondSV, Len, contextEnabled, BB, Cond.offset);

}

static void walkPathConditions(PathConditionTypes Ty, std::vector<PathCondition>& Conds, bool contextEnabled, ShadowBB* BB, uint32_t stackDepth) {

  for(std::vector<PathCondition>::iterator it = Conds.begin(), itend = Conds.end(); it != itend; ++it) {

    if(stackDepth != it->fromStackIdx || BB->invar->BB != it->fromBB)
      continue;

    walkPathCondition(Ty, *it, contextEnabled, BB);

  }

}

void llvm::doTLCallMerge(ShadowBB* BB, InlineAttempt* IA) {

  TLMerger V(BB->IA, false);
  IA->visitLiveReturnBlocks(V);
  V.doMerge();
  
  BB->tlStore = V.newMap;

}

static void walkPathConditionsIn(PathConditions& PC, uint32_t stackIdx, ShadowBB* BB, bool contextEnabled, bool secondPass) {

  walkPathConditions(PathConditionTypeIntmem, PC.IntmemPathConditions, 
		     contextEnabled, BB, stackIdx);
  walkPathConditions(PathConditionTypeString, PC.StringPathConditions, 
		     contextEnabled, BB, stackIdx);

  for(std::vector<PathFunc>::iterator it = PC.FuncPathConditions.begin(),
	itend = PC.FuncPathConditions.end(); it != itend; ++it) {

    if(it->stackIdx != stackIdx)
      continue;
    
    it->IA->BBs[0]->tlStore = BB->tlStore;
    // Path conditions can be treated like committed code, as the user is responsible for checking
    // their applicability.
    it->IA->findTentativeLoads(/* commitDisabledHere = */false, secondPass);
    doTLCallMerge(BB, it->IA);
    
  }

}

void llvm::TLWalkPathConditions(ShadowBB* BB, bool contextEnabled, bool secondPass) {

  InlineAttempt* IA = BB->IA->getFunctionRoot();
  
  if(IA->targetCallInfo)
    walkPathConditionsIn(GlobalIHP->pathConditions, IA->targetCallInfo->targetStackDepth, BB, contextEnabled, secondPass);

  if(BB->IA->invarInfo->pathConditions)
    walkPathConditionsIn(*BB->IA->invarInfo->pathConditions, UINT_MAX, BB, contextEnabled, secondPass);

}

static void walkCopyInst(ShadowValue CopyFrom, ShadowValue CopyTo, ShadowValue LenSV, bool contextEnabled, ShadowBB* BB) {

  uint64_t Len;
  if(!tryGetConstantInt(LenSV, Len))
    return;

  markGoodBytes(CopyTo, Len, contextEnabled, BB);
  markGoodBytes(CopyFrom, Len, contextEnabled, BB);

}


static void updateTLStore(ShadowInstruction* SI, bool contextEnabled) {

  if(inst_is<AllocaInst>(SI)) {

    ShadowValue SV(SI);
    ShadowValue Base;
    getBaseObject(SV, Base);
    markGoodBytes(ShadowValue(SI), SI->parent->IA->getFunctionRoot()->localAllocas[Base.u.PtrOrFd.idx].storeSize, contextEnabled, SI->parent);

  }
  else if(LoadInst* LI = dyn_cast_inst<LoadInst>(SI)) {

    if(LI->isVolatile() && (!GlobalIHP->programSingleThreaded) && !SI->parent->IA->pass->volatileLoadIsSimple(LI))
      markAllObjectsTentative(SI->parent);
    else
      markGoodBytes(SI->getOperand(0), GlobalAA->getTypeStoreSize(LI->getType()), contextEnabled, SI->parent);

  }
  else if(StoreInst* StoreI = dyn_cast_inst<StoreInst>(SI)) {

    // I don't think there's a need to regard a volatile /store/ as a yield point, as this is *outgoing* interthread communication
    // if it communication at all. Compare pthread_unlock which is not a yield point to pthread_lock, which is.
    //if(StoreI->isVolatile())
    //markAllObjectsTentative(SI->parent);
    //else
    markGoodBytes(SI->getOperand(1), GlobalAA->getTypeStoreSize(StoreI->getValueOperand()->getType()), contextEnabled, SI->parent);

  }
  else if(CallInst* CallI = dyn_cast_inst<CallInst>(SI)) {

    if(inst_is<MemSetInst>(SI)) {

      uint64_t MemSize;
      if(!tryGetConstantInt(SI->getCallArgOperand(2), MemSize))
	return;

      markGoodBytes(SI->getCallArgOperand(0), MemSize, contextEnabled, SI->parent);

    }
    else if(inst_is<MemTransferInst>(SI)) {

      walkCopyInst(SI->getCallArgOperand(0), SI->getCallArgOperand(1), SI->getCallArgOperand(2), contextEnabled, SI->parent);

    }
    else {

      Function* F = getCalledFunction(SI);
      DenseMap<Function*, specialfunctions>::iterator findit;
      if(ReadFile* RF = SI->parent->IA->tryGetReadFile(SI)) {

	markGoodBytes(SI->getCallArgOperand(1), RF->readSize, contextEnabled, SI->parent);

      }
      else if((findit = SpecialFunctionMap.find(F)) != SpecialFunctionMap.end()) {

	switch(findit->second) {

	case SF_REALLOC:

	  walkCopyInst(SI, SI->getCallArgOperand(0), SI->getCallArgOperand(1), contextEnabled, SI->parent);
	  // Fall through to:

	case SF_MALLOC:
	  
	  {
	  
	    ShadowValue SV(SI);
	    ShadowValue Base;
	    getBaseObject(SV, Base);

	    markGoodBytes(SV, GlobalIHP->heap[Base.u.PtrOrFd.idx].storeSize, contextEnabled, SI->parent);

	  }

	default:
	  break;

	}

      }
      else if(((!F) && !GlobalIHP->programSingleThreaded) || GlobalIHP->yieldFunctions.count(F)) {

	if(GlobalIHP->pessimisticLocks.count(CallI)) {

	  // Pessimistic locks clobber at specialisation time;
	  // no runtime checking required.
	  return;

	}
	
	SmallDenseMap<CallInst*, std::vector<GlobalVariable*>, 4>::iterator findit =
	  GlobalIHP->lockDomains.find(CallI);

	if(findit != GlobalIHP->lockDomains.end()) {

	  for(std::vector<GlobalVariable*>::iterator it = findit->second.begin(),
		itend = findit->second.end(); it != itend; ++it) {

	    ShadowGV* SGV = &GlobalIHP->shadowGlobals[GlobalIHP->getShadowGlobalIndex(*it)];
	    ShadowValue SV(SGV);
	    TLMapPointer* TLObj = SI->parent->getWritableTLStore(SV);
	    // Mark whole object tentative:
	    TLObj->M->clear();

	  }

	}
	else {

	  // No explicit domain given; clobbers everything.
	  markAllObjectsTentative(SI->parent);

	}

      }

    }

  }

}

static bool shouldCheckRead(ImprovedVal& Ptr, uint64_t Size, ShadowBB* BB) {

  // Read from null?
  if(Ptr.V.isNullPointer())
    return false;

  // Read from constant global?
  if(Ptr.V.isGV() && Ptr.V.u.GV->G->isConstant())
    return false;

  bool verbose = false;

  if(verbose)
    errs() << "Read from " << itcache(Ptr.V) << ":\n";

  TLMapPointer* Map = BB->tlStore->getReadableStoreFor(Ptr.V);
  if(!Map) {
    if(verbose)
      errs() << "Whole map: " << BB->tlStore->allOthersClobbered << "\n";
    return BB->tlStore->allOthersClobbered;
  }

  if(verbose) {

    for(TLMapTy::iterator it = Map->M->begin(), itend = Map->M->end(); it != itend; ++it) {

      errs() << it.start() << "-" << it.stop() << "\n";

    }

  }

  TLMapTy::iterator it = Map->M->find(Ptr.Offset);
  bool coveredByMap = (it != Map->M->end() && ((int64_t)it.start()) <= Ptr.Offset && ((int64_t)it.stop()) >= Ptr.Offset + ((int64_t)Size));

  return !coveredByMap;
    
}

ThreadLocalState IntegrationAttempt::shouldCheckCopy(ShadowInstruction& SI, ShadowValue PtrOp, ShadowValue LenSV) {

  uint64_t Len;
  bool LenValid = tryGetConstantInt(LenSV, Len);
  std::pair<ValSetType, ImprovedVal> Ptr;

  if((!LenValid) || (!tryGetUniqueIV(PtrOp, Ptr)) || Ptr.first != ValSetTypePB)
    return TLS_NEVERCHECK;

  if(Len == 0)
    return TLS_NEVERCHECK;

  // memcpyValues is unpopulated if the copy didn't "work" during specialisation,
  // so there is nothing to check.
  DenseMap<ShadowInstruction*, SmallVector<IVSRange, 4> >::iterator findit = GlobalIHP->memcpyValues.find(&SI);
  if(findit == GlobalIHP->memcpyValues.end() || !findit->second.size())
    return TLS_NEVERCHECK;

  // Check each concrete value that was successfully read during information prop
  for(SmallVector<IVSRange, 4>::iterator it = findit->second.begin(),
	itend = findit->second.end(); it != itend; ++it) {

    if(it->second.isWhollyUnknown())
      continue;

    ImprovedVal ReadPtr = Ptr.second;
    ReadPtr.Offset += it->first.first;
    if(shouldCheckRead(ReadPtr, it->first.second - it->first.first, SI.parent))
      return TLS_MUSTCHECK;

  }

  // No value requires a runtime check
  return TLS_NOCHECK;
    
}

ThreadLocalState IntegrationAttempt::shouldCheckLoadFrom(ShadowInstruction& SI, ImprovedVal& Ptr, uint64_t LoadSize) {

  if(Ptr.V.isNullOrConst())
    return TLS_NEVERCHECK;

  ImprovedValSetMulti* IV = dyn_cast<ImprovedValSetMulti>(SI.i.PB);
  if(IV) {

    SmallVector<IVSRange, 4> vals;
    for(ImprovedValSetMulti::MapIt it = IV->Map.begin(), itend = IV->Map.end(); it != itend; ++it) {

      if(it.val().isWhollyUnknown())
	continue;      

      ImprovedVal ReadPtr = Ptr;
      ReadPtr.Offset += it.start();
      if(shouldCheckRead(ReadPtr, it.stop() - it.start(), SI.parent))
	return TLS_MUSTCHECK;

    }

    return TLS_NOCHECK;

  }

  return shouldCheckRead(Ptr, LoadSize, SI.parent) ? TLS_MUSTCHECK : TLS_NOCHECK;
  
}

ThreadLocalState IntegrationAttempt::shouldCheckLoad(ShadowInstruction& SI) {

  if(GlobalIHP->programSingleThreaded)
    return TLS_NEVERCHECK;

  if(inst_is<LoadInst>(&SI)) {

    // Load doesn't extract any useful information?
    ImprovedValSetSingle* IVS = dyn_cast<ImprovedValSetSingle>(SI.i.PB);
    if(IVS && IVS->isWhollyUnknown())
      return TLS_NEVERCHECK;

    ShadowValue PtrOp = SI.getOperand(0);
    std::pair<ValSetType, ImprovedVal> Single;
    ImprovedValSet* IV;

    uint64_t LoadSize = GlobalAA->getTypeStoreSize(SI.getType());

    getIVOrSingleVal(PtrOp, IV, Single);
    if(IV) {

      ImprovedValSetSingle* IVS = cast<ImprovedValSetSingle>(IV);

      if(IVS->isWhollyUnknown() || IVS->SetType != ValSetTypePB)
	return TLS_NEVERCHECK;

      ThreadLocalState result = TLS_NEVERCHECK;

      for(uint32_t i = 0, ilim = IVS->Values.size(); i != ilim && result != TLS_MUSTCHECK; ++i)
	result = std::min(shouldCheckLoadFrom(SI, IVS->Values[i], LoadSize), result);

      return result;

    }
    else {

      if(Single.first != ValSetTypePB)
	return TLS_NEVERCHECK;
      return shouldCheckLoadFrom(SI, Single.second, LoadSize);

    }

  }
  else if(inst_is<MemTransferInst>(&SI)) {

    ShadowValue PtrOp = SI.getCallArgOperand(1);
    ShadowValue Len = SI.getCallArgOperand(2);

    return shouldCheckCopy(SI, PtrOp, Len);

  }
  else {

    // Realloc instruction
    return shouldCheckCopy(SI, SI.getCallArgOperand(0), SI.getCallArgOperand(1));

  }

}

bool ShadowInstruction::isCopyInst() {

  if(inst_is<MemTransferInst>(this))
    return true;

  if(inst_is<CallInst>(this)) {

    Function* F = getCalledFunction(this);
    if(F && F->getName() == "realloc")
      return true;

  }

  return false;

}

void llvm::doTLStoreMerge(ShadowBB* BB) {

  TLMerger V(BB->IA, false);
  BB->IA->visitNormalPredecessorsBW(BB, &V, /* ctx = */0);
  V.doMerge();

  BB->tlStore = V.newMap;

}

void InlineAttempt::findTentativeLoads(bool commitDisabledHere, bool secondPass) {

  if(isRootMainCall()) {
    BBs[0]->tlStore = new TLLocalStore(0);
    BBs[0]->tlStore->allOthersClobbered = false;
  }

  if(invarInfo->frameSize != -1 || !Callers.size()) {
    BBs[0]->tlStore = BBs[0]->tlStore->getWritableFrameList();
    BBs[0]->tlStore->pushStackFrame(this);
  }

  findTentativeLoadsInLoop(0, commitDisabledHere, secondPass);

}

void IntegrationAttempt::TLAnalyseInstruction(ShadowInstruction& SI, bool commitDisabledHere, bool secondPass) {

  // Known always good (as opposed to TLS_NOCHECK, resulting from a previous tentative loads run?)
  if(SI.isThreadLocal == TLS_NEVERCHECK)
    return;

  if(inst_is<LoadInst>(&SI) || SI.isCopyInst()) {

    // Known that we must check when this block is reached from a loop preheader?
    // If so whether it is tentative from the latch is irrelevant.
    if(secondPass && SI.isThreadLocal == TLS_MUSTCHECK)
      return;

    SI.isThreadLocal = shouldCheckLoad(SI);
    
    if(SI.isThreadLocal == TLS_MUSTCHECK)
      readsTentativeData = true;

  }
  
  updateTLStore(&SI, !commitDisabledHere);

}

void IntegrationAttempt::findTentativeLoadsInUnboundedLoop(const Loop* UL, bool commitDisabledHere, bool secondPass) {

  ShadowLoopInvar* NewLInfo = invarInfo->LInfo[UL];
  ShadowBB* BB = getBB(NewLInfo->headerIdx);

  // Give header its store:
  BB->tlStore = getBB(NewLInfo->preheaderIdx)->tlStore;
  
  if(!edgeIsDead(getBBInvar(NewLInfo->latchIdx), getBBInvar(NewLInfo->headerIdx))) {

    if(!secondPass) {
      // Passing true for the last parameter causes the store to be given to the header from the latch
      // and not to any exit blocks. 
      findTentativeLoadsInLoop(UL, commitDisabledHere, false, true);
      BB->tlStore = getBB(NewLInfo->latchIdx)->tlStore;
    }
    findTentativeLoadsInLoop(UL, commitDisabledHere, true);

  }
  else {

    findTentativeLoadsInLoop(UL, commitDisabledHere, secondPass);

  }

}

void IntegrationAttempt::findTentativeLoadsInLoop(const Loop* L, bool commitDisabledHere, bool secondPass, bool latchToHeader) {

  // Don't repeat search due to sharing:
  if(tentativeLoadsRun)
    return;

  TLProgress();

  ShadowLoopInvar* LInfo = L ? invarInfo->LInfo[L] : 0;
  
  uint32_t startIdx;
  if(L)
    startIdx = LInfo->headerIdx;
  else
    startIdx = 0;

  for(uint32_t i = startIdx, ilim = nBBs + BBsOffset; i != ilim && ((!L) || L->contains(getBBInvar(i)->naturalScope)); ++i) {

    ShadowBB* BB = getBB(i);
    if(!BB)
      continue;
    
    if(BB->invar->naturalScope != L) {

      ShadowLoopInvar* NewLInfo = invarInfo->LInfo[BB->invar->naturalScope];

      PeelAttempt* LPA;
      if((LPA = getPeelAttempt(BB->invar->naturalScope)) && LPA->isTerminated()) {

	LPA->Iterations[0]->BBs[0]->tlStore = getBB(NewLInfo->preheaderIdx)->tlStore;
	bool commitDisabled = commitDisabledHere || !LPA->isEnabled();
	uint32_t latchIdx = NewLInfo->latchIdx;

	for(uint32_t j = 0, jlim = LPA->Iterations.size(); j != jlim; ++j) {

	  LPA->Iterations[j]->findTentativeLoadsInLoop(BB->invar->naturalScope, commitDisabled, secondPass);
	  if(j + 1 != jlim)
	    LPA->Iterations[j + 1]->BBs[0]->tlStore = LPA->Iterations[j]->getBB(latchIdx)->tlStore;

	}
	
      }
      else {

	findTentativeLoadsInUnboundedLoop(BB->invar->naturalScope, 
					  commitDisabledHere || (LPA && !LPA->isEnabled()),
					  secondPass);

      }

      while(i != ilim && BB->invar->naturalScope->contains(getBBInvar(i)->naturalScope))
	++i;
      --i;
      continue;

    }

    if(i != startIdx) {

      doTLStoreMerge(BB);

    }

    TLWalkPathConditions(BB, !commitDisabledHere, secondPass);

    bool brokeOnUnreachableCall = false;

    for(uint32_t j = 0, jlim = BB->invar->insts.size(); j != jlim; ++j) {

      ShadowInstruction& SI = BB->insts[j];
      TLAnalyseInstruction(SI, commitDisabledHere, secondPass);
      
      if(inst_is<CallInst>(&SI)) {

	if(InlineAttempt* IA = getInlineAttempt(&SI)) {

	  IA->BBs[0]->tlStore = BB->tlStore;
	  IA->findTentativeLoads(commitDisabledHere || !IA->isEnabled(), secondPass);
	  doTLCallMerge(BB, IA);

	  if(!BB->tlStore) {

	    // Call exit unreachable
	    brokeOnUnreachableCall = true;
	    break;

	  }	    

	}

      }

    }

    if(!BB->tlStore) {

      // Block doesn't have a store due to a never-returns call.
      // Can't have any successors either in this case.

      release_assert(brokeOnUnreachableCall);
      continue;

    }

    // Give a store copy to each successor block that needs it. If latchToHeader is true,
    // ignore branches to outside the current loop; otherwise ignore any latch->header edge.

    for(uint32_t i = 0; i < BB->invar->succIdxs.size(); ++i) {

      if(!BB->succsAlive[i])
	continue;
      
      ShadowBBInvar* SuccBBI = getBBInvar(BB->invar->succIdxs[i]);
      if(L) {

	if(L != this->L && latchToHeader && !L->contains(SuccBBI->naturalScope))
	  continue;
	else if(L != this->L && (!latchToHeader) && SuccBBI->idx == LInfo->headerIdx) {
	  release_assert(BB->invar->idx == LInfo->latchIdx);
	  continue;
	}

      }

      // Create a store reference for each live successor
      ++BB->tlStore->refCount;

    }

    // Drop stack allocations here.

    if(BB->invar->succIdxs.size() == 0) {

      if(invarInfo->frameSize != -1) {
	BB->tlStore = BB->tlStore->getWritableFrameList();
	BB->tlStore->popStackFrame();
      }

    }

    // Drop the reference belonging to this block.

    if(!isa<ReturnInst>(BB->invar->BB->getTerminator()))
      BB->tlStore->dropReference();
    
  }

}

void IntegrationAttempt::resetTentativeLoads() {

  tentativeLoadsRun = false;

  for(DenseMap<ShadowInstruction*, InlineAttempt*>::iterator it = inlineChildren.begin(),
	itend = inlineChildren.end(); it != itend; ++it) {

    it->second->resetTentativeLoads();

  }

  for(DenseMap<const Loop*, PeelAttempt*>::iterator it = peelChildren.begin(),
	itend = peelChildren.end(); it != itend; ++it) {
    
    if(!it->second->isTerminated())
      continue;
    
    for(uint32_t i = 0; i < it->second->Iterations.size(); ++i)
      it->second->Iterations[i]->resetTentativeLoads();
    
  }

}

// Our main interface to other passes:

bool llvm::requiresRuntimeCheck(ShadowValue V, bool includeSpecialChecks) {

  if(GlobalIHP->omitChecks)
    return false;

  if(!V.isInst())
    return false;

  return V.u.I->parent->IA->requiresRuntimeCheck2(V, includeSpecialChecks);

}

void IntegrationAttempt::countTentativeInstructions() {

  if(isCommitted())
    return;

  for(uint32_t i = BBsOffset, ilim = BBsOffset + nBBs; i != ilim; ++i) {

    ShadowBBInvar* BBI = getBBInvar(i);
    ShadowBB* BB = getBB(*BBI);
    if(!BB)
      continue;

    if(BBI->naturalScope != L) {

      const Loop* subL = immediateChildLoop(L, BBI->naturalScope);
      PeelAttempt* LPA;
      if((LPA = getPeelAttempt(subL)) && LPA->isTerminated()) {

	while(i != ilim && subL->contains(getBBInvar(i)->naturalScope))
	  ++i;
	--i;
	continue;

      }

    }

    for(uint32_t j = 0, jlim = BBI->insts.size(); j != jlim; ++j) {

      ShadowInstruction* SI = &BB->insts[j];

      // This should count only instructions that are checked because their result might be
      // invalidated by the concurrent action of other threads in the same address space.
      // Instructions with SI->needsRuntimeCheck set are checked to implement a path condition
      // or other check and so should not be included in the count.
      
      if(requiresRuntimeCheck2(ShadowValue(SI), false) && SI->needsRuntimeCheck == RUNTIME_CHECK_NONE)
	++checkedInstructionsHere;

    }

  }

  checkedInstructionsChildren = checkedInstructionsHere;

  for(DenseMap<ShadowInstruction*, InlineAttempt*>::iterator it = inlineChildren.begin(),
	itend = inlineChildren.end(); it != itend; ++it) {

    it->second->countTentativeInstructions();
    checkedInstructionsChildren += it->second->checkedInstructionsChildren;

  }

  for(DenseMap<const Loop*, PeelAttempt*>::iterator it = peelChildren.begin(),
	itend = peelChildren.end(); it != itend; ++it) {

    if(!it->second->isTerminated())
      continue;

    for(uint32_t i = 0, ilim = it->second->Iterations.size(); i != ilim; ++i) {
     
      it->second->Iterations[i]->countTentativeInstructions();
      checkedInstructionsChildren += it->second->Iterations[i]->checkedInstructionsChildren;

    }

  }

}

bool PeelAttempt::containsTentativeLoads() {

  for(uint32_t i = 0, ilim = Iterations.size(); i != ilim; ++i)
    if(Iterations[i]->containsTentativeLoads())
      return true;

  return false;

}

bool IntegrationAttempt::containsTentativeLoads() {

  return readsTentativeData;

}

bool IntegrationAttempt::requiresRuntimeCheck2(ShadowValue V, bool includeSpecialChecks) {

  release_assert(V.isInst());
  ShadowInstruction* SI = V.u.I;

  if(SI->getType()->isVoidTy())
    return false;

  // This indicates a member of a disabled loop that hasn't been analysed.
  if(!SI->i.PB)
    return false;

  if(SI->needsRuntimeCheck == RUNTIME_CHECK_AS_EXPECTED)
    return true;
  if(includeSpecialChecks && SI->needsRuntimeCheck == RUNTIME_CHECK_SPECIAL)
    return true;

  if(inst_is<LoadInst>(SI) || inst_is<MemTransferInst>(SI)) {
    
    if(SI->isThreadLocal == TLS_MUSTCHECK)
      return true;
    
  }
  else if (inst_is<CallInst>(SI)) {
      
    InlineAttempt* IA = getInlineAttempt(SI);
    if(IA && (!IA->isEnabled()) && IA->containsTentativeLoads())
      return !SI->i.PB->isWhollyUnknown();

  }
  else if(inst_is<PHINode>(SI)) {

    ShadowBB* BB = SI->parent;
    for(uint32_t i = 0, ilim = BB->invar->predIdxs.size(); i != ilim; ++i) {

      ShadowBBInvar* predBBI = getBBInvar(BB->invar->predIdxs[i]);
      if(predBBI->naturalScope != L && ((!L) || L->contains(predBBI->naturalScope))) {

	PeelAttempt* LPA = getPeelAttempt(immediateChildLoop(L, predBBI->naturalScope));
	if(LPA && LPA->isTerminated() && (!LPA->isEnabled()) && LPA->containsTentativeLoads())
	  return !SI->i.PB->isWhollyUnknown();

      }

    }

  }

  return false;

}

void IntegrationAttempt::addCheckpointFailedBlocks() {

  if(isCommitted())
    return;

  for(uint32_t i = BBsOffset, ilim = BBsOffset + nBBs; i != ilim; ++i) {

    ShadowBBInvar* BBI = getBBInvar(i);
    ShadowBB* BB = getBB(*BBI);

    if(!BB)
      continue;

    if(BBI->naturalScope != L) {

      const Loop* subL = immediateChildLoop(L, BBI->naturalScope);
      PeelAttempt* LPA;

      if((LPA = getPeelAttempt(subL)) && LPA->isTerminated() && LPA->isEnabled()) {

	for(uint32_t k = 0, klim = LPA->Iterations.size(); k != klim; ++k)
	  LPA->Iterations[k]->addCheckpointFailedBlocks();	

	while(i != ilim && subL->contains(getBBInvar(i)->naturalScope))
	  ++i;
	--i;
	continue;
	
      }

    }

    for(uint32_t j = 0, jlim = BBI->insts.size(); j != jlim; ++j) {

      ShadowInstruction* SI = &BB->insts[j];
      InlineAttempt* IA;

      if(requiresRuntimeCheck2(ShadowValue(SI), false)) {

	// Treat tested exit PHIs as a block.
	if(inst_is<PHINode>(SI) && (j + 1) != jlim && inst_is<PHINode>(&BB->insts[j+1]))
	  continue;

	getFunctionRoot()->markBlockAndSuccsFailed(i, j + 1);

      }
      else if(SI->needsRuntimeCheck == RUNTIME_CHECK_SPECIAL) {

	// Special checks *precede* the instruction
	getFunctionRoot()->markBlockAndSuccsFailed(i, j);

      }
      else if((IA = getInlineAttempt(SI)) && IA->isEnabled()) {

	IA->addCheckpointFailedBlocks();
	if(IA->hasFailedReturnPath())
	  getFunctionRoot()->markBlockAndSuccsFailed(i, j + 1);

      }

    }

  }

}

void llvm::rerunTentativeLoads(ShadowInstruction* SI, InlineAttempt* IA) {

  if(IA->readsTentativeData) {

    // Conservatively assume that our inability to check where the data went
    // means we must assume it clobbered everything.
    errs() << "Warning: disabled context " << IA->SeqNumber << " reads tentative information\n";
    SI->parent->tlStore = SI->parent->tlStore->getEmptyMap();
    SI->parent->tlStore->allOthersClobbered = true;

  }
  else {

    // As it does not read tentative information, the context simply has no effect.
    // Use a copy of the TL map backed up on entry for this purpose.
    release_assert(IA->backupTlStore);
    SI->parent->tlStore->dropReference();
    SI->parent->tlStore = IA->backupTlStore;

  }

}
