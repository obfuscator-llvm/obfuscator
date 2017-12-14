//===--- ModuleMap.h - Describe the layout of modules -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the ModuleMap interface, which describes the layout of a
// module as it relates to headers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEX_MODULEMAP_H
#define LLVM_CLANG_LEX_MODULEMAP_H

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace clang {

class DirectoryEntry;
class FileEntry;
class FileManager;
class DiagnosticConsumer;
class DiagnosticsEngine;
class HeaderSearch;
class ModuleMapParser;

/// \brief A mechanism to observe the actions of the module map parser as it
/// reads module map files.
class ModuleMapCallbacks {
public:
  virtual ~ModuleMapCallbacks() = default;

  /// \brief Called when a module map file has been read.
  ///
  /// \param FileStart A SourceLocation referring to the start of the file's
  /// contents.
  /// \param File The file itself.
  /// \param IsSystem Whether this is a module map from a system include path.
  virtual void moduleMapFileRead(SourceLocation FileStart,
                                 const FileEntry &File, bool IsSystem) {}

  /// \brief Called when a header is added during module map parsing.
  ///
  /// \param Filename The header file itself.
  virtual void moduleMapAddHeader(StringRef Filename) {}

  /// \brief Called when an umbrella header is added during module map parsing.
  ///
  /// \param FileMgr FileManager instance
  /// \param Header The umbrella header to collect.
  virtual void moduleMapAddUmbrellaHeader(FileManager *FileMgr,
                                          const FileEntry *Header) {}
};
  
class ModuleMap {
  SourceManager &SourceMgr;
  DiagnosticsEngine &Diags;
  const LangOptions &LangOpts;
  const TargetInfo *Target;
  HeaderSearch &HeaderInfo;

  llvm::SmallVector<std::unique_ptr<ModuleMapCallbacks>, 1> Callbacks;
  
  /// \brief The directory used for Clang-supplied, builtin include headers,
  /// such as "stdint.h".
  const DirectoryEntry *BuiltinIncludeDir;
  
  /// \brief Language options used to parse the module map itself.
  ///
  /// These are always simple C language options.
  LangOptions MMapLangOpts;

  // The module that the main source file is associated with (the module
  // named LangOpts::CurrentModule, if we've loaded it).
  Module *SourceModule;

  /// \brief The top-level modules that are known.
  llvm::StringMap<Module *> Modules;

  /// \brief The number of modules we have created in total.
  unsigned NumCreatedModules;

public:
  /// \brief Flags describing the role of a module header.
  enum ModuleHeaderRole {
    /// \brief This header is normally included in the module.
    NormalHeader  = 0x0,
    /// \brief This header is included but private.
    PrivateHeader = 0x1,
    /// \brief This header is part of the module (for layering purposes) but
    /// should be textually included.
    TextualHeader = 0x2,
    // Caution: Adding an enumerator needs other changes.
    // Adjust the number of bits for KnownHeader::Storage.
    // Adjust the bitfield HeaderFileInfo::HeaderRole size.
    // Adjust the HeaderFileInfoTrait::ReadData streaming.
    // Adjust the HeaderFileInfoTrait::EmitData streaming.
    // Adjust ModuleMap::addHeader.
  };

  /// Convert a header kind to a role. Requires Kind to not be HK_Excluded.
  static ModuleHeaderRole headerKindToRole(Module::HeaderKind Kind);
  /// Convert a header role to a kind.
  static Module::HeaderKind headerRoleToKind(ModuleHeaderRole Role);

  /// \brief A header that is known to reside within a given module,
  /// whether it was included or excluded.
  class KnownHeader {
    llvm::PointerIntPair<Module *, 2, ModuleHeaderRole> Storage;

  public:
    KnownHeader() : Storage(nullptr, NormalHeader) { }
    KnownHeader(Module *M, ModuleHeaderRole Role) : Storage(M, Role) { }

    friend bool operator==(const KnownHeader &A, const KnownHeader &B) {
      return A.Storage == B.Storage;
    }
    friend bool operator!=(const KnownHeader &A, const KnownHeader &B) {
      return A.Storage != B.Storage;
    }

    /// \brief Retrieve the module the header is stored in.
    Module *getModule() const { return Storage.getPointer(); }

    /// \brief The role of this header within the module.
    ModuleHeaderRole getRole() const { return Storage.getInt(); }

    /// \brief Whether this header is available in the module.
    bool isAvailable() const {
      return getModule()->isAvailable();
    }

    /// \brief Whether this header is accessible from the specified module.
    bool isAccessibleFrom(Module *M) const {
      return !(getRole() & PrivateHeader) ||
             (M && M->getTopLevelModule() == getModule()->getTopLevelModule());
    }

    // \brief Whether this known header is valid (i.e., it has an
    // associated module).
    explicit operator bool() const {
      return Storage.getPointer() != nullptr;
    }
  };

  typedef llvm::SmallPtrSet<const FileEntry *, 1> AdditionalModMapsSet;

private:
  typedef llvm::DenseMap<const FileEntry *, SmallVector<KnownHeader, 1>>
  HeadersMap;

  /// \brief Mapping from each header to the module that owns the contents of
  /// that header.
  HeadersMap Headers;

  /// Map from file sizes to modules with lazy header directives of that size.
  mutable llvm::DenseMap<off_t, llvm::TinyPtrVector<Module*>> LazyHeadersBySize;
  /// Map from mtimes to modules with lazy header directives with those mtimes.
  mutable llvm::DenseMap<time_t, llvm::TinyPtrVector<Module*>>
              LazyHeadersByModTime;

  /// \brief Mapping from directories with umbrella headers to the module
  /// that is generated from the umbrella header.
  ///
  /// This mapping is used to map headers that haven't explicitly been named
  /// in the module map over to the module that includes them via its umbrella
  /// header.
  llvm::DenseMap<const DirectoryEntry *, Module *> UmbrellaDirs;

  /// \brief The set of attributes that can be attached to a module.
  struct Attributes {
    Attributes()
        : IsSystem(), IsExternC(), IsExhaustive(), NoUndeclaredIncludes() {}

    /// \brief Whether this is a system module.
    unsigned IsSystem : 1;

    /// \brief Whether this is an extern "C" module.
    unsigned IsExternC : 1;

    /// \brief Whether this is an exhaustive set of configuration macros.
    unsigned IsExhaustive : 1;

    /// \brief Whether files in this module can only include non-modular headers
    /// and headers from used modules.
    unsigned NoUndeclaredIncludes : 1;
  };

  /// \brief A directory for which framework modules can be inferred.
  struct InferredDirectory {
    InferredDirectory() : InferModules() {}

    /// \brief Whether to infer modules from this directory.
    unsigned InferModules : 1;

    /// \brief The attributes to use for inferred modules.
    Attributes Attrs;

    /// \brief If \c InferModules is non-zero, the module map file that allowed
    /// inferred modules.  Otherwise, nullptr.
    const FileEntry *ModuleMapFile;

    /// \brief The names of modules that cannot be inferred within this
    /// directory.
    SmallVector<std::string, 2> ExcludedModules;
  };

  /// \brief A mapping from directories to information about inferring
  /// framework modules from within those directories.
  llvm::DenseMap<const DirectoryEntry *, InferredDirectory> InferredDirectories;

  /// A mapping from an inferred module to the module map that allowed the
  /// inference.
  llvm::DenseMap<const Module *, const FileEntry *> InferredModuleAllowedBy;

  llvm::DenseMap<const Module *, AdditionalModMapsSet> AdditionalModMaps;

  /// \brief Describes whether we haved parsed a particular file as a module
  /// map.
  llvm::DenseMap<const FileEntry *, bool> ParsedModuleMap;

  friend class ModuleMapParser;
  
  /// \brief Resolve the given export declaration into an actual export
  /// declaration.
  ///
  /// \param Mod The module in which we're resolving the export declaration.
  ///
  /// \param Unresolved The export declaration to resolve.
  ///
  /// \param Complain Whether this routine should complain about unresolvable
  /// exports.
  ///
  /// \returns The resolved export declaration, which will have a NULL pointer
  /// if the export could not be resolved.
  Module::ExportDecl 
  resolveExport(Module *Mod, const Module::UnresolvedExportDecl &Unresolved,
                bool Complain) const;

  /// \brief Resolve the given module id to an actual module.
  ///
  /// \param Id The module-id to resolve.
  ///
  /// \param Mod The module in which we're resolving the module-id.
  ///
  /// \param Complain Whether this routine should complain about unresolvable
  /// module-ids.
  ///
  /// \returns The resolved module, or null if the module-id could not be
  /// resolved.
  Module *resolveModuleId(const ModuleId &Id, Module *Mod, bool Complain) const;

  /// Add an unresolved header to a module.
  void addUnresolvedHeader(Module *Mod,
                           Module::UnresolvedHeaderDirective Header);

  /// Look up the given header directive to find an actual header file.
  ///
  /// \param M The module in which we're resolving the header directive.
  /// \param Header The header directive to resolve.
  /// \param RelativePathName Filled in with the relative path name from the
  ///        module to the resolved header.
  /// \return The resolved file, if any.
  const FileEntry *findHeader(Module *M,
                              const Module::UnresolvedHeaderDirective &Header,
                              SmallVectorImpl<char> &RelativePathName);

  /// Resolve the given header directive.
  void resolveHeader(Module *M,
                     const Module::UnresolvedHeaderDirective &Header);

  /// Attempt to resolve the specified header directive as naming a builtin
  /// header.
  /// \return \c true if a corresponding builtin header was found.
  bool resolveAsBuiltinHeader(Module *M,
                              const Module::UnresolvedHeaderDirective &Header);

  /// \brief Looks up the modules that \p File corresponds to.
  ///
  /// If \p File represents a builtin header within Clang's builtin include
  /// directory, this also loads all of the module maps to see if it will get
  /// associated with a specific module (e.g. in /usr/include).
  HeadersMap::iterator findKnownHeader(const FileEntry *File);

  /// \brief Searches for a module whose umbrella directory contains \p File.
  ///
  /// \param File The header to search for.
  ///
  /// \param IntermediateDirs On success, contains the set of directories
  /// searched before finding \p File.
  KnownHeader findHeaderInUmbrellaDirs(const FileEntry *File,
                    SmallVectorImpl<const DirectoryEntry *> &IntermediateDirs);

  /// \brief Given that \p File is not in the Headers map, look it up within
  /// umbrella directories and find or create a module for it.
  KnownHeader findOrCreateModuleForHeaderInUmbrellaDir(const FileEntry *File);

  /// \brief A convenience method to determine if \p File is (possibly nested)
  /// in an umbrella directory.
  bool isHeaderInUmbrellaDirs(const FileEntry *File) {
    SmallVector<const DirectoryEntry *, 2> IntermediateDirs;
    return static_cast<bool>(findHeaderInUmbrellaDirs(File, IntermediateDirs));
  }

  Module *inferFrameworkModule(const DirectoryEntry *FrameworkDir,
                               Attributes Attrs, Module *Parent);

public:
  /// \brief Construct a new module map.
  ///
  /// \param SourceMgr The source manager used to find module files and headers.
  /// This source manager should be shared with the header-search mechanism,
  /// since they will refer to the same headers.
  ///
  /// \param Diags A diagnostic engine used for diagnostics.
  ///
  /// \param LangOpts Language options for this translation unit.
  ///
  /// \param Target The target for this translation unit.
  ModuleMap(SourceManager &SourceMgr, DiagnosticsEngine &Diags,
            const LangOptions &LangOpts, const TargetInfo *Target,
            HeaderSearch &HeaderInfo);

  /// \brief Destroy the module map.
  ///
  ~ModuleMap();

  /// \brief Set the target information.
  void setTarget(const TargetInfo &Target);

  /// \brief Set the directory that contains Clang-supplied include
  /// files, such as our stdarg.h or tgmath.h.
  void setBuiltinIncludeDir(const DirectoryEntry *Dir) {
    BuiltinIncludeDir = Dir;
  }

  /// \brief Get the directory that contains Clang-supplied include files.
  const DirectoryEntry *getBuiltinDir() const {
    return BuiltinIncludeDir;
  }

  /// \brief Is this a compiler builtin header?
  static bool isBuiltinHeader(StringRef FileName);

  /// \brief Add a module map callback.
  void addModuleMapCallbacks(std::unique_ptr<ModuleMapCallbacks> Callback) {
    Callbacks.push_back(std::move(Callback));
  }

  /// \brief Retrieve the module that owns the given header file, if any.
  ///
  /// \param File The header file that is likely to be included.
  ///
  /// \param AllowTextual If \c true and \p File is a textual header, return
  /// its owning module. Otherwise, no KnownHeader will be returned if the
  /// file is only known as a textual header.
  ///
  /// \returns The module KnownHeader, which provides the module that owns the
  /// given header file.  The KnownHeader is default constructed to indicate
  /// that no module owns this header file.
  KnownHeader findModuleForHeader(const FileEntry *File,
                                  bool AllowTextual = false);

  /// \brief Retrieve all the modules that contain the given header file. This
  /// may not include umbrella modules, nor information from external sources,
  /// if they have not yet been inferred / loaded.
  ///
  /// Typically, \ref findModuleForHeader should be used instead, as it picks
  /// the preferred module for the header.
  ArrayRef<KnownHeader> findAllModulesForHeader(const FileEntry *File) const;

  /// Resolve all lazy header directives for the specified file.
  ///
  /// This ensures that the HeaderFileInfo on HeaderSearch is up to date. This
  /// is effectively internal, but is exposed so HeaderSearch can call it.
  void resolveHeaderDirectives(const FileEntry *File) const;

  /// Resolve all lazy header directives for the specified module.
  void resolveHeaderDirectives(Module *Mod) const;

  /// \brief Reports errors if a module must not include a specific file.
  ///
  /// \param RequestingModule The module including a file.
  ///
  /// \param RequestingModuleIsModuleInterface \c true if the inclusion is in
  ///        the interface of RequestingModule, \c false if it's in the
  ///        implementation of RequestingModule. Value is ignored and
  ///        meaningless if RequestingModule is nullptr.
  ///
  /// \param FilenameLoc The location of the inclusion's filename.
  ///
  /// \param Filename The included filename as written.
  ///
  /// \param File The included file.
  void diagnoseHeaderInclusion(Module *RequestingModule,
                               bool RequestingModuleIsModuleInterface,
                               SourceLocation FilenameLoc, StringRef Filename,
                               const FileEntry *File);

  /// \brief Determine whether the given header is part of a module
  /// marked 'unavailable'.
  bool isHeaderInUnavailableModule(const FileEntry *Header) const;

  /// \brief Determine whether the given header is unavailable as part
  /// of the specified module.
  bool isHeaderUnavailableInModule(const FileEntry *Header,
                                   const Module *RequestingModule) const;

  /// \brief Retrieve a module with the given name.
  ///
  /// \param Name The name of the module to look up.
  ///
  /// \returns The named module, if known; otherwise, returns null.
  Module *findModule(StringRef Name) const;

  /// \brief Retrieve a module with the given name using lexical name lookup,
  /// starting at the given context.
  ///
  /// \param Name The name of the module to look up.
  ///
  /// \param Context The module context, from which we will perform lexical
  /// name lookup.
  ///
  /// \returns The named module, if known; otherwise, returns null.
  Module *lookupModuleUnqualified(StringRef Name, Module *Context) const;

  /// \brief Retrieve a module with the given name within the given context,
  /// using direct (qualified) name lookup.
  ///
  /// \param Name The name of the module to look up.
  /// 
  /// \param Context The module for which we will look for a submodule. If
  /// null, we will look for a top-level module.
  ///
  /// \returns The named submodule, if known; otherwose, returns null.
  Module *lookupModuleQualified(StringRef Name, Module *Context) const;
  
  /// \brief Find a new module or submodule, or create it if it does not already
  /// exist.
  ///
  /// \param Name The name of the module to find or create.
  ///
  /// \param Parent The module that will act as the parent of this submodule,
  /// or NULL to indicate that this is a top-level module.
  ///
  /// \param IsFramework Whether this is a framework module.
  ///
  /// \param IsExplicit Whether this is an explicit submodule.
  ///
  /// \returns The found or newly-created module, along with a boolean value
  /// that will be true if the module is newly-created.
  std::pair<Module *, bool> findOrCreateModule(StringRef Name, Module *Parent,
                                               bool IsFramework,
                                               bool IsExplicit);

  /// \brief Create a new module for a C++ Modules TS module interface unit.
  /// The module must not already exist, and will be configured for the current
  /// compilation.
  ///
  /// Note that this also sets the current module to the newly-created module.
  ///
  /// \returns The newly-created module.
  Module *createModuleForInterfaceUnit(SourceLocation Loc, StringRef Name);

  /// \brief Infer the contents of a framework module map from the given
  /// framework directory.
  Module *inferFrameworkModule(const DirectoryEntry *FrameworkDir,
                               bool IsSystem, Module *Parent);

  /// \brief Retrieve the module map file containing the definition of the given
  /// module.
  ///
  /// \param Module The module whose module map file will be returned, if known.
  ///
  /// \returns The file entry for the module map file containing the given
  /// module, or NULL if the module definition was inferred.
  const FileEntry *getContainingModuleMapFile(const Module *Module) const;

  /// \brief Get the module map file that (along with the module name) uniquely
  /// identifies this module.
  ///
  /// The particular module that \c Name refers to may depend on how the module
  /// was found in header search. However, the combination of \c Name and
  /// this module map will be globally unique for top-level modules. In the case
  /// of inferred modules, returns the module map that allowed the inference
  /// (e.g. contained 'module *'). Otherwise, returns
  /// getContainingModuleMapFile().
  const FileEntry *getModuleMapFileForUniquing(const Module *M) const;

  void setInferredModuleAllowedBy(Module *M, const FileEntry *ModuleMap);

  /// \brief Get any module map files other than getModuleMapFileForUniquing(M)
  /// that define submodules of a top-level module \p M. This is cheaper than
  /// getting the module map file for each submodule individually, since the
  /// expected number of results is very small.
  AdditionalModMapsSet *getAdditionalModuleMapFiles(const Module *M) {
    auto I = AdditionalModMaps.find(M);
    if (I == AdditionalModMaps.end())
      return nullptr;
    return &I->second;
  }

  void addAdditionalModuleMapFile(const Module *M, const FileEntry *ModuleMap) {
    AdditionalModMaps[M].insert(ModuleMap);
  }

  /// \brief Resolve all of the unresolved exports in the given module.
  ///
  /// \param Mod The module whose exports should be resolved.
  ///
  /// \param Complain Whether to emit diagnostics for failures.
  ///
  /// \returns true if any errors were encountered while resolving exports,
  /// false otherwise.
  bool resolveExports(Module *Mod, bool Complain);

  /// \brief Resolve all of the unresolved uses in the given module.
  ///
  /// \param Mod The module whose uses should be resolved.
  ///
  /// \param Complain Whether to emit diagnostics for failures.
  ///
  /// \returns true if any errors were encountered while resolving uses,
  /// false otherwise.
  bool resolveUses(Module *Mod, bool Complain);

  /// \brief Resolve all of the unresolved conflicts in the given module.
  ///
  /// \param Mod The module whose conflicts should be resolved.
  ///
  /// \param Complain Whether to emit diagnostics for failures.
  ///
  /// \returns true if any errors were encountered while resolving conflicts,
  /// false otherwise.
  bool resolveConflicts(Module *Mod, bool Complain);

  /// \brief Sets the umbrella header of the given module to the given
  /// header.
  void setUmbrellaHeader(Module *Mod, const FileEntry *UmbrellaHeader,
                         Twine NameAsWritten);

  /// \brief Sets the umbrella directory of the given module to the given
  /// directory.
  void setUmbrellaDir(Module *Mod, const DirectoryEntry *UmbrellaDir,
                      Twine NameAsWritten);

  /// \brief Adds this header to the given module.
  /// \param Role The role of the header wrt the module.
  void addHeader(Module *Mod, Module::Header Header,
                 ModuleHeaderRole Role, bool Imported = false);

  /// \brief Marks this header as being excluded from the given module.
  void excludeHeader(Module *Mod, Module::Header Header);

  /// \brief Parse the given module map file, and record any modules we 
  /// encounter.
  ///
  /// \param File The file to be parsed.
  ///
  /// \param IsSystem Whether this module map file is in a system header
  /// directory, and therefore should be considered a system module.
  ///
  /// \param HomeDir The directory in which relative paths within this module
  ///        map file will be resolved.
  ///
  /// \param ID The FileID of the file to process, if we've already entered it.
  ///
  /// \param Offset [inout] On input the offset at which to start parsing. On
  ///        output, the offset at which the module map terminated.
  ///
  /// \param ExternModuleLoc The location of the "extern module" declaration
  ///        that caused us to load this module map file, if any.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool parseModuleMapFile(const FileEntry *File, bool IsSystem,
                          const DirectoryEntry *HomeDir, FileID ID = FileID(),
                          unsigned *Offset = nullptr,
                          SourceLocation ExternModuleLoc = SourceLocation());

  /// \brief Dump the contents of the module map, for debugging purposes.
  void dump();
  
  typedef llvm::StringMap<Module *>::const_iterator module_iterator;
  module_iterator module_begin() const { return Modules.begin(); }
  module_iterator module_end()   const { return Modules.end(); }
};
  
} // end namespace clang

#endif // LLVM_CLANG_LEX_MODULEMAP_H
