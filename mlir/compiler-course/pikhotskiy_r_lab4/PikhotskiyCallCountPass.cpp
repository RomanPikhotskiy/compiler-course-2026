#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;

namespace {
class PikhotskiyCallCountPass
    : public PassWrapper<PikhotskiyCallCountPass, OperationPass<ModuleOp>> {
public:
  StringRef getArgument() const final { return "pikhotskiy-call-count"; }
  StringRef getDescription() const final {
    return "Counts incoming calls from other func.func operations and stores "
           "the result in a call_count attribute";
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    Builder builder(&getContext());

    llvm::DenseMap<Operation *, int64_t> incomingCalls;
    module.walk([&](func::FuncOp function) { incomingCalls[function] = 0; });

    SymbolTableCollection symbols;
    module.walk([&](func::CallOp callOp) {
      auto caller = callOp->getParentOfType<func::FuncOp>();
      // Resolve the symbol in its own scope, not just by its printed name.
      auto callee = symbols.lookupNearestSymbolFrom<func::FuncOp>(
          callOp, callOp.getCalleeAttr());
      if (!caller || !callee || caller == callee)
        return;
      auto it = incomingCalls.find(callee);
      if (it != incomingCalls.end())
        ++it->second;
    });

    module.walk([&](func::FuncOp function) {
      int64_t count = incomingCalls.lookup(function);
      function->setAttr("call_count", builder.getI64IntegerAttr(count));
    });
  }
};
} // namespace

MLIR_DECLARE_EXPLICIT_TYPE_ID(PikhotskiyCallCountPass)
MLIR_DEFINE_EXPLICIT_TYPE_ID(PikhotskiyCallCountPass)

mlir::PassPluginLibraryInfo getPikhotskiyCallCountPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "PikhotskiyCallCountPass", "1.0",
          []() { mlir::PassRegistration<PikhotskiyCallCountPass>(); }};
}

extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return getPikhotskiyCallCountPassPluginInfo();
}
