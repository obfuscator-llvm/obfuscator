; RUN: opt < %s -basicaa -aa-eval -print-all-alias-modref-info -disable-output 2>&1 | FileCheck %s
target datalayout = "e-p:32:32:32-i1:8:32-i8:8:32-i16:16:32-i32:32:32-i64:32:32-f32:32:32-f64:32:32-v64:32:64-v128:32:128-a0:0:32-n32"
target triple = "arm-apple-ios"

declare void @llvm.memset.p0i8.i64(i8* nocapture, i8, i64, i32, i1) nounwind
declare void @llvm.memcpy.p0i8.p0i8.i64(i8* nocapture, i8* nocapture, i64, i32, i1) nounwind

declare void @a_readonly_func(i8 *) noinline nounwind readonly
declare void @a_writeonly_func(i8 *) noinline nounwind writeonly

define void @test2(i8* %P, i8* %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test2:

; CHECK:   MayAlias:     i8* %P, i8* %Q
; CHECK:   Just Mod:  Ptr: i8* %P     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK:   Just Ref:  Ptr: i8* %Q     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK:   Just Mod:  Ptr: i8* %P     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK:   Just Ref:  Ptr: i8* %Q     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK:   Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK:   Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test2a(i8* noalias %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test2a:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test2b(i8* noalias %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  %R = getelementptr i8, i8* %P, i64 12
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test2b:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: NoAlias:      i8* %P, i8* %R
; CHECK: NoAlias:      i8* %Q, i8* %R
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test2c(i8* noalias %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  %R = getelementptr i8, i8* %P, i64 11
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test2c:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: NoAlias:      i8* %P, i8* %R
; CHECK: NoAlias:      i8* %Q, i8* %R
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test2d(i8* noalias %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  %R = getelementptr i8, i8* %P, i64 -12
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test2d:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: NoAlias:      i8* %P, i8* %R
; CHECK: NoAlias:      i8* %Q, i8* %R
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test2e(i8* noalias %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  %R = getelementptr i8, i8* %P, i64 -11
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test2e:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: NoAlias:      i8* %P, i8* %R
; CHECK: NoAlias:      i8* %Q, i8* %R
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %R        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %R, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test3(i8* %P, i8* %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test3:

; CHECK: MayAlias:     i8* %P, i8* %Q
; CHECK: Just Mod:  Ptr: i8* %P     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %P     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
}

define void @test3a(i8* noalias %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test3a:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 8, i32 1, i1 false)
}

define void @test4(i8* %P, i8* noalias %Q) nounwind ssp {
  tail call void @llvm.memset.p0i8.i64(i8* %P, i8 42, i64 8, i32 1, i1 false)
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test4:

; CHECK: NoAlias:      i8* %P, i8* %Q
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memset.p0i8.i64(i8* %P, i8 42, i64 8, i32 1, i1 false)
; CHECK: NoModRef:  Ptr: i8* %Q        <->  tail call void @llvm.memset.p0i8.i64(i8* %P, i8 42, i64 8, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %P        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q        <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memset.p0i8.i64(i8* %P, i8 42, i64 8, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memset.p0i8.i64(i8* %P, i8 42, i64 8, i32 1, i1 false)
}

define void @test5(i8* %P, i8* %Q, i8* %R) nounwind ssp {
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %R, i64 12, i32 1, i1 false)
  ret void

; CHECK-LABEL: Function: test5:

; CHECK: MayAlias:     i8* %P, i8* %Q
; CHECK: MayAlias:     i8* %P, i8* %R
; CHECK: MayAlias:     i8* %Q, i8* %R
; CHECK: Just Mod:  Ptr: i8* %P     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %Q     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Both ModRef:  Ptr: i8* %R     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
; CHECK: Just Mod:  Ptr: i8* %P     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %R, i64 12, i32 1, i1 false)
; CHECK: Both ModRef:  Ptr: i8* %Q     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %R, i64 12, i32 1, i1 false)
; CHECK: Just Ref:  Ptr: i8* %R     <->  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %R, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %R, i64 12, i32 1, i1 false)
; CHECK: Just Mod:   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %R, i64 12, i32 1, i1 false) <->   tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %P, i8* %Q, i64 12, i32 1, i1 false)
}

define void @test6(i8* %P) nounwind ssp {
  call void @llvm.memset.p0i8.i64(i8* %P, i8 -51, i64 32, i32 8, i1 false)
  call void @a_readonly_func(i8* %P)
  ret void

; CHECK-LABEL: Function: test6:

; CHECK: Just Mod:  Ptr: i8* %P        <->  call void @llvm.memset.p0i8.i64(i8* %P, i8 -51, i64 32, i32 8, i1 false)
; CHECK: Just Ref:  Ptr: i8* %P        <->  call void @a_readonly_func(i8* %P)
; CHECK: Just Mod:   call void @llvm.memset.p0i8.i64(i8* %P, i8 -51, i64 32, i32 8, i1 false) <->   call void @a_readonly_func(i8* %P)
; CHECK: Just Ref:   call void @a_readonly_func(i8* %P) <->   call void @llvm.memset.p0i8.i64(i8* %P, i8 -51, i64 32, i32 8, i1 false)
}

define void @test7(i8* %P) nounwind ssp {
  call void @a_writeonly_func(i8* %P)
  call void @a_readonly_func(i8* %P)
  ret void

; CHECK-LABEL: Function: test7:

; CHECK: Just Mod:  Ptr: i8* %P        <->  call void @a_writeonly_func(i8* %P)
; CHECK: Just Ref:  Ptr: i8* %P        <->  call void @a_readonly_func(i8* %P)
; CHECK: Just Mod:   call void @a_writeonly_func(i8* %P) <->   call void @a_readonly_func(i8* %P)
; CHECK: Just Ref:   call void @a_readonly_func(i8* %P) <->   call void @a_writeonly_func(i8* %P)
}

declare void @an_inaccessiblememonly_func() nounwind inaccessiblememonly
declare void @an_inaccessibleorargmemonly_func(i8 *) nounwind inaccessiblemem_or_argmemonly
declare void @an_argmemonly_func(i8 *) nounwind argmemonly

define void @test8(i8* %p) {
entry:
  %q = getelementptr i8, i8* %p, i64 16
  call void @a_readonly_func(i8* %p)
  call void @an_inaccessiblememonly_func()
  call void @a_writeonly_func(i8* %q)
  call void @an_inaccessiblememonly_func()
  call void @an_inaccessibleorargmemonly_func(i8* %q)
  call void @an_argmemonly_func(i8* %q)
  ret void

; CHECK-LABEL: Function: test8
; CHECK: NoModRef:  Ptr: i8* %p <->  call void @an_inaccessiblememonly_func()
; CHECK: NoModRef:  Ptr: i8* %q <->  call void @an_inaccessiblememonly_func()
; CHECK: NoModRef:  Ptr: i8* %p <->  call void @an_inaccessibleorargmemonly_func(i8* %q)
; CHECK: Both ModRef:  Ptr: i8* %q <->  call void @an_inaccessibleorargmemonly_func(i8* %q)
; CHECK: NoModRef:  Ptr: i8* %p <->  call void @an_argmemonly_func(i8* %q)
; CHECK: Both ModRef:  Ptr: i8* %q <->  call void @an_argmemonly_func(i8* %q)
; CHECK: Just Ref: call void @a_readonly_func(i8* %p) <-> call void @an_inaccessiblememonly_func()
; CHECK: Just Ref: call void @a_readonly_func(i8* %p) <-> call void @an_inaccessibleorargmemonly_func(i8* %q)
; CHECK: Just Ref: call void @a_readonly_func(i8* %p) <-> call void @an_argmemonly_func(i8* %q)
; CHECK: Both ModRef: call void @an_inaccessiblememonly_func() <-> call void @a_readonly_func(i8* %p)
; CHECK: Both ModRef: call void @an_inaccessiblememonly_func() <-> call void @a_writeonly_func(i8* %q)
; CHECK: Both ModRef: call void @an_inaccessiblememonly_func() <-> call void @an_inaccessiblememonly_func()
; CHECK: Both ModRef: call void @an_inaccessiblememonly_func() <-> call void @an_inaccessibleorargmemonly_func(i8* %q)
; CHECK: NoModRef: call void @an_inaccessiblememonly_func() <-> call void @an_argmemonly_func(i8* %q)
; CHECK: Just Mod: call void @a_writeonly_func(i8* %q) <-> call void @an_inaccessiblememonly_func()
; CHECK: Just Mod: call void @a_writeonly_func(i8* %q) <-> call void @an_inaccessibleorargmemonly_func(i8* %q)
; CHECK: Just Mod: call void @a_writeonly_func(i8* %q) <-> call void @an_argmemonly_func(i8* %q)
; CHECK: Both ModRef: call void @an_inaccessibleorargmemonly_func(i8* %q) <-> call void @a_readonly_func(i8* %p)
; CHECK: Both ModRef: call void @an_inaccessibleorargmemonly_func(i8* %q) <-> call void @a_writeonly_func(i8* %q)
; CHECK: Both ModRef: call void @an_inaccessibleorargmemonly_func(i8* %q) <-> call void @an_inaccessiblememonly_func()
; CHECK: Both ModRef: call void @an_inaccessibleorargmemonly_func(i8* %q) <-> call void @an_argmemonly_func(i8* %q)
; CHECK: Both ModRef: call void @an_argmemonly_func(i8* %q) <-> call void @a_readonly_func(i8* %p)
; CHECK: Both ModRef: call void @an_argmemonly_func(i8* %q) <-> call void @a_writeonly_func(i8* %q)
; CHECK: NoModRef: call void @an_argmemonly_func(i8* %q) <-> call void @an_inaccessiblememonly_func()
; CHECK: Both ModRef: call void @an_argmemonly_func(i8* %q) <-> call void @an_inaccessibleorargmemonly_func(i8* %q)
}
