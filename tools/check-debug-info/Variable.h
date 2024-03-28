#ifndef VARIABLE_H
#define VARIABLE_H

#include "klee/ADT/Ref.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/Printing.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <tuple>
#include <utility>

namespace klee {
class Expr;
}

namespace llvm {
class DbgVariableIntrinsic;
class DILocalVariable;
class Instruction;
class Value;
} // namespace llvm

struct Variable {
  const llvm::DILocalVariable *diVariable;

  // The following are extracted from `diVariable` for easier access because
  // they are frequently used.

  llvm::StringRef name;
  llvm::StringRef declFile;
  unsigned int declLine;

  // Imported from optional diagnostics
  bool unused = false;

  bool operator==(const Variable &other) const {
    return std::tie(name, declFile, declLine) ==
           std::tie(other.name, other.declFile, other.declLine);
  }

  bool operator!=(const Variable &other) const { return !(*this == other); }

  bool operator<(const Variable &other) const {
    return std::tie(name, declFile, declLine) <
           std::tie(other.name, other.declFile, other.declLine);
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Variable &variable) {
  out << "`" << variable.name << "` ";
  out << "(decl src ln " << variable.declLine << ")";
  return out;
}

using Values = llvm::SmallVector<const llvm::Value *, 2>;

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Values &values) {
  if (values.empty()) {
    out << "<empty>";
    return out;
  }

  if (values.size() == 1) {
    out << klee::printValue(*values[0]);
    return out;
  }

  out << "[ ";
  for (const auto *value : values) {
    out << klee::printValue(*value);
    if (value != values.back())
      out << ", ";
  }
  out << " ]";

  return out;
}

using Exprs = llvm::SmallVector<klee::ref<klee::Expr>, 2>;

struct Assignment {
  // TODO: Pointer to the associated `Variable`...?

  // Source line by which all producers are available and assignment occurs
  // Although this seemed good enough for matching initially, there are too many
  // cases where this is unknown (constants) or inaccurate (reused values).
  unsigned int producedLine;
  // Column info is generally too unreliable for matching across versions
  unsigned int producedColumn;
  // Source line where the assignment is known to already be live
  // Typically this comes from the instruction after assignment
  unsigned int liveLine;
  // Order in which the assignment was first encountered during execution
  llvm::Optional<unsigned int> encounter;

  // Everything below is not checked during comparison

  // `dbg.declare` or `dbg.value` intrinsic
  const llvm::DbgVariableIntrinsic *varIntrinsic;
  // Input values used to compute this assignment
  Values producers;
  // Event that defines / uses the producers
  // Either a `dbg.value` intrinsic
  // or a memory operation (store, call with address-taken local)
  const llvm::Instruction *event;
  // Event assembly line
  // No longer used for ordering, but still nice for logging
  unsigned int asmLine;
  // ref<Expr> cannot be `const` if we want `std::swap` to work...
  // TODO: Work out if this is a bug in `ref`
  Exprs producedSymbolicValues;
  // Index of incoming block at collection time
  // Used to select the right portion of phi nodes
  // KLEE does provide the runtime value for us, but still nice for logging
  unsigned int incomingBlockIndex;
  // Caches the evaluated symbolic value computed by `evaluate`
  klee::ref<klee::Expr> evaluatedSymbolicValue;
  // Whether assignment appears to be removable by optimisation
  // TODO: Track different removability reasons separately...?
  bool removable = false;
  // Whether assignment is debug meaningful
  // During execution, memory assignments only remain meaningful if the last
  // seen variable intrinsic is implicit (referencing memory).
  bool meaningful = true;

  bool operator==(const Assignment &other) const {
    return encounter == other.encounter;
  }

  bool operator!=(const Assignment &other) const { return !(*this == other); }

  bool operator<(const Assignment &other) const {
    return encounter < other.encounter;
  }

  bool operator<=(const Assignment &other) const { return !(other < *this); }

  static bool isExplicitValueEvent(const llvm::Instruction *event) {
    return llvm::isa<llvm::DbgValueInst>(event);
  }
  bool isExplicitValue() const { return isExplicitValueEvent(event); }

  static bool isImplicitMemoryEvent(const llvm::Instruction *event) {
    return !isExplicitValueEvent(event);
  }
  bool isImplicitMemory() const { return isImplicitMemoryEvent(event); }

  static bool isProgramCallEvent(const llvm::Instruction *event) {
    if (const auto *callInst = llvm::dyn_cast<llvm::CallInst>(event)) {
      // Ensure this calls a program function (not an intrinsic)
      // (This assumes an intrinsic is unlikely to be called indirectly)
      return callInst->isIndirectCall() ||
             callInst->getCalledFunction()->getIntrinsicID() ==
                 llvm::Intrinsic::not_intrinsic;
    }
    return false;
  }
  bool isProgramCall() const { return isProgramCallEvent(event); }

  bool isValueConsistent(const Variable &var, const llvm::Value *other) const;

  // This starts from the produced symbolic value and evaluates the expression
  // attached to the variable intrinsic (if any).
  klee::ref<klee::Expr> evaluate();
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Assignment &assignment) {
  out << "asm ln " << assignment.asmLine;
  out << ", prod ln " << assignment.producedLine;
  out << "." << assignment.producedColumn;
  out << ", live ln " << assignment.liveLine;
  out << ", enc " << assignment.encounter;
  return out;
}

using VariablesSet = llvm::SmallSet<Variable, 8>;

using Assignments = llvm::SmallVector<Assignment>;
using VToAs = std::map<Variable, Assignments>;

using EncounterToA = std::map<unsigned int, Assignment *>;
using VToEncounterToA = std::map<Variable, EncounterToA>;

using VA = std::pair<Variable, Assignment *>;
using VAs = llvm::SmallVector<VA>;

#endif
