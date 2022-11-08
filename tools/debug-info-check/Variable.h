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

using Exprs = llvm::SmallVector<klee::ref<klee::Expr>, 2>;

struct Assignment {
  // TODO: Pointer to the associated `Variable`...?

  unsigned int startLine;
  unsigned int startColumn;
  // IDs are generated from `asmLine` relative ordering to ease comparison
  // TODO: Maybe remove this now that we have improved matching...?
  unsigned int id;

  // Not checked during comparison

  // dbg.declare or dbg.value intrinsic
  const llvm::DbgVariableIntrinsic *varIntrinsic;
  // Input values used to compute this assignment
  Values producers;
  // Store instruction or dbg.value intrinsic that uses the producers
  const llvm::Instruction *user;
  // User assembly line
  // Used for relative ordering with the same start line
  unsigned int asmLine;
  // ref<Expr> cannot be `const` if we want `std::swap` to work...
  // TODO: Work out if this is a bug in `ref`
  Exprs producedSymbolicValues;
  // Caches the evaluated symbolic value computed by `evaluate`
  klee::ref<klee::Expr> evaluatedSymbolicValue;

  bool operator==(const Assignment &other) const {
    return std::tie(startLine, startColumn, id) ==
           std::tie(other.startLine, other.startColumn, other.id);
  }

  bool operator!=(const Assignment &other) const { return !(*this == other); }

  bool operator<(const Assignment &other) const {
    return std::tie(startLine, startColumn, id) <
           std::tie(other.startLine, other.startColumn, other.id);
  }

  bool isValueConsistent(const Variable &var, const llvm::Value *other) const;

  // This starts from the produced symbolic value and evaluates the expression
  // attached to the variable intrinsic (if any).
  klee::ref<klee::Expr> evaluate();
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const Assignment &assignment) {
  out << assignment.id << ", ";
  out << "src line " << assignment.startLine;
  out << ", column " << assignment.startColumn;
  return out;
}

using VariablesSet = llvm::SmallSet<Variable, 8>;

using Assignments = llvm::SmallVector<Assignment>;
using VToAs = std::map<Variable, Assignments>;

using Location = std::pair<unsigned int, unsigned int>;
using RangeToA = llvm::IntervalMap<Location, Assignment *, 8,
                                   llvm::IntervalMapHalfOpenInfo<Location>>;
using VToRangeToA = std::map<Variable, RangeToA>;

using VA = std::pair<Variable, Assignment *>;
using VAs = llvm::SmallVector<VA>;

#endif
