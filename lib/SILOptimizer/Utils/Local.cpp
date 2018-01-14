//===--- Local.cpp - Functions that perform local SIL transformations. ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/Local.h"
#include "swift/SILOptimizer/Utils/CFG.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include <deque>

using namespace swift;

static llvm::cl::opt<bool> EnableExpandAll("enable-expand-all",
                                           llvm::cl::init(false));

/// Creates an increment on \p Ptr before insertion point \p InsertPt that
/// creates a strong_retain if \p Ptr has reference semantics itself or a
/// retain_value if \p Ptr is a non-trivial value without reference-semantics.
NullablePtr<SILInstruction>
swift::createIncrementBefore(SILValue Ptr, SILInstruction *InsertPt) {
  // If we have a trivial type, just bail, there is no work to do.
  if (Ptr->getType().isTrivial(InsertPt->getModule()))
    return nullptr;

  // Set up the builder we use to insert at our insertion point.
  SILBuilder B(InsertPt);
  auto Loc = getCompilerGeneratedLocation();

  // If Ptr is refcounted itself, create the strong_retain and
  // return.
  if (Ptr->getType().isReferenceCounted(B.getModule())) {
    if (Ptr->getType().is<UnownedStorageType>())
      return B.createUnownedRetain(Loc, Ptr, B.getDefaultAtomicity());
    else
      return B.createStrongRetain(Loc, Ptr, B.getDefaultAtomicity());
  }

  // Otherwise, create the retain_value.
  return B.createRetainValue(Loc, Ptr, B.getDefaultAtomicity());
}

/// Creates a decrement on \p Ptr before insertion point \p InsertPt that
/// creates a strong_release if \p Ptr has reference semantics itself or
/// a release_value if \p Ptr is a non-trivial value without reference-semantics.
NullablePtr<SILInstruction>
swift::createDecrementBefore(SILValue Ptr, SILInstruction *InsertPt) {
  if (Ptr->getType().isTrivial(InsertPt->getModule()))
    return nullptr;

  // Setup the builder we will use to insert at our insertion point.
  SILBuilder B(InsertPt);
  auto Loc = getCompilerGeneratedLocation();

  // If Ptr has reference semantics itself, create a strong_release.
  if (Ptr->getType().isReferenceCounted(B.getModule())) {
    if (Ptr->getType().is<UnownedStorageType>())
      return B.createUnownedRelease(Loc, Ptr, B.getDefaultAtomicity());
    else
      return B.createStrongRelease(Loc, Ptr, B.getDefaultAtomicity());
  }

  // Otherwise create a release value.
  return B.createReleaseValue(Loc, Ptr, B.getDefaultAtomicity());
}

/// \brief Perform a fast local check to see if the instruction is dead.
///
/// This routine only examines the state of the instruction at hand.
bool
swift::isInstructionTriviallyDead(SILInstruction *I) {
  // At Onone, consider all uses, including the debug_info.
  // This way, debug_info is preserved at Onone.
  if (I->hasUsesOfAnyResult() &&
      I->getFunction()->getEffectiveOptimizationMode() <=
        OptimizationMode::NoOptimization)
    return false;

  if (!onlyHaveDebugUsesOfAllResults(I) || isa<TermInst>(I))
    return false;

  if (auto *BI = dyn_cast<BuiltinInst>(I)) {
    // Although the onFastPath builtin has no side-effects we don't want to
    // remove it.
    if (BI->getBuiltinInfo().ID == BuiltinValueKind::OnFastPath)
      return false;
    return !BI->mayHaveSideEffects();
  }

  // condfail instructions that obviously can't fail are dead.
  if (auto *CFI = dyn_cast<CondFailInst>(I))
    if (auto *ILI = dyn_cast<IntegerLiteralInst>(CFI->getOperand()))
      if (!ILI->getValue())
        return true;

  // mark_uninitialized is never dead.
  if (isa<MarkUninitializedInst>(I))
    return false;
  if (isa<MarkUninitializedBehaviorInst>(I))
    return false;

  if (isa<DebugValueInst>(I) || isa<DebugValueAddrInst>(I))
    return false;

  // These invalidate enums so "write" memory, but that is not an essential
  // operation so we can remove these if they are trivially dead.
  if (isa<UncheckedTakeEnumDataAddrInst>(I))
    return true;
  
  if (!I->mayHaveSideEffects())
    return true;

  return false;
}

/// \brief Return true if this is a release instruction and the released value
/// is a part of a guaranteed parameter.
bool swift::isIntermediateRelease(SILInstruction *I,
                                  EpilogueARCFunctionInfo *EAFI) {
  // Check whether this is a release instruction.
  if (!isa<StrongReleaseInst>(I) && !isa<ReleaseValueInst>(I))
    return false;

  // OK. we have a release instruction.
  // Check whether this is a release on part of a guaranteed function argument.
  SILValue Op = stripValueProjections(I->getOperand(0));
  auto *Arg = dyn_cast<SILFunctionArgument>(Op);
  if (!Arg)
    return false;

  // This is a release on a guaranteed parameter. Its not the final release.
  if (Arg->hasConvention(SILArgumentConvention::Direct_Guaranteed))
    return true;

  // This is a release on an owned parameter and its not the epilogue release.
  // Its not the final release.
  auto Rel = EAFI->computeEpilogueARCInstructions(
      EpilogueARCContext::EpilogueARCKind::Release, Arg);
  if (Rel.size() && !Rel.count(I))
    return true;

  // Failed to prove anything.
  return false;
}

namespace {
  using CallbackTy = std::function<void(SILInstruction *)>;
} // end anonymous namespace

void swift::
recursivelyDeleteTriviallyDeadInstructions(ArrayRef<SILInstruction *> IA,
                                           bool Force, CallbackTy Callback) {
  // Delete these instruction and others that become dead after it's deleted.
  llvm::SmallPtrSet<SILInstruction *, 8> DeadInsts;
  for (auto I : IA) {
    // If the instruction is not dead and force is false, do nothing.
    if (Force || isInstructionTriviallyDead(I))
      DeadInsts.insert(I);
  }
  llvm::SmallPtrSet<SILInstruction *, 8> NextInsts;
  while (!DeadInsts.empty()) {
    for (auto I : DeadInsts) {
      // Call the callback before we mutate the to be deleted instruction in any
      // way.
      Callback(I);

      // Check if any of the operands will become dead as well.
      MutableArrayRef<Operand> Ops = I->getAllOperands();
      for (Operand &Op : Ops) {
        SILValue OpVal = Op.get();
        if (!OpVal)
          continue;

        // Remove the reference from the instruction being deleted to this
        // operand.
        Op.drop();

        // If the operand is an instruction that is only used by the instruction
        // being deleted, delete it.
        if (auto *OpValInst = OpVal->getDefiningInstruction())
          if (!DeadInsts.count(OpValInst) &&
              isInstructionTriviallyDead(OpValInst))
            NextInsts.insert(OpValInst);
      }

      // If we have a function ref inst, we need to especially drop its function
      // argument so that it gets a proper ref decrement.
      auto *FRI = dyn_cast<FunctionRefInst>(I);
      if (FRI && FRI->getReferencedFunction())
        FRI->dropReferencedFunction();
    }

    for (auto I : DeadInsts) {
      // This will remove this instruction and all its uses.
      
      eraseFromParentWithDebugInsts(I);
    }

    NextInsts.swap(DeadInsts);
    NextInsts.clear();
  }
}

/// \brief If the given instruction is dead, delete it along with its dead
/// operands.
///
/// \param I The instruction to be deleted.
/// \param Force If Force is set, don't check if the top level instruction is
///        considered dead - delete it regardless.
void swift::recursivelyDeleteTriviallyDeadInstructions(SILInstruction *I,
                                                       bool Force,
                                                       CallbackTy Callback) {

  ArrayRef<SILInstruction *> AI = ArrayRef<SILInstruction *>(I);
  recursivelyDeleteTriviallyDeadInstructions(AI, Force, Callback);
}

void swift::eraseUsesOfInstruction(SILInstruction *Inst,
                                   CallbackTy Callback) {
  for (auto result : Inst->getResults()) {
    while (!result->use_empty()) {
      auto UI = result->use_begin();
      auto *User = UI->getUser();
      assert(User && "User should never be NULL!");

      // If the instruction itself has any uses, recursively zap them so that
      // nothing uses this instruction.
      eraseUsesOfInstruction(User, Callback);

      // Walk through the operand list and delete any random instructions that
      // will become trivially dead when this instruction is removed.

      for (auto &Op : User->getAllOperands()) {
        if (auto *OpI = Op.get()->getDefiningInstruction()) {
          // Don't recursively delete the instruction we're working on.
          // FIXME: what if we're being recursively invoked?
          if (OpI != Inst) {
            Op.drop();
            recursivelyDeleteTriviallyDeadInstructions(OpI, false, Callback);
          }
        }
      }
      Callback(User);
      User->eraseFromParent();
    }
  }
}

void swift::
collectUsesOfValue(SILValue V, llvm::SmallPtrSetImpl<SILInstruction *> &Insts) {
  for (auto UI = V->use_begin(), E = V->use_end(); UI != E; UI++) {
    auto *User = UI->getUser();
    // Instruction has been processed.
    if (!Insts.insert(User).second)
      continue;

    // Collect the users of this instruction.
    for (auto result : User->getResults())
      collectUsesOfValue(result, Insts);
  }
}

void swift::eraseUsesOfValue(SILValue V) {
  llvm::SmallPtrSet<SILInstruction *, 4> Insts;
  // Collect the uses.
  collectUsesOfValue(V, Insts);
  // Erase the uses, we can have instructions that become dead because
  // of the removal of these instructions, leave to DCE to cleanup.
  // Its not safe to do recursively delete here as some of the SILInstruction
  // maybe tracked by this set.
  for (auto I : Insts) {
    I->replaceAllUsesOfAllResultsWithUndef();
    I->eraseFromParent();
  }
}

// Devirtualization of functions with covariant return types produces
// a result that is not an apply, but takes an apply as an
// argument. Attempt to dig the apply out from this result.
FullApplySite swift::findApplyFromDevirtualizedResult(SILValue V) {
  if (auto Apply = FullApplySite::isa(V))
    return Apply;

  if (isa<UpcastInst>(V) || isa<EnumInst>(V) || isa<UncheckedRefCastInst>(V))
    return findApplyFromDevirtualizedResult(
             cast<SingleValueInstruction>(V)->getOperand(0));

  return FullApplySite();
}

SILValue swift::isPartialApplyOfReabstractionThunk(PartialApplyInst *PAI) {
  if (PAI->getNumArguments() != 1)
    return SILValue();

  auto *Fun = PAI->getReferencedFunction();
  if (!Fun)
    return SILValue();

  // Make sure we have a reabstraction thunk.
  if (Fun->isThunk() != IsReabstractionThunk)
    return SILValue();

  // The argument should be a closure.
  auto Arg = PAI->getArgument(0);
  if (!Arg->getType().is<SILFunctionType>() ||
      !Arg->getType().isReferenceCounted(PAI->getFunction()->getModule()))
    return SILValue();

  return Arg;
}


// Replace a dead apply with a new instruction that computes the same
// value, and delete the old apply.
void swift::replaceDeadApply(ApplySite Old, ValueBase *New) {
  auto *OldApply = Old.getInstruction();
  if (!isa<TryApplyInst>(OldApply))
    cast<SingleValueInstruction>(OldApply)->replaceAllUsesWith(New);
  recursivelyDeleteTriviallyDeadInstructions(OldApply, true);
}

bool swift::hasArchetypes(SubstitutionList Subs) {
  // Check whether any of the substitutions are dependent.
  return llvm::any_of(Subs, [](const Substitution &S) {
    return S.getReplacement()->hasArchetype();
  });
}

bool swift::mayBindDynamicSelf(SILFunction *F) {
  if (!F->hasSelfMetadataParam())
    return false;

  SILValue MDArg = F->getSelfMetadataArgument();

  for (Operand *MDUse : F->getSelfMetadataArgument()->getUses()) {
    SILInstruction *MDUser = MDUse->getUser();
    for (Operand &TypeDepOp : MDUser->getTypeDependentOperands()) {
      if (TypeDepOp.get() == MDArg)
        return true;
    }
  }
  return false;
}

/// Find a new position for an ApplyInst's FuncRef so that it dominates its
/// use. Not that FunctionRefInsts may be shared by multiple ApplyInsts.
void swift::placeFuncRef(ApplyInst *AI, DominanceInfo *DT) {
  FunctionRefInst *FuncRef = cast<FunctionRefInst>(AI->getCallee());
  SILBasicBlock *DomBB =
    DT->findNearestCommonDominator(AI->getParent(), FuncRef->getParent());
  if (DomBB == AI->getParent() && DomBB != FuncRef->getParent())
    // Prefer to place the FuncRef immediately before the call. Since we're
    // moving FuncRef up, this must be the only call to it in the block.
    FuncRef->moveBefore(AI);
  else
    // Otherwise, conservatively stick it at the beginning of the block.
    FuncRef->moveBefore(&*DomBB->begin());
}

/// \brief Add an argument, \p val, to the branch-edge that is pointing into
/// block \p Dest. Return a new instruction and do not erase the old
/// instruction.
TermInst *swift::addArgumentToBranch(SILValue Val, SILBasicBlock *Dest,
                                     TermInst *Branch) {
  SILBuilderWithScope Builder(Branch);

  if (auto *CBI = dyn_cast<CondBranchInst>(Branch)) {
    SmallVector<SILValue, 8> TrueArgs;
    SmallVector<SILValue, 8> FalseArgs;

    for (auto A : CBI->getTrueArgs())
      TrueArgs.push_back(A);

    for (auto A : CBI->getFalseArgs())
      FalseArgs.push_back(A);

    if (Dest == CBI->getTrueBB()) {
      TrueArgs.push_back(Val);
      assert(TrueArgs.size() == Dest->getNumArguments());
    } else {
      FalseArgs.push_back(Val);
      assert(FalseArgs.size() == Dest->getNumArguments());
    }

    return Builder.createCondBranch(CBI->getLoc(), CBI->getCondition(), CBI->getTrueBB(), TrueArgs, CBI->getFalseBB(), FalseArgs, CBI->getTrueBBCount(), CBI->getFalseBBCount());
  }

  if (auto *BI = dyn_cast<BranchInst>(Branch)) {
    SmallVector<SILValue, 8> Args;

    for (auto A : BI->getArgs())
      Args.push_back(A);

    Args.push_back(Val);
    assert(Args.size() == Dest->getNumArguments());
    return Builder.createBranch(BI->getLoc(), BI->getDestBB(), Args);
  }

  llvm_unreachable("unsupported terminator");
}

SILLinkage swift::getSpecializedLinkage(SILFunction *F, SILLinkage L) {
  if (hasPrivateVisibility(L) &&
      !F->isSerialized()) {
    // Specializations of private symbols should remain so, unless
    // they were serialized, which can only happen when specializing
    // definitions from a standard library built with -sil-serialize-all.
    return SILLinkage::Private;
  }

  return SILLinkage::Shared;
}

/// Remove all instructions in the body of \p BB in safe manner by using
/// undef.
void swift::clearBlockBody(SILBasicBlock *BB) {
  // Instructions in the dead block may be used by other dead blocks.  Replace
  // any uses of them with undef values.
  while (!BB->empty()) {
    // Grab the last instruction in the BB.
    auto *Inst = &BB->back();

    // Replace any still-remaining uses with undef values and erase.
    Inst->replaceAllUsesOfAllResultsWithUndef();
    Inst->eraseFromParent();
  }
}

// Handle the mechanical aspects of removing an unreachable block.
void swift::removeDeadBlock(SILBasicBlock *BB) {
  // Clear the body of BB.
  clearBlockBody(BB);

  // Now that the BB is empty, eliminate it.
  BB->eraseFromParent();
}

/// Cast a value into the expected, ABI compatible type if necessary.
/// This may happen e.g. when:
/// - a type of the return value is a subclass of the expected return type.
/// - actual return type and expected return type differ in optionality.
/// - both types are tuple-types and some of the elements need to be casted.
///
/// If CheckOnly flag is set, then this function only checks if the
/// required casting is possible. If it is not possible, then None
/// is returned.
///
/// If CheckOnly is not set, then a casting code is generated and the final
/// casted value is returned.
///
/// NOTE: We intentionally combine the checking of the cast's handling possibility
/// and the transformation performing the cast in the same function, to avoid
/// any divergence between the check and the implementation in the future.
///
/// NOTE: The implementation of this function is very closely related to the
/// rules checked by SILVerifier::requireABICompatibleFunctionTypes.
SILValue swift::castValueToABICompatibleType(SILBuilder *B, SILLocation Loc,
                                             SILValue Value,
                                             SILType SrcTy, SILType DestTy) {

  // No cast is required if types are the same.
  if (SrcTy == DestTy)
    return Value;

  assert(SrcTy.isAddress() == DestTy.isAddress() &&
         "Addresses aren't compatible with values");

  if (SrcTy.isAddress() && DestTy.isAddress()) {
    // Cast between two addresses and that's it.
    return B->createUncheckedAddrCast(Loc, Value, DestTy);
  }

  // If both types are classes and dest is the superclass of src,
  // simply perform an upcast.
  if (DestTy.isExactSuperclassOf(SrcTy)) {
    return B->createUpcast(Loc, Value, DestTy);
  }

  if (SrcTy.isHeapObjectReferenceType() &&
      DestTy.isHeapObjectReferenceType()) {
    return B->createUncheckedRefCast(Loc, Value, DestTy);
  }

  if (auto mt1 = SrcTy.getAs<AnyMetatypeType>()) {
    if (auto mt2 = DestTy.getAs<AnyMetatypeType>()) {
      if (mt1->getRepresentation() == mt2->getRepresentation()) {
        // If B.Type needs to be casted to A.Type and
        // A is a superclass of B, then it can be done by means
        // of a simple upcast.
        if (mt2.getInstanceType()->isExactSuperclassOf(
              mt1.getInstanceType())) {
          return B->createUpcast(Loc, Value, DestTy);
        }
 
        // Cast between two metatypes and that's it.
        return B->createUncheckedBitCast(Loc, Value, DestTy);
      }
    }
  }

  // Check if src and dest types are optional.
  auto OptionalSrcTy = SrcTy.getAnyOptionalObjectType();
  auto OptionalDestTy = DestTy.getAnyOptionalObjectType();

  // Both types are optional.
  if (OptionalDestTy && OptionalSrcTy) {
    // If both wrapped types are classes and dest is the superclass of src,
    // simply perform an upcast.
    if (OptionalDestTy.isExactSuperclassOf(OptionalSrcTy)) {
      // Insert upcast.
      return B->createUpcast(Loc, Value, DestTy);
    }

    // Unwrap the original optional value.
    auto *SomeDecl = B->getASTContext().getOptionalSomeDecl();
    auto *NoneBB = B->getFunction().createBasicBlock();
    auto *SomeBB = B->getFunction().createBasicBlock();
    auto *CurBB = B->getInsertionPoint()->getParent();

    auto *ContBB = CurBB->split(B->getInsertionPoint());
    ContBB->createPHIArgument(DestTy, ValueOwnershipKind::Owned);

    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 1> CaseBBs;
    CaseBBs.push_back(std::make_pair(SomeDecl, SomeBB));
    B->setInsertionPoint(CurBB);
    B->createSwitchEnum(Loc, Value, NoneBB, CaseBBs);

    // Handle the Some case.
    B->setInsertionPoint(SomeBB);
    SILValue UnwrappedValue =  B->createUncheckedEnumData(Loc, Value,
                                                          SomeDecl);
    // Cast the unwrapped value.
    auto CastedUnwrappedValue =
        castValueToABICompatibleType(B, Loc, UnwrappedValue,
                                     OptionalSrcTy,
                                     OptionalDestTy);
    // Wrap into optional.
    auto CastedValue =  B->createOptionalSome(Loc, CastedUnwrappedValue, DestTy);
    B->createBranch(Loc, ContBB, {CastedValue});

    // Handle the None case.
    B->setInsertionPoint(NoneBB);
    CastedValue = B->createOptionalNone(Loc, DestTy);
    B->createBranch(Loc, ContBB, {CastedValue});
    B->setInsertionPoint(ContBB->begin());

    return ContBB->getArgument(0);
  }

  // Src is not optional, but dest is optional.
  if (!OptionalSrcTy && OptionalDestTy) {
    auto OptionalSrcCanTy = OptionalType::get(SrcTy.getSwiftRValueType())
      ->getCanonicalType();
    auto LoweredOptionalSrcType = SILType::getPrimitiveObjectType(
      OptionalSrcCanTy);

    // Wrap the source value into an optional first.
    SILValue WrappedValue = B->createOptionalSome(Loc, Value,
                                                  LoweredOptionalSrcType);
    // Cast the wrapped value.
    return castValueToABICompatibleType(B, Loc, WrappedValue,
                                        WrappedValue->getType(),
                                        DestTy);
  }

  // Handle tuple types.
  // Extract elements, cast each of them, create a new tuple.
  if (auto SrcTupleTy = SrcTy.getAs<TupleType>()) {
    SmallVector<SILValue, 8> ExpectedTuple;
    for (unsigned i = 0, e = SrcTupleTy->getNumElements(); i < e; i++) {
      SILValue Element = B->createTupleExtract(Loc, Value, i);
      // Cast the value if necessary.
      Element = castValueToABICompatibleType(B, Loc, Element,
                                             SrcTy.getTupleElementType(i),
                                             DestTy.getTupleElementType(i));
      ExpectedTuple.push_back(Element);
    }

    return B->createTuple(Loc, DestTy, ExpectedTuple);
  }

  // Function types are interchangeable if they're also ABI-compatible.
  if (SrcTy.is<SILFunctionType>()) {
    if (DestTy.is<SILFunctionType>()) {
      // Insert convert_function.
      return B->createConvertFunction(Loc, Value, DestTy);
    }
  }

  llvm::errs() << "Source type: " << SrcTy << "\n";
  llvm::errs() << "Destination type: " << DestTy << "\n";
  llvm_unreachable("Unknown combination of types for casting");
}

ProjectBoxInst *swift::getOrCreateProjectBox(AllocBoxInst *ABI, unsigned Index){
  SILBasicBlock::iterator Iter(ABI);
  Iter++;
  assert(Iter != ABI->getParent()->end() &&
         "alloc_box cannot be the last instruction of a block");
  SILInstruction *NextInst = &*Iter;
  if (auto *PBI = dyn_cast<ProjectBoxInst>(NextInst)) {
    if (PBI->getOperand() == ABI && PBI->getFieldIndex() == Index)
      return PBI;
  }

  SILBuilder B(NextInst);
  return B.createProjectBox(ABI->getLoc(), ABI, Index);
}

//===----------------------------------------------------------------------===//
//                       String Concatenation Optimizer
//===----------------------------------------------------------------------===//

namespace {
/// This is a helper class that performs optimization of string literals
/// concatenation.
class StringConcatenationOptimizer {
  /// Apply instruction being optimized.
  ApplyInst *AI;
  /// Builder to be used for creation of new instructions.
  SILBuilder &Builder;
  /// Left string literal operand of a string concatenation.
  StringLiteralInst *SLILeft = nullptr;
  /// Right string literal operand of a string concatenation.
  StringLiteralInst *SLIRight = nullptr;
  /// Function used to construct the left string literal.
  FunctionRefInst *FRILeft = nullptr;
  /// Function used to construct the right string literal.
  FunctionRefInst *FRIRight = nullptr;
  /// Apply instructions used to construct left string literal.
  ApplyInst *AILeft = nullptr;
  /// Apply instructions used to construct right string literal.
  ApplyInst *AIRight = nullptr;
  /// String literal conversion function to be used.
  FunctionRefInst *FRIConvertFromBuiltin = nullptr;
  /// Result type of a function producing the concatenated string literal.
  SILValue FuncResultType;

  /// Internal helper methods
  bool extractStringConcatOperands();
  void adjustEncodings();
  APInt getConcatenatedLength();
  bool isAscii() const;

public:
  StringConcatenationOptimizer(ApplyInst *AI, SILBuilder &Builder)
      : AI(AI), Builder(Builder) {}

  /// Tries to optimize a given apply instruction if it is a
  /// concatenation of string literals.
  ///
  /// Returns a new instruction if optimization was possible.
  SingleValueInstruction *optimize();
};

} // end anonymous namespace

/// Checks operands of a string concatenation operation to see if
/// optimization is applicable.
///
/// Returns false if optimization is not possible.
/// Returns true and initializes internal fields if optimization is possible.
bool StringConcatenationOptimizer::extractStringConcatOperands() {
  auto *Fn = AI->getReferencedFunction();
  if (!Fn)
    return false;

  if (AI->getNumArguments() != 3 || !Fn->hasSemanticsAttr("string.concat"))
    return false;

  assert(Fn->getRepresentation() == SILFunctionTypeRepresentation::Method);

  // Left and right operands of a string concatenation operation.
  AILeft = dyn_cast<ApplyInst>(AI->getOperand(1));
  AIRight = dyn_cast<ApplyInst>(AI->getOperand(2));

  if (!AILeft || !AIRight)
    return false;

  FRILeft = dyn_cast<FunctionRefInst>(AILeft->getCallee());
  FRIRight = dyn_cast<FunctionRefInst>(AIRight->getCallee());

  if (!FRILeft || !FRIRight)
    return false;

  auto *FRILeftFun = FRILeft->getReferencedFunction();
  auto *FRIRightFun = FRIRight->getReferencedFunction();

  if (FRILeftFun->getEffectsKind() >= EffectsKind::ReadWrite ||
      FRIRightFun->getEffectsKind() >= EffectsKind::ReadWrite)
    return false;

  if (!FRILeftFun->hasSemanticsAttrs() || !FRIRightFun->hasSemanticsAttrs())
    return false;

  auto AILeftOperandsNum = AILeft->getNumOperands();
  auto AIRightOperandsNum = AIRight->getNumOperands();

  // makeUTF16 should have following parameters:
  // (start: RawPointer, utf16CodeUnitCount: Word)
  // makeUTF8 should have following parameters:
  // (start: RawPointer, utf8CodeUnitCount: Word, isASCII: Int1)
  if (!((FRILeftFun->hasSemanticsAttr("string.makeUTF16") &&
         AILeftOperandsNum == 4) ||
        (FRILeftFun->hasSemanticsAttr("string.makeUTF8") &&
         AILeftOperandsNum == 5) ||
        (FRIRightFun->hasSemanticsAttr("string.makeUTF16") &&
         AIRightOperandsNum == 4) ||
        (FRIRightFun->hasSemanticsAttr("string.makeUTF8") &&
         AIRightOperandsNum == 5)))
    return false;

  assert(FRILeftFun->getRepresentation() ==
         SILFunctionTypeRepresentation::Method);
  assert(FRIRightFun->getRepresentation() ==
         SILFunctionTypeRepresentation::Method);

  SLILeft = dyn_cast<StringLiteralInst>(AILeft->getOperand(1));
  SLIRight = dyn_cast<StringLiteralInst>(AIRight->getOperand(1));

  if (!SLILeft || !SLIRight)
    return false;

  // Only UTF-8 and UTF-16 encoded string literals are supported by this
  // optimization.
  if (SLILeft->getEncoding() != StringLiteralInst::Encoding::UTF8 &&
      SLILeft->getEncoding() != StringLiteralInst::Encoding::UTF16)
    return false;

  if (SLIRight->getEncoding() != StringLiteralInst::Encoding::UTF8 &&
      SLIRight->getEncoding() != StringLiteralInst::Encoding::UTF16)
    return false;

  return true;
}

/// Ensures that both string literals to be concatenated use the same
/// UTF encoding. Converts UTF-8 into UTF-16 if required.
void StringConcatenationOptimizer::adjustEncodings() {
  if (SLILeft->getEncoding() == SLIRight->getEncoding()) {
    FRIConvertFromBuiltin = FRILeft;
    if (SLILeft->getEncoding() == StringLiteralInst::Encoding::UTF8) {
      FuncResultType = AILeft->getOperand(4);
    } else {
      FuncResultType = AILeft->getOperand(3);
    }
    return;
  }

  Builder.setCurrentDebugScope(AI->getDebugScope());

  // If one of the string literals is UTF8 and another one is UTF16,
  // convert the UTF8-encoded string literal into UTF16-encoding first.
  if (SLILeft->getEncoding() == StringLiteralInst::Encoding::UTF8 &&
      SLIRight->getEncoding() == StringLiteralInst::Encoding::UTF16) {
    FuncResultType = AIRight->getOperand(3);
    FRIConvertFromBuiltin = FRIRight;
    // Convert UTF8 representation into UTF16.
    SLILeft = Builder.createStringLiteral(AI->getLoc(), SLILeft->getValue(),
                                          StringLiteralInst::Encoding::UTF16);
  }

  if (SLIRight->getEncoding() == StringLiteralInst::Encoding::UTF8 &&
      SLILeft->getEncoding() == StringLiteralInst::Encoding::UTF16) {
    FuncResultType = AILeft->getOperand(3);
    FRIConvertFromBuiltin = FRILeft;
    // Convert UTF8 representation into UTF16.
    SLIRight = Builder.createStringLiteral(AI->getLoc(), SLIRight->getValue(),
                                           StringLiteralInst::Encoding::UTF16);
  }

  // It should be impossible to have two operands with different
  // encodings at this point.
  assert(SLILeft->getEncoding() == SLIRight->getEncoding() &&
        "Both operands of string concatenation should have the same encoding");
}

/// Computes the length of a concatenated string literal.
APInt StringConcatenationOptimizer::getConcatenatedLength() {
  // Real length of string literals computed based on its contents.
  // Length is in code units.
  auto SLILenLeft = SLILeft->getCodeUnitCount();
  (void) SLILenLeft;
  auto SLILenRight = SLIRight->getCodeUnitCount();
  (void) SLILenRight;

  // Length of string literals as reported by string.make functions.
  auto *LenLeft = dyn_cast<IntegerLiteralInst>(AILeft->getOperand(2));
  auto *LenRight = dyn_cast<IntegerLiteralInst>(AIRight->getOperand(2));

  // Real and reported length should be the same.
  assert(SLILenLeft == LenLeft->getValue() &&
         "Size of string literal in @_semantics(string.make) is wrong");

  assert(SLILenRight == LenRight->getValue() &&
         "Size of string literal in @_semantics(string.make) is wrong");


  // Compute length of the concatenated literal.
  return LenLeft->getValue() + LenRight->getValue();
}

/// Computes the isAscii flag of a concatenated UTF8-encoded string literal.
bool StringConcatenationOptimizer::isAscii() const{
  // Add the isASCII argument in case of UTF8.
  // IsASCII is true only if IsASCII of both literals is true.
  auto *AsciiLeft = dyn_cast<IntegerLiteralInst>(AILeft->getOperand(3));
  auto *AsciiRight = dyn_cast<IntegerLiteralInst>(AIRight->getOperand(3));
  auto IsAsciiLeft = AsciiLeft->getValue() == 1;
  auto IsAsciiRight = AsciiRight->getValue() == 1;
  return IsAsciiLeft && IsAsciiRight;
}

SingleValueInstruction *StringConcatenationOptimizer::optimize() {
  // Bail out if string literals concatenation optimization is
  // not possible.
  if (!extractStringConcatOperands())
    return nullptr;

  // Perform string literal encodings adjustments if needed.
  adjustEncodings();

  // Arguments of the new StringLiteralInst to be created.
  SmallVector<SILValue, 4> Arguments;

  // Encoding to be used for the concatenated string literal.
  auto Encoding = SLILeft->getEncoding();

  // Create a concatenated string literal.
  Builder.setCurrentDebugScope(AI->getDebugScope());
  auto LV = SLILeft->getValue();
  auto RV = SLIRight->getValue();
  auto *NewSLI =
      Builder.createStringLiteral(AI->getLoc(), LV + Twine(RV), Encoding);
  Arguments.push_back(NewSLI);

  // Length of the concatenated literal according to its encoding.
  auto *Len = Builder.createIntegerLiteral(
      AI->getLoc(), AILeft->getOperand(2)->getType(), getConcatenatedLength());
  Arguments.push_back(Len);

  // isAscii flag for UTF8-encoded string literals.
  if (Encoding == StringLiteralInst::Encoding::UTF8) {
    bool IsAscii = isAscii();
    auto ILType = AILeft->getOperand(3)->getType();
    auto *Ascii =
        Builder.createIntegerLiteral(AI->getLoc(), ILType, intmax_t(IsAscii));
    Arguments.push_back(Ascii);
  }

  // Type.
  Arguments.push_back(FuncResultType);

  return Builder.createApply(AI->getLoc(), FRIConvertFromBuiltin,
                             SubstitutionList(), Arguments,
                             false);
}

/// Top level entry point
SingleValueInstruction *
swift::tryToConcatenateStrings(ApplyInst *AI, SILBuilder &B) {
  return StringConcatenationOptimizer(AI, B).optimize();
}

//===----------------------------------------------------------------------===//
//                              Closure Deletion
//===----------------------------------------------------------------------===//

static bool useDoesNotKeepClosureAlive(const SILInstruction *I) {
  switch (I->getKind()) {
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::RetainValueInst:
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::DebugValueInst:
    return true;
  default:
    return false;
  }
}

static bool useHasTransitiveOwnership(const SILInstruction *I) {
  // convert_function is used to add the @noescape attribute. It does not change
  // ownership of the function value.
  return isa<ConvertFunctionInst>(I);
}

static SILValue createLifetimeExtendedAllocStack(
    SILBuilder &Builder, SILLocation Loc, SILValue Arg,
    ArrayRef<SILBasicBlock *> ExitingBlocks, InstModCallbacks Callbacks) {
  AllocStackInst *ASI = nullptr;
  {
    // Save our insert point and create a new alloc_stack in the initial BB and
    // dealloc_stack in all exit blocks.
    auto *OldInsertPt = &*Builder.getInsertionPoint();
    Builder.setInsertionPoint(Builder.getFunction().begin()->begin());
    ASI = Builder.createAllocStack(Loc, Arg->getType());
    Callbacks.CreatedNewInst(ASI);

    for (auto *BB : ExitingBlocks) {
      Builder.setInsertionPoint(BB->getTerminator());
      Callbacks.CreatedNewInst(Builder.createDeallocStack(Loc, ASI));
    }
    Builder.setInsertionPoint(OldInsertPt);
  }
  assert(ASI != nullptr);

  // Then perform a copy_addr [take] [init] right after the partial_apply from
  // the original address argument to the new alloc_stack that we have
  // created.
  Callbacks.CreatedNewInst(
      Builder.createCopyAddr(Loc, Arg, ASI, IsTake, IsInitialization));

  // Return the new alloc_stack inst that has the appropriate live range to
  // destroy said values.
  return ASI;
}

static bool shouldDestroyPartialApplyCapturedArg(SILValue Arg,
                                                 SILParameterInfo PInfo,
                                                 SILModule &M) {
  // If we have a non-trivial type and the argument is passed in @inout, we do
  // not need to destroy it here. This is something that is implicit in the
  // partial_apply design that will be revisited when partial_apply is
  // redesigned.
  if (PInfo.isIndirectMutating())
    return false;

  // If we have a trivial type, we do not need to put in any extra releases.
  if (Arg->getType().isTrivial(M))
    return false;

  // We handle all other cases.
  return true;
}

// *HEY YOU, YES YOU, PLEASE READ*. Even though a textual partial apply is
// printed with the convention of the closed over function upon it, all
// non-inout arguments to a partial_apply are passed at +1. This includes
// arguments that will eventually be passed as guaranteed or in_guaranteed to
// the closed over function. This is because the partial apply is building up a
// boxed aggregate to send off to the closed over function. Of course when you
// call the function, the proper conventions will be used.
void swift::releasePartialApplyCapturedArg(SILBuilder &Builder, SILLocation Loc,
                                           SILValue Arg, SILParameterInfo PInfo,
                                           InstModCallbacks Callbacks) {
  if (!shouldDestroyPartialApplyCapturedArg(Arg, PInfo, Builder.getModule()))
    return;

  // Otherwise, we need to destroy the argument. If we have an address, we
  // insert a destroy_addr and return. Any live range issues must have been
  // dealt with by our caller.
  if (Arg->getType().isAddress()) {
    // Then emit the destroy_addr for this arg
    SILInstruction *NewInst = Builder.emitDestroyAddrAndFold(Loc, Arg);
    Callbacks.CreatedNewInst(NewInst);
    return;
  }

  // Otherwise, we have an object. We emit the most optimized form of release
  // possible for that value.

  // If we have qualified ownership, we should just emit a destroy value.
  if (Arg->getFunction()->hasQualifiedOwnership()) {
    Callbacks.CreatedNewInst(Builder.createDestroyValue(Loc, Arg));
    return;
  }

  if (Arg->getType().hasReferenceSemantics()) {
    auto U = Builder.emitStrongRelease(Loc, Arg);
    if (U.isNull())
      return;

    if (auto *SRI = U.dyn_cast<StrongRetainInst *>()) {
      Callbacks.DeleteInst(SRI);
      return;
    }

    Callbacks.CreatedNewInst(U.get<StrongReleaseInst *>());
    return;
  }

  auto U = Builder.emitReleaseValue(Loc, Arg);
  if (U.isNull())
    return;

  if (auto *RVI = U.dyn_cast<RetainValueInst *>()) {
    Callbacks.DeleteInst(RVI);
    return;
  }

  Callbacks.CreatedNewInst(U.get<ReleaseValueInst *>());
}

/// For each captured argument of PAI, decrement the ref count of the captured
/// argument as appropriate at each of the post dominated release locations
/// found by Tracker.
static bool releaseCapturedArgsOfDeadPartialApply(PartialApplyInst *PAI,
                                                  ReleaseTracker &Tracker,
                                                  InstModCallbacks Callbacks) {
  SILBuilderWithScope Builder(PAI);
  SILLocation Loc = PAI->getLoc();
  CanSILFunctionType PAITy =
      PAI->getCallee()->getType().getAs<SILFunctionType>();

  ArrayRef<SILParameterInfo> Params = PAITy->getParameters();
  llvm::SmallVector<SILValue, 8> Args;
  for (SILValue v : PAI->getArguments()) {
    // If any of our arguments contain open existentials, bail. We do not
    // support this for now so that we can avoid having to re-order stack
    // locations (a larger change).
    if (v->getType().hasOpenedExistential())
      return false;
    Args.emplace_back(v);
  }
  unsigned Delta = Params.size() - Args.size();
  assert(Delta <= Params.size() && "Error, more Args to partial apply than "
                                   "params in its interface.");
  Params = Params.drop_front(Delta);

  llvm::SmallVector<SILBasicBlock *, 2> ExitingBlocks;
  PAI->getFunction()->findExitingBlocks(ExitingBlocks);

  // Go through our argument list and create new alloc_stacks for each
  // non-trivial address value. This ensures that the memory location that we
  // are cleaning up has the same live range as the partial_apply. Otherwise, we
  // may be inserting destroy_addr of alloc_stack that have already been passed
  // to a dealloc_stack.
  for (unsigned i : reversed(indices(Args))) {
    SILValue Arg = Args[i];
    SILParameterInfo PInfo = Params[i];

    // If we are not going to destroy this partial_apply, continue.
    if (!shouldDestroyPartialApplyCapturedArg(Arg, PInfo, Builder.getModule()))
      continue;

    // If we have an object, we will not have live range issues, just continue.
    if (Arg->getType().isObject())
      continue;

    // Now that we know that we have a non-argument address, perform a take-init
    // of Arg into a lifetime extended alloc_stack
    Args[i] = createLifetimeExtendedAllocStack(Builder, Loc, Arg, ExitingBlocks,
                                               Callbacks);
  }

  // Emit a destroy for each captured closure argument at each final release
  // point.
  for (auto *FinalRelease : Tracker.getFinalReleases()) {
    Builder.setInsertionPoint(FinalRelease);
    for (unsigned i : indices(Args)) {
      SILValue Arg = Args[i];
      SILParameterInfo Param = Params[i];

      releasePartialApplyCapturedArg(Builder, Loc, Arg, Param, Callbacks);
    }
  }

  return true;
}

/// TODO: Generalize this to general objects.
bool swift::tryDeleteDeadClosure(SingleValueInstruction *Closure,
                                 InstModCallbacks Callbacks) {
  // We currently only handle locally identified values that do not escape. We
  // also assume that the partial apply does not capture any addresses.
  if (!isa<PartialApplyInst>(Closure) && !isa<ThinToThickFunctionInst>(Closure))
    return false;

  // We only accept a user if it is an ARC object that can be removed if the
  // object is dead. This should be expanded in the future. This also ensures
  // that we are locally identified and non-escaping since we only allow for
  // specific ARC users.
  ReleaseTracker Tracker(useDoesNotKeepClosureAlive, useHasTransitiveOwnership);

  // Find the ARC Users and the final retain, release.
  if (!getFinalReleasesForValue(SILValue(Closure), Tracker))
    return false;

  // If we have a partial_apply, release each captured argument at each one of
  // the final release locations of the partial apply.
  if (auto *PAI = dyn_cast<PartialApplyInst>(Closure)) {
    // If we can not decrement the ref counts of the dead partial apply for any
    // reason, bail.
    if (!releaseCapturedArgsOfDeadPartialApply(PAI, Tracker, Callbacks))
      return false;
  }

  // Then delete all user instructions in reverse so that leaf uses are deleted
  // first.
  for (auto *User : reverse(Tracker.getTrackedUsers())) {
    assert(User->getResults().empty()
           || useHasTransitiveOwnership(User)
                  && "We expect only ARC operations without "
                     "results. This is true b/c of "
                     "isARCOperationRemovableIfObjectIsDead");
    Callbacks.DeleteInst(User);
  }

  // Finally delete the closure.
  Callbacks.DeleteInst(Closure);

  return true;
}

//===----------------------------------------------------------------------===//
//                             Value Lifetime
//===----------------------------------------------------------------------===//

void ValueLifetimeAnalysis::propagateLiveness() {
  assert(LiveBlocks.empty() && "frontier computed twice");

  auto DefBB = DefValue->getParentBlock();
  llvm::SmallVector<SILBasicBlock *, 64> Worklist;
  int NumUsersBeforeDef = 0;

  // Find the initial set of blocks where the value is live, because
  // it is used in those blocks.
  for (SILInstruction *User : UserSet) {
    SILBasicBlock *UserBlock = User->getParent();
    if (LiveBlocks.insert(UserBlock))
      Worklist.push_back(UserBlock);

    // A user in the DefBB could potentially be located before the DefValue.
    if (UserBlock == DefBB)
      NumUsersBeforeDef++;
  }
  // Don't count any users in the DefBB which are actually located _after_
  // the DefValue.
  auto InstIter = DefValue->getIterator();
  while (NumUsersBeforeDef > 0 && ++InstIter != DefBB->end()) {
    if (UserSet.count(&*InstIter))
      NumUsersBeforeDef--;
  }

  // Now propagate liveness backwards until we hit the block that defines the
  // value.
  while (!Worklist.empty()) {
    auto *BB = Worklist.pop_back_val();

    // Don't go beyond the definition.
    if (BB == DefBB && NumUsersBeforeDef == 0)
      continue;

    for (SILBasicBlock *Pred : BB->getPredecessorBlocks()) {
      // If it's already in the set, then we've already queued and/or
      // processed the predecessors.
      if (LiveBlocks.insert(Pred))
        Worklist.push_back(Pred);
    }
  }
}

SILInstruction *ValueLifetimeAnalysis:: findLastUserInBlock(SILBasicBlock *BB) {
  // Walk backwards in BB looking for last use of the value.
  for (auto II = BB->rbegin(); II != BB->rend(); ++II) {
    assert(DefValue != &*II && "Found def before finding use!");

    if (UserSet.count(&*II))
      return &*II;
  }
  llvm_unreachable("Expected to find use of value in block!");
}

bool ValueLifetimeAnalysis::computeFrontier(Frontier &Fr, Mode mode,
                                            DeadEndBlocks *DEBlocks) {
  assert(!isAliveAtBeginOfBlock(DefValue->getFunction()->getEntryBlock()) &&
         "Can't compute frontier for def which does not dominate all uses");

  bool NoCriticalEdges = true;

  // Exit-blocks from the lifetime region. The value is live at the end of
  // a predecessor block but not in the frontier block itself.
  llvm::SmallSetVector<SILBasicBlock *, 16> FrontierBlocks;

  // Blocks where the value is live at the end of the block and which have
  // a frontier block as successor.
  llvm::SmallSetVector<SILBasicBlock *, 16> LiveOutBlocks;

  /// The lifetime ends if we have a live block and a not-live successor.
  for (SILBasicBlock *BB : LiveBlocks) {
    if (DEBlocks && DEBlocks->isDeadEnd(BB))
      continue;

    bool LiveInSucc = false;
    bool DeadInSucc = false;
    for (const SILSuccessor &Succ : BB->getSuccessors()) {
      if (isAliveAtBeginOfBlock(Succ)) {
        LiveInSucc = true;
      } else if (!DEBlocks || !DEBlocks->isDeadEnd(Succ)) {
        DeadInSucc = true;
      }
    }
    if (!LiveInSucc) {
      // The value is not live in any of the successor blocks. This means the
      // block contains a last use of the value. The next instruction after
      // the last use is part of the frontier.
      SILInstruction *LastUser = findLastUserInBlock(BB);
      if (!isa<TermInst>(LastUser)) {
        Fr.push_back(&*std::next(LastUser->getIterator()));
        continue;
      }
      // In case the last user is a TermInst we add all successor blocks to the
      // frontier (see below).
      assert(DeadInSucc && "The final using TermInst must have successors");
    }
    if (DeadInSucc) {
      if (mode == UsersMustPostDomDef)
        return false;

      // The value is not live in some of the successor blocks.
      LiveOutBlocks.insert(BB);
      for (const SILSuccessor &Succ : BB->getSuccessors()) {
        if (!isAliveAtBeginOfBlock(Succ)) {
          // It's an "exit" edge from the lifetime region.
          FrontierBlocks.insert(Succ);
        }
      }
    }
  }
  // Handle "exit" edges from the lifetime region.
  llvm::SmallPtrSet<SILBasicBlock *, 16> UnhandledFrontierBlocks;
  for (SILBasicBlock *FrontierBB: FrontierBlocks) {
    assert(mode != UsersMustPostDomDef);
    bool needSplit = false;
    // If the value is live only in part of the predecessor blocks we have to
    // split those predecessor edges.
    for (SILBasicBlock *Pred : FrontierBB->getPredecessorBlocks()) {
      if (!LiveOutBlocks.count(Pred)) {
        needSplit = true;
        break;
      }
    }
    if (needSplit) {
      if (mode == DontModifyCFG)
        return false;
      // We need to split the critical edge to create a frontier instruction.
      UnhandledFrontierBlocks.insert(FrontierBB);
    } else {
      // The first instruction of the exit-block is part of the frontier.
      Fr.push_back(&*FrontierBB->begin());
    }
  }
  // Split critical edges from the lifetime region to not yet handled frontier
  // blocks.
  for (SILBasicBlock *FrontierPred : LiveOutBlocks) {
    assert(mode != UsersMustPostDomDef);
    auto *T = FrontierPred->getTerminator();
    // Cache the successor blocks because splitting critical edges invalidates
    // the successor list iterator of T.
    llvm::SmallVector<SILBasicBlock *, 4> SuccBlocks;
    for (const SILSuccessor &Succ : T->getSuccessors())
      SuccBlocks.push_back(Succ);

    for (unsigned i = 0, e = SuccBlocks.size(); i != e; ++i) {
      if (UnhandledFrontierBlocks.count(SuccBlocks[i])) {
        assert(mode == AllowToModifyCFG);
        assert(isCriticalEdge(T, i) && "actually not a critical edge?");
        SILBasicBlock *NewBlock = splitEdge(T, i);
        // The single terminator instruction is part of the frontier.
        Fr.push_back(&*NewBlock->begin());
        NoCriticalEdges = false;
      }
    }
  }
  return NoCriticalEdges;
}

bool ValueLifetimeAnalysis::isWithinLifetime(SILInstruction *Inst) {
  SILBasicBlock *BB = Inst->getParent();
  // Check if the value is not live anywhere in Inst's block.
  if (!LiveBlocks.count(BB))
    return false;
  for (const SILSuccessor &Succ : BB->getSuccessors()) {
    // If the value is live at the beginning of any successor block it is also
    // live at the end of BB and therefore Inst is definitely in the lifetime
    // region (Note that we don't check in upward direction against the value's
    // definition).
    if (isAliveAtBeginOfBlock(Succ))
      return true;
  }
  // The value is live in the block but not at the end of the block. Check if
  // Inst is located before (or at) the last use.
  for (auto II = BB->rbegin(); II != BB->rend(); ++II) {
    if (UserSet.count(&*II)) {
      return true;
    }
    if (Inst == &*II)
      return false;
  }
  llvm_unreachable("Expected to find use of value in block!");
}

void ValueLifetimeAnalysis::dump() const {
  llvm::errs() << "lifetime of def: " << *DefValue;
  for (SILInstruction *Use : UserSet) {
    llvm::errs() << "  use: " << *Use;
  }
  llvm::errs() << "  live blocks:";
  for (SILBasicBlock *BB : LiveBlocks) {
    llvm::errs() << ' ' << BB->getDebugID();
  }
  llvm::errs() << '\n';
}

bool swift::simplifyUsers(SingleValueInstruction *I) {
  bool Changed = false;

  for (auto UI = I->use_begin(), UE = I->use_end(); UI != UE; ) {
    SILInstruction *User = UI->getUser();
    ++UI;

    auto SVI = dyn_cast<SingleValueInstruction>(User);
    if (!SVI) continue;

    SILValue S = simplifyInstruction(SVI);
    if (!S)
      continue;

    SVI->replaceAllUsesWith(S);
    SVI->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

/// True if a type can be expanded
/// without a significant increase to code size.
bool swift::shouldExpand(SILModule &Module, SILType Ty) {
  if (EnableExpandAll) {
    return true;
  }
  if (Ty.isAddressOnly(Module)) {
    return false;
  }
  unsigned numFields = Module.Types.countNumberOfFields(Ty);
  if (numFields > 6) {
    return false;
  }
  return true;
}

/// Some support functions for the global-opt and let-properties-opts

/// Check if a given type is a simple type, i.e. a builtin
/// integer or floating point type or a struct/tuple whose members
/// are of simple types.
/// TODO: Cache the "simple" flag for types to avoid repeating checks.
bool swift::isSimpleType(SILType SILTy, SILModule& Module) {
  // Classes can never be initialized statically at compile-time.
  if (SILTy.getClassOrBoundGenericClass()) {
    return false;
  }

  if (!SILTy.isTrivial(Module))
    return false;

  return true;
}

/// Check if the value of V is computed by means of a simple initialization.
/// Store the actual SILValue into Val and the reversed list of instructions
/// initializing it in Insns.
/// The check is performed by recursively walking the computation of the
/// SIL value being analyzed.
/// TODO: Move into utils.
bool
swift::analyzeStaticInitializer(SILValue V,
                                SmallVectorImpl<SILInstruction *> &Insns) {
  // Save every instruction we see.
  // TODO: MultiValueInstruction?
  if (auto I = dyn_cast<SingleValueInstruction>(V))
    Insns.push_back(I);

  if (auto *SI = dyn_cast<StructInst>(V)) {
    // If it is not a struct which is a simple type, bail.
    if (!isSimpleType(SI->getType(), SI->getModule()))
      return false;
    for (auto &Op: SI->getAllOperands()) {
      // If one of the struct instruction operands is not
      // a simple initializer, bail.
      if (!analyzeStaticInitializer(Op.get(), Insns))
        return false;
    }
    return true;
  }

  if (auto *TI = dyn_cast<TupleInst>(V)) {
    // If it is not a tuple which is a simple type, bail.
    if (!isSimpleType(TI->getType(), TI->getModule()))
      return false;
    for (auto &Op: TI->getAllOperands()) {
      // If one of the struct instruction operands is not
      // a simple initializer, bail.
      if (!analyzeStaticInitializer(Op.get(), Insns))
        return false;
    }
    return true;
  }

  if (auto *bi = dyn_cast<BuiltinInst>(V)) {
    switch (bi->getBuiltinInfo().ID) {
    case BuiltinValueKind::FPTrunc:
      if (auto *LI = dyn_cast<LiteralInst>(bi->getArguments()[0])) {
        return analyzeStaticInitializer(LI, Insns);
      }
      return false;
    default:
      return false;
    }
  }

  if (isa<IntegerLiteralInst>(V)
      || isa<FloatLiteralInst>(V)
      || isa<StringLiteralInst>(V)) {
    return true;
  }

  return false;
}

/// Replace load sequence which may contain
/// a chain of struct_element_addr followed by a load.
/// The sequence is traversed inside out, i.e.
/// starting with the innermost struct_element_addr
/// Move into utils.
void swift::replaceLoadSequence(SILInstruction *I,
                                SILValue Value,
                                SILBuilder &B) {
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    LI->replaceAllUsesWith(Value);
    return;
  }

  // It is a series of struct_element_addr followed by load.
  if (auto *SEAI = dyn_cast<StructElementAddrInst>(I)) {
    auto *SEI = B.createStructExtract(SEAI->getLoc(), Value, SEAI->getField());
    for (auto SEAIUse : SEAI->getUses()) {
      replaceLoadSequence(SEAIUse->getUser(), SEI, B);
    }
    return;
  }

  if (auto *TEAI = dyn_cast<TupleElementAddrInst>(I)) {
    auto *TEI = B.createTupleExtract(TEAI->getLoc(), Value, TEAI->getFieldNo());
    for (auto TEAIUse : TEAI->getUses()) {
      replaceLoadSequence(TEAIUse->getUser(), TEI, B);
    }
    return;
  }

  llvm_unreachable("Unknown instruction sequence for reading from a global");
}

/// Are the callees that could be called through Decl statically
/// knowable based on the Decl and the compilation mode?
bool swift::calleesAreStaticallyKnowable(SILModule &M, SILDeclRef Decl) {
  if (Decl.isForeign)
    return false;

  const DeclContext *AssocDC = M.getAssociatedContext();
  if (!AssocDC)
    return false;

  auto *AFD = Decl.getAbstractFunctionDecl();
  assert(AFD && "Expected abstract function decl!");

  // Only handle members defined within the SILModule's associated context.
  if (!AFD->isChildContextOf(AssocDC))
    return false;

  if (AFD->isDynamic())
    return false;

  if (!AFD->hasAccess())
    return false;

  // Only consider 'private' members, unless we are in whole-module compilation.
  switch (AFD->getEffectiveAccess()) {
  case AccessLevel::Open:
    return false;
  case AccessLevel::Public:
    if (isa<ConstructorDecl>(AFD)) {
      // Constructors are special: a derived class in another module can
      // "override" a constructor if its class is "open", although the
      // constructor itself is not open.
      auto *ND = AFD->getDeclContext()
          ->getAsNominalTypeOrNominalTypeExtensionContext();
      if (ND->getEffectiveAccess() == AccessLevel::Open)
        return false;
    }
    LLVM_FALLTHROUGH;
  case AccessLevel::Internal:
    return M.isWholeModule();
  case AccessLevel::FilePrivate:
  case AccessLevel::Private:
    return true;
  }

  llvm_unreachable("Unhandled access level in switch.");
}

void swift::hoistAddressProjections(Operand &Op, SILInstruction *InsertBefore,
                                    DominanceInfo *DomTree) {
  SILValue V = Op.get();
  SILInstruction *Prev = nullptr;
  auto *InsertPt = InsertBefore;
  while (true) {
    SILValue Incoming = stripSinglePredecessorArgs(V);
    
    // Forward the incoming arg from a single predecessor.
    if (V != Incoming) {
      if (V == Op.get()) {
        // If we are the operand itself set the operand to the incoming
        // argument.
        Op.set(Incoming);
        V = Incoming;
      } else {
        // Otherwise, set the previous projections operand to the incoming
        // argument.
        assert(Prev && "Must have seen a projection");
        Prev->setOperand(0, Incoming);
        V = Incoming;
      }
    }
    
    switch (V->getKind()) {
      case ValueKind::StructElementAddrInst:
      case ValueKind::TupleElementAddrInst:
      case ValueKind::RefElementAddrInst:
      case ValueKind::RefTailAddrInst:
      case ValueKind::UncheckedTakeEnumDataAddrInst: {
        auto *Inst = cast<SingleValueInstruction>(V);
        // We are done once the current projection dominates the insert point.
        if (DomTree->dominates(Inst->getParent(), InsertBefore->getParent()))
          return;
        
        // Move the current projection and memorize it for the next iteration.
        Prev = Inst;
        Inst->moveBefore(InsertPt);
        InsertPt = Inst;
        V = Inst->getOperand(0);
        continue;
      }
      default:
        assert(DomTree->dominates(V->getParentBlock(), InsertBefore->getParent()) &&
               "The projected value must dominate the insertion point");
        return;
    }
  }
}

