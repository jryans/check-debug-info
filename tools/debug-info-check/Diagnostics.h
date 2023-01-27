// Adapted from clang/include/clang/Tooling/DiagnosticsYaml.h
// This version flattens imports and simplifies structs

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/YAMLTraits.h"

#include <string>

namespace clang {
namespace tooling {

/// A text replacement.
///
/// Represents a SourceManager independent replacement of a range of text in a
/// specific file.
struct Replacement {
  std::string filePath;
  unsigned offset = 0;
  unsigned length = 0;
  std::string replacementText;
};

/// Represents a range within a specific source file.
struct FileByteRange {
  std::string filePath;
  unsigned fileOffset;
  unsigned length;
};

/// Represents the diagnostic message with the error message associated
/// and the information on the location of the problem.
struct DiagnosticMessage {
  std::string message;
  std::string filePath;
  unsigned fileOffset;

  /// Fixes for this diagnostic.
  llvm::SmallVector<Replacement, 1> fixes;

  /// Extra source ranges associated with the note, in addition to the location
  /// of the Message itself.
  llvm::SmallVector<FileByteRange, 1> ranges;
};

/// Represents the diagnostic with the level of severity and possible
/// fixes to be applied.
struct Diagnostic {
  enum Level { Remark, Warning, Error };

  /// Name identifying the Diagnostic.
  std::string name;

  /// Message associated to the diagnostic.
  DiagnosticMessage message;

  /// Potential notes about the diagnostic.
  llvm::SmallVector<DiagnosticMessage, 1> notes;

  /// Diagnostic level. Can indicate either an error or a warning.
  Level level;

  /// A build directory of the diagnostic source file.
  ///
  /// It's an absolute path which is `directory` field of the source file in
  /// compilation database. If users don't specify the compilation database
  /// directory, it is the current directory where clang-tidy runs.
  ///
  /// Note: it is empty in unittest.
  std::string buildDirectory;
};

/// Collection of Diagnostics generated from a single translation unit.
struct TranslationUnitDiagnostics {
  /// Name of the main source for the translation unit.
  std::string mainSourceFile;
  std::vector<Diagnostic> diagnostics;
};

} // namespace tooling
} // namespace clang

LLVM_YAML_IS_SEQUENCE_VECTOR(clang::tooling::Replacement)
LLVM_YAML_IS_SEQUENCE_VECTOR(clang::tooling::Diagnostic)
LLVM_YAML_IS_SEQUENCE_VECTOR(clang::tooling::DiagnosticMessage)
LLVM_YAML_IS_SEQUENCE_VECTOR(clang::tooling::FileByteRange)

namespace llvm {
namespace yaml {

template <> struct MappingTraits<clang::tooling::Replacement> {
  static void mapping(IO &io, clang::tooling::Replacement &r) {
    io.mapRequired("FilePath", r.filePath);
    io.mapRequired("Offset", r.offset);
    io.mapRequired("Length", r.length);
    io.mapRequired("ReplacementText", r.replacementText);
  }
};

template <> struct MappingTraits<clang::tooling::FileByteRange> {
  static void mapping(IO &io, clang::tooling::FileByteRange &r) {
    io.mapRequired("FilePath", r.filePath);
    io.mapRequired("FileOffset", r.fileOffset);
    io.mapRequired("Length", r.length);
  }
};

template <> struct MappingTraits<clang::tooling::DiagnosticMessage> {
  static void mapping(IO &io, clang::tooling::DiagnosticMessage &m) {
    io.mapRequired("Message", m.message);
    io.mapOptional("FilePath", m.filePath);
    io.mapOptional("FileOffset", m.fileOffset);
    io.mapRequired("Replacements", m.fixes);
    io.mapOptional("Ranges", m.ranges);
  }
};

template <> struct MappingTraits<clang::tooling::Diagnostic> {
  static void mapping(IO &io, clang::tooling::Diagnostic &d) {
    io.mapRequired("DiagnosticName", d.name);
    io.mapRequired("DiagnosticMessage", d.message);
    io.mapOptional("Notes", d.notes);
    io.mapOptional("Level", d.level);
    io.mapOptional("BuildDirectory", d.buildDirectory);
  }
};

template <> struct MappingTraits<clang::tooling::TranslationUnitDiagnostics> {
  static void mapping(IO &io, clang::tooling::TranslationUnitDiagnostics &doc) {
    io.mapRequired("MainSourceFile", doc.mainSourceFile);
    io.mapRequired("Diagnostics", doc.diagnostics);
  }
};

template <> struct ScalarEnumerationTraits<clang::tooling::Diagnostic::Level> {
  static void enumeration(IO &io, clang::tooling::Diagnostic::Level &value) {
    io.enumCase(value, "Warning", clang::tooling::Diagnostic::Warning);
    io.enumCase(value, "Error", clang::tooling::Diagnostic::Error);
    io.enumCase(value, "Remark", clang::tooling::Diagnostic::Remark);
  }
};

} // end namespace yaml
} // end namespace llvm

#endif // DIAGNOSTICS_H
