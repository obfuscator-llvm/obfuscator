// RUN: %clang_cc1 -triple i686-windows-gnu -emit-llvm -o - %s \
// RUN:    | FileCheck %s --check-prefix=GNU32
// RUN: %clang_cc1 -triple x86_64-windows-gnu -emit-llvm -o - %s \
// RUN:    | FileCheck %s --check-prefix=GNU64
// RUN: %clang_cc1 -triple x86_64-windows-msvc -emit-llvm -o - %s \
// RUN:    | FileCheck %s --check-prefix=MSC64

struct {
  char c;
  long double ldb;
} agggregate_LD = {};
// GNU32: %struct.anon = type { i8, x86_fp80 }
// GNU32: @agggregate_LD = global %struct.anon zeroinitializer, align 4
// GNU64: %struct.anon = type { i8, x86_fp80 }
// GNU64: @agggregate_LD = global %struct.anon zeroinitializer, align 16
// MSC64: %struct.anon = type { i8, double }
// MSC64: @agggregate_LD = global %struct.anon zeroinitializer, align 8

long double dataLD = 1.0L;
// GNU32: @dataLD = global x86_fp80 0xK3FFF8000000000000000, align 4
// GNU64: @dataLD = global x86_fp80 0xK3FFF8000000000000000, align 16
// MSC64: @dataLD = global double 1.000000e+00, align 8

long double _Complex dataLDC = {1.0L, 1.0L};
// GNU32: @dataLDC = global { x86_fp80, x86_fp80 } { x86_fp80 0xK3FFF8000000000000000, x86_fp80 0xK3FFF8000000000000000 }, align 4
// GNU64: @dataLDC = global { x86_fp80, x86_fp80 } { x86_fp80 0xK3FFF8000000000000000, x86_fp80 0xK3FFF8000000000000000 }, align 16
// MSC64: @dataLDC = global { double, double } { double 1.000000e+00, double 1.000000e+00 }, align 8

long double TestLD(long double x) {
  return x * x;
}
// GNU32: define x86_fp80 @TestLD(x86_fp80 %x)
// GNU64: define void @TestLD(x86_fp80* noalias sret %agg.result, x86_fp80*)
// MSC64: define double @TestLD(double %x)

long double _Complex TestLDC(long double _Complex x) {
  return x * x;
}
// GNU32: define void @TestLDC({ x86_fp80, x86_fp80 }* noalias sret %agg.result, { x86_fp80, x86_fp80 }* byval align 4 %x)
// GNU64: define void @TestLDC({ x86_fp80, x86_fp80 }* noalias sret %agg.result, { x86_fp80, x86_fp80 }* %x)
// MSC64: define void @TestLDC({ double, double }* noalias sret %agg.result, { double, double }* %x)
