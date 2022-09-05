#include "ValuesCollector.h"
#include "Variable.h"

#include "klee/ADT/Ref.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/Cell.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/Printing.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"
#include "klee/Support/ModuleUtil.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <vector>

using namespace klee;
using namespace llvm;

#define DEBUG_TYPE "values-collector"

class VCHandler : public InterpreterHandler {
private:
  StringRef outputDir;
  VAs &varsAssignments;
  std::unique_ptr<llvm::raw_fd_ostream> infoStream;
  Interpreter *interpreter;

public:
  VCHandler(StringRef outputDir, VAs &varsAssignments)
      : outputDir(outputDir), varsAssignments(varsAssignments),
        infoStream(openOutputFile("info")) {}

  void setInterpreter(Interpreter *interp) { interpreter = interp; }

  llvm::raw_ostream &getInfoStream() const override { return *infoStream; }

  std::string getOutputFilename(const std::string &filename) override;
  std::unique_ptr<llvm::raw_fd_ostream>
  openOutputFile(const std::string &filename) override;

  void incPathsCompleted() override {}
  void incPathsExplored(std::uint32_t num = 1) override {}

  void visitBeforeExecution(ExecutionState &state, KInstruction *ki) override;
  void recordValue(const Value *producer, ref<Expr> symbolicValue);

  void processTestCase(const ExecutionState &state, const char *err,
                       const char *suffix) override {}
};

std::string VCHandler::getOutputFilename(const std::string &filename) {
  SmallString<128> path(outputDir);
  sys::path::append(path, filename);
  return path.c_str();
}

std::unique_ptr<llvm::raw_fd_ostream>
VCHandler::openOutputFile(const std::string &filename) {
  std::string error;
  std::string path = getOutputFilename(filename);
  auto f = klee_open_output_file(path, error);
  if (!f) {
    klee_warning("Error opening file `%s`. You may have run out of file "
                 "descriptors: try to increase the maximum number of open file "
                 "descriptors by using ulimit (%s).",
                 path.c_str(), error.c_str());
    return nullptr;
  }
  return f;
}

void VCHandler::visitBeforeExecution(ExecutionState &state, KInstruction *ki) {
  const auto *instruction = ki->inst;

  // Stores are visited for writes to `dbg.declare` intrinsic targets
  // `dbg.value` intrinsics are visited for their operands (incl. constants)
  if (!isa<StoreInst>(*instruction) && !isa<DbgValueInst>(*instruction))
    return;

  if (const auto *storeInstruction = dyn_cast<StoreInst>(instruction)) {
    ref<Expr> symbolicValue = interpreter->getOperandCell(state, ki, 0).value;
    recordValue(storeInstruction, symbolicValue);
  } else if (const auto *valueIntrinsic = dyn_cast<DbgValueInst>(instruction)) {
    // Calls (incl. intrinsics) store the call target as operand 0, so their
    // real operands are shifted over by 1.
    ref<Expr> symbolicValue = interpreter->getOperandCell(state, ki, 1).value;
    recordValue(valueIntrinsic, symbolicValue);
  }
}

void VCHandler::recordValue(const Value *producer, ref<Expr> symbolicValue) {
  assert(producer && "Symbolic value producer missing");
  if (!symbolicValue)
    return;

  // Look for assignments where this was the producer
  // TODO: Gather all producers up front first for faster filtering...?
  const auto matchingAssignments =
      make_filter_range(varsAssignments, [&](VA &pair) {
        const auto &assignment = pair.second;
        // TODO: Re-think producer vs. intrinsic structure...?
        return assignment.producer == producer ||
               assignment.varIntrinsic == producer;
      });

  for (VA &pair : matchingAssignments) {
    const auto &var = pair.first;
    auto &assignment = pair.second;
    assignment.producedSymbolicValue = symbolicValue;
    KLEE_DEBUG(dbgs() << "Collected value for `" << var.name << "`\n");
    KLEE_DEBUG(dbgs() << printValue(*producer) << "\n");
    KLEE_DEBUG(dbgs() << symbolicValue << "\n");
  }
}

std::unique_ptr<Interpreter>
collectValues(StringRef runtimeDir, std::unique_ptr<llvm::Module> mainModule,
              StringRef functionName, StringRef outputDir,
              VAs &varsAssignments) {
  LLVMContext &ctx = mainModule->getContext();
  const std::string &moduleTriple = mainModule->getTargetTriple();
  std::string hostTriple = llvm::sys::getDefaultTargetTriple();

  if (moduleTriple != hostTriple)
    klee_warning("Module and host target triples do not match: '%s' != '%s'\n"
                 "This may cause unexpected crashes or assertion violations.",
                 moduleTriple.c_str(), hostTriple.c_str());

  // Detect architecture
  std::string optSuffix = "64"; // Fall back to 64bit
  if (moduleTriple.find("i686") != std::string::npos ||
      moduleTriple.find("i586") != std::string::npos ||
      moduleTriple.find("i486") != std::string::npos ||
      moduleTriple.find("i386") != std::string::npos)
    optSuffix = "32";

  // Add runtime build configuration
  optSuffix += "_";
  optSuffix += RUNTIME_CONFIGURATION;

  // Push the module as the first entry
  std::vector<std::unique_ptr<llvm::Module>> modules;
  modules.emplace_back(std::move(mainModule));

  Interpreter::ModuleOptions moduleOpts(runtimeDir.str(), functionName.str(),
                                        optSuffix,
                                        /*Optimize=*/false,
                                        /*CheckDivZero=*/false,
                                        /*CheckOvershift=*/false);

  // TODO: WithPOSIXRuntime...?
  // TODO: libc++...?
  // TODO: Other libcs...?

  SmallString<128> runtimePath(runtimeDir);
  llvm::sys::path::append(runtimePath,
                          "libkleeRuntimeFreestanding" + optSuffix + ".bca");
  std::string errorMsg;
  if (!klee::loadFile(runtimePath.c_str(), ctx, modules, errorMsg))
    klee_error("error loading freestanding support '%s': %s",
               runtimePath.c_str(), errorMsg.c_str());

  // TODO: Program args and environment...?

  Interpreter::InterpreterOptions interpreterOpts;
  VCHandler handler(outputDir, varsAssignments);
  std::unique_ptr<Interpreter> interpreter(
      Interpreter::create(ctx, interpreterOpts, &handler));
  assert(interpreter);
  handler.setInterpreter(interpreter.get());

  // `interpreter` now acts as though it owns the modules, though it doesn't
  // make that entirely clear since it takes the `unique_ptr` by reference...
  // Need to keep `interpreter` alive to avoid the modules being deleted.
  auto finalModule = interpreter->setModule(modules, moduleOpts);
  Function *mainFn = finalModule->getFunction(functionName);
  if (!mainFn) {
    klee_error("Entry function '%s' not found in module.", functionName.data());
  }

  // TODO: Externals and globals check...?
  // TODO: Start time...?
  // TODO: Replaying...?
  // TODO: Seeds...?
  // TODO: Change directory...?

  char *argv[0] = {};
  char *envp[0] = {};
  interpreter->runFunctionAsMain(mainFn, /*argc=*/0, argv, envp);

  // TODO: End time...?
  // TODO: More stats...?

  uint64_t forks = *theStatisticManager->getStatisticByName("Forks");

  handler.getInfoStream() << "KLEE: done: explored paths = " << 1 + forks
                          << "\n";

  // Stats manager holds onto data after execution
  // Reset it to get ready for the next run
  theStatisticManager->reset();

  // The `interpreter` owns the `KModule` and `Expr`s, so we return it here to
  // keep those bits alive.
  return interpreter;
}
