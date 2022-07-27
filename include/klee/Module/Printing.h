#ifndef KLEE_MODULE_PRINTING_H
#define KLEE_MODULE_PRINTING_H

#include <string>

namespace llvm {
class Instruction;
}

namespace klee {

std::string printInstruction(const llvm::Instruction &instruction);

} // namespace klee

#endif
