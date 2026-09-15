#include "X86.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace {

constexpr unsigned kMaxInlineInstrCount = 15;
constexpr unsigned kMaxSelfInlineDepth = 3;
constexpr unsigned kMaxRounds = 64;

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
  return countBodyInstructions(MF) <= kMaxInlineInstrCount;
}

MachineInstr *cloneForCaller(MachineFunction &CallerMF, const MachineInstr &Src,
                             const MachineRegisterInfo &CalleeMRI,
                             DenseMap<Register, Register> &VRegMap) {
  MachineInstr *Clone = CallerMF.CloneMachineInstr(&Src);
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
                      const MachineFunction &CalleeMF) {
  MachineBasicBlock *CallBB = CallMI.getParent();
  if (!CallBB)
    return false;

  DenseMap<Register, Register> VRegMap;
  const MachineRegisterInfo &CalleeMRI = CalleeMF.getRegInfo();

  MachineBasicBlock::iterator InsertPos = CallMI.getIterator();
  const MachineBasicBlock &CalleeEntry = CalleeMF.front();
  SmallVector<MachineInstr *, 16> Clones;
  for (const MachineInstr &MI : CalleeEntry) {
    if (MI.isDebugInstr() || MI.isCFIInstruction())
      continue;
    if (MI.isTerminator())
      continue;

    Clones.push_back(cloneForCaller(CallerMF, MI, CalleeMRI, VRegMap));
  }

  // For a self-call, finish reading the original body before inserting clones.
  for (MachineInstr *Clone : Clones)
    CallBB->insert(InsertPos, Clone);

  CallMI.eraseFromParent();
  return true;
}

class PikhotskiyInlineBackendPass : public MachineFunctionPass {
public:
  static char ID;

  PikhotskiyInlineBackendPass() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    MachineModuleInfo &MMI =
        getAnalysis<MachineModuleInfoWrapperPass>().getMMI();

    bool Changed = false;
    unsigned SelfInlineDepth = 0;

    for (unsigned Round = 0; Round < kMaxRounds; ++Round) {
      bool RoundChanged = false;
      bool InlinedSelf = false;

      for (MachineBasicBlock &MBB : MF) {
        for (auto It = MBB.begin(); It != MBB.end();) {
          MachineInstr &MI = *It++;
          if (!MI.isCall())
            continue;

          const Function *CalleeF = extractDirectCallee(MI);
          if (!CalleeF)
            continue;

          MachineFunction *CalleeMF = MMI.getMachineFunction(*CalleeF);
          if (!CalleeMF || !canInlineCallee(*CalleeMF))
            continue;

          const bool IsSelfCall = (CalleeF == &MF.getFunction());
          if (IsSelfCall && SelfInlineDepth >= kMaxSelfInlineDepth)
            continue;

          if (!inlineAtCallsite(MF, MI, *CalleeMF))
            continue;

          Changed = true;
          RoundChanged = true;
          InlinedSelf |= IsSelfCall;
        }
      }

      if (InlinedSelf)
        ++SelfInlineDepth;
      if (!RoundChanged)
        break;
    }

    return Changed;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char PikhotskiyInlineBackendPass::ID = 0;

} // namespace

static RegisterPass<PikhotskiyInlineBackendPass>
    X("pikhotskiy-inline-backend",
      "Inline small direct backend calls with bounded recursion", false, false);
