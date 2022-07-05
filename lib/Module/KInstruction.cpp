//===-- KInstruction.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Module/KInstruction.h"
#include <string>

using namespace llvm;
using namespace klee;

/***/

KInstruction::~KInstruction() {
  delete[] operands;
}

std::string KInstruction::getSourceLocation() const {
  if (!info->file.empty())
    return info->file + ":l" + std::to_string(info->line) + ":c" +
           std::to_string(info->column);
  else return "[no debug info]";
}
