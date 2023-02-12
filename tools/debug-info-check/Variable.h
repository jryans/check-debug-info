#ifndef VARIABLE_H
#define VARIABLE_H

#include "klee/ADT/Ref.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/Printing.h"

#include "llvm/ADT/IntervalMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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
  unsigned int declLine;

  // Imported from optional diagnostics
  bool unused = false;

  bool operator==(const Variable &other) const {
    return std::tie(name, declLine) == std::tie(other.name, other.declLine);
  }

  bool operator!=(const Variable &other) const { return !(*this == other); }

  bool operator<(const Variable &other) const {
    return std::tie(name, declLine) < std::tie(other.name, other.declLine);
  }
};

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

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Variable &variable) {
  out << "`" << variable.name << "` ";
  out << "(decl src ln " << variable.declLine << ")";
  return out;
}

using Exprs = llvm::SmallVector<klee::ref<klee::Expr>, 2>;

struct Assignment {
  // TODO: Pointer to the associated `Variable`...?

  unsigned int startLine;
  unsigned int startColumn;
  // Generation used to distinguish assignments with the same source coordinates
  // (e.g. initialisation and advancement on the same line)
  // Computed via the dominator tree (earlier generations dominate)
  unsigned int generation;

  // Everything below is not checked during comparison

  // dbg.declare or dbg.value intrinsic
  const llvm::DbgVariableIntrinsic *varIntrinsic;
  // Input values used to compute this assignment
  Values producers;
  // Store instruction or dbg.value intrinsic that uses the producers
  const llvm::Instruction *user;
  // User assembly line
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

  bool operator==(const Assignment &other) const {
    // If either one is missing column info, ignore it for comparison purposes
    if (!startColumn || !other.startColumn) {
      return std::tie(startLine, generation) ==
             std::tie(other.startLine, other.generation);
    }
    return std::tie(startLine, startColumn, generation) ==
           std::tie(other.startLine, other.startColumn, other.generation);
  }

  bool operator!=(const Assignment &other) const { return !(*this == other); }

  bool operator<(const Assignment &other) const {
    // If either one is missing column info, ignore it for comparison purposes
    if (!startColumn || !other.startColumn) {
      return std::tie(startLine, generation) <
             std::tie(other.startLine, other.generation);
    }
    return std::tie(startLine, startColumn, generation) <
           std::tie(other.startLine, other.startColumn, other.generation);
  }

  bool operator<=(const Assignment &other) const { return !(other < *this); }

  bool isValueConsistent(const Variable &var, const llvm::Value *other) const;

  // This starts from the produced symbolic value and evaluates the expression
  // attached to the variable intrinsic (if any).
  klee::ref<klee::Expr> evaluate();
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Assignment &assignment) {
  out << "src ln " << assignment.startLine;
  out << ", col " << assignment.startColumn;
  out << ", gen " << assignment.generation;
  return out;
}

using VariablesSet = llvm::SmallSet<Variable, 8>;

using Assignments = llvm::SmallVector<Assignment>;
using VToAs = std::map<Variable, Assignments>;

struct Location {
  unsigned int line;
  unsigned int column;
  // Generation used to distinguish assignments with the same source coordinates
  // (e.g. initialisation and advancement on the same line)
  // Computed via the dominator tree (earlier generations dominate)
  unsigned int generation;

  bool operator==(const Location &other) const {
    // If either one is missing column info, ignore it for comparison purposes
    if (!column || !other.column) {
      return std::tie(line, generation) ==
             std::tie(other.line, other.generation);
    }
    return std::tie(line, column, generation) ==
           std::tie(other.line, other.column, other.generation);
  }

  bool operator!=(const Location &other) const { return !(*this == other); }

  bool operator<(const Location &other) const {
    // If either one is missing column info, ignore it for comparison purposes
    if (!column || !other.column) {
      return std::tie(line, generation) <
             std::tie(other.line, other.generation);
    }
    return std::tie(line, column, generation) <
           std::tie(other.line, other.column, other.generation);
  }

  bool operator<=(const Location &other) const { return !(other < *this); }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Location &location) {
  out << "src ln " << location.line;
  out << ", col " << location.column;
  out << ", gen " << location.generation;
  return out;
}

using RangeToA = llvm::IntervalMap<Location, Assignment *, 8,
                                   llvm::IntervalMapHalfOpenInfo<Location>>;
using VToRangeToA = std::map<Variable, RangeToA>;

using VA = std::pair<Variable, Assignment *>;
using VAs = llvm::SmallVector<VA>;

#endif
