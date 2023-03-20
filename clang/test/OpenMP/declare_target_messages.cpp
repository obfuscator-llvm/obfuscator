// RUN: %clang_cc1 -triple x86_64-apple-macos10.7.0 -verify -fopenmp -fnoopenmp-use-tls -ferror-limit 100 -o - %s

// RUN: %clang_cc1 -triple x86_64-apple-macos10.7.0 -verify -fopenmp-simd -fnoopenmp-use-tls -ferror-limit 100 -o - %s

#pragma omp end declare target // expected-error {{unexpected OpenMP directive '#pragma omp end declare target'}}

int a, b;
__thread int t; // expected-note {{defined as threadprivate or thread local}}

#pragma omp declare target . // expected-error {{expected '(' after 'declare target'}}

#pragma omp declare target
void f();
#pragma omp end declare target shared(a) // expected-warning {{extra tokens at the end of '#pragma omp end declare target' are ignored}}

#pragma omp declare target map(a) // expected-error {{unexpected 'map' clause, only 'to' or 'link' clauses expected}}

#pragma omp declare target to(foo1) // expected-error {{use of undeclared identifier 'foo1'}}

#pragma omp declare target link(foo2) // expected-error {{use of undeclared identifier 'foo2'}}

void c();

void func() {} // expected-note {{'func' defined here}}

#pragma omp declare target link(func) allocate(a) // expected-error {{function name is not allowed in 'link' clause}} expected-error {{unexpected 'allocate' clause, only 'to' or 'link' clauses expected}}

extern int b;

struct NonT {
  int a;
};

typedef int sint;

template <typename T>
T bla1() { return 0; }

#pragma omp declare target
template <typename T>
T bla2() { return 0; }
#pragma omp end declare target

template<>
float bla2() { return 1.0; }

#pragma omp declare target
void blub2() {
  bla2<float>();
  bla2<int>();
}
#pragma omp end declare target

void t2() {
#pragma omp target
  {
    bla2<float>();
    bla2<long>();
  }
}

#pragma omp declare target
  void abc();
#pragma omp end declare target
void cba();
#pragma omp end declare target // expected-error {{unexpected OpenMP directive '#pragma omp end declare target'}}

#pragma omp declare target  
  #pragma omp declare target
    void def();
  #pragma omp end declare target
  void fed();

#pragma omp declare target
#pragma omp threadprivate(a) // expected-note {{defined as threadprivate or thread local}}
extern int b;
int g;

struct T {
  int a;
  virtual int method();
};

class VC {
  T member;
  NonT member1;
  public:
    virtual int method() { T a; return 0; }
};

struct C {
  NonT a;
  sint b;
  int method();
  int method1();
};

int C::method1() {
  return 0;
}

void foo(int p) {
  a = 0; // expected-error {{threadprivate variables cannot be used in target constructs}}
  b = 0;
  t = 1; // expected-error {{threadprivate variables cannot be used in target constructs}}
  C object;
  VC object1;
  g = object.method();
  g += object.method1();
  g += object1.method() + p;
  f();
  c();
}
#pragma omp declare target
void foo1() {}
#pragma omp end declare target

#pragma omp end declare target
#pragma omp end declare target
#pragma omp end declare target // expected-error {{unexpected OpenMP directive '#pragma omp end declare target'}}

int C::method() {
  return 0;
}

struct S {
#pragma omp declare target
  int v;
#pragma omp end declare target
};

int main (int argc, char **argv) {
#pragma omp declare target // expected-error {{unexpected OpenMP directive '#pragma omp declare target'}}
  int v;
#pragma omp end declare target // expected-error {{unexpected OpenMP directive '#pragma omp end declare target'}}
  foo(v);
  return (0);
}

namespace {
#pragma omp declare target // expected-note {{to match this '#pragma omp declare target'}}
  int x;
} //  expected-error {{expected '#pragma omp end declare target'}}
#pragma omp end declare target // expected-error {{unexpected OpenMP directive '#pragma omp end declare target'}}

#pragma omp declare target link(S) // expected-error {{'S' used in declare target directive is not a variable or a function name}}

#pragma omp declare target (x, x) // expected-error {{'x' appears multiple times in clauses on the same declare target directive}}
#pragma omp declare target to(x) to(x) // expected-error {{'x' appears multiple times in clauses on the same declare target directive}}
#pragma omp declare target link(x) // expected-error {{'x' must not appear in both clauses 'to' and 'link'}}

#pragma omp declare target // expected-error {{expected '#pragma omp end declare target'}} expected-note {{to match this '#pragma omp declare target'}}
