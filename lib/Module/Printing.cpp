#include "klee/Module/Printing.h"

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"

#include <regex>
#include <string>

using namespace llvm;

namespace klee {

std::string printInstruction(const Instruction &instruction) {
  std::string str;
  raw_string_ostream out(str);

  // Print instruction as normal
  out << instruction;
  out.flush();

  // Inline debug location
  const auto debugLoc = instruction.getDebugLoc();
  if (debugLoc) {
    out << ", l" << debugLoc.getLine() << " c" << debugLoc.getCol();
    out.flush();
    Regex dbgAttachment(", !dbg ![0-9]+");
    str = dbgAttachment.sub("", str);
  }

  // Remove alignment
  Regex alignment(", align [0-9]+");
  str = alignment.sub("", str);

  return str;
}

std::string printModule(const Module &module) {
  std::string str;
  raw_string_ostream out(str);

  // Print module as normal
  out << module;
  out.flush();

  // Remove alignment
  std::regex alignment(", align [0-9]+");
  str = std::regex_replace(str, alignment, "");

  // Simplify intrinsics
  std::regex intrinsicPrefix("call void @llvm.");
  str = std::regex_replace(str, intrinsicPrefix, "@");

  // Remove "metadata"
  std::regex metadata("metadata ");
  str = std::regex_replace(str, metadata, "");

  // Remove empty `DIExpression`s
  std::regex emptyDiExpression(", !DIExpression\\(\\)");
  str = std::regex_replace(str, emptyDiExpression, "");

  return str;
}

} // namespace klee
