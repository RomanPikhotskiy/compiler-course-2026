#include "X86.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include <algorithm>

using namespace llvm;

namespace {

constexpr unsigned kMaxInlineInstrCount = 15;
constexpr unsigned kMaxSelfInlineDepth = 3;
constexpr unsigned kMaxCallChain = 64;

struct InlineBody {
  MachineFunction *MF = nullptr;
  SmallVector<MachineInstr *, 16> Instructions;
};

const Function *extractDirectCallee(const MachineInstr &MI) {
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isGlobal())
      continue;
    return dyn_cast<Function>(MO.getGlobal());
  }
  return nullptr;
}

unsigned countBodyInstructions(const MachineFunction &MF) {
  if (MF.empty())
    return 0;

  unsigned Count = 0;
  const MachineBasicBlock &Entry = MF.front();
  for (const MachineInstr &MI : Entry) {
    if (MI.isDebugInstr() || MI.isCFIInstruction())
      continue;
    if (MI.isTerminator())
      continue;
    ++Count;
  }
  return Count;
}

bool canInlineCallee(const MachineFunction &MF) {
  if (MF.empty())
    return false;
  if (std::next(MF.begin()) != MF.end())
    return false;

  const MachineFrameInfo &Frame = MF.getFrameInfo();
  if (Frame.getNumObjects() || Frame.hasVarSizedObjects() ||
      Frame.getStackSize())
    return false;

  const MachineBasicBlock &Entry = MF.front();
  if (Entry.empty() || Entry.isEHPad() || !Entry.back().isReturn() ||
      Entry.back().isCall())
    return false;

  for (const MachineInstr &MI : Entry) {
    if (MI.isDebugInstr() || MI.isCFIInstruction())
      continue;
    if (MI.isTerminator() && &MI != &Entry.back())
      return false;
    if (MI.getFlag(MachineInstr::FrameSetup) ||
        MI.getFlag(MachineInstr::FrameDestroy))
      return false;
    for (const MachineOperand &MO : MI.operands()) {
      // These operands refer to data owned by the callee, not by the caller.
      if (MO.isFI() || MO.isCPI() || MO.isJTI() || MO.isMBB() ||
          MO.isBlockAddress() || MO.isTargetIndex())
        return false;
      if (MI.isReturn() && MO.isImm() && MO.getImm() != 0)
        return false;
    }
  }
  return countBodyInstructions(MF) <= kMaxInlineInstrCount;
}

MachineInstr *cloneForCaller(MachineFunction &CallerMF, const MachineInstr &Src,
                             const MachineRegisterInfo &CalleeMRI,
                             DenseMap<Register, Register> &VRegMap) {
  MachineInstr *Clone = CallerMF.CloneMachineInstr(&Src);
  SmallVector<MachineMemOperand *, 2> MemoryOperands;
  for (const MachineMemOperand *MMO : Src.memoperands())
    MemoryOperands.push_back(CallerMF.getMachineMemOperand(
        MMO, MMO->getPointerInfo(), MMO->getSize()));
  Clone->setMemRefs(CallerMF, MemoryOperands);
  MachineRegisterInfo &CallerMRI = CallerMF.getRegInfo();

  for (MachineOperand &MO : Clone->operands()) {
    if (!MO.isReg())
      continue;
    Register R = MO.getReg();
    if (!R.isVirtual())
      continue;

    Register &Mapped = VRegMap[R];
    if (!Mapped) {
      const TargetRegisterClass *RC = CalleeMRI.getRegClass(R);
      Mapped = CallerMRI.createVirtualRegister(RC);
      CallerMF.getProperties().reset(
          MachineFunctionProperties::Property::NoVRegs);
    }
    MO.setReg(Mapped);
  }

  return Clone;
}

bool inlineAtCallsite(MachineFunction &CallerMF, MachineInstr &CallMI,
                      const DenseMap<const Function *, InlineBody> &Bodies,
                      SmallVectorImpl<const Function *> &CallPath) {
  if (!CallMI.isCall() || CallMI.isTerminator() ||
      CallPath.size() >= kMaxCallChain)
    return false;
  const Function *CalleeF = extractDirectCallee(CallMI);
  auto BodyIt = Bodies.find(CalleeF);
  if (BodyIt == Bodies.end())
    return false;
  // The root function is already in the path. Allow three recursive edges.
  if (std::count(CallPath.begin(), CallPath.end(), CalleeF) >
      kMaxSelfInlineDepth)
    return false;

  MachineBasicBlock *CallBB = CallMI.getParent();
  if (!CallBB)
    return false;

  DenseMap<Register, Register> VRegMap;
  const InlineBody &Body = BodyIt->second;
  const MachineRegisterInfo &CalleeMRI = Body.MF->getRegInfo();

  MachineBasicBlock::iterator InsertPos = CallMI.getIterator();
  SmallVector<MachineInstr *, 16> Clones;
  for (const MachineInstr *MI : Body.Instructions)
    Clones.push_back(cloneForCaller(CallerMF, *MI, CalleeMRI, VRegMap));

  for (MachineInstr *Clone : Clones)
    CallBB->insert(InsertPos, Clone);

  CallMI.eraseFromParent();
  CallPath.push_back(CalleeF);
  for (MachineInstr *Clone : Clones)
    inlineAtCallsite(CallerMF, *Clone, Bodies, CallPath);
  CallPath.pop_back();
  return true;
}

class PikhotskiyInlineBackendPass : public ModulePass {
public:
  static char ID;

  PikhotskiyInlineBackendPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    MachineModuleInfo &MMI =
        getAnalysis<MachineModuleInfoWrapperPass>().getMMI();

    // Snapshot all eligible bodies before modifying any of them. The module
    // pass also runs before llc frees individual MachineFunctions.
    DenseMap<const Function *, InlineBody> Bodies;
    for (Function &F : M) {
      MachineFunction *MF = MMI.getMachineFunction(F);
      if (!MF || !canInlineCallee(*MF))
        continue;
      InlineBody &Body = Bodies[&F];
      Body.MF = MF;
      for (const MachineInstr &MI : MF->front()) {
        if (!MI.isDebugInstr() && !MI.isCFIInstruction() && !MI.isTerminator())
          Body.Instructions.push_back(MF->CloneMachineInstr(&MI));
      }
    }

    bool Changed = false;
    for (Function &F : M) {
      MachineFunction *MF = MMI.getMachineFunction(F);
      if (!MF)
        continue;
      SmallVector<const Function *, 8> CallPath{&F};
      for (MachineBasicBlock &MBB : *MF) {
        for (auto It = MBB.begin(); It != MBB.end();) {
          MachineInstr &MI = *It++;
          Changed |= inlineAtCallsite(*MF, MI, Bodies, CallPath);
        }
      }
    }

    for (auto &Entry : Bodies)
      for (MachineInstr *MI : Entry.second.Instructions)
        Entry.second.MF->deleteMachineInstr(MI);
    return Changed;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    AU.addPreserved<MachineModuleInfoWrapperPass>();
    ModulePass::getAnalysisUsage(AU);
  }
};

char PikhotskiyInlineBackendPass::ID = 0;

} // namespace

static RegisterPass<PikhotskiyInlineBackendPass>
    X("pikhotskiy-inline-backend",
      "Inline small direct backend calls with bounded recursion", false, false);
