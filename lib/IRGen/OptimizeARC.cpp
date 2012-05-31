//===--- OptimizeARC.cpp - Reference Counting Optimizations ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements optimizations for reference counting, object
// allocation, and other runtime entrypoints.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "swift-optimize"
#include "swift/Subsystems.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

STATISTIC(NumNoopDeleted,
          "Number of no-op swift calls eliminated");
STATISTIC(NumRetainReleasePairs,
          "Number of swift retain/release pairs eliminated");
STATISTIC(NumAllocateReleasePairs,
          "Number of swift allocate/release pairs eliminated");
STATISTIC(NumStoreOnlyObjectsEliminated,
          "Number of swift stored-only objects eliminated");

//===----------------------------------------------------------------------===//
//                            Utility Functions
//===----------------------------------------------------------------------===//

enum RT_Kind {
  /// An instruction with this classification is known to not access (read or
  /// write) memory.
  RT_NoMemoryAccessed,
  
  /// SwiftHeapObject *swift_retain(SwiftHeapObject *object)
  RT_Retain,
  
  // void swift_retain_noresult(SwiftHeapObject *object)
  RT_RetainNoResult,
  
  // (i64,i64,i64) swift_retainAndReturnThree(SwiftHeapObject *obj, i64,i64,i64)
  RT_RetainAndReturnThree,
  
  /// void swift_release(SwiftHeapObject *object)
  RT_Release,
  
  /// SwiftHeapObject *swift_allocObject(SwiftHeapMetadata *metadata,
  ///                                    size_t size, size_t alignment)
  RT_AllocObject,
  
  /// This is not a runtime function that we support.  Maybe it is not a call,
  /// or is a call to something we don't care about.
  RT_Unknown,
};

/// classifyInstruction - Take a look at the specified instruction and classify
/// it into what kind of runtime entrypoint it is, if any.
static RT_Kind classifyInstruction(const Instruction &I) {
  if (!I.mayReadOrWriteMemory())
    return RT_NoMemoryAccessed;
  
  // Non-calls or calls to indirect functions are unknown.
  const CallInst *CI = dyn_cast<CallInst>(&I);
  if (CI == 0) return RT_Unknown;
  Function *F = CI->getCalledFunction();
  if (F == 0) return RT_Unknown;
  
  return StringSwitch<RT_Kind>(F->getName())
    .Case("swift_retain", RT_Retain)
    .Case("swift_retain_noresult", RT_RetainNoResult)
    .Case("swift_release", RT_Release)
    .Case("swift_allocObject", RT_AllocObject)
    .Case("swift_retainAndReturnThree", RT_RetainAndReturnThree)
    .Default(RT_Unknown);
}

/// getRetain - Return a callable function for swift_retain.  F is the function
/// being operated on, ObjectPtrTy is an instance of the object pointer type to
/// use, and Cache is a null-initialized place to make subsequent requests
/// faster.
static Constant *getRetain(Function &F, Type *ObjectPtrTy, Constant *&Cache) {
  if (Cache) return Cache;
  
  AttributeWithIndex Attrs[] = {
    { Attribute::NoUnwind, ~0U }
  };
  auto AttrList = AttrListPtr::get(Attrs);

  Module *M = F.getParent();
  return Cache = M->getOrInsertFunction("swift_retain", AttrList,
                                        ObjectPtrTy, ObjectPtrTy, NULL);
}

/// getRetainNoResult - Return a callable function for swift_retain_noresult.
/// F is the function being operated on, ObjectPtrTy is an instance of the
/// object pointer type to use, and Cache is a null-initialized place to make
/// subsequent requests faster.
static Constant *getRetainNoResult(Function &F, Type *ObjectPtrTy,
                                   Constant *&Cache) {
  if (Cache) return Cache;
 
  AttributeWithIndex Attrs[] = {
    { Attribute::NoCapture, 1 },
    { Attribute::NoUnwind, ~0U },
  };
  auto AttrList = AttrListPtr::get(Attrs);
  Module *M = F.getParent();
  return Cache = M->getOrInsertFunction("swift_retain_noresult", AttrList,
                                        Type::getVoidTy(F.getContext()),
                                        ObjectPtrTy, NULL);
}

/// getRetainAndReturnThree - Return a callable function for
/// swift_retainAndReturnThree.  F is the function being operated on,
/// ObjectPtrTy is an instance of the object pointer type to use, and Cache is a
/// null-initialized place to make subsequent requests faster.
static Constant *getRetainAndReturnThree(Function &F, Type *ObjectPtrTy,
                                         Constant *&Cache) {
  if (Cache) return Cache;
  
  AttributeWithIndex Attrs[] = {
    { Attribute::NoUnwind, ~0U },
  };
  auto AttrList = AttrListPtr::get(Attrs);
  Module *M = F.getParent();
  
  Type *Int64Ty = Type::getInt64Ty(F.getContext());
  Type *RetTy = StructType::get(Int64Ty, Int64Ty, Int64Ty, NULL);
  
  return Cache = M->getOrInsertFunction("swift_retainAndReturnThree", AttrList,
                                        RetTy, ObjectPtrTy,
                                        Int64Ty, Int64Ty, Int64Ty, NULL);
}

//===----------------------------------------------------------------------===//
//                          Input Function Canonicalizer
//===----------------------------------------------------------------------===//

/// updateCallValueUses - We have something like this:
///   %z = ptrtoint %swift.refcounted* %2 to i64
///   %3 = call { i64, i64, i64 } 
///           @swift_retainAndReturnThree(..., i64 %x, i64 %1, i64 %z)
///   %a = extractvalue { i64, i64, i64 } %3, 0
///   %b = extractvalue { i64, i64, i64 } %3, 1
///   %c = extractvalue { i64, i64, i64 } %3, 2
///   %a1 = inttoptr i64 %a to i8*
///   %c1 = inttoptr i64 %c to %swift.refcounted*
///
/// This function is invoked three times (once each for the three arg/retvalues
/// that need to be replaced) and tries a best effort to patch up things to
/// avoid all the casts.  "Inst" coming into here is the call to
/// swift_retainAndReturnThree or to an extract that returns all three words.
///
static void updateCallValueUses(CallInst &CI, unsigned EltNo) {
  Value *Op = CI.getArgOperand(1+EltNo);
  for (auto UI = CI.use_begin(), E = CI.use_end(); UI != E; ++UI) {
    ExtractValueInst *Extract = dyn_cast<ExtractValueInst>(*UI);

    // Make sure this extract is relevant to EltNo.
    if (Extract == 0 || Extract->getNumIndices() != 1 ||
        Extract->getIndices()[0] != EltNo)
      continue;

    // Both the input and result should be i64's.  
    assert(Extract->getType() == Op->getType() && "Should have i64's here");

    for (auto UI2 = Extract->use_begin(), E = Extract->use_end(); UI2 != E; ) {
      IntToPtrInst *ExtractUser = dyn_cast<IntToPtrInst>(*UI2++);
      PtrToIntInst *OpCast = dyn_cast<PtrToIntInst>(Op);
      if (ExtractUser && OpCast &&
          OpCast->getOperand(0)->getType() == ExtractUser->getType()) {
        ExtractUser->replaceAllUsesWith(OpCast->getOperand(0));
        ExtractUser->eraseFromParent();
      }
    }
    
    // Stitch up anything other than the ptrtoint -> inttoptr.
    Extract->replaceAllUsesWith(Op);

    // Zap the dead ExtractValue's.
    RecursivelyDeleteTriviallyDeadInstructions(Extract);
    return;
  }
}

/// canonicalizeInputFunction - Functions like swift_retain return an
/// argument as a low-level performance optimization.  This makes it difficult
/// to reason about pointer equality though, so undo it as an initial
/// canonicalization step.  After this step, all swift_retain's have been
/// replaced with swift_retain_noresult.
///
/// This also does some trivial peep-hole optimizations as we go.
static bool canonicalizeInputFunction(Function &F) {
  Constant *RetainNoResultCache = 0;
  
  bool Changed = false;
  for (auto &BB : F)
  for (auto I = BB.begin(); I != BB.end(); ) {
    Instruction &Inst = *I++;
    
    switch (classifyInstruction(Inst)) {
    case RT_Unknown:
    case RT_AllocObject:
    case RT_NoMemoryAccessed:
      break;
    case RT_RetainNoResult: {
      CallInst &CI = cast<CallInst>(Inst);
      Value *ArgVal = CI.getArgOperand(0);
      // retain_noresult(null) is a no-op.
      if (isa<ConstantPointerNull>(ArgVal)) {
        CI.eraseFromParent();
        Changed = true;
        ++NumNoopDeleted;
        continue;
      }
      break;
    }
    case RT_Retain: {
      // If any x = swift_retain(y)'s got here, canonicalize them into:
      //   x = y; swift_retain_noresult(y).
      // This is important even though the front-end doesn't generate them,
      // because inlined functions can be ARC optimized, and thus may contain
      // swift_retain calls.
      CallInst &CI = cast<CallInst>(Inst);
      Value *ArgVal = CI.getArgOperand(0);

      // Rewrite uses of the result to use the argument.
      if (!CI.use_empty()) {
        Inst.replaceAllUsesWith(ArgVal);
        Changed = true;
      }
      
      // Insert a call to swift_retain_noresult to replace this and reset the
      // iterator so that we visit it next.
      I = CallInst::Create(getRetainNoResult(F, ArgVal->getType(),
                                             RetainNoResultCache),
                           ArgVal, "", &CI);
      CI.eraseFromParent();
      Changed = true;
      break;
    }
    case RT_Release: {
      CallInst &CI = cast<CallInst>(Inst);
      // swift_release(null) is a noop, zap it. 
      Value *ArgVal = CI.getArgOperand(0);
      if (isa<ConstantPointerNull>(ArgVal)) {
        CI.eraseFromParent();
        Changed = true;
        ++NumNoopDeleted;
        continue;
      }
      break;
    }
    case RT_RetainAndReturnThree: {
      // (a,b,c) = swift_retainAndReturnThree(obj, d,e,f)
      // -> swift_retain_noresult(obj)
      // -> (a,b,c) = (d,e,f)
      //
      // The important case of doing this is when this function has been inlined
      // into another function.  In this case, there is no return anymore.
      CallInst &CI = cast<CallInst>(Inst);
      
      IRBuilder<> B(&CI);
      Type *HeapObjectTy = CI.getArgOperand(0)->getType();
      
      // Reprocess starting at the new swift_retain_noresult.
      I = B.CreateCall(getRetainNoResult(F, HeapObjectTy, RetainNoResultCache),
                       CI.getArgOperand(0));
      
      // See if we can eliminate all of the extractvalue's that are hanging off
      // the swift_retainAndReturnThree.  This is important to eliminate casts
      // that will block optimizations and generally results in better IR.  Note
      // that this is just a best-effort attempt though.
      updateCallValueUses(CI, 0);
      updateCallValueUses(CI, 1);
      updateCallValueUses(CI, 2);
      
      // If our best-effort wasn't good enough, fall back to generating terrible
      // but correct code.
      if (!CI.use_empty()) {
        Value *V = UndefValue::get(CI.getType());
        V = B.CreateInsertValue(V, CI.getArgOperand(1), 0U);
        V = B.CreateInsertValue(V, CI.getArgOperand(2), 1U);
        V = B.CreateInsertValue(V, CI.getArgOperand(3), 2U);
        CI.replaceAllUsesWith(V);
      }

      CI.eraseFromParent();
      Changed = true;
      break;
    }
    }
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
//                         Release() Motion
//===----------------------------------------------------------------------===//

/// performLocalReleaseMotion - Scan backwards from the specified release,
/// moving it earlier in the function if possible, over instructions that do not
/// access the released object.  If we get to a retain or allocation of the
/// object, zap both.
static bool performLocalReleaseMotion(CallInst &Release, BasicBlock &BB) {
  // FIXME: Call classifier should identify the object for us.  Too bad C++
  // doesn't have convenient oneof's.
  Value *ReleasedObject = Release.getArgOperand(0);
  
  BasicBlock::iterator BBI = &Release;
  
  // Scan until we get to the top of the block.
  while (BBI != BB.begin()) {
    --BBI;
  
    // Don't analyze PHI nodes.  We can't move retains before them and they 
    // aren't "interesting".
    if (isa<PHINode>(BBI) ||
        // If we found the instruction that defines the value we're releasing,
        // don't push the release past it.
        &*BBI == ReleasedObject) {
      ++BBI;
      goto OutOfLoop;
    }
    
    switch (classifyInstruction(*BBI)) {
    case RT_Retain: // Canonicalized away, shouldn't exist.
      case RT_RetainAndReturnThree:
      assert(0 && "these entrypoints should be canonicalized away");
    case RT_NoMemoryAccessed:
      // Skip over random instructions that don't touch memory.  They don't need
      // protection by retain/release.
      continue;
        
    case RT_Release: {
      // If we get to a release, we can generally ignore it and scan past it.
      // However, if we get to a release of obviously the same object, we stop
      // scanning here because it should have already be moved as early as
      // possible, so there is no reason to move its friend to the same place.
      //
      // NOTE: If this occurs frequently, maybe we can have a release(Obj, N)
      // API to drop multiple retain counts at once.
      CallInst &ThisRelease = cast<CallInst>(*BBI);
      Value *ThisReleasedObject = ThisRelease.getArgOperand(0);
      if (ThisReleasedObject == ReleasedObject) {
        //Release.dump(); ThisRelease.dump(); BB.getParent()->dump();
        ++BBI;
        goto OutOfLoop;
      }
      continue;
    }

    case RT_RetainNoResult: {  // swift_retain_noresult(obj)
      CallInst &Retain = cast<CallInst>(*BBI);
      Value *RetainedObject = Retain.getArgOperand(0);
      
      // If the retain and release are to obviously pointer-equal objects, then
      // we can delete both of them.  We have proven that they do not protect
      // anything of value.
      if (RetainedObject == ReleasedObject) {
        // Note: this assumes the retain was properly canonicalized!
        Retain.eraseFromParent();
        Release.eraseFromParent();
        ++NumRetainReleasePairs;
        return true;
      }
      
      // Otherwise, this is a retain of an object that is not statically known
      // to be the same object.  It may still be dynamically the same object
      // though.  In this case, we can't move the release past it.
      // TODO: Strengthen analysis.
      ++BBI;
      goto OutOfLoop;
    }

    case RT_AllocObject: {   // %obj = swift_alloc(...)
      CallInst &Allocation = cast<CallInst>(*BBI);
        
      // If this is an allocation of an unrelated object, just ignore it.
      // TODO: This is not safe without proving the object being released is not
      // related to the allocated object.  Consider something silly like this:
      //   A = allocate()
      //   B = bitcast A to object
      //   release(B)
      if (ReleasedObject != &Allocation) {
        // Release.dump(); BB.getParent()->dump();
        ++BBI;
        goto OutOfLoop;
      }

      // If this is a release right after an allocation of the object, then we
      // can zap both.
      Allocation.replaceAllUsesWith(UndefValue::get(Allocation.getType()));
      Allocation.eraseFromParent();
      Release.eraseFromParent();
      ++NumAllocateReleasePairs;
      return true;
    }

    case RT_Unknown:
      // BBI->dump();
      // Otherwise, we get to something unknown/unhandled.  Bail out for now.
      ++BBI;
      goto OutOfLoop;
    }
  }
OutOfLoop:

  
  // If we got to the top of the block, (and if the instruction didn't start
  // there) move the release to the top of the block.
  // TODO: This is where we'd plug in some global algorithms someday.
  if (&*BBI != &Release) {
    Release.moveBefore(BBI);
    return true;
  }
  
  return false;
}

//===----------------------------------------------------------------------===//
//                       Store-Only Object Elimination
//===----------------------------------------------------------------------===//

/// performStoreOnlyObjectElimination - Scan the graph of uses of the specified
/// object allocation.  If the object does not escape and is only stored to
/// (this happens because GVN and other optimizations hoists forward substitutes
/// all stores to the object to eliminate all loads from it), then zap the
/// object and all accesses related to it.
static bool performStoreOnlyObjectElimination(CallInst &Allocation,
                                              BasicBlock::iterator &BBI) {
  // Do a depth first search exploring all of the uses of the object pointer,
  // following through casts, pointer adjustments etc.  If we find any loads or
  // any escape sites of the object, we give up.  If we succeed in walking the
  // entire graph of uses, we can remove the resultant set.
  SmallSetVector<Instruction*, 16> InvolvedInstructions;
  SmallVector<Instruction*, 16> Worklist;
  Worklist.push_back(&Allocation);
  
  // Stores - Keep track of all of the store instructions we see.
  SmallVector<StoreInst*, 16> Stores;

  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();
    
    // Insert the instruction into our InvolvedInstructions set.  If we have
    // already seen it, then don't reprocess all of the uses.
    if (!InvolvedInstructions.insert(I)) continue;
    
    // Okay, this is the first time we've seen this instruction, proceed.
    switch (classifyInstruction(*I)) {
    case RT_Retain:
    case RT_RetainAndReturnThree:
      assert(0 && "These should be canonicalized away");

    case RT_AllocObject:
      // If this is a different swift_allocObject than we started with, then
      // there is some computation feeding into a size or alignment computation
      // that we have to keep... unless we can delete *that* entire object as
      // well.
      break;

      // If no memory is accessed, then something is being done with the
      // pointer: maybe it is bitcast or GEP'd. Since there are no side effects,
      // it is perfectly fine to delete this instruction if all uses of the
      // instruction are also eliminable.
    case RT_NoMemoryAccessed:
      if (I->mayHaveSideEffects() || isa<TerminatorInst>(I))
        return false;
      break;
        
      // It is perfectly fine to eliminate various retains and releases of this
      // object: we are zapping all accesses or none.
    case RT_Release:
    case RT_RetainNoResult:
      break;
        
      // If this is an unknown instruction, we have more interesting things to
      // consider.
    case RT_Unknown:
      // Otherwise, this really is some unhandled instruction.  Bail out.
      return false;
    }
    
    // Okay, if we got here, the instruction can be eaten so-long as all of its
    // uses can be.  Scan through the uses and add them to the worklist for
    // recursive processing.
    for (auto UI = I->use_begin(), E = I->use_end(); UI != E; ++UI) {
      Instruction *User = cast<Instruction>(*UI);
      
      // Handle stores as a special case here: we want to make sure that the
      // object is being stored *to*, not itself being stored (which would be an
      // escape point).  Since stores themselves don't have any uses, we can
      // short-cut the classification scheme above.
      if (StoreInst *SI = dyn_cast<StoreInst>(User)) {
        // If this is a store *to* the object, we can zap it.
        if (UI.getOperandNo() == StoreInst::getPointerOperandIndex()) {
          InvolvedInstructions.insert(SI);
          continue;
        }
        // Otherwise, using the object as a source (or size) is an escape.
        return false;
      }
      if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(User)) {
        // If this is a memset/memcpy/memmove *to* the object, we can zap it.
        if (UI.getOperandNo() == 0) {
          InvolvedInstructions.insert(MI);
          continue;
        }
        // Otherwise, using the object as a source (or size) is an escape.
        return false;
      }
      
      // Otherwise, normal instructions just go on the worklist for processing.
      Worklist.push_back(User);
    }
  }
  
  // Ok, we succeeded!  This means we can zap all of the instructions that use
  // the object.  One thing we have to be careful of is to make sure that we
  // don't invalidate "BBI" (the iterator the outer walk of the optimization
  // pass is using, and indicates the next instruction to process).  This would
  // happen if we delete the instruction it is pointing to.  Advance the
  // iterator if that would happen.
  while (InvolvedInstructions.count(BBI))
    ++BBI;
  
  // Zap all of the instructions.
  for (auto I : InvolvedInstructions) {
    if (!I->use_empty())
      I->replaceAllUsesWith(UndefValue::get(I->getType()));
    I->eraseFromParent();
  }
  
  ++NumStoreOnlyObjectsEliminated;
  return true;
}

/// performGeneralOptimizations - This does a forward scan over basic blocks,
/// looking for interesting local optimizations that can be done.
static bool performGeneralOptimizations(Function &F) {
  bool Changed = false;
  
  // TODO: This is a really trivial local algorithm.  It could be much better.
  for (BasicBlock &BB : F) {
    for (BasicBlock::iterator BBI = BB.begin(), E = BB.end(); BBI != E; ) {
      // Preincrement the iterator to avoid invalidation and out trouble.
      Instruction &I = *BBI++;

      // Do various optimizations based on the instruction we find.
      switch (classifyInstruction(I)) {
      default: break;
      case RT_Release:
        Changed |= performLocalReleaseMotion(cast<CallInst>(I), BB);
        break;
      case RT_AllocObject:
        Changed |= performStoreOnlyObjectElimination(cast<CallInst>(I), BBI);
        break;
      }
    }
  }
  return Changed;
}


//===----------------------------------------------------------------------===//
//                      Return Argument Optimizer
//===----------------------------------------------------------------------===//

/// isInstructionRangeNoops - Return true if the range [I+1,E) just contains
/// insertvalue and other noop instructions.
static bool isInstructionRangeNoops(BasicBlock::iterator I, 
                                    BasicBlock::iterator E) {
  for (++I; I != E; ++I)
    if (!isa<InsertValueInst>(*I) && !isa<ExtractValueInst>(*I) &&
        !isa<IntToPtrInst>(*I))
      return false;
  return true;
}

/// optimizeReturn3 - Look to see if we can optimize "ret (a,b,c)" - where one
/// of the three values was retained right before the return, into a
/// swift_retainAndReturnThree call.  This is particularly common when returning
/// a string or array slice.
static bool optimizeReturn3(ReturnInst *TheReturn) {
  // Ignore ret void.
  if (TheReturn->getNumOperands() == 0) return false;
  
  // See if this is a return of three things.
  Value *RetVal = TheReturn->getOperand(0);
  StructType *RetSTy = dyn_cast<StructType>(RetVal->getType());
  if (RetSTy == 0 || RetSTy->getNumElements() != 3) return false;

  // See if we can find scalars that feed into the return instruction.  If not,
  // bail out.
  Value *RetVals[3];
  for (unsigned i = 0; i != 3; ++i) {
    RetVals[i] = FindInsertedValue(RetVal, i);
    if (RetVals[i] == 0) return false;
    
    // If the scalar isn't int64 or pointer type, we can't transform it.
    if (!isa<PointerType>(RetVals[i]->getType()) &&
        !RetVals[i]->getType()->isIntegerTy(64))
      return false;
  }
  
  Constant *Cache = 0;  // Not utilized.
  
  // Inevitably, we have a series of insertvalues (and sometimes casts)
  // between the swift_retain and the return.  Check to see if any of the values
  // returned is retained, and if so, if the instructions between the retain and
  // the return are *just* bitcasts and inserts.
  for (unsigned i = 0; i != 3; ++i) {
    Instruction *InsertedElt = dyn_cast<Instruction>(RetVals[i]);
    if (InsertedElt == 0 || InsertedElt->getParent() != TheReturn->getParent())
      continue;
    
    // TODO: Skip noop ptr<->int casts if it matters.
    
    // See if we have a swift_retain.  Since this has to be in the same basic
    // block, it must already have been lowered to swift_retain for it to be a
    // candidate.
    if (classifyInstruction(*InsertedElt) != RT_Retain ||
        // The only use allowed is the insertelement that feeds return.
        !InsertedElt->hasNUses(1))
      continue;
      
    // Okay, we have a retain.  See if there is anything "scary" between the
    // retain and the return.
    if (!isInstructionRangeNoops(InsertedElt, TheReturn))
      continue;
    
    CallInst &TheRetain = cast<CallInst>(*InsertedElt);
    
    // Okay, there isn't.  This means that we can perform the transformation.
    // Get the argument to swift_retain (the result will be zapped when we zap
    // the call) as the object to retain (of %swift.refcounted* type).
    Value *RetainedObject = TheRetain.getArgOperand(0);
    RetVals[i] = RetainedObject;

    // Insert any new instructions before the return.
    IRBuilder<> B(TheReturn);
    Type *Int64Ty = B.getInt64Ty();

    // The swift_retainAndReturnThree function takes the three arguments as i64.
    // Cast the arguments to i64 if needed.
    
    // Update the element with a cast to i64 if needed.
    for (Value *&Elt : RetVals) {
      if (isa<PointerType>(Elt->getType()))
        Elt = B.CreatePtrToInt(Elt, Int64Ty);
    }
    
    // Call swift_retainAndReturnThree with our pointer to retain and the three
    // i64's.
    Function &F = *TheReturn->getParent()->getParent();
    Value *LibCall = getRetainAndReturnThree(F,RetainedObject->getType(),Cache);
    Value *NR = B.CreateCall4(LibCall, RetainedObject, RetVals[0], RetVals[1],
                              RetVals[2]);

    // The return type of the libcall is (i64,i64,i64).  Since at least one of
    // the pointers is a pointer (we retained it afterall!) we have to unpack
    // the elements, bitcast at least that one, and then repack to the proper
    // type expected by the ret instruction.
    for (unsigned i = 0; i != 3; ++i) {
      RetVals[i] = B.CreateExtractValue(NR, i);
      if (RetVals[i]->getType() != RetSTy->getElementType(i))
        RetVals[i] = B.CreateIntToPtr(RetVals[i], RetSTy->getElementType(i));
    }
    
    // Repack into an aggregate that can be returned.
    Value *RV = UndefValue::get(RetVal->getType());
    for (unsigned i = 0; i != 3; ++i)
      RV = B.CreateInsertValue(RV, RetVals[i], i);
    
    // Return the right thing and zap any instruction tree of inserts that
    // existed just to feed the old return.
    TheReturn->setOperand(0, RV);
    RecursivelyDeleteTriviallyDeadInstructions(RetVal);

    // Zap the retain that we're subsuming and we're done!
    if (!TheRetain.use_empty())
      TheRetain.replaceAllUsesWith(UndefValue::get(TheRetain.getType()));
    TheRetain.eraseFromParent();
    return true;
  }
  
  return false;
}

/// optimizeArgumentReturnFunctions - Functions like swift_retain return an
/// argument as a low-level performance optimization.  Manually make use of this
/// to reduce register pressure.
///
/// Coming into this function, we assume that the code is in canonical form:
/// none of these calls have any uses of their return values.
static bool optimizeArgumentReturnFunctions(Function &F) {
  Constant *RetainCache = nullptr;
  bool Changed = false;
  
  SmallVector<ReturnInst*, 8> Returns;
  
  // Since all of the calls are canonicalized, we know that we can just walk
  // through the function and collect the interesting heap object definitions by
  // getting the argument to these functions.  
  DenseMap<Value*, TinyPtrVector<Instruction*>> DefsOfValue;
  
  // Keep track of which order we see values in since iteration over a densemap
  // isn't in a deterministic order, and isn't efficient anyway.
  SmallVector<Value*, 16> DefOrder;

  // Do a first pass over the function, collecting all interesting definitions.
  // In this pass, we rewrite any intra-block uses that we can, since the
  // SSAUpdater doesn't handle them.
  DenseMap<Value*, Value*> LocalUpdates;
  for (BasicBlock &BB : F) {
    for (auto II = BB.begin(), E = BB.end(); II != E; ) {
      // Preincrement iterator to avoid iteration issues in the loop.
      Instruction &Inst = *II++;
      
      switch (classifyInstruction(Inst)) {
      case RT_Retain: assert(0 && "This should be canonicalized away!");
      case RT_RetainAndReturnThree:
      case RT_RetainNoResult: {
        Value *ArgVal = cast<CallInst>(Inst).getArgOperand(0);

        // First step: rewrite swift_retain_noresult to swift_retain, exposing
        // the result value.
        CallInst &CI =
           *CallInst::Create(getRetain(F, ArgVal->getType(), RetainCache),
                             ArgVal, "", &Inst);
        Inst.eraseFromParent();

        TinyPtrVector<Instruction*> &GlobalEntry = DefsOfValue[ArgVal];

        // If this is the first definition of a value for the argument that
        // we've seen, keep track of it in DefOrder.
        if (GlobalEntry.empty())
          DefOrder.push_back(ArgVal);

        // Check to see if there is already an entry for this basic block.  If
        // there is another local entry, switch to using the local value and
        // remove the previous value from the GlobalEntry.
        Value *&LocalEntry = LocalUpdates[ArgVal];
        if (LocalEntry) {
          Changed = true;
          CI.setArgOperand(0, LocalEntry);
          assert(GlobalEntry.back() == LocalEntry && "Local/Global mismatch?");
          GlobalEntry.pop_back();
        }
        
        LocalEntry = &CI;
        GlobalEntry.push_back(&CI);
        continue;
      }
      case RT_Unknown:
      case RT_Release:
      case RT_AllocObject:
      case RT_NoMemoryAccessed:
        // Remember returns in the first pass.
        if (ReturnInst *RI = dyn_cast<ReturnInst>(&Inst))
          Returns.push_back(RI);

        // Just remap any uses in the value.
        break;
      }
      
      // Check to see if there are any uses of a value in the LocalUpdates
      // map.  If so, remap it now to the locally defined version.
      for (unsigned i = 0, e = Inst.getNumOperands(); i != e; ++i)
        if (Value *V = LocalUpdates.lookup(Inst.getOperand(i))) {
          Changed = true;
          Inst.setOperand(i, V);
        }
    }
    LocalUpdates.clear();
  }
    
  // Now that we've collected all of the interesting heap object values that are
  // passed into argument-returning functions, rewrite uses of these pointers
  // with optimized lifetime-shorted versions of it.
  for (Value *Ptr : DefOrder) {
    // If Ptr is an instruction, remember its block.  If not, use the entry
    // block as its block (it must be an argument, constant, etc).
    BasicBlock *PtrBlock;
    if (Instruction *PI = dyn_cast<Instruction>(Ptr))
      PtrBlock = PI->getParent();
    else
      PtrBlock = &F.getEntryBlock();
    
    TinyPtrVector<Instruction*> &Defs = DefsOfValue[Ptr];
    // This is the same problem as SSA construction, so we just use LLVM's
    // SSAUpdater, with each retain as a definition of the virtual value.
    SSAUpdater Updater;
    Updater.Initialize(Ptr->getType(), Ptr->getName());
    
    // Set the return value of each of these calls as a definition of the
    // virtual value.
    for (auto D : Defs)
      Updater.AddAvailableValue(D->getParent(), D);
    
    // If we didn't add a definition for Ptr's block, then Ptr itself is
    // available in its block.
    if (!Updater.HasValueForBlock(PtrBlock))
      Updater.AddAvailableValue(PtrBlock, Ptr);
      

    // Rewrite uses of Ptr to their optimized forms.
    for (auto UI = Ptr->use_begin(), E = Ptr->use_end(); UI != E; ) {
      // Make sure to increment the use iterator before potentially rewriting
      // it.
      Use &U = UI.getUse();
      ++UI;
      
      // If the use is in the same block that defines it and the User is not a
      // PHI node, then this is a local use that shouldn't be rewritten.
      Instruction *User = cast<Instruction>(U.getUser());
      if (User->getParent() == PtrBlock && !isa<PHINode>(User))
        continue;
      
      // Otherwise, change it if profitable!
      Updater.RewriteUse(U);
      
      if (U.get() != Ptr)
        Changed = true;
    }
  }

  // Scan through all the returns to see if there are any that can be optimized.
  for (ReturnInst *RI : Returns)
    Changed |= optimizeReturn3(RI);
  
  
  return Changed;
}

//===----------------------------------------------------------------------===//
//                            SwiftARCOpt Pass
//===----------------------------------------------------------------------===//

namespace {
  class SwiftARCOpt : public FunctionPass {
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesCFG();
    }
    virtual bool runOnFunction(Function &F);
    
  public:
    static char ID;
    SwiftARCOpt() : FunctionPass(ID) {
      initializeObjCARCExpandPass(*PassRegistry::getPassRegistry());
    }
  };
}
namespace llvm {
  void initializeSwiftARCOptPass(PassRegistry&);
}

char SwiftARCOpt::ID = 0;
INITIALIZE_PASS(SwiftARCOpt,
                "swift-arc-optimize", "Swift ARC optimization", false, false)

// Optimization passes.
llvm::FunctionPass *swift::createSwiftARCOptPass() {
  return new SwiftARCOpt();
}


bool SwiftARCOpt::runOnFunction(Function &F) {
  bool Changed = false;
  
  // First thing: canonicalize swift_retain and similar calls so that nothing
  // uses their result.  This exposes the copy that the function does to the
  // optimizer.
  Changed |= canonicalizeInputFunction(F);
  
  // Next, do a pass with a couple of optimizations:
  // 1) release() motion, eliminating retain/release pairs when it turns out
  //    that a pair is not protecting anything that accesses the guarded heap
  //    object.
  // 2) deletion of stored-only objects - objects that are allocated and
  //    potentially retained and released, but are only stored to and don't
  //    escape.
  Changed |= performGeneralOptimizations(F);
  
  // Finally, rewrite remaining heap object uses to make use of the implicit
  // copy that swift_retain and similar functions perform.
  Changed |= optimizeArgumentReturnFunctions(F);
  return Changed;
}
