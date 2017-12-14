// RUN: %clang_cc1 %s -triple=i686-pc-win32 -fms-extensions -emit-llvm -o - | FileCheck %s

int f();

// CHECK: $"\01?x@selectany_init@@3HA" = comdat any
// CHECK: $"\01?x@?$A@H@explicit_template_instantiation@@2HB" = comdat any
// CHECK: $"\01?x@?$A@H@implicit_template_instantiation@@2HB" = comdat any

namespace simple_init {
#pragma init_seg(compiler)
int x = f();
// CHECK: @"\01?x@simple_init@@3HA" = global i32 0, align 4
// CHECK: @__cxx_init_fn_ptr = private constant void ()* @"\01??__Ex@simple_init@@YAXXZ", section ".CRT$XCC"

#pragma init_seg(lib)
int y = f();
// CHECK: @"\01?y@simple_init@@3HA" = global i32 0, align 4
// CHECK: @__cxx_init_fn_ptr.1 = private constant void ()* @"\01??__Ey@simple_init@@YAXXZ", section ".CRT$XCL"

#pragma init_seg(user)
int z = f();
// CHECK: @"\01?z@simple_init@@3HA" = global i32 0, align 4
// No function pointer!  This one goes on @llvm.global_ctors.
}

#pragma init_seg(".asdf")

namespace internal_init {
namespace {
int x = f();
// CHECK: @"\01?x@?A@internal_init@@3HA" = internal global i32 0, align 4
// CHECK: @__cxx_init_fn_ptr.2 = private constant void ()* @"\01??__Ex@?A@internal_init@@YAXXZ", section ".asdf"
}
}

namespace selectany_init {
int __declspec(selectany) x = f();
// CHECK: @"\01?x@selectany_init@@3HA" = weak_odr global i32 0, comdat, align 4
// CHECK: @__cxx_init_fn_ptr.3 = private constant void ()* @"\01??__Ex@selectany_init@@YAXXZ", section ".asdf", comdat($"\01?x@selectany_init@@3HA")
}

namespace explicit_template_instantiation {
template <typename T> struct A { static const int x; };
template <typename T> const int A<T>::x = f();
template struct A<int>;
// CHECK: @"\01?x@?$A@H@explicit_template_instantiation@@2HB" = weak_odr global i32 0, comdat, align 4
// CHECK: @__cxx_init_fn_ptr.4 = private constant void ()* @"\01??__Ex@?$A@H@explicit_template_instantiation@@2HB@YAXXZ", section ".asdf", comdat($"\01?x@?$A@H@explicit_template_instantiation@@2HB")
}

namespace implicit_template_instantiation {
template <typename T> struct A { static const int x; };
template <typename T> const int A<T>::x = f();
int g() { return A<int>::x; }
// CHECK: @"\01?x@?$A@H@implicit_template_instantiation@@2HB" = linkonce_odr global i32 0, comdat, align 4
// CHECK: @__cxx_init_fn_ptr.5 = private constant void ()* @"\01??__Ex@?$A@H@implicit_template_instantiation@@2HB@YAXXZ", section ".asdf", comdat($"\01?x@?$A@H@implicit_template_instantiation@@2HB")
}

// ... and here's where we emitted user level ctors.
// CHECK: @llvm.global_ctors = appending global [1 x { i32, void ()*, i8* }]
// CHECK: [{ i32, void ()*, i8* } { i32 65535, void ()* @_GLOBAL__sub_I_pragma_init_seg.cpp, i8* null }]

// We have to mark everything used so we can survive globalopt, even through
// LTO.  There's no way LLVM could really understand if data in the .asdf
// section is really used or dead.
//
// CHECK: @llvm.used = appending global [6 x i8*]
// CHECK: [i8* bitcast (void ()** @__cxx_init_fn_ptr to i8*),
// CHECK: i8* bitcast (void ()** @__cxx_init_fn_ptr.1 to i8*),
// CHECK: i8* bitcast (void ()** @__cxx_init_fn_ptr.2 to i8*),
// CHECK: i8* bitcast (void ()** @__cxx_init_fn_ptr.3 to i8*),
// CHECK: i8* bitcast (void ()** @__cxx_init_fn_ptr.4 to i8*),
// CHECK: i8* bitcast (void ()** @__cxx_init_fn_ptr.5 to i8*)], section "llvm.metadata"
