//===- unittest/Tooling/RecursiveASTVisitorTest.cpp -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TestVisitor.h"
#include <stack>

using namespace clang;

namespace {

class LambdaExprVisitor : public ExpectedLocationVisitor<LambdaExprVisitor> {
public:
  bool VisitLambdaExpr(LambdaExpr *Lambda) {
    PendingBodies.push(Lambda);
    Match("", Lambda->getIntroducerRange().getBegin());
    return true;
  }
  /// For each call to VisitLambdaExpr, we expect a subsequent call (with
  /// proper nesting) to TraverseLambdaBody.
  bool TraverseLambdaBody(LambdaExpr *Lambda) {
    EXPECT_FALSE(PendingBodies.empty());
    EXPECT_EQ(PendingBodies.top(), Lambda);
    PendingBodies.pop();
    return TraverseStmt(Lambda->getBody());
  }
  /// Determine whether TraverseLambdaBody has been called for every call to
  /// VisitLambdaExpr.
  bool allBodiesHaveBeenTraversed() const {
    return PendingBodies.empty();
  }
private:
  std::stack<LambdaExpr *> PendingBodies;
};

TEST(RecursiveASTVisitor, VisitsLambdaExpr) {
  LambdaExprVisitor Visitor;
  Visitor.ExpectMatch("", 1, 12);
  EXPECT_TRUE(Visitor.runOver("void f() { []{ return; }(); }",
                              LambdaExprVisitor::Lang_CXX11));
}

TEST(RecursiveASTVisitor, TraverseLambdaBodyCanBeOverridden) {
  LambdaExprVisitor Visitor;
  EXPECT_TRUE(Visitor.runOver("void f() { []{ return; }(); }",
                              LambdaExprVisitor::Lang_CXX11));
  EXPECT_TRUE(Visitor.allBodiesHaveBeenTraversed());
}

TEST(RecursiveASTVisitor, VisitsAttributedLambdaExpr) {
  LambdaExprVisitor Visitor;
  Visitor.ExpectMatch("", 1, 12);
  EXPECT_TRUE(Visitor.runOver(
      "void f() { [] () __attribute__ (( fastcall )) { return; }(); }",
      LambdaExprVisitor::Lang_CXX14));
}

// Matches the (optional) capture-default of a lambda-introducer.
class LambdaDefaultCaptureVisitor
  : public ExpectedLocationVisitor<LambdaDefaultCaptureVisitor> {
public:
  bool VisitLambdaExpr(LambdaExpr *Lambda) {
    if (Lambda->getCaptureDefault() != LCD_None) {
      Match("", Lambda->getCaptureDefaultLoc());
    }
    return true;
  }
};

TEST(RecursiveASTVisitor, HasCaptureDefaultLoc) {
  LambdaDefaultCaptureVisitor Visitor;
  Visitor.ExpectMatch("", 1, 20);
  EXPECT_TRUE(Visitor.runOver("void f() { int a; [=]{a;}; }",
                              LambdaDefaultCaptureVisitor::Lang_CXX11));
}

// Checks for lambda classes that are not marked as implicitly-generated.
// (There should be none.)
class ClassVisitor : public ExpectedLocationVisitor<ClassVisitor> {
public:
  ClassVisitor() : SawNonImplicitLambdaClass(false) {}
  bool VisitCXXRecordDecl(CXXRecordDecl* record) {
    if (record->isLambda() && !record->isImplicit())
      SawNonImplicitLambdaClass = true;
    return true;
  }

  bool sawOnlyImplicitLambdaClasses() const {
    return !SawNonImplicitLambdaClass;
  }

private:
  bool SawNonImplicitLambdaClass;
};

TEST(RecursiveASTVisitor, LambdaClosureTypesAreImplicit) {
  ClassVisitor Visitor;
  EXPECT_TRUE(Visitor.runOver("auto lambda = []{};", ClassVisitor::Lang_CXX11));
  EXPECT_TRUE(Visitor.sawOnlyImplicitLambdaClasses());
}


// Check to ensure that attributes and expressions within them are being
// visited.
class AttrVisitor : public ExpectedLocationVisitor<AttrVisitor> {
public:
  bool VisitMemberExpr(MemberExpr *ME) {
    Match(ME->getMemberDecl()->getNameAsString(), ME->getLocStart());
    return true;
  }
  bool VisitAttr(Attr *A) {
    Match("Attr", A->getLocation());
    return true;
  }
  bool VisitGuardedByAttr(GuardedByAttr *A) {
    Match("guarded_by", A->getLocation());
    return true;
  }
};


TEST(RecursiveASTVisitor, AttributesAreVisited) {
  AttrVisitor Visitor;
  Visitor.ExpectMatch("Attr", 4, 24);
  Visitor.ExpectMatch("guarded_by", 4, 24);
  Visitor.ExpectMatch("mu1",  4, 35);
  Visitor.ExpectMatch("Attr", 5, 29);
  Visitor.ExpectMatch("mu1",  5, 54);
  Visitor.ExpectMatch("mu2",  5, 59);
  EXPECT_TRUE(Visitor.runOver(
    "class Foo {\n"
    "  int mu1;\n"
    "  int mu2;\n"
    "  int a __attribute__((guarded_by(mu1)));\n"
    "  void bar() __attribute__((exclusive_locks_required(mu1, mu2)));\n"
    "};\n"));
}

// Check to ensure that implicit default argument expressions are visited.
class IntegerLiteralVisitor
    : public ExpectedLocationVisitor<IntegerLiteralVisitor> {
public:
  bool VisitIntegerLiteral(const IntegerLiteral *IL) {
    Match("literal", IL->getLocation());
    return true;
  }
};

TEST(RecursiveASTVisitor, DefaultArgumentsAreVisited) {
  IntegerLiteralVisitor Visitor;
  Visitor.ExpectMatch("literal", 1, 15, 2);
  EXPECT_TRUE(Visitor.runOver("int f(int i = 1);\n"
                              "static int k = f();\n"));
}

// Check to ensure that InitListExpr is visited twice, once each for the
// syntactic and semantic form.
class InitListExprPreOrderVisitor
    : public ExpectedLocationVisitor<InitListExprPreOrderVisitor> {
public:
  bool VisitInitListExpr(InitListExpr *ILE) {
    Match(ILE->isSemanticForm() ? "semantic" : "syntactic", ILE->getLocStart());
    return true;
  }
};

class InitListExprPostOrderVisitor
    : public ExpectedLocationVisitor<InitListExprPostOrderVisitor> {
public:
  bool shouldTraversePostOrder() const { return true; }

  bool VisitInitListExpr(InitListExpr *ILE) {
    Match(ILE->isSemanticForm() ? "semantic" : "syntactic", ILE->getLocStart());
    return true;
  }
};

class InitListExprPreOrderNoQueueVisitor
    : public ExpectedLocationVisitor<InitListExprPreOrderNoQueueVisitor> {
public:
  bool TraverseInitListExpr(InitListExpr *ILE) {
    return ExpectedLocationVisitor::TraverseInitListExpr(ILE);
  }

  bool VisitInitListExpr(InitListExpr *ILE) {
    Match(ILE->isSemanticForm() ? "semantic" : "syntactic", ILE->getLocStart());
    return true;
  }
};

class InitListExprPostOrderNoQueueVisitor
    : public ExpectedLocationVisitor<InitListExprPostOrderNoQueueVisitor> {
public:
  bool shouldTraversePostOrder() const { return true; }

  bool TraverseInitListExpr(InitListExpr *ILE) {
    return ExpectedLocationVisitor::TraverseInitListExpr(ILE);
  }

  bool VisitInitListExpr(InitListExpr *ILE) {
    Match(ILE->isSemanticForm() ? "semantic" : "syntactic", ILE->getLocStart());
    return true;
  }
};

TEST(RecursiveASTVisitor, InitListExprIsPreOrderVisitedTwice) {
  InitListExprPreOrderVisitor Visitor;
  Visitor.ExpectMatch("syntactic", 2, 21);
  Visitor.ExpectMatch("semantic", 2, 21);
  EXPECT_TRUE(Visitor.runOver("struct S { int x; };\n"
                              "static struct S s = {.x = 0};\n",
                              InitListExprPreOrderVisitor::Lang_C));
}

TEST(RecursiveASTVisitor, InitListExprIsPostOrderVisitedTwice) {
  InitListExprPostOrderVisitor Visitor;
  Visitor.ExpectMatch("syntactic", 2, 21);
  Visitor.ExpectMatch("semantic", 2, 21);
  EXPECT_TRUE(Visitor.runOver("struct S { int x; };\n"
                              "static struct S s = {.x = 0};\n",
                              InitListExprPostOrderVisitor::Lang_C));
}

TEST(RecursiveASTVisitor, InitListExprIsPreOrderNoQueueVisitedTwice) {
  InitListExprPreOrderNoQueueVisitor Visitor;
  Visitor.ExpectMatch("syntactic", 2, 21);
  Visitor.ExpectMatch("semantic", 2, 21);
  EXPECT_TRUE(Visitor.runOver("struct S { int x; };\n"
                              "static struct S s = {.x = 0};\n",
                              InitListExprPreOrderNoQueueVisitor::Lang_C));
}

TEST(RecursiveASTVisitor, InitListExprIsPostOrderNoQueueVisitedTwice) {
  InitListExprPostOrderNoQueueVisitor Visitor;
  Visitor.ExpectMatch("syntactic", 2, 21);
  Visitor.ExpectMatch("semantic", 2, 21);
  EXPECT_TRUE(Visitor.runOver("struct S { int x; };\n"
                              "static struct S s = {.x = 0};\n",
                              InitListExprPostOrderNoQueueVisitor::Lang_C));
}

// Check to ensure that nested name specifiers are visited.
class NestedNameSpecifiersVisitor
    : public ExpectedLocationVisitor<NestedNameSpecifiersVisitor> {
public:
  bool VisitRecordTypeLoc(RecordTypeLoc RTL) {
    if (!RTL)
      return true;
    Match(RTL.getDecl()->getName(), RTL.getNameLoc());
    return true;
  }

  bool TraverseNestedNameSpecifierLoc(NestedNameSpecifierLoc NNS) {
    if (!NNS)
      return true;
    if (const NamespaceDecl *ND =
            NNS.getNestedNameSpecifier()->getAsNamespace())
      Match(ND->getName(), NNS.getLocalBeginLoc());
    return ExpectedLocationVisitor::TraverseNestedNameSpecifierLoc(NNS);
  }
};

TEST(RecursiveASTVisitor,
     NestedNameSpecifiersForTemplateSpecializationsAreVisited) {
  StringRef Source = R"(
namespace ns {
struct Outer {
    template<typename T, typename U>
    struct Nested { };

    template<typename T>
    static T x;
};
}

template<>
struct ns::Outer::Nested<int, int>;

template<>
struct ns::Outer::Nested<int, int> { };

template<typename T>
struct ns::Outer::Nested<int, T> { };

template<>
int ns::Outer::x<int> = 0;
)";
  NestedNameSpecifiersVisitor Visitor;
  Visitor.ExpectMatch("ns", 13, 8);
  Visitor.ExpectMatch("ns", 16, 8);
  Visitor.ExpectMatch("ns", 19, 8);
  Visitor.ExpectMatch("ns", 22, 5);
  Visitor.ExpectMatch("Outer", 13, 12);
  Visitor.ExpectMatch("Outer", 16, 12);
  Visitor.ExpectMatch("Outer", 19, 12);
  Visitor.ExpectMatch("Outer", 22, 9);
  EXPECT_TRUE(Visitor.runOver(Source, NestedNameSpecifiersVisitor::Lang_CXX14));
}

} // end anonymous namespace
