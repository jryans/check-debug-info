#ifndef VARIABLE_H
#define VARIABLE_H

#include "klee/ADT/Ref.h"
#include "klee/Expr/Expr.h"

#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <tuple>

namespace klee {
class Expr;
}

namespace llvm {
class DbgVariableIntrinsic;
class Value;
} // namespace llvm

struct Variable {
  llvm::StringRef name;
  unsigned int declLine;

  bool operator==(const Variable &other) const {
    return std::tie(name, declLine) == std::tie(other.name, other.declLine);
  }

  bool operator<(const Variable &other) const {
    return std::tie(name, declLine) < std::tie(other.name, other.declLine);
  }
};

struct LiveValueRange {
  unsigned int startLine;
  unsigned int endLine = UINT32_MAX;
  // Range IDs are generated from `asmLine` relative ordering to ease comparison
  unsigned int id;

  // Not checked during comparison

  const llvm::Value *producer;
  const llvm::DbgVariableIntrinsic *varIntrinsic;
  // Producer or variable intrinsic assembly line
  // Used for relative ordering of ranges with the same start line
  unsigned int asmLine;
  // This cannot be `const` if we want `std::swap` to work...
  // TODO: Work out if this is a bug in `ref`
  klee::ref<klee::Expr> producedSymbolicValue;

  bool operator==(const LiveValueRange &other) const {
    return std::tie(startLine, endLine, id) ==
           std::tie(other.startLine, other.endLine, other.id);
  }

  bool operator<(const LiveValueRange &other) const {
    return std::tie(startLine, endLine, id) <
           std::tie(other.startLine, other.endLine, other.id);
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &out,
                                     const LiveValueRange &range) {
  out << range.id << ": ";
  out << "[" << range.startLine << ", ";
  if (range.endLine == UINT32_MAX)
    out << "∞";
  else
    out << range.endLine;
  out << ")";
  return out;
}

using VariablesSet = llvm::SmallSet<Variable, 8>;

using LVRs = llvm::SmallVector<LiveValueRange>;
// There might be a good match for this in LLVM's data structures, but wasn't
// quite sure...
using VariableToLVRs = std::map<Variable, LVRs>;
using VariableAndLVR = std::pair<Variable, LiveValueRange>;
using VariablesAndLVRs = llvm::SmallVector<VariableAndLVR>;

#endif
