#include "klee/Module/Printing.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/AsmParser/SlotMapping.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
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
    std::regex dbgAttachment(", !dbg ![0-9]+");
    str = std::regex_replace(str, dbgAttachment, "");
  }

  // Remove alignment
  std::regex alignment(", align [0-9]+");
  str = std::regex_replace(str, alignment, "");

  // Simplify intrinsics
  std::regex intrinsicPrefix("call void @llvm.");
  str = std::regex_replace(str, intrinsicPrefix, "@");

  // Remove "metadata" prefix
  std::regex metadataPrefix("metadata ");
  str = std::regex_replace(str, metadataPrefix, "");

  // Remove empty `DIExpression`s
  std::regex emptyDiExpression(", !DIExpression\\(\\)");
  str = std::regex_replace(str, emptyDiExpression, "");

  return str;
}

std::string printValue(const Value &value) {
  if (const auto *instruction = dyn_cast<Instruction>(&value)) {
    return printInstruction(*instruction);
  }

  std::string str;
  raw_string_ostream out(str);

  // Print value as normal
  out << value;
  return out.str();
}

std::string printModule(const Module &module) {
  std::string str;
  raw_string_ostream out(str);

  // Print module as normal
  out << module;
  out.flush();

  // In order to inline metadata later on, we need a mapping from IR slot
  // number to metadata node, but the IR printer treats that as private state.
  // Luckily, the parser makes it available, so we can parse the thing we just
  // printed to get the mapping.
  SMDiagnostic err;
  SlotMapping slots;
  parseAssemblyString(str, err, module.getContext(), &slots);

  // Remove alignment
  std::regex alignment(", align [0-9]+");
  str = std::regex_replace(str, alignment, "");

  // Simplify intrinsics
  std::regex intrinsicPrefix("call void @llvm.");
  str = std::regex_replace(str, intrinsicPrefix, "@");

  // Remove "metadata" prefix
  std::regex metadataPrefix("metadata ");
  str = std::regex_replace(str, metadataPrefix, "");

  // Remove empty `DIExpression`s
  std::regex emptyDiExpression(", !DIExpression\\(\\)");
  str = std::regex_replace(str, emptyDiExpression, "");

  // Remove trailing function definition attributes
  std::regex defineAttrs("(define.+?\\) ).*\\{");
  str = std::regex_replace(str, defineAttrs, "$1{");

  // Inline local variable uses
  std::regex varMdUse(", !([0-9]+)");
  for (std::sregex_iterator i(str.begin(), str.end(), varMdUse), e; i != e;
       ++i) {
    const auto &match = *i;
    std::string slotStr = match[1].str();
    const char *slotChars = slotStr.c_str();
    char *slotCharsEnd;
    unsigned slot = std::strtoul(slotChars, &slotCharsEnd, 10);
    const auto &mdNodeIter = slots.MetadataNodes.find(slot);
    if (mdNodeIter == slots.MetadataNodes.end())
      continue;
    const auto &mdNode = mdNodeIter->second;
    if (mdNode->getMetadataID() != Metadata::DILocalVariableKind)
      continue;
    if (const auto *variable = cast<DILocalVariable>(mdNode)) {
      std::string varInline;
      raw_string_ostream varInlineOut(varInline);
      varInlineOut << ", \"" << variable->getName() << "\"";
      varInlineOut << " l" << variable->getLine();
      varInlineOut.flush();
      str.replace(match.position(), match.length(), varInline);
    }
  }

  // Inline location attachments
  std::regex locMdAtt(", !dbg !([0-9]+)");
  for (std::sregex_iterator i(str.begin(), str.end(), locMdAtt), e; i != e;
       ++i) {
    const auto &match = *i;
    std::string slotStr = match[1].str();
    const char *slotChars = slotStr.c_str();
    char *slotCharsEnd;
    unsigned slot = std::strtoul(slotChars, &slotCharsEnd, 10);
    const auto &mdNodeIter = slots.MetadataNodes.find(slot);
    if (mdNodeIter == slots.MetadataNodes.end())
      continue;
    const auto &mdNode = mdNodeIter->second;
    if (mdNode->getMetadataID() != Metadata::DILocationKind)
      continue;
    if (const auto *location = cast<DILocation>(mdNode)) {
      std::string locInline;
      raw_string_ostream locInlineOut(locInline);
      auto line = location->getLine();
      locInlineOut << ", l" << line;
      if (line)
        locInlineOut << " c" << location->getColumn();
      locInlineOut.flush();
      str.replace(match.position(), match.length(), locInline);
    }
  }

  return str;
}

} // namespace klee
