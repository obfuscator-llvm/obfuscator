//===--- PrecompiledPreamble.h - Build precompiled preambles ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Helper class to build precompiled preamble.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_FRONTEND_PRECOMPILED_PREAMBLE_H
#define LLVM_CLANG_FRONTEND_PRECOMPILED_PREAMBLE_H

#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/MD5.h"
#include <memory>
#include <system_error>
#include <type_traits>

namespace llvm {
class MemoryBuffer;
}

namespace clang {
namespace vfs {
class FileSystem;
}

class CompilerInstance;
class CompilerInvocation;
class DeclGroupRef;
class PCHContainerOperations;

/// A size of the preamble and a flag required by
/// PreprocessorOptions::PrecompiledPreambleBytes.
struct PreambleBounds {
  PreambleBounds(unsigned Size, bool PreambleEndsAtStartOfLine)
      : Size(Size), PreambleEndsAtStartOfLine(PreambleEndsAtStartOfLine) {}

  /// \brief Size of the preamble in bytes.
  unsigned Size;
  /// \brief Whether the preamble ends at the start of a new line.
  ///
  /// Used to inform the lexer as to whether it's starting at the beginning of
  /// a line after skipping the preamble.
  bool PreambleEndsAtStartOfLine;
};

/// \brief Runs lexer to compute suggested preamble bounds.
PreambleBounds ComputePreambleBounds(const LangOptions &LangOpts,
                                     llvm::MemoryBuffer *Buffer,
                                     unsigned MaxLines);

class PreambleCallbacks;

/// A class holding a PCH and all information to check whether it is valid to
/// reuse the PCH for the subsequent runs. Use BuildPreamble to create PCH and
/// CanReusePreamble + AddImplicitPreamble to make use of it.
class PrecompiledPreamble {
  class TempPCHFile;
  struct PreambleFileHash;

public:
  /// \brief Try to build PrecompiledPreamble for \p Invocation. See
  /// BuildPreambleError for possible error codes.
  ///
  /// \param Invocation Original CompilerInvocation with options to compile the
  /// file.
  ///
  /// \param MainFileBuffer Buffer with the contents of the main file.
  ///
  /// \param Bounds Bounds of the preamble, result of calling
  /// ComputePreambleBounds.
  ///
  /// \param Diagnostics Diagnostics engine to be used while building the
  /// preamble.
  ///
  /// \param VFS An instance of vfs::FileSystem to be used for file
  /// accesses.
  ///
  /// \param PCHContainerOps An instance of PCHContainerOperations.
  ///
  /// \param Callbacks A set of callbacks to be executed when building
  /// the preamble.
  static llvm::ErrorOr<PrecompiledPreamble>
  Build(const CompilerInvocation &Invocation,
        const llvm::MemoryBuffer *MainFileBuffer, PreambleBounds Bounds,
        DiagnosticsEngine &Diagnostics, IntrusiveRefCntPtr<vfs::FileSystem> VFS,
        std::shared_ptr<PCHContainerOperations> PCHContainerOps,
        PreambleCallbacks &Callbacks);

  PrecompiledPreamble(PrecompiledPreamble &&) = default;
  PrecompiledPreamble &operator=(PrecompiledPreamble &&) = default;

  /// PreambleBounds used to build the preamble
  PreambleBounds getBounds() const;

  /// Check whether PrecompiledPreamble can be reused for the new contents(\p
  /// MainFileBuffer) of the main file.
  bool CanReuse(const CompilerInvocation &Invocation,
                const llvm::MemoryBuffer *MainFileBuffer, PreambleBounds Bounds,
                vfs::FileSystem *VFS) const;

  /// Changes options inside \p CI to use PCH from this preamble. Also remaps
  /// main file to \p MainFileBuffer.
  void AddImplicitPreamble(CompilerInvocation &CI,
                           llvm::MemoryBuffer *MainFileBuffer) const;

private:
  PrecompiledPreamble(TempPCHFile PCHFile, std::vector<char> PreambleBytes,
                      bool PreambleEndsAtStartOfLine,
                      llvm::StringMap<PreambleFileHash> FilesInPreamble);

  /// A temp file that would be deleted on destructor call. If destructor is not
  /// called for any reason, the file will be deleted at static objects'
  /// destruction.
  /// An assertion will fire if two TempPCHFiles are created with the same name,
  /// so it's not intended to be used outside preamble-handling.
  class TempPCHFile {
  public:
    // A main method used to construct TempPCHFile.
    static llvm::ErrorOr<TempPCHFile> CreateNewPreamblePCHFile();

    /// Call llvm::sys::fs::createTemporaryFile to create a new temporary file.
    static llvm::ErrorOr<TempPCHFile> createInSystemTempDir(const Twine &Prefix,
                                                            StringRef Suffix);
    /// Create a new instance of TemporaryFile for file at \p Path. Use with
    /// extreme caution, there's an assertion checking that there's only a
    /// single instance of TempPCHFile alive for each path.
    static llvm::ErrorOr<TempPCHFile> createFromCustomPath(const Twine &Path);

  private:
    TempPCHFile(std::string FilePath);

  public:
    TempPCHFile(TempPCHFile &&Other);
    TempPCHFile &operator=(TempPCHFile &&Other);

    TempPCHFile(const TempPCHFile &) = delete;
    ~TempPCHFile();

    /// A path where temporary file is stored.
    llvm::StringRef getFilePath() const;

  private:
    void RemoveFileIfPresent();

  private:
    llvm::Optional<std::string> FilePath;
  };

  /// Data used to determine if a file used in the preamble has been changed.
  struct PreambleFileHash {
    /// All files have size set.
    off_t Size = 0;

    /// Modification time is set for files that are on disk.  For memory
    /// buffers it is zero.
    time_t ModTime = 0;

    /// Memory buffers have MD5 instead of modification time.  We don't
    /// compute MD5 for on-disk files because we hope that modification time is
    /// enough to tell if the file was changed.
    llvm::MD5::MD5Result MD5 = {};

    static PreambleFileHash createForFile(off_t Size, time_t ModTime);
    static PreambleFileHash
    createForMemoryBuffer(const llvm::MemoryBuffer *Buffer);

    friend bool operator==(const PreambleFileHash &LHS,
                           const PreambleFileHash &RHS) {
      return LHS.Size == RHS.Size && LHS.ModTime == RHS.ModTime &&
             LHS.MD5 == RHS.MD5;
    }
    friend bool operator!=(const PreambleFileHash &LHS,
                           const PreambleFileHash &RHS) {
      return !(LHS == RHS);
    }
  };

  /// Manages the lifetime of temporary file that stores a PCH.
  TempPCHFile PCHFile;
  /// Keeps track of the files that were used when computing the
  /// preamble, with both their buffer size and their modification time.
  ///
  /// If any of the files have changed from one compile to the next,
  /// the preamble must be thrown away.
  llvm::StringMap<PreambleFileHash> FilesInPreamble;
  /// The contents of the file that was used to precompile the preamble. Only
  /// contains first PreambleBounds::Size bytes. Used to compare if the relevant
  /// part of the file has not changed, so that preamble can be reused.
  std::vector<char> PreambleBytes;
  /// See PreambleBounds::PreambleEndsAtStartOfLine
  bool PreambleEndsAtStartOfLine;
};

/// A set of callbacks to gather useful information while building a preamble.
class PreambleCallbacks {
public:
  virtual ~PreambleCallbacks() = default;

  /// Called after FrontendAction::Execute(), but before
  /// FrontendAction::EndSourceFile(). Can be used to transfer ownership of
  /// various CompilerInstance fields before they are destroyed.
  virtual void AfterExecute(CompilerInstance &CI);
  /// Called after PCH has been emitted. \p Writer may be used to retrieve
  /// information about AST, serialized in PCH.
  virtual void AfterPCHEmitted(ASTWriter &Writer);
  /// Called for each TopLevelDecl.
  /// NOTE: To allow more flexibility a custom ASTConsumer could probably be
  /// used instead, but having only this method allows a simpler API.
  virtual void HandleTopLevelDecl(DeclGroupRef DG);
  /// Called for each macro defined in the Preamble.
  /// NOTE: To allow more flexibility a custom PPCallbacks could probably be
  /// used instead, but having only this method allows a simpler API.
  virtual void HandleMacroDefined(const Token &MacroNameTok,
                                  const MacroDirective *MD);
};

enum class BuildPreambleError {
  PreambleIsEmpty = 1,
  CouldntCreateTempFile,
  CouldntCreateTargetInfo,
  CouldntCreateVFSOverlay,
  BeginSourceFileFailed,
  CouldntEmitPCH
};

class BuildPreambleErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override;
  std::string message(int condition) const override;
};

std::error_code make_error_code(BuildPreambleError Error);
} // namespace clang

namespace std {
template <>
struct is_error_code_enum<clang::BuildPreambleError> : std::true_type {};
} // namespace std

#endif
