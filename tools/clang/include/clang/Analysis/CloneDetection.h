//===--- CloneDetection.h - Finds code clones in an AST ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// /file
/// This file defines classes for searching and anlyzing source code clones.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_CLONEDETECTION_H
#define LLVM_CLANG_AST_CLONEDETECTION_H

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Regex.h"
#include <vector>

namespace clang {

class Stmt;
class Decl;
class VarDecl;
class ASTContext;
class CompoundStmt;

namespace clone_detection {

/// Returns a string that represents all macro expansions that expanded into the
/// given SourceLocation.
///
/// If 'getMacroStack(A) == getMacroStack(B)' is true, then the SourceLocations
/// A and B are expanded from the same macros in the same order.
std::string getMacroStack(SourceLocation Loc, ASTContext &Context);

/// Collects the data of a single Stmt.
///
/// This class defines what a code clone is: If it collects for two statements
/// the same data, then those two statements are considered to be clones of each
/// other.
///
/// All collected data is forwarded to the given data consumer of the type T.
/// The data consumer class needs to provide a member method with the signature:
///   update(StringRef Str)
template <typename T>
class StmtDataCollector : public ConstStmtVisitor<StmtDataCollector<T>> {

  ASTContext &Context;
  /// The data sink to which all data is forwarded.
  T &DataConsumer;

public:
  /// Collects data of the given Stmt.
  /// \param S The given statement.
  /// \param Context The ASTContext of S.
  /// \param DataConsumer The data sink to which all data is forwarded.
  StmtDataCollector(const Stmt *S, ASTContext &Context, T &DataConsumer)
      : Context(Context), DataConsumer(DataConsumer) {
    this->Visit(S);
  }

  typedef unsigned DataPiece;

  // Below are utility methods for appending different data to the vector.

  void addData(DataPiece Integer) {
    DataConsumer.update(
        StringRef(reinterpret_cast<char *>(&Integer), sizeof(Integer)));
  }

  void addData(llvm::StringRef Str) { DataConsumer.update(Str); }

  void addData(const QualType &QT) { addData(QT.getAsString()); }

// The functions below collect the class specific data of each Stmt subclass.

// Utility macro for defining a visit method for a given class. This method
// calls back to the ConstStmtVisitor to visit all parent classes.
#define DEF_ADD_DATA(CLASS, CODE)                                              \
  void Visit##CLASS(const CLASS *S) {                                          \
    CODE;                                                                      \
    ConstStmtVisitor<StmtDataCollector>::Visit##CLASS(S);                      \
  }

  DEF_ADD_DATA(Stmt, {
    addData(S->getStmtClass());
    // This ensures that macro generated code isn't identical to macro-generated
    // code.
    addData(getMacroStack(S->getLocStart(), Context));
    addData(getMacroStack(S->getLocEnd(), Context));
  })
  DEF_ADD_DATA(Expr, { addData(S->getType()); })

  //--- Builtin functionality ----------------------------------------------//
  DEF_ADD_DATA(ArrayTypeTraitExpr, { addData(S->getTrait()); })
  DEF_ADD_DATA(ExpressionTraitExpr, { addData(S->getTrait()); })
  DEF_ADD_DATA(PredefinedExpr, { addData(S->getIdentType()); })
  DEF_ADD_DATA(TypeTraitExpr, {
    addData(S->getTrait());
    for (unsigned i = 0; i < S->getNumArgs(); ++i)
      addData(S->getArg(i)->getType());
  })

  //--- Calls --------------------------------------------------------------//
  DEF_ADD_DATA(CallExpr, {
    // Function pointers don't have a callee and we just skip hashing it.
    if (const FunctionDecl *D = S->getDirectCallee()) {
      // If the function is a template specialization, we also need to handle
      // the template arguments as they are not included in the qualified name.
      if (auto Args = D->getTemplateSpecializationArgs()) {
        std::string ArgString;

        // Print all template arguments into ArgString
        llvm::raw_string_ostream OS(ArgString);
        for (unsigned i = 0; i < Args->size(); ++i) {
          Args->get(i).print(Context.getLangOpts(), OS);
          // Add a padding character so that 'foo<X, XX>()' != 'foo<XX, X>()'.
          OS << '\n';
        }
        OS.flush();

        addData(ArgString);
      }
      addData(D->getQualifiedNameAsString());
    }
  })

  //--- Exceptions ---------------------------------------------------------//
  DEF_ADD_DATA(CXXCatchStmt, { addData(S->getCaughtType()); })

  //--- C++ OOP Stmts ------------------------------------------------------//
  DEF_ADD_DATA(CXXDeleteExpr, {
    addData(S->isArrayFormAsWritten());
    addData(S->isGlobalDelete());
  })

  //--- Casts --------------------------------------------------------------//
  DEF_ADD_DATA(ObjCBridgedCastExpr, { addData(S->getBridgeKind()); })

  //--- Miscellaneous Exprs ------------------------------------------------//
  DEF_ADD_DATA(BinaryOperator, { addData(S->getOpcode()); })
  DEF_ADD_DATA(UnaryOperator, { addData(S->getOpcode()); })

  //--- Control flow -------------------------------------------------------//
  DEF_ADD_DATA(GotoStmt, { addData(S->getLabel()->getName()); })
  DEF_ADD_DATA(IndirectGotoStmt, {
    if (S->getConstantTarget())
      addData(S->getConstantTarget()->getName());
  })
  DEF_ADD_DATA(LabelStmt, { addData(S->getDecl()->getName()); })
  DEF_ADD_DATA(MSDependentExistsStmt, { addData(S->isIfExists()); })
  DEF_ADD_DATA(AddrLabelExpr, { addData(S->getLabel()->getName()); })

  //--- Objective-C --------------------------------------------------------//
  DEF_ADD_DATA(ObjCIndirectCopyRestoreExpr, { addData(S->shouldCopy()); })
  DEF_ADD_DATA(ObjCPropertyRefExpr, {
    addData(S->isSuperReceiver());
    addData(S->isImplicitProperty());
  })
  DEF_ADD_DATA(ObjCAtCatchStmt, { addData(S->hasEllipsis()); })

  //--- Miscellaneous Stmts ------------------------------------------------//
  DEF_ADD_DATA(CXXFoldExpr, {
    addData(S->isRightFold());
    addData(S->getOperator());
  })
  DEF_ADD_DATA(GenericSelectionExpr, {
    for (unsigned i = 0; i < S->getNumAssocs(); ++i) {
      addData(S->getAssocType(i));
    }
  })
  DEF_ADD_DATA(LambdaExpr, {
    for (const LambdaCapture &C : S->captures()) {
      addData(C.isPackExpansion());
      addData(C.getCaptureKind());
      if (C.capturesVariable())
        addData(C.getCapturedVar()->getType());
    }
    addData(S->isGenericLambda());
    addData(S->isMutable());
  })
  DEF_ADD_DATA(DeclStmt, {
    auto numDecls = std::distance(S->decl_begin(), S->decl_end());
    addData(static_cast<DataPiece>(numDecls));
    for (const Decl *D : S->decls()) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(D)) {
        addData(VD->getType());
      }
    }
  })
  DEF_ADD_DATA(AsmStmt, {
    addData(S->isSimple());
    addData(S->isVolatile());
    addData(S->generateAsmString(Context));
    for (unsigned i = 0; i < S->getNumInputs(); ++i) {
      addData(S->getInputConstraint(i));
    }
    for (unsigned i = 0; i < S->getNumOutputs(); ++i) {
      addData(S->getOutputConstraint(i));
    }
    for (unsigned i = 0; i < S->getNumClobbers(); ++i) {
      addData(S->getClobber(i));
    }
  })
  DEF_ADD_DATA(AttributedStmt, {
    for (const Attr *A : S->getAttrs()) {
      addData(std::string(A->getSpelling()));
    }
  })
};
} // namespace clone_detection

/// Identifies a list of statements.
///
/// Can either identify a single arbitrary Stmt object, a continuous sequence of
/// child statements inside a CompoundStmt or no statements at all.
class StmtSequence {
  /// If this object identifies a sequence of statements inside a CompoundStmt,
  /// S points to this CompoundStmt. If this object only identifies a single
  /// Stmt, then S is a pointer to this Stmt.
  const Stmt *S;

  /// The declaration that contains the statements.
  const Decl *D;

  /// If EndIndex is non-zero, then S is a CompoundStmt and this StmtSequence
  /// instance is representing the CompoundStmt children inside the array
  /// [StartIndex, EndIndex).
  unsigned StartIndex;
  unsigned EndIndex;

public:
  /// Constructs a StmtSequence holding multiple statements.
  ///
  /// The resulting StmtSequence identifies a continuous sequence of statements
  /// in the body of the given CompoundStmt. Which statements of the body should
  /// be identified needs to be specified by providing a start and end index
  /// that describe a non-empty sub-array in the body of the given CompoundStmt.
  ///
  /// \param Stmt A CompoundStmt that contains all statements in its body.
  /// \param D The Decl containing this Stmt.
  /// \param StartIndex The inclusive start index in the children array of
  ///                   \p Stmt
  /// \param EndIndex The exclusive end index in the children array of \p Stmt.
  StmtSequence(const CompoundStmt *Stmt, const Decl *D, unsigned StartIndex,
               unsigned EndIndex);

  /// Constructs a StmtSequence holding a single statement.
  ///
  /// \param Stmt An arbitrary Stmt.
  /// \param D The Decl containing this Stmt.
  StmtSequence(const Stmt *Stmt, const Decl *D);

  /// Constructs an empty StmtSequence.
  StmtSequence();

  typedef const Stmt *const *iterator;

  /// Returns an iterator pointing to the first statement in this sequence.
  iterator begin() const;

  /// Returns an iterator pointing behind the last statement in this sequence.
  iterator end() const;

  /// Returns the first statement in this sequence.
  ///
  /// This method should only be called on a non-empty StmtSequence object.
  const Stmt *front() const {
    assert(!empty());
    return begin()[0];
  }

  /// Returns the last statement in this sequence.
  ///
  /// This method should only be called on a non-empty StmtSequence object.
  const Stmt *back() const {
    assert(!empty());
    return begin()[size() - 1];
  }

  /// Returns the number of statements this object holds.
  unsigned size() const {
    if (holdsSequence())
      return EndIndex - StartIndex;
    if (S == nullptr)
      return 0;
    return 1;
  }

  /// Returns true if and only if this StmtSequence contains no statements.
  bool empty() const { return size() == 0; }

  /// Returns the related ASTContext for the stored Stmts.
  ASTContext &getASTContext() const;

  /// Returns the declaration that contains the stored Stmts.
  const Decl *getContainingDecl() const {
    assert(D);
    return D;
  }

  /// Returns true if this objects holds a list of statements.
  bool holdsSequence() const { return EndIndex != 0; }

  /// Returns the start sourcelocation of the first statement in this sequence.
  ///
  /// This method should only be called on a non-empty StmtSequence object.
  SourceLocation getStartLoc() const;

  /// Returns the end sourcelocation of the last statement in this sequence.
  ///
  /// This method should only be called on a non-empty StmtSequence object.
  SourceLocation getEndLoc() const;

  /// Returns the source range of the whole sequence - from the beginning
  /// of the first statement to the end of the last statement.
  SourceRange getSourceRange() const;

  bool operator==(const StmtSequence &Other) const {
    return std::tie(S, StartIndex, EndIndex) ==
           std::tie(Other.S, Other.StartIndex, Other.EndIndex);
  }

  bool operator!=(const StmtSequence &Other) const {
    return std::tie(S, StartIndex, EndIndex) !=
           std::tie(Other.S, Other.StartIndex, Other.EndIndex);
  }

  /// Returns true if and only if this sequence covers a source range that
  /// contains the source range of the given sequence \p Other.
  ///
  /// This method should only be called on a non-empty StmtSequence object
  /// and passed a non-empty StmtSequence object.
  bool contains(const StmtSequence &Other) const;
};

/// Searches for similar subtrees in the AST.
///
/// First, this class needs several declarations with statement bodies which
/// can be passed via analyzeCodeBody. Afterwards all statements can be
/// searched for clones by calling findClones with a given list of constraints
/// that should specify the wanted properties of the clones.
///
/// The result of findClones can be further constrained with the constrainClones
/// method.
///
/// This class only searches for clones in exectuable source code
/// (e.g. function bodies). Other clones (e.g. cloned comments or declarations)
/// are not supported.
class CloneDetector {

public:
  /// A collection of StmtSequences that share an arbitrary property.
  typedef llvm::SmallVector<StmtSequence, 8> CloneGroup;

  /// Generates and stores search data for all statements in the body of
  /// the given Decl.
  void analyzeCodeBody(const Decl *D);

  /// Constrains the given list of clone groups with the given constraint.
  ///
  /// The constraint is expected to have a method with the signature
  ///     `void constrain(std::vector<CloneDetector::CloneGroup> &Sequences)`
  /// as this is the interface that the CloneDetector uses for applying the
  /// constraint. The constraint is supposed to directly modify the passed list
  /// so that all clones in the list fulfill the specific property this
  /// constraint ensures.
  template <typename T>
  static void constrainClones(std::vector<CloneGroup> &CloneGroups, T C) {
    C.constrain(CloneGroups);
  }

  /// Constrains the given list of clone groups with the given list of
  /// constraints.
  ///
  /// The constraints are applied in sequence in the order in which they are
  /// passed to this function.
  template <typename T1, typename... Ts>
  static void constrainClones(std::vector<CloneGroup> &CloneGroups, T1 C,
                              Ts... ConstraintList) {
    constrainClones(CloneGroups, C);
    constrainClones(CloneGroups, ConstraintList...);
  }

  /// Searches for clones in all previously passed statements.
  /// \param Result Output parameter to which all created clone groups are
  ///               added.
  /// \param ConstraintList The constraints that should be applied to the
  //         result.
  template <typename... Ts>
  void findClones(std::vector<CloneGroup> &Result, Ts... ConstraintList) {
    // The initial assumption is that there is only one clone group and every
    // statement is a clone of the others. This clone group will then be
    // split up with the help of the constraints.
    CloneGroup AllClones;
    AllClones.reserve(Sequences.size());
    for (const auto &C : Sequences) {
      AllClones.push_back(C);
    }

    Result.push_back(AllClones);

    constrainClones(Result, ConstraintList...);
  }

private:
  CloneGroup Sequences;
};

/// This class is a utility class that contains utility functions for building
/// custom constraints.
class CloneConstraint {
public:
  /// Removes all groups by using a filter function.
  /// \param CloneGroups The list of CloneGroups that is supposed to be
  ///                    filtered.
  /// \param Filter The filter function that should return true for all groups
  ///               that should be removed from the list.
  static void
  filterGroups(std::vector<CloneDetector::CloneGroup> &CloneGroups,
               std::function<bool(const CloneDetector::CloneGroup &)> Filter) {
    CloneGroups.erase(
        std::remove_if(CloneGroups.begin(), CloneGroups.end(), Filter),
        CloneGroups.end());
  }

  /// Splits the given CloneGroups until the given Compare function returns true
  /// for all clones in a single group.
  /// \param CloneGroups A list of CloneGroups that should be modified.
  /// \param Compare The comparison function that all clones are supposed to
  ///                pass. Should return true if and only if two clones belong
  ///                to the same CloneGroup.
  static void splitCloneGroups(
      std::vector<CloneDetector::CloneGroup> &CloneGroups,
      std::function<bool(const StmtSequence &, const StmtSequence &)> Compare);
};

/// Searches all children of the given clones for type II clones (i.e. they are
/// identical in every aspect beside the used variable names).
class RecursiveCloneTypeIIConstraint {

  /// Generates and saves a hash code for the given Stmt.
  /// \param S The given Stmt.
  /// \param D The Decl containing S.
  /// \param StmtsByHash Output parameter that will contain the hash codes for
  ///                    each StmtSequence in the given Stmt.
  /// \return The hash code of the given Stmt.
  ///
  /// If the given Stmt is a CompoundStmt, this method will also generate
  /// hashes for all possible StmtSequences in the children of this Stmt.
  size_t saveHash(const Stmt *S, const Decl *D,
                  std::vector<std::pair<size_t, StmtSequence>> &StmtsByHash);

public:
  void constrain(std::vector<CloneDetector::CloneGroup> &Sequences);
};

/// Ensures that every clone has at least the given complexity.
///
/// Complexity is here defined as the total amount of children of a statement.
/// This constraint assumes the first statement in the group is representative
/// for all other statements in the group in terms of complexity.
class MinComplexityConstraint {
  unsigned MinComplexity;

public:
  MinComplexityConstraint(unsigned MinComplexity)
      : MinComplexity(MinComplexity) {}

  size_t calculateStmtComplexity(const StmtSequence &Seq,
                                 const std::string &ParentMacroStack = "");

  void constrain(std::vector<CloneDetector::CloneGroup> &CloneGroups) {
    CloneConstraint::filterGroups(
        CloneGroups, [this](const CloneDetector::CloneGroup &A) {
          if (!A.empty())
            return calculateStmtComplexity(A.front()) < MinComplexity;
          else
            return false;
        });
  }
};

/// Ensures that all clone groups contain at least the given amount of clones.
class MinGroupSizeConstraint {
  unsigned MinGroupSize;

public:
  MinGroupSizeConstraint(unsigned MinGroupSize = 2)
      : MinGroupSize(MinGroupSize) {}

  void constrain(std::vector<CloneDetector::CloneGroup> &CloneGroups) {
    CloneConstraint::filterGroups(CloneGroups,
                                  [this](const CloneDetector::CloneGroup &A) {
                                    return A.size() < MinGroupSize;
                                  });
  }
};

/// Ensures that no clone group fully contains another clone group.
struct OnlyLargestCloneConstraint {
  void constrain(std::vector<CloneDetector::CloneGroup> &Result);
};

struct FilenamePatternConstraint {
  StringRef IgnoredFilesPattern;
  std::shared_ptr<llvm::Regex> IgnoredFilesRegex;

  FilenamePatternConstraint(StringRef IgnoredFilesPattern) 
      : IgnoredFilesPattern(IgnoredFilesPattern) {
    IgnoredFilesRegex = std::make_shared<llvm::Regex>("^(" +
        IgnoredFilesPattern.str() + "$)");
  }

  bool isAutoGenerated(const CloneDetector::CloneGroup &Group);

  void constrain(std::vector<CloneDetector::CloneGroup> &CloneGroups) {
    CloneConstraint::filterGroups(
        CloneGroups, [this](const CloneDetector::CloneGroup &Group) {
          return isAutoGenerated(Group);
        });
  }
};

/// Analyzes the pattern of the referenced variables in a statement.
class VariablePattern {

  /// Describes an occurence of a variable reference in a statement.
  struct VariableOccurence {
    /// The index of the associated VarDecl in the Variables vector.
    size_t KindID;
    /// The statement in the code where the variable was referenced.
    const Stmt *Mention;

    VariableOccurence(size_t KindID, const Stmt *Mention)
        : KindID(KindID), Mention(Mention) {}
  };

  /// All occurences of referenced variables in the order of appearance.
  std::vector<VariableOccurence> Occurences;
  /// List of referenced variables in the order of appearance.
  /// Every item in this list is unique.
  std::vector<const VarDecl *> Variables;

  /// Adds a new variable referenced to this pattern.
  /// \param VarDecl The declaration of the variable that is referenced.
  /// \param Mention The SourceRange where this variable is referenced.
  void addVariableOccurence(const VarDecl *VarDecl, const Stmt *Mention);

  /// Adds each referenced variable from the given statement.
  void addVariables(const Stmt *S);

public:
  /// Creates an VariablePattern object with information about the given
  /// StmtSequence.
  VariablePattern(const StmtSequence &Sequence) {
    for (const Stmt *S : Sequence)
      addVariables(S);
  }

  /// Describes two clones that reference their variables in a different pattern
  /// which could indicate a programming error.
  struct SuspiciousClonePair {
    /// Utility class holding the relevant information about a single
    /// clone in this pair.
    struct SuspiciousCloneInfo {
      /// The variable which referencing in this clone was against the pattern.
      const VarDecl *Variable;
      /// Where the variable was referenced.
      const Stmt *Mention;
      /// The variable that should have been referenced to follow the pattern.
      /// If Suggestion is a nullptr then it's not possible to fix the pattern
      /// by referencing a different variable in this clone.
      const VarDecl *Suggestion;
      SuspiciousCloneInfo(const VarDecl *Variable, const Stmt *Mention,
                          const VarDecl *Suggestion)
          : Variable(Variable), Mention(Mention), Suggestion(Suggestion) {}
      SuspiciousCloneInfo() {}
    };
    /// The first clone in the pair which always has a suggested variable.
    SuspiciousCloneInfo FirstCloneInfo;
    /// This other clone in the pair which can have a suggested variable.
    SuspiciousCloneInfo SecondCloneInfo;
  };

  /// Counts the differences between this pattern and the given one.
  /// \param Other The given VariablePattern to compare with.
  /// \param FirstMismatch Output parameter that will be filled with information
  ///        about the first difference between the two patterns. This parameter
  ///        can be a nullptr, in which case it will be ignored.
  /// \return Returns the number of differences between the pattern this object
  ///         is following and the given VariablePattern.
  ///
  /// For example, the following statements all have the same pattern and this
  /// function would return zero:
  ///
  ///   if (a < b) return a; return b;
  ///   if (x < y) return x; return y;
  ///   if (u2 < u1) return u2; return u1;
  ///
  /// But the following statement has a different pattern (note the changed
  /// variables in the return statements) and would have two differences when
  /// compared with one of the statements above.
  ///
  ///   if (a < b) return b; return a;
  ///
  /// This function should only be called if the related statements of the given
  /// pattern and the statements of this objects are clones of each other.
  unsigned countPatternDifferences(
      const VariablePattern &Other,
      VariablePattern::SuspiciousClonePair *FirstMismatch = nullptr);
};

/// Ensures that all clones reference variables in the same pattern.
struct MatchingVariablePatternConstraint {
  void constrain(std::vector<CloneDetector::CloneGroup> &CloneGroups);
};

} // end namespace clang

#endif // LLVM_CLANG_AST_CLONEDETECTION_H
