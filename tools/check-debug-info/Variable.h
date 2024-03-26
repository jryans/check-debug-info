#ifndef VARIABLE_H
#define VARIABLE_H

#include "klee/ADT/Ref.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/Printing.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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

// TODO: Rename this to `ValueEvent` or `Sample`...?
// Loading from memory now manifests as an instance of this, even though that's
// clearly not an "assignment".
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

  // dbg.declare or dbg.value intrinsic
  const llvm::DbgVariableIntrinsic *varIntrinsic;
  // Input values used to compute this assignment
  Values producers;
  // Memory operation or dbg.value intrinsic that defines / uses the producers
  // Definition events (e.g. load from declared address) are distinguished by
  // having a single producer which is also the event itself.
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

  bool isDefinitionEvent() {
    return producers.size() == 1 &&
           producers[0] == llvm::cast<llvm::Value>(event);
  }

  bool isUseEvent() { return !isDefinitionEvent(); }

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
