#ifndef VALUESCOLLECTOR_H
#define VALUESCOLLECTOR_H

#include "Variable.h"

#include "klee/ADT/Ref.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Expr.h"

#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

namespace klee {
struct ExecutionEvent;
class ExecutionState;
struct KInstruction;
} // namespace klee

namespace llvm {
class Function;
class Module;
class StringRef;
class Value;
} // namespace llvm

class VCHandler : public klee::InterpreterHandler {
private:
  llvm::StringRef outputDir;
  std::unique_ptr<llvm::raw_fd_ostream> infoStream;
  klee::Interpreter *interpreter;
  VAs *varsAssignments;

public:
  VCHandler(llvm::StringRef outputDir)
      : outputDir(outputDir), infoStream(openOutputFile("info")) {}

  void setInterpreter(klee::Interpreter *interp) { interpreter = interp; }
  void setVarsAssignments(VAs *v) { varsAssignments = v; }

  llvm::raw_ostream &getInfoStream() const override { return *infoStream; }

  std::string getOutputFilename(const std::string &filename) override;
  std::unique_ptr<llvm::raw_fd_ostream>
  openOutputFile(const std::string &filename) override;

  void incPathsCompleted() override {}
  void incPathsExplored(std::uint32_t num = 1) override {}

  void visitBeforeExecution(klee::ExecutionState &state,
                            klee::ExecutionEvent &event,
                            klee::KInstruction *ki) override;
  void recordValue(klee::ExecutionState &state, klee::ExecutionEvent &event,
                   const llvm::Instruction *user, const llvm::Value *producer,
                   klee::ref<klee::Expr> symbolicValue);

  void processTestCase(const klee::ExecutionState &state, const char *err,
                       const char *suffix) override {}

private:
  klee::ref<klee::Expr> resolvePointers(klee::ExecutionState &state,
                                        const llvm::Value *producer,
                                        klee::ref<klee::Expr> symbolicValue);
};

class ValuesCollector {
private:
  std::unique_ptr<VCHandler> handler;
  std::unique_ptr<klee::Interpreter> interpreter;
  llvm::Function *entryFn;

public:
  void prepare(llvm::StringRef runtimeDir, std::unique_ptr<llvm::Module> module,
               llvm::StringRef functionName, llvm::StringRef outputDir);

  void collect(VAs *varsAssignments);

  llvm::Module *getModule() const { return interpreter->getModule(); }

  bool isFunctionCovered(const llvm::Function &function) const {
    return interpreter->isFunctionCovered(function);
  }

  bool hasCompleteExecution() const {
    return interpreter->hasCompleteExecution();
  }
};

#endif
