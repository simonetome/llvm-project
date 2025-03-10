//===- AMDGPUAttributor.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file This pass uses Attributor framework to deduce AMDGPU attributes.
//
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "GCNSubtarget.h"
#include "Utils/AMDGPUBaseInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/IntrinsicsR600.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO/Attributor.h"

#define DEBUG_TYPE "amdgpu-attributor"

using namespace llvm;

#define AMDGPU_ATTRIBUTE(Name, Str) Name##_POS,

enum ImplicitArgumentPositions {
  #include "AMDGPUAttributes.def"
  LAST_ARG_POS
};

#define AMDGPU_ATTRIBUTE(Name, Str) Name = 1 << Name##_POS,

enum ImplicitArgumentMask {
  NOT_IMPLICIT_INPUT = 0,
  #include "AMDGPUAttributes.def"
  ALL_ARGUMENT_MASK = (1 << LAST_ARG_POS) - 1
};

#define AMDGPU_ATTRIBUTE(Name, Str) {Name, Str},
static constexpr std::pair<ImplicitArgumentMask,
                           StringLiteral> ImplicitAttrs[] = {
 #include "AMDGPUAttributes.def"
};

// We do not need to note the x workitem or workgroup id because they are always
// initialized.
//
// TODO: We should not add the attributes if the known compile time workgroup
// size is 1 for y/z.
static ImplicitArgumentMask
intrinsicToAttrMask(Intrinsic::ID ID, bool &NonKernelOnly, bool &NeedsImplicit,
                    bool HasApertureRegs, bool SupportsGetDoorBellID) {
  unsigned CodeObjectVersion = AMDGPU::getAmdhsaCodeObjectVersion();
  switch (ID) {
  case Intrinsic::amdgcn_workitem_id_x:
    NonKernelOnly = true;
    return WORKITEM_ID_X;
  case Intrinsic::amdgcn_workgroup_id_x:
    NonKernelOnly = true;
    return WORKGROUP_ID_X;
  case Intrinsic::amdgcn_workitem_id_y:
  case Intrinsic::r600_read_tidig_y:
    return WORKITEM_ID_Y;
  case Intrinsic::amdgcn_workitem_id_z:
  case Intrinsic::r600_read_tidig_z:
    return WORKITEM_ID_Z;
  case Intrinsic::amdgcn_workgroup_id_y:
  case Intrinsic::r600_read_tgid_y:
    return WORKGROUP_ID_Y;
  case Intrinsic::amdgcn_workgroup_id_z:
  case Intrinsic::r600_read_tgid_z:
    return WORKGROUP_ID_Z;
  case Intrinsic::amdgcn_lds_kernel_id:
    return LDS_KERNEL_ID;
  case Intrinsic::amdgcn_dispatch_ptr:
    return DISPATCH_PTR;
  case Intrinsic::amdgcn_dispatch_id:
    return DISPATCH_ID;
  case Intrinsic::amdgcn_implicitarg_ptr:
    return IMPLICIT_ARG_PTR;
  // Need queue_ptr anyway. But under V5, we also need implicitarg_ptr to access
  // queue_ptr.
  case Intrinsic::amdgcn_queue_ptr:
    NeedsImplicit = (CodeObjectVersion == 5);
    return QUEUE_PTR;
  case Intrinsic::amdgcn_is_shared:
  case Intrinsic::amdgcn_is_private:
    if (HasApertureRegs)
      return NOT_IMPLICIT_INPUT;
    // Under V5, we need implicitarg_ptr + offsets to access private_base or
    // shared_base. For pre-V5, however, need to access them through queue_ptr +
    // offsets.
    return CodeObjectVersion == 5 ? IMPLICIT_ARG_PTR : QUEUE_PTR;
  case Intrinsic::trap:
    if (SupportsGetDoorBellID) // GetDoorbellID support implemented since V4.
      return CodeObjectVersion >= 4 ? NOT_IMPLICIT_INPUT : QUEUE_PTR;
    NeedsImplicit = (CodeObjectVersion == 5); // Need impicitarg_ptr under V5.
    return QUEUE_PTR;
  default:
    return NOT_IMPLICIT_INPUT;
  }
}

static bool castRequiresQueuePtr(unsigned SrcAS) {
  return SrcAS == AMDGPUAS::LOCAL_ADDRESS || SrcAS == AMDGPUAS::PRIVATE_ADDRESS;
}

static bool isDSAddress(const Constant *C) {
  const GlobalValue *GV = dyn_cast<GlobalValue>(C);
  if (!GV)
    return false;
  unsigned AS = GV->getAddressSpace();
  return AS == AMDGPUAS::LOCAL_ADDRESS || AS == AMDGPUAS::REGION_ADDRESS;
}

/// Returns true if the function requires the implicit argument be passed
/// regardless of the function contents.
static bool funcRequiresHostcallPtr(const Function &F) {
  // Sanitizers require the hostcall buffer passed in the implicit arguments.
  return F.hasFnAttribute(Attribute::SanitizeAddress) ||
         F.hasFnAttribute(Attribute::SanitizeThread) ||
         F.hasFnAttribute(Attribute::SanitizeMemory) ||
         F.hasFnAttribute(Attribute::SanitizeHWAddress) ||
         F.hasFnAttribute(Attribute::SanitizeMemTag);
}

namespace {
class AMDGPUInformationCache : public InformationCache {
public:
  AMDGPUInformationCache(const Module &M, AnalysisGetter &AG,
                         BumpPtrAllocator &Allocator,
                         SetVector<Function *> *CGSCC, TargetMachine &TM)
      : InformationCache(M, AG, Allocator, CGSCC), TM(TM) {}
  TargetMachine &TM;

  enum ConstantStatus { DS_GLOBAL = 1 << 0, ADDR_SPACE_CAST = 1 << 1 };

  /// Check if the subtarget has aperture regs.
  bool hasApertureRegs(Function &F) {
    const GCNSubtarget &ST = TM.getSubtarget<GCNSubtarget>(F);
    return ST.hasApertureRegs();
  }

  /// Check if the subtarget supports GetDoorbellID.
  bool supportsGetDoorbellID(Function &F) {
    const GCNSubtarget &ST = TM.getSubtarget<GCNSubtarget>(F);
    return ST.supportsGetDoorbellID();
  }

  std::pair<unsigned, unsigned> getFlatWorkGroupSizes(const Function &F) {
    const GCNSubtarget &ST = TM.getSubtarget<GCNSubtarget>(F);
    return ST.getFlatWorkGroupSizes(F);
  }

  std::pair<unsigned, unsigned>
  getMaximumFlatWorkGroupRange(const Function &F) {
    const GCNSubtarget &ST = TM.getSubtarget<GCNSubtarget>(F);
    return {ST.getMinFlatWorkGroupSize(), ST.getMaxFlatWorkGroupSize()};
  }

private:
  /// Check if the ConstantExpr \p CE requires the queue pointer.
  static bool visitConstExpr(const ConstantExpr *CE) {
    if (CE->getOpcode() == Instruction::AddrSpaceCast) {
      unsigned SrcAS = CE->getOperand(0)->getType()->getPointerAddressSpace();
      return castRequiresQueuePtr(SrcAS);
    }
    return false;
  }

  /// Get the constant access bitmap for \p C.
  uint8_t getConstantAccess(const Constant *C) {
    auto It = ConstantStatus.find(C);
    if (It != ConstantStatus.end())
      return It->second;

    uint8_t Result = 0;
    if (isDSAddress(C))
      Result = DS_GLOBAL;

    if (const auto *CE = dyn_cast<ConstantExpr>(C))
      if (visitConstExpr(CE))
        Result |= ADDR_SPACE_CAST;

    for (const Use &U : C->operands()) {
      const auto *OpC = dyn_cast<Constant>(U);
      if (!OpC)
        continue;

      Result |= getConstantAccess(OpC);
    }
    return Result;
  }

public:
  /// Returns true if \p Fn needs the queue pointer because of \p C.
  bool needsQueuePtr(const Constant *C, Function &Fn) {
    bool IsNonEntryFunc = !AMDGPU::isEntryFunctionCC(Fn.getCallingConv());
    bool HasAperture = hasApertureRegs(Fn);

    // No need to explore the constants.
    if (!IsNonEntryFunc && HasAperture)
      return false;

    uint8_t Access = getConstantAccess(C);

    // We need to trap on DS globals in non-entry functions.
    if (IsNonEntryFunc && (Access & DS_GLOBAL))
      return true;

    return !HasAperture && (Access & ADDR_SPACE_CAST);
  }

private:
  /// Used to determine if the Constant needs the queue pointer.
  DenseMap<const Constant *, uint8_t> ConstantStatus;
};

struct AAAMDAttributes : public StateWrapper<
  BitIntegerState<uint16_t, ALL_ARGUMENT_MASK, 0>, AbstractAttribute> {
  using Base = StateWrapper<BitIntegerState<uint16_t, ALL_ARGUMENT_MASK, 0>,
                            AbstractAttribute>;

  AAAMDAttributes(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Create an abstract attribute view for the position \p IRP.
  static AAAMDAttributes &createForPosition(const IRPosition &IRP,
                                            Attributor &A);

  /// See AbstractAttribute::getName().
  const std::string getName() const override { return "AAAMDAttributes"; }

  /// See AbstractAttribute::getIdAddr().
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAAMDAttributes.
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};
const char AAAMDAttributes::ID = 0;

struct AAUniformWorkGroupSize
    : public StateWrapper<BooleanState, AbstractAttribute> {
  using Base = StateWrapper<BooleanState, AbstractAttribute>;
  AAUniformWorkGroupSize(const IRPosition &IRP, Attributor &A) : Base(IRP) {}

  /// Create an abstract attribute view for the position \p IRP.
  static AAUniformWorkGroupSize &createForPosition(const IRPosition &IRP,
                                                   Attributor &A);

  /// See AbstractAttribute::getName().
  const std::string getName() const override {
    return "AAUniformWorkGroupSize";
  }

  /// See AbstractAttribute::getIdAddr().
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAAMDAttributes.
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};
const char AAUniformWorkGroupSize::ID = 0;

struct AAUniformWorkGroupSizeFunction : public AAUniformWorkGroupSize {
  AAUniformWorkGroupSizeFunction(const IRPosition &IRP, Attributor &A)
      : AAUniformWorkGroupSize(IRP, A) {}

  void initialize(Attributor &A) override {
    Function *F = getAssociatedFunction();
    CallingConv::ID CC = F->getCallingConv();

    if (CC != CallingConv::AMDGPU_KERNEL)
      return;

    bool InitialValue = false;
    if (F->hasFnAttribute("uniform-work-group-size"))
      InitialValue = F->getFnAttribute("uniform-work-group-size")
                         .getValueAsString()
                         .equals("true");

    if (InitialValue)
      indicateOptimisticFixpoint();
    else
      indicatePessimisticFixpoint();
  }

  ChangeStatus updateImpl(Attributor &A) override {
    ChangeStatus Change = ChangeStatus::UNCHANGED;

    auto CheckCallSite = [&](AbstractCallSite CS) {
      Function *Caller = CS.getInstruction()->getFunction();
      LLVM_DEBUG(dbgs() << "[AAUniformWorkGroupSize] Call " << Caller->getName()
                        << "->" << getAssociatedFunction()->getName() << "\n");

      const auto &CallerInfo = A.getAAFor<AAUniformWorkGroupSize>(
          *this, IRPosition::function(*Caller), DepClassTy::REQUIRED);

      Change = Change | clampStateAndIndicateChange(this->getState(),
                                                    CallerInfo.getState());

      return true;
    };

    bool AllCallSitesKnown = true;
    if (!A.checkForAllCallSites(CheckCallSite, *this, true, AllCallSitesKnown))
      return indicatePessimisticFixpoint();

    return Change;
  }

  ChangeStatus manifest(Attributor &A) override {
    SmallVector<Attribute, 8> AttrList;
    LLVMContext &Ctx = getAssociatedFunction()->getContext();

    AttrList.push_back(Attribute::get(Ctx, "uniform-work-group-size",
                                      getAssumed() ? "true" : "false"));
    return IRAttributeManifest::manifestAttrs(A, getIRPosition(), AttrList,
                                              /* ForceReplace */ true);
  }

  bool isValidState() const override {
    // This state is always valid, even when the state is false.
    return true;
  }

  const std::string getAsStr() const override {
    return "AMDWorkGroupSize[" + std::to_string(getAssumed()) + "]";
  }

  /// See AbstractAttribute::trackStatistics()
  void trackStatistics() const override {}
};

AAUniformWorkGroupSize &
AAUniformWorkGroupSize::createForPosition(const IRPosition &IRP,
                                          Attributor &A) {
  if (IRP.getPositionKind() == IRPosition::IRP_FUNCTION)
    return *new (A.Allocator) AAUniformWorkGroupSizeFunction(IRP, A);
  llvm_unreachable(
      "AAUniformWorkGroupSize is only valid for function position");
}

struct AAAMDAttributesFunction : public AAAMDAttributes {
  AAAMDAttributesFunction(const IRPosition &IRP, Attributor &A)
      : AAAMDAttributes(IRP, A) {}

  void initialize(Attributor &A) override {
    Function *F = getAssociatedFunction();

    // If the function requires the implicit arg pointer due to sanitizers,
    // assume it's needed even if explicitly marked as not requiring it.
    const bool NeedsHostcall = funcRequiresHostcallPtr(*F);
    if (NeedsHostcall) {
      removeAssumedBits(IMPLICIT_ARG_PTR);
      removeAssumedBits(HOSTCALL_PTR);
    }

    for (auto Attr : ImplicitAttrs) {
      if (NeedsHostcall &&
          (Attr.first == IMPLICIT_ARG_PTR || Attr.first == HOSTCALL_PTR))
        continue;

      if (F->hasFnAttribute(Attr.second))
        addKnownBits(Attr.first);
    }

    if (F->isDeclaration())
      return;

    // Ignore functions with graphics calling conventions, these are currently
    // not allowed to have kernel arguments.
    if (AMDGPU::isGraphics(F->getCallingConv())) {
      indicatePessimisticFixpoint();
      return;
    }
  }

  ChangeStatus updateImpl(Attributor &A) override {
    Function *F = getAssociatedFunction();
    // The current assumed state used to determine a change.
    auto OrigAssumed = getAssumed();

    // Check for Intrinsics and propagate attributes.
    const AACallEdges &AAEdges = A.getAAFor<AACallEdges>(
        *this, this->getIRPosition(), DepClassTy::REQUIRED);
    if (AAEdges.hasNonAsmUnknownCallee())
      return indicatePessimisticFixpoint();

    bool IsNonEntryFunc = !AMDGPU::isEntryFunctionCC(F->getCallingConv());

    bool NeedsImplicit = false;
    auto &InfoCache = static_cast<AMDGPUInformationCache &>(A.getInfoCache());
    bool HasApertureRegs = InfoCache.hasApertureRegs(*F);
    bool SupportsGetDoorbellID = InfoCache.supportsGetDoorbellID(*F);

    for (Function *Callee : AAEdges.getOptimisticEdges()) {
      Intrinsic::ID IID = Callee->getIntrinsicID();
      if (IID == Intrinsic::not_intrinsic) {
        const AAAMDAttributes &AAAMD = A.getAAFor<AAAMDAttributes>(
          *this, IRPosition::function(*Callee), DepClassTy::REQUIRED);
        *this &= AAAMD;
        continue;
      }

      bool NonKernelOnly = false;
      ImplicitArgumentMask AttrMask =
          intrinsicToAttrMask(IID, NonKernelOnly, NeedsImplicit,
                              HasApertureRegs, SupportsGetDoorbellID);
      if (AttrMask != NOT_IMPLICIT_INPUT) {
        if ((IsNonEntryFunc || !NonKernelOnly))
          removeAssumedBits(AttrMask);
      }
    }

    // Need implicitarg_ptr to acess queue_ptr, private_base, and shared_base.
    if (NeedsImplicit)
      removeAssumedBits(IMPLICIT_ARG_PTR);

    if (isAssumed(QUEUE_PTR) && checkForQueuePtr(A)) {
      // Under V5, we need implicitarg_ptr + offsets to access private_base or
      // shared_base. We do not actually need queue_ptr.
      if (AMDGPU::getAmdhsaCodeObjectVersion() == 5)
        removeAssumedBits(IMPLICIT_ARG_PTR);
      else
        removeAssumedBits(QUEUE_PTR);
    }

    if (funcRetrievesMultigridSyncArg(A)) {
      assert(!isAssumed(IMPLICIT_ARG_PTR) &&
             "multigrid_sync_arg needs implicitarg_ptr");
      removeAssumedBits(MULTIGRID_SYNC_ARG);
    }

    if (funcRetrievesHostcallPtr(A)) {
      assert(!isAssumed(IMPLICIT_ARG_PTR) && "hostcall needs implicitarg_ptr");
      removeAssumedBits(HOSTCALL_PTR);
    }

    if (funcRetrievesHeapPtr(A)) {
      assert(!isAssumed(IMPLICIT_ARG_PTR) && "heap_ptr needs implicitarg_ptr");
      removeAssumedBits(HEAP_PTR);
    }

    if (isAssumed(QUEUE_PTR) && funcRetrievesQueuePtr(A)) {
      assert(!isAssumed(IMPLICIT_ARG_PTR) && "queue_ptr needs implicitarg_ptr");
      removeAssumedBits(QUEUE_PTR);
    }

    if (isAssumed(LDS_KERNEL_ID) && funcRetrievesLDSKernelId(A)) {
      removeAssumedBits(LDS_KERNEL_ID);
    }

    return getAssumed() != OrigAssumed ? ChangeStatus::CHANGED
                                       : ChangeStatus::UNCHANGED;
  }

  ChangeStatus manifest(Attributor &A) override {
    SmallVector<Attribute, 8> AttrList;
    LLVMContext &Ctx = getAssociatedFunction()->getContext();

    for (auto Attr : ImplicitAttrs) {
      if (isKnown(Attr.first))
        AttrList.push_back(Attribute::get(Ctx, Attr.second));
    }

    return IRAttributeManifest::manifestAttrs(A, getIRPosition(), AttrList,
                                              /* ForceReplace */ true);
  }

  const std::string getAsStr() const override {
    std::string Str;
    raw_string_ostream OS(Str);
    OS << "AMDInfo[";
    for (auto Attr : ImplicitAttrs)
      OS << ' ' << Attr.second;
    OS << " ]";
    return OS.str();
  }

  /// See AbstractAttribute::trackStatistics()
  void trackStatistics() const override {}

private:
  bool checkForQueuePtr(Attributor &A) {
    Function *F = getAssociatedFunction();
    bool IsNonEntryFunc = !AMDGPU::isEntryFunctionCC(F->getCallingConv());

    auto &InfoCache = static_cast<AMDGPUInformationCache &>(A.getInfoCache());

    bool NeedsQueuePtr = false;

    auto CheckAddrSpaceCasts = [&](Instruction &I) {
      unsigned SrcAS = static_cast<AddrSpaceCastInst &>(I).getSrcAddressSpace();
      if (castRequiresQueuePtr(SrcAS)) {
        NeedsQueuePtr = true;
        return false;
      }
      return true;
    };

    bool HasApertureRegs = InfoCache.hasApertureRegs(*F);

    // `checkForAllInstructions` is much more cheaper than going through all
    // instructions, try it first.

    // The queue pointer is not needed if aperture regs is present.
    if (!HasApertureRegs) {
      bool UsedAssumedInformation = false;
      A.checkForAllInstructions(CheckAddrSpaceCasts, *this,
                                {Instruction::AddrSpaceCast},
                                UsedAssumedInformation);
    }

    // If we found  that we need the queue pointer, nothing else to do.
    if (NeedsQueuePtr)
      return true;

    if (!IsNonEntryFunc && HasApertureRegs)
      return false;

    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        for (const Use &U : I.operands()) {
          if (const auto *C = dyn_cast<Constant>(U)) {
            if (InfoCache.needsQueuePtr(C, *F))
              return true;
          }
        }
      }
    }

    return false;
  }

  bool funcRetrievesMultigridSyncArg(Attributor &A) {
    auto Pos = llvm::AMDGPU::getMultigridSyncArgImplicitArgPosition();
    AA::RangeTy Range(Pos, 8);
    return funcRetrievesImplicitKernelArg(A, Range);
  }

  bool funcRetrievesHostcallPtr(Attributor &A) {
    auto Pos = llvm::AMDGPU::getHostcallImplicitArgPosition();
    AA::RangeTy Range(Pos, 8);
    return funcRetrievesImplicitKernelArg(A, Range);
  }

  bool funcRetrievesHeapPtr(Attributor &A) {
    if (AMDGPU::getAmdhsaCodeObjectVersion() != 5)
      return false;
    AA::RangeTy Range(AMDGPU::ImplicitArg::HEAP_PTR_OFFSET, 8);
    return funcRetrievesImplicitKernelArg(A, Range);
  }

  bool funcRetrievesQueuePtr(Attributor &A) {
    if (AMDGPU::getAmdhsaCodeObjectVersion() != 5)
      return false;
    AA::RangeTy Range(AMDGPU::ImplicitArg::QUEUE_PTR_OFFSET, 8);
    return funcRetrievesImplicitKernelArg(A, Range);
  }

  bool funcRetrievesImplicitKernelArg(Attributor &A, AA::RangeTy Range) {
    // Check if this is a call to the implicitarg_ptr builtin and it
    // is used to retrieve the hostcall pointer. The implicit arg for
    // hostcall is not used only if every use of the implicitarg_ptr
    // is a load that clearly does not retrieve any byte of the
    // hostcall pointer. We check this by tracing all the uses of the
    // initial call to the implicitarg_ptr intrinsic.
    auto DoesNotLeadToKernelArgLoc = [&](Instruction &I) {
      auto &Call = cast<CallBase>(I);
      if (Call.getIntrinsicID() != Intrinsic::amdgcn_implicitarg_ptr)
        return true;

      const auto &PointerInfoAA = A.getAAFor<AAPointerInfo>(
          *this, IRPosition::callsite_returned(Call), DepClassTy::REQUIRED);

      return PointerInfoAA.forallInterferingAccesses(
          Range, [](const AAPointerInfo::Access &Acc, bool IsExact) {
            return Acc.getRemoteInst()->isDroppable();
          });
    };

    bool UsedAssumedInformation = false;
    return !A.checkForAllCallLikeInstructions(DoesNotLeadToKernelArgLoc, *this,
                                              UsedAssumedInformation);
  }

  bool funcRetrievesLDSKernelId(Attributor &A) {
    auto DoesNotRetrieve = [&](Instruction &I) {
      auto &Call = cast<CallBase>(I);
      return Call.getIntrinsicID() != Intrinsic::amdgcn_lds_kernel_id;
    };
    bool UsedAssumedInformation = false;
    return !A.checkForAllCallLikeInstructions(DoesNotRetrieve, *this,
                                              UsedAssumedInformation);
  }
};

AAAMDAttributes &AAAMDAttributes::createForPosition(const IRPosition &IRP,
                                                    Attributor &A) {
  if (IRP.getPositionKind() == IRPosition::IRP_FUNCTION)
    return *new (A.Allocator) AAAMDAttributesFunction(IRP, A);
  llvm_unreachable("AAAMDAttributes is only valid for function position");
}

/// Propagate amdgpu-flat-work-group-size attribute.
struct AAAMDFlatWorkGroupSize
    : public StateWrapper<IntegerRangeState, AbstractAttribute, uint32_t> {
  using Base = StateWrapper<IntegerRangeState, AbstractAttribute, uint32_t>;
  AAAMDFlatWorkGroupSize(const IRPosition &IRP, Attributor &A)
      : Base(IRP, 32) {}

  /// See AbstractAttribute::getState(...).
  IntegerRangeState &getState() override { return *this; }
  const IntegerRangeState &getState() const override { return *this; }

  void initialize(Attributor &A) override {
    Function *F = getAssociatedFunction();
    auto &InfoCache = static_cast<AMDGPUInformationCache &>(A.getInfoCache());
    unsigned MinGroupSize, MaxGroupSize;
    std::tie(MinGroupSize, MaxGroupSize) = InfoCache.getFlatWorkGroupSizes(*F);
    intersectKnown(
        ConstantRange(APInt(32, MinGroupSize), APInt(32, MaxGroupSize + 1)));

    if (AMDGPU::isEntryFunctionCC(F->getCallingConv()))
      indicatePessimisticFixpoint();
  }

  ChangeStatus updateImpl(Attributor &A) override {
    ChangeStatus Change = ChangeStatus::UNCHANGED;

    auto CheckCallSite = [&](AbstractCallSite CS) {
      Function *Caller = CS.getInstruction()->getFunction();
      LLVM_DEBUG(dbgs() << "[AAAMDFlatWorkGroupSize] Call " << Caller->getName()
                        << "->" << getAssociatedFunction()->getName() << '\n');

      const auto &CallerInfo = A.getAAFor<AAAMDFlatWorkGroupSize>(
          *this, IRPosition::function(*Caller), DepClassTy::REQUIRED);

      Change |=
          clampStateAndIndicateChange(this->getState(), CallerInfo.getState());

      return true;
    };

    bool AllCallSitesKnown = true;
    if (!A.checkForAllCallSites(CheckCallSite, *this, true, AllCallSitesKnown))
      return indicatePessimisticFixpoint();

    return Change;
  }

  ChangeStatus manifest(Attributor &A) override {
    SmallVector<Attribute, 8> AttrList;
    Function *F = getAssociatedFunction();
    LLVMContext &Ctx = F->getContext();

    auto &InfoCache = static_cast<AMDGPUInformationCache &>(A.getInfoCache());
    unsigned Min, Max;
    std::tie(Min, Max) = InfoCache.getMaximumFlatWorkGroupRange(*F);

    // Don't add the attribute if it's the implied default.
    if (getAssumed().getLower() == Min && getAssumed().getUpper() - 1 == Max)
      return ChangeStatus::UNCHANGED;

    SmallString<10> Buffer;
    raw_svector_ostream OS(Buffer);
    OS << getAssumed().getLower() << ',' << getAssumed().getUpper() - 1;

    AttrList.push_back(
        Attribute::get(Ctx, "amdgpu-flat-work-group-size", OS.str()));
    return IRAttributeManifest::manifestAttrs(A, getIRPosition(), AttrList,
                                              /* ForceReplace */ true);
  }

  const std::string getAsStr() const override {
    std::string Str;
    raw_string_ostream OS(Str);
    OS << "AMDFlatWorkGroupSize[";
    OS << getAssumed().getLower() << ',' << getAssumed().getUpper() - 1;
    OS << ']';
    return OS.str();
  }

  /// See AbstractAttribute::trackStatistics()
  void trackStatistics() const override {}

  /// Create an abstract attribute view for the position \p IRP.
  static AAAMDFlatWorkGroupSize &createForPosition(const IRPosition &IRP,
                                                   Attributor &A);

  /// See AbstractAttribute::getName()
  const std::string getName() const override {
    return "AAAMDFlatWorkGroupSize";
  }

  /// See AbstractAttribute::getIdAddr()
  const char *getIdAddr() const override { return &ID; }

  /// This function should return true if the type of the \p AA is
  /// AAAMDFlatWorkGroupSize
  static bool classof(const AbstractAttribute *AA) {
    return (AA->getIdAddr() == &ID);
  }

  /// Unique ID (due to the unique address)
  static const char ID;
};

const char AAAMDFlatWorkGroupSize::ID = 0;

AAAMDFlatWorkGroupSize &
AAAMDFlatWorkGroupSize::createForPosition(const IRPosition &IRP,
                                          Attributor &A) {
  if (IRP.getPositionKind() == IRPosition::IRP_FUNCTION)
    return *new (A.Allocator) AAAMDFlatWorkGroupSize(IRP, A);
  llvm_unreachable(
      "AAAMDFlatWorkGroupSize is only valid for function position");
}

class AMDGPUAttributor : public ModulePass {
public:
  AMDGPUAttributor() : ModulePass(ID) {}

  /// doInitialization - Virtual method overridden by subclasses to do
  /// any necessary initialization before any pass is run.
  bool doInitialization(Module &) override {
    auto *TPC = getAnalysisIfAvailable<TargetPassConfig>();
    if (!TPC)
      report_fatal_error("TargetMachine is required");

    TM = &TPC->getTM<TargetMachine>();
    return false;
  }

  bool runOnModule(Module &M) override {
    SetVector<Function *> Functions;
    AnalysisGetter AG;
    for (Function &F : M) {
      if (!F.isIntrinsic())
        Functions.insert(&F);
    }

    CallGraphUpdater CGUpdater;
    BumpPtrAllocator Allocator;
    AMDGPUInformationCache InfoCache(M, AG, Allocator, nullptr, *TM);
    DenseSet<const char *> Allowed(
        {&AAAMDAttributes::ID, &AAUniformWorkGroupSize::ID,
         &AAPotentialValues::ID, &AAAMDFlatWorkGroupSize::ID, &AACallEdges::ID,
         &AAPointerInfo::ID, &AAPotentialConstantValues::ID});

    AttributorConfig AC(CGUpdater);
    AC.Allowed = &Allowed;
    AC.IsModulePass = true;
    AC.DefaultInitializeLiveInternals = false;

    Attributor A(Functions, InfoCache, AC);

    for (Function &F : M) {
      if (!F.isIntrinsic()) {
        A.getOrCreateAAFor<AAAMDAttributes>(IRPosition::function(F));
        A.getOrCreateAAFor<AAUniformWorkGroupSize>(IRPosition::function(F));
        if (!AMDGPU::isEntryFunctionCC(F.getCallingConv())) {
          A.getOrCreateAAFor<AAAMDFlatWorkGroupSize>(IRPosition::function(F));
        }
      }
    }

    ChangeStatus Change = A.run();
    return Change == ChangeStatus::CHANGED;
  }

  StringRef getPassName() const override { return "AMDGPU Attributor"; }
  TargetMachine *TM;
  static char ID;
};
} // namespace

char AMDGPUAttributor::ID = 0;

Pass *llvm::createAMDGPUAttributorPass() { return new AMDGPUAttributor(); }
INITIALIZE_PASS(AMDGPUAttributor, DEBUG_TYPE, "AMDGPU Attributor", false, false)
