; RUN: llc < %s -asm-verbose=false -disable-wasm-explicit-locals | FileCheck %s

; Test that function pointer casts are replaced with wrappers.

target datalayout = "e-m:e-p:32:32-i64:64-n32:64-S128"
target triple = "wasm32-unknown-unknown-wasm"

; CHECK-LABEL: test:
; CHECK-NEXT: call        .Lbitcast@FUNCTION{{$}}
; CHECK-NEXT: call        .Lbitcast@FUNCTION{{$}}
; CHECK-NEXT: call        .Lbitcast.1@FUNCTION{{$}}
; CHECK-NEXT: i32.const   $push[[L0:[0-9]+]]=, 0
; CHECK-NEXT: call        .Lbitcast.2@FUNCTION, $pop[[L0]]{{$}}
; CHECK-NEXT: i32.const   $push[[L1:[0-9]+]]=, 0
; CHECK-NEXT: call        .Lbitcast.2@FUNCTION, $pop[[L1]]{{$}}
; CHECK-NEXT: i32.const   $push[[L2:[0-9]+]]=, 0
; CHECK-NEXT: call        .Lbitcast.2@FUNCTION, $pop[[L2]]{{$}}
; CHECK-NEXT: call        foo0@FUNCTION
; CHECK-NEXT: i32.call    $drop=, .Lbitcast.3@FUNCTION{{$}}
; CHECK-NEXT: call        foo2@FUNCTION{{$}}
; CHECK-NEXT: call        foo1@FUNCTION{{$}}
; CHECK-NEXT: call        foo3@FUNCTION{{$}}
; CHECK-NEXT: end_function

; CHECK-LABEL: test_varargs:
; CHECK:      set_global
; CHECK:      i32.const   $push[[L3:[0-9]+]]=, 0{{$}}
; CHECK-NEXT: call        vararg@FUNCTION, $pop[[L3]]{{$}}
; CHECK-NEXT: i32.const   $push[[L4:[0-9]+]]=, 0{{$}}
; CHECK-NEXT: i32.store   0($[[L5:[0-9]+]]), $pop[[L4]]{{$}}
; CHECK-NEXT: call        plain@FUNCTION, $[[L5]]{{$}}

; CHECK-LABEL: .Lbitcast:
; CHECK-NEXT: call        has_i32_arg@FUNCTION, $0{{$}}
; CHECK-NEXT: end_function

; CHECK-LABEL: .Lbitcast.1:
; CHECK-NEXT: call        $drop=, has_i32_ret@FUNCTION{{$}}
; CHECK-NEXT: end_function

; CHECK-LABEL: .Lbitcast.2:
; CHECK-NEXT: .param      i32
; CHECK-NEXT: call        foo0@FUNCTION{{$}}
; CHECK-NEXT: end_function

; CHECK-LABEL: .Lbitcast.3:
; CHECK-NEXT: .result     i32
; CHECK-NEXT: call        foo1@FUNCTION{{$}}
; CHECK-NEXT: copy_local  $push0=, $0
; CHECK-NEXT: end_function

declare void @has_i32_arg(i32)
declare i32 @has_i32_ret()
declare void @vararg(...)
declare void @plain(i32)

declare void @foo0()
declare void @foo1()
declare void @foo2()
declare void @foo3()

define void @test() {
entry:
  call void bitcast (void (i32)* @has_i32_arg to void ()*)()
  call void bitcast (void (i32)* @has_i32_arg to void ()*)()
  call void bitcast (i32 ()* @has_i32_ret to void ()*)()
  call void bitcast (void ()* @foo0 to void (i32)*)(i32 0)
  %p = bitcast void ()* @foo0 to void (i32)*
  call void %p(i32 0)
  %q = bitcast void ()* @foo0 to void (i32)*
  call void %q(i32 0)
  %r = bitcast void (i32)* %q to void ()*
  call void %r()
  %t = call i32 bitcast (void ()* @foo1 to i32 ()*)()
  call void bitcast (void ()* @foo2 to void ()*)()
  call void @foo1()
  call void @foo3()

  ret void
}

define void @test_varargs() {
  call void bitcast (void (...)* @vararg to void (i32)*)(i32 0)
  call void (...) bitcast (void (i32)* @plain to void (...)*)(i32 0)
  ret void
}
