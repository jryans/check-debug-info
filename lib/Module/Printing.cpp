#include "klee/Module/Printing.h"

#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"

#include <string>

using namespace llvm;

namespace klee {

std::string printInstruction(const Instruction &instruction) {
  std::string str;
  raw_string_ostream out(str);

  // Print instruction as normal
  out << instruction;
  out.flush();

  // Inline debug location if present
  const auto debugLoc = instruction.getDebugLoc();
  if (debugLoc) {
    out << ", l" << debugLoc.getLine() << " c" << debugLoc.getCol();
    out.flush();
    Regex dbgAttachment(", !dbg ![0-9]+");
    str = dbgAttachment.sub("", str);
  }

  // Remove alignment if present
  Regex alignment(", align [0-9]+");
  str = alignment.sub("", str);

  return str;
}

} // namespace klee
