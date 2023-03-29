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

#include <climits>
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

  // Source line by which all producers are available and assignment occurs
  // Although this seemed good enough for matching initially, there are too many
  // cases where this is unknown (constants) or inaccurate (reused values).
  unsigned int producedLine;
  // Column info is generally too unreliable for matching across versions
  unsigned int producedColumn;
  // Source line where the assignment is known to already be live
  // Typically this comes from the instruction after assignment
  unsigned int liveLine;
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
    return std::tie(liveLine, generation) ==
           std::tie(other.liveLine, other.generation);
  }

  bool operator!=(const Assignment &other) const { return !(*this == other); }

  bool operator<(const Assignment &other) const {
    return std::tie(liveLine, generation) <
           std::tie(other.liveLine, other.generation);
  }

  bool operator<=(const Assignment &other) const { return !(other < *this); }

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
  out << ", gen " << assignment.generation;
  return out;
}

using VariablesSet = llvm::SmallSet<Variable, 8>;

using Assignments = llvm::SmallVector<Assignment>;
using VToAs = std::map<Variable, Assignments>;

struct Location {
  unsigned int line;
  // Generation used to distinguish assignments with the same source coordinates
  // (e.g. initialisation and advancement on the same line)
  // Computed via the dominator tree (earlier generations dominate)
  unsigned int generation;

  bool operator==(const Location &other) const {
    return std::tie(line, generation) == std::tie(other.line, other.generation);
  }

  bool operator!=(const Location &other) const { return !(*this == other); }

  bool operator<(const Location &other) const {
    return std::tie(line, generation) < std::tie(other.line, other.generation);
  }

  bool operator>(const Location &other) const { return other < *this; }

  bool operator<=(const Location &other) const { return !(other < *this); }

  bool operator>=(const Location &other) const { return !(*this < other); }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Location &location) {
  out << "live ln ";
  if (location.line == UINT_MAX)
    out << "∞";
  else
    out << location.line;
  out << ", gen ";
  if (location.generation == UINT_MAX)
    out << "∞";
  else
    out << location.generation;
  return out;
}

using RangeToA = llvm::IntervalMap<Location, Assignment *, 8,
                                   llvm::IntervalMapHalfOpenInfo<Location>>;
using VToRangeToA = std::map<Variable, RangeToA>;

using VA = std::pair<Variable, Assignment *>;
using VAs = llvm::SmallVector<VA>;

#endif
