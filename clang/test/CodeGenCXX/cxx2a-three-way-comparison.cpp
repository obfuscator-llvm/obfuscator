// RUN: %clang_cc1 -std=c++2a -emit-llvm %s -o - -triple %itanium_abi_triple | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -std=c++2a -emit-llvm %s -o - -triple x86_64-pc-win32 2>&1 | FileCheck %s --check-prefix=MSABI
// RUN: not %clang_cc1 -std=c++2a -emit-llvm %s -o - -triple %itanium_abi_triple -DBUILTIN 2>&1 | FileCheck %s --check-prefix=BUILTIN

struct A {
  void operator<=>(int);
};

// ITANIUM: define {{.*}}@_ZN1AssEi(
// MSABI: define {{.*}}@"??__MA@@QEAAXH@Z"(
void A::operator<=>(int) {}

// ITANIUM: define {{.*}}@_Zssi1A(
// MSABI: define {{.*}}@"??__M@YAXHUA@@@Z"(
void operator<=>(int, A) {}

int operator<=>(A, A);

// ITANIUM: define {{.*}}_Z1f1A(
// MSABI: define {{.*}}@"?f@@YAHUA@@@Z"(
int f(A a) {
  // ITANIUM: %[[RET:.*]] = call {{.*}}_Zss1AS_(
  // ITANIUM: ret i32 %[[RET]]
  // MSABI: %[[RET:.*]] = call {{.*}}"??__M@YAHUA@@0@Z"(
  // MSABI: ret i32 %[[RET]]
  return a <=> a;
}

#ifdef BUILTIN
void builtin(int a) {
  a <=> a; // BUILTIN: cannot compile this scalar expression yet
}
#endif
