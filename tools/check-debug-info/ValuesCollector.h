#ifndef VALUESCOLLECTOR_H
#define VALUESCOLLECTOR_H

#include "Files.h"
#include "Variable.h"

#include "klee/ADT/Ref.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Expr.h"

#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <string>

namespace klee {
struct ExecutionEvent;
class ExecutionState;
class InstructionInfoTable;
struct KInstruction;
} // namespace klee

namespace llvm {
class Function;
class Module;
class StringRef;
class Value;
} // namespace llvm

class PointerExpr : public klee::ConstantExpr {
private:
  klee::ref<klee::Expr> derefExpr;

  PointerExpr(const llvm::APInt &pointer, klee::ref<klee::Expr> derefExpr)
      : klee::ConstantExpr(pointer), derefExpr(derefExpr) {}

public:
  klee::ref<klee::Expr> deref() const override { return derefExpr; }

  static klee::ref<PointerExpr> alloc(const llvm::APInt &pointer,
                                      klee::ref<klee::Expr> derefExpr) {
    klee::ref<PointerExpr> r(new PointerExpr(pointer, derefExpr));
    r->computeHash();
    return r;
  }

  static klee::ref<PointerExpr> alloc(uint64_t pointer,
                                      klee::ref<klee::Expr> derefExpr) {
    return alloc(llvm::APInt(klee::Expr::Int64, pointer), derefExpr);
  }

  static klee::ref<PointerExpr> create(uint64_t pointer,
                                       klee::ref<klee::Expr> derefExpr) {
    return alloc(pointer, derefExpr);
  }
};

class VCHandler : public klee::InterpreterHandler {
private:
  llvm::StringRef outputDir;
  std::unique_ptr<llvm::raw_fd_ostream> infoStream;
  klee::Interpreter *interpreter;
  VAs *varsAssignments;
  // TODO: Could store this on `Variable` if it were unique
  std::map<Variable, unsigned int> nextEncounters;

public:
  VCHandler() {}

  void setInterpreter(klee::Interpreter *interp) { interpreter = interp; }
  void setOutputDir(llvm::StringRef outputDirectory) {
    outputDir = outputDirectory;
    infoStream = openOutputFile("info");
  }
  void setVarsAssignments(VAs *v) {
    varsAssignments = v;
    nextEncounters.clear();
  }

  llvm::raw_ostream &getInfoStream() const override { return *infoStream; }

  std::string getOutputFilename(const std::string &filename) override {
    return ::getOutputFilename(outputDir, filename);
  }
  std::unique_ptr<llvm::raw_fd_ostream>
  openOutputFile(const std::string &filename) override {
    return ::openOutputFile(outputDir, filename);
  }

  void incPathsCompleted() override {}
  void incPathsExplored(std::uint32_t num = 1) override {}

  void visitBeforeExecution(klee::ExecutionState &state,
                            klee::ExecutionEvent &execEvent,
                            klee::KInstruction *ki) override;
  void visitAfterExecution(klee::ExecutionState &state,
                           klee::ExecutionEvent &execEvent,
                           klee::KInstruction *ki) override;
  void recordValue(klee::ExecutionState &state, klee::ExecutionEvent &execEvent,
                   const llvm::Instruction *valueEvent,
                   const llvm::Value *producer,
                   klee::ref<klee::Expr> symbolicValue);

  void processTestCase(const klee::ExecutionState &state, const char *err,
                       const char *suffix) override {}

private:
  klee::ref<klee::Expr>
  resolvePointers(klee::ExecutionState &state, const llvm::Value *producer,
                  klee::ref<klee::Expr> symbolicValue,
                  const llvm::DbgVariableIntrinsic *varIntrinsic);
};

struct ExecutionValidity {
  bool functionCovered;
  bool executionComplete;
  bool withinTimeLimit;
  bool withinForkLimit;

  bool isCompleteButUncovered() const {
    return executionComplete && !functionCovered;
  }

  const char *functionCoveredStr() const {
    return functionCovered ? "true" : "false";
  }

  const char *executionCompleteStr() const {
    return executionComplete ? "true" : "false";
  }

  const char *withinTimeLimitStr() const {
    return withinTimeLimit ? "true" : "false";
  }

  const char *withinForkLimitStr() const {
    return withinForkLimit ? "true" : "false";
  }
};

class ValuesCollector {
private:
  std::unique_ptr<VCHandler> handler;
  std::unique_ptr<klee::Interpreter> interpreter;
  llvm::Function *function;

public:
  void prepare(const llvm::StringRef moduleDir,
               const llvm::StringRef runtimeDir,
               std::unique_ptr<llvm::Module> module);

  void collect(const llvm::StringRef functionName,
               const llvm::StringRef outputDir, VAs *varsAssignments);

  const llvm::Module *getModule() const { return interpreter->getModule(); }

  const klee::InstructionInfoTable &getInstructionInfoTable() const {
    return interpreter->getInstructionInfoTable();
  }

  bool isFunctionCovered() const {
    return interpreter->isFunctionCovered(*function);
  }

  bool isExecutionComplete() const {
    return interpreter->isExecutionComplete();
  }

  bool isWithinTimeLimit() const { return interpreter->isWithinTimeLimit(); }

  bool isWithinForkLimit() const { return interpreter->isWithinForkLimit(); }

  ExecutionValidity getExecutionValidity() const {
    return ExecutionValidity{
        isFunctionCovered(),
        isExecutionComplete(),
        isWithinTimeLimit(),
        isWithinForkLimit(),
    };
  }
};

#endif
