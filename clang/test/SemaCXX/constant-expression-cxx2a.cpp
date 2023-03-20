// RUN: %clang_cc1 -std=c++2a -verify %s -fcxx-exceptions -triple=x86_64-linux-gnu

#include "Inputs/std-compare.h"

namespace std {
  struct type_info;
};

namespace ThreeWayComparison {
  struct A {
    int n;
    constexpr friend int operator<=>(const A &a, const A &b) {
      return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
    }
  };
  static_assert(A{1} <=> A{2} < 0);
  static_assert(A{2} <=> A{1} > 0);
  static_assert(A{2} <=> A{2} == 0);

  static_assert(1 <=> 2 < 0);
  static_assert(2 <=> 1 > 0);
  static_assert(1 <=> 1 == 0);
  constexpr int k = (1 <=> 1, 0);
  // expected-warning@-1 {{three-way comparison result unused}}

  static_assert(std::strong_ordering::equal == 0);

  constexpr void f() {
    void(1 <=> 1);
  }

  struct MemPtr {
    void foo() {}
    void bar() {}
    int data;
    int data2;
    long data3;
  };

  struct MemPtr2 {
    void foo() {}
    void bar() {}
    int data;
    int data2;
    long data3;
  };
  using MemPtrT = void (MemPtr::*)();

  using FnPtrT = void (*)();

  void FnPtr1() {}
  void FnPtr2() {}

#define CHECK(...) ((__VA_ARGS__) ? void() : throw "error")
#define CHECK_TYPE(...) static_assert(__is_same(__VA_ARGS__));

constexpr bool test_constexpr_success = [] {
  {
    auto &EQ = std::strong_ordering::equal;
    auto &LESS = std::strong_ordering::less;
    auto &GREATER = std::strong_ordering::greater;
    using SO = std::strong_ordering;
    auto eq = (42 <=> 42);
    CHECK_TYPE(decltype(eq), SO);
    CHECK(eq.test_eq(EQ));

    auto less = (-1 <=> 0);
    CHECK_TYPE(decltype(less), SO);
    CHECK(less.test_eq(LESS));

    auto greater = (42l <=> 1u);
    CHECK_TYPE(decltype(greater), SO);
    CHECK(greater.test_eq(GREATER));
  }
  {
    using PO = std::partial_ordering;
    auto EQUIV = PO::equivalent;
    auto LESS = PO::less;
    auto GREATER = PO::greater;

    auto eq = (42.0 <=> 42.0);
    CHECK_TYPE(decltype(eq), PO);
    CHECK(eq.test_eq(EQUIV));

    auto less = (39.0 <=> 42.0);
    CHECK_TYPE(decltype(less), PO);
    CHECK(less.test_eq(LESS));

    auto greater = (-10.123 <=> -101.1);
    CHECK_TYPE(decltype(greater), PO);
    CHECK(greater.test_eq(GREATER));
  }
  {
    using SE = std::strong_equality;
    auto EQ = SE::equal;
    auto NEQ = SE::nonequal;

    MemPtrT P1 = &MemPtr::foo;
    MemPtrT P12 = &MemPtr::foo;
    MemPtrT P2 = &MemPtr::bar;
    MemPtrT P3 = nullptr;

    auto eq = (P1 <=> P12);
    CHECK_TYPE(decltype(eq), SE);
    CHECK(eq.test_eq(EQ));

    auto neq = (P1 <=> P2);
    CHECK_TYPE(decltype(eq), SE);
    CHECK(neq.test_eq(NEQ));

    auto eq2 = (P3 <=> nullptr);
    CHECK_TYPE(decltype(eq2), SE);
    CHECK(eq2.test_eq(EQ));
  }
  {
    using SE = std::strong_equality;
    auto EQ = SE::equal;
    auto NEQ = SE::nonequal;

    FnPtrT F1 = &FnPtr1;
    FnPtrT F12 = &FnPtr1;
    FnPtrT F2 = &FnPtr2;
    FnPtrT F3 = nullptr;

    auto eq = (F1 <=> F12);
    CHECK_TYPE(decltype(eq), SE);
    CHECK(eq.test_eq(EQ));

    auto neq = (F1 <=> F2);
    CHECK_TYPE(decltype(neq), SE);
    CHECK(neq.test_eq(NEQ));
  }
  { // mixed nullptr tests
    using SO = std::strong_ordering;
    using SE = std::strong_equality;

    int x = 42;
    int *xp = &x;

    MemPtrT mf = nullptr;
    MemPtrT mf2 = &MemPtr::foo;
    auto r3 = (mf <=> nullptr);
    CHECK_TYPE(decltype(r3), std::strong_equality);
    CHECK(r3.test_eq(SE::equal));
  }

  return true;
}();

template <auto LHS, auto RHS, bool ExpectTrue = false>
constexpr bool test_constexpr() {
  using nullptr_t = decltype(nullptr);
  using LHSTy = decltype(LHS);
  using RHSTy = decltype(RHS);
  // expected-note@+1 {{subexpression not valid in a constant expression}}
  auto Res = (LHS <=> RHS);
  if constexpr (__is_same(LHSTy, nullptr_t) || __is_same(RHSTy, nullptr_t)) {
    CHECK_TYPE(decltype(Res), std::strong_equality);
  }
  if (ExpectTrue)
    return Res == 0;
  return Res != 0;
}
int dummy = 42;
int dummy2 = 101;

constexpr bool tc1 = test_constexpr<nullptr, &dummy>();
constexpr bool tc2 = test_constexpr<&dummy, nullptr>();

// OK, equality comparison only
constexpr bool tc3 = test_constexpr<&MemPtr::foo, nullptr>();
constexpr bool tc4 = test_constexpr<nullptr, &MemPtr::foo>();
constexpr bool tc5 = test_constexpr<&MemPtr::foo, &MemPtr::bar>();

constexpr bool tc6 = test_constexpr<&MemPtr::data, nullptr>();
constexpr bool tc7 = test_constexpr<nullptr, &MemPtr::data>();
constexpr bool tc8 = test_constexpr<&MemPtr::data, &MemPtr::data2>();

// expected-error@+1 {{must be initialized by a constant expression}}
constexpr bool tc9 = test_constexpr<&dummy, &dummy2>(); // expected-note {{in call}}

template <class T, class R, class I>
constexpr T makeComplex(R r, I i) {
  T res{r, i};
  return res;
};

template <class T, class ResultT>
constexpr bool complex_test(T x, T y, ResultT Expect) {
  auto res = x <=> y;
  CHECK_TYPE(decltype(res), ResultT);
  return res.test_eq(Expect);
}
static_assert(complex_test(makeComplex<_Complex double>(0.0, 0.0),
                           makeComplex<_Complex double>(0.0, 0.0),
                           std::weak_equality::equivalent));
static_assert(complex_test(makeComplex<_Complex double>(0.0, 0.0),
                           makeComplex<_Complex double>(1.0, 0.0),
                           std::weak_equality::nonequivalent));
static_assert(complex_test(makeComplex<_Complex double>(0.0, 0.0),
                           makeComplex<_Complex double>(0.0, 1.0),
                           std::weak_equality::nonequivalent));
static_assert(complex_test(makeComplex<_Complex int>(0, 0),
                           makeComplex<_Complex int>(0, 0),
                           std::strong_equality::equal));
static_assert(complex_test(makeComplex<_Complex int>(0, 0),
                           makeComplex<_Complex int>(1, 0),
                           std::strong_equality::nonequal));
// TODO: defaulted operator <=>
} // namespace ThreeWayComparison

constexpr bool for_range_init() {
  int k = 0;
  for (int arr[3] = {1, 2, 3}; int n : arr) k += n;
  return k == 6;
}
static_assert(for_range_init());

namespace Virtual {
  struct NonZeroOffset { int padding = 123; };

  // Ensure that we pick the right final overrider during construction.
  struct A {
    virtual constexpr char f() const { return 'A'; }
    char a = f();
  };
  struct NoOverrideA : A {};
  struct B : NonZeroOffset, NoOverrideA {
    virtual constexpr char f() const { return 'B'; }
    char b = f();
  };
  struct NoOverrideB : B {};
  struct C : NonZeroOffset, A {
    virtual constexpr char f() const { return 'C'; }
    A *pba;
    char c = ((A*)this)->f();
    char ba = pba->f();
    constexpr C(A *pba) : pba(pba) {}
  };
  struct D : NonZeroOffset, NoOverrideB, C { // expected-warning {{inaccessible}}
    virtual constexpr char f() const { return 'D'; }
    char d = f();
    constexpr D() : C((B*)this) {}
  };
  constexpr D d;
  static_assert(((B&)d).a == 'A');
  static_assert(((C&)d).a == 'A');
  static_assert(d.b == 'B');
  static_assert(d.c == 'C');
  // During the construction of C, the dynamic type of B's A is B.
  static_assert(d.ba == 'B');
  static_assert(d.d == 'D');
  static_assert(d.f() == 'D');
  constexpr const A &a = (B&)d;
  constexpr const B &b = d;
  static_assert(a.f() == 'D');
  static_assert(b.f() == 'D');

  // FIXME: It is unclear whether this should be permitted.
  D d_not_constexpr;
  static_assert(d_not_constexpr.f() == 'D'); // expected-error {{constant expression}} expected-note {{virtual function called on object 'd_not_constexpr' whose dynamic type is not constant}}

  // Check that we apply a proper adjustment for a covariant return type.
  struct Covariant1 {
    D d;
    virtual const A *f() const;
  };
  template<typename T>
  struct Covariant2 : Covariant1 {
    virtual const T *f() const;
  };
  template<typename T>
  struct Covariant3 : Covariant2<T> {
    constexpr virtual const D *f() const { return &this->d; }
  };

  constexpr Covariant3<B> cb;
  constexpr Covariant3<C> cc;

  constexpr const Covariant1 *cb1 = &cb;
  constexpr const Covariant2<B> *cb2 = &cb;
  static_assert(cb1->f()->a == 'A');
  static_assert(cb1->f() == (B*)&cb.d);
  static_assert(cb1->f()->f() == 'D');
  static_assert(cb2->f()->b == 'B');
  static_assert(cb2->f() == &cb.d);
  static_assert(cb2->f()->f() == 'D');

  constexpr const Covariant1 *cc1 = &cc;
  constexpr const Covariant2<C> *cc2 = &cc;
  static_assert(cc1->f()->a == 'A');
  static_assert(cc1->f() == (C*)&cc.d);
  static_assert(cc1->f()->f() == 'D');
  static_assert(cc2->f()->c == 'C');
  static_assert(cc2->f() == &cc.d);
  static_assert(cc2->f()->f() == 'D');

  static_assert(cb.f()->d == 'D');
  static_assert(cc.f()->d == 'D');

  struct Abstract {
    constexpr virtual void f() = 0; // expected-note {{declared here}}
    constexpr Abstract() { do_it(); } // expected-note {{in call to}}
    constexpr void do_it() { f(); } // expected-note {{pure virtual function 'Virtual::Abstract::f' called}}
  };
  struct PureVirtualCall : Abstract { void f(); }; // expected-note {{in call to 'Abstract}}
  constexpr PureVirtualCall pure_virtual_call; // expected-error {{constant expression}} expected-note {{in call to 'PureVirtualCall}}
}

namespace DynamicCast {
  struct A2 { virtual void a2(); };
  struct A : A2 { virtual void a(); };
  struct B : A {};
  struct C2 { virtual void c2(); };
  struct C : A, C2 { A *c = dynamic_cast<A*>(static_cast<C2*>(this)); };
  struct D { virtual void d(); };
  struct E { virtual void e(); };
  struct F : B, C, D, private E { void *f = dynamic_cast<void*>(static_cast<D*>(this)); };
  struct Padding { virtual void padding(); };
  struct G : Padding, F {};

  constexpr G g;

  // During construction of C, A is unambiguous subobject of dynamic type C.
  static_assert(g.c == (C*)&g);
  // ... but in the complete object, the same is not true, so the runtime fails.
  static_assert(dynamic_cast<const A*>(static_cast<const C2*>(&g)) == nullptr);

  // dynamic_cast<void*> produces a pointer to the object of the dynamic type.
  static_assert(g.f == (void*)(F*)&g);
  static_assert(dynamic_cast<const void*>(static_cast<const D*>(&g)) == &g);

  // expected-note@+1 {{reference dynamic_cast failed: 'DynamicCast::A' is an ambiguous base class of dynamic type 'DynamicCast::G' of operand}}
  constexpr int d_a = (dynamic_cast<const A&>(static_cast<const D&>(g)), 0); // expected-error {{}}

  // Can navigate from A2 to its A...
  static_assert(&dynamic_cast<A&>((A2&)(B&)g) == &(A&)(B&)g);
  // ... and from B to its A ...
  static_assert(&dynamic_cast<A&>((B&)g) == &(A&)(B&)g);
  // ... but not from D.
  // expected-note@+1 {{reference dynamic_cast failed: 'DynamicCast::A' is an ambiguous base class of dynamic type 'DynamicCast::G' of operand}}
  static_assert(&dynamic_cast<A&>((D&)g) == &(A&)(B&)g); // expected-error {{}}

  // Can cast from A2 to sibling class D.
  static_assert(&dynamic_cast<D&>((A2&)(B&)g) == &(D&)g);

  // Cannot cast from private base E to derived class F.
  // expected-note@+1 {{reference dynamic_cast failed: static type 'DynamicCast::E' of operand is a non-public base class of dynamic type 'DynamicCast::G'}}
  constexpr int e_f = (dynamic_cast<F&>((E&)g), 0); // expected-error {{}}

  // Cannot cast from B to private sibling E.
  // expected-note@+1 {{reference dynamic_cast failed: 'DynamicCast::E' is a non-public base class of dynamic type 'DynamicCast::G' of operand}}
  constexpr int b_e = (dynamic_cast<E&>((B&)g), 0); // expected-error {{}}

  struct Unrelated { virtual void unrelated(); };
  // expected-note@+1 {{reference dynamic_cast failed: dynamic type 'DynamicCast::G' of operand does not have a base class of type 'DynamicCast::Unrelated'}}
  constexpr int b_unrelated = (dynamic_cast<Unrelated&>((B&)g), 0); // expected-error {{}}
  // expected-note@+1 {{reference dynamic_cast failed: dynamic type 'DynamicCast::G' of operand does not have a base class of type 'DynamicCast::Unrelated'}}
  constexpr int e_unrelated = (dynamic_cast<Unrelated&>((E&)g), 0); // expected-error {{}}
}

namespace TypeId {
  struct A {
    const std::type_info &ti = typeid(*this);
  };
  struct A2 : A {};
  static_assert(&A().ti == &typeid(A));
  static_assert(&typeid((A2())) == &typeid(A2));
  extern A2 extern_a2;
  static_assert(&typeid(extern_a2) == &typeid(A2));

  constexpr A2 a2;
  constexpr const A &a1 = a2;
  static_assert(&typeid(a1) == &typeid(A));

  struct B {
    virtual void f();
    const std::type_info &ti1 = typeid(*this);
  };
  struct B2 : B {
    const std::type_info &ti2 = typeid(*this);
  };
  static_assert(&B2().ti1 == &typeid(B));
  static_assert(&B2().ti2 == &typeid(B2));
  extern B2 extern_b2;
  // expected-note@+1 {{typeid applied to object 'extern_b2' whose dynamic type is not constant}}
  static_assert(&typeid(extern_b2) == &typeid(B2)); // expected-error {{constant expression}}

  constexpr B2 b2;
  constexpr const B &b1 = b2;
  static_assert(&typeid(b1) == &typeid(B2));

  constexpr bool side_effects() {
    // Not polymorphic nor a glvalue.
    bool OK = true;
    (void)typeid(OK = false, A2()); // expected-warning {{has no effect}}
    if (!OK) return false;

    // Not polymorphic.
    A2 a2;
    (void)typeid(OK = false, a2); // expected-warning {{has no effect}}
    if (!OK) return false;

    // Not a glvalue.
    (void)typeid(OK = false, B2()); // expected-warning {{has no effect}}
    if (!OK) return false;

    // Polymorphic glvalue: operand evaluated.
    OK = false;
    B2 b2;
    (void)typeid(OK = true, b2); // expected-warning {{will be evaluated}}
    return OK;
  }
  static_assert(side_effects());
}

namespace Union {
  struct Base {
    int y; // expected-note {{here}}
  };
  struct A : Base {
    int x;
    int arr[3];
    union { int p, q; };
  };
  union B {
    A a;
    int b;
  };
  constexpr int read_wrong_member() { // expected-error {{never produces a constant}}
    B b = {.b = 1};
    return b.a.x; // expected-note {{read of member 'a' of union with active member 'b'}}
  }
  constexpr int change_member() {
    B b = {.b = 1};
    b.a.x = 1;
    return b.a.x;
  }
  static_assert(change_member() == 1);
  constexpr int change_member_then_read_wrong_member() { // expected-error {{never produces a constant}}
    B b = {.b = 1};
    b.a.x = 1;
    return b.b; // expected-note {{read of member 'b' of union with active member 'a'}}
  }
  constexpr int read_wrong_member_indirect() { // expected-error {{never produces a constant}}
    B b = {.b = 1};
    int *p = &b.a.y;
    return *p; // expected-note {{read of member 'a' of union with active member 'b'}}
  }
  constexpr int read_uninitialized() {
    B b = {.b = 1};
    int *p = &b.a.y;
    b.a.x = 1;
    return *p; // expected-note {{read of uninitialized object}}
  }
  static_assert(read_uninitialized() == 0); // expected-error {{constant}} expected-note {{in call}}
  constexpr void write_wrong_member_indirect() { // expected-error {{never produces a constant}}
    B b = {.b = 1};
    int *p = &b.a.y;
    *p = 1; // expected-note {{assignment to member 'a' of union with active member 'b'}}
  }
  constexpr int write_uninitialized() {
    B b = {.b = 1};
    int *p = &b.a.y;
    b.a.x = 1;
    *p = 1;
    return *p;
  }
  static_assert(write_uninitialized() == 1);
  constexpr int change_member_indirectly() {
    B b = {.b = 1};
    b.a.arr[1] = 1;
    int &r = b.a.y;
    r = 123;

    b.b = 2;
    b.a.y = 3;
    b.a.arr[2] = 4;
    return b.a.arr[2];
  }
  static_assert(change_member_indirectly() == 4);
  constexpr B return_uninit() {
    B b = {.b = 1};
    b.a.x = 2;
    return b;
  }
  constexpr B uninit = return_uninit(); // expected-error {{constant expression}} expected-note {{subobject of type 'int' is not initialized}}
  static_assert(return_uninit().a.x == 2);
  constexpr A return_uninit_struct() {
    B b = {.b = 1};
    b.a.x = 2;
    return b.a;
  }
  // FIXME: It's unclear that this should be valid. Copying a B involves
  // copying the object representation of the union, but copying an A invokes a
  // copy constructor that copies the object elementwise, and reading from
  // b.a.y is undefined.
  static_assert(return_uninit_struct().x == 2);
  constexpr B return_init_all() {
    B b = {.b = 1};
    b.a.x = 2;
    b.a.y = 3;
    b.a.arr[0] = 4;
    b.a.arr[1] = 5;
    b.a.arr[2] = 6;
    return b;
  }
  static_assert(return_init_all().a.x == 2);
  static_assert(return_init_all().a.y == 3);
  static_assert(return_init_all().a.arr[0] == 4);
  static_assert(return_init_all().a.arr[1] == 5);
  static_assert(return_init_all().a.arr[2] == 6);
  static_assert(return_init_all().a.p == 7); // expected-error {{}} expected-note {{read of member 'p' of union with no active member}}
  static_assert(return_init_all().a.q == 8); // expected-error {{}} expected-note {{read of member 'q' of union with no active member}}
  constexpr B init_all = return_init_all();

  constexpr bool test_no_member_change =  []{
    union U { char dummy = {}; };
    U u1;
    U u2;
    u1 = u2;
    return true;
  }();

  struct S1 {
    int n;
  };
  struct S2 : S1 {};
  struct S3 : S2 {};
  void f() {
    S3 s;
    s.n = 0;
  }
}

namespace TwosComplementShifts {
  using uint32 = __UINT32_TYPE__;
  using int32 = __INT32_TYPE__;
  static_assert(uint32(int32(0x1234) << 16) == 0x12340000);
  static_assert(uint32(int32(0x1234) << 19) == 0x91a00000);
  static_assert(uint32(int32(0x1234) << 20) == 0x23400000); // expected-warning {{requires 34 bits}}
  static_assert(uint32(int32(0x1234) << 24) == 0x34000000); // expected-warning {{requires 38 bits}}
  static_assert(uint32(int32(-1) << 31) == 0x80000000);

  static_assert(-1 >> 1 == -1);
  static_assert(-1 >> 31 == -1);
  static_assert(-2 >> 1 == -1);
  static_assert(-3 >> 1 == -2);
  static_assert(-4 >> 1 == -2);
}
