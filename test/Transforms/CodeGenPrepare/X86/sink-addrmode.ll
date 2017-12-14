; RUN: opt -S -codegenprepare < %s | FileCheck %s

target datalayout =
"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128"
target triple = "x86_64-unknown-linux-gnu"

@x = external global [1 x [2 x <4 x float>]]

; Can we sink single addressing mode computation to use?
define void @test1(i1 %cond, i64* %base) {
; CHECK-LABEL: @test1
; CHECK: getelementptr i8, {{.+}} 40
entry:
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
  %v = load i32, i32* %casted, align 4
  br label %fallthrough

fallthrough:
  ret void
}

declare void @foo(i32)

; Make sure sinking two copies of addressing mode into different blocks works
define void @test2(i1 %cond, i64* %base) {
; CHECK-LABEL: @test2
entry:
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
; CHECK-LABEL: if.then:
; CHECK: getelementptr i8, {{.+}} 40
  %v1 = load i32, i32* %casted, align 4
  call void @foo(i32 %v1)
  %cmp = icmp eq i32 %v1, 0
  br i1 %cmp, label %next, label %fallthrough

next:
; CHECK-LABEL: next:
; CHECK: getelementptr i8, {{.+}} 40
  %v2 = load i32, i32* %casted, align 4
  call void @foo(i32 %v2)
  br label %fallthrough

fallthrough:
  ret void
}

; If we have two loads in the same block, only need one copy of addressing mode
; - instruction selection will duplicate if needed
define void @test3(i1 %cond, i64* %base) {
; CHECK-LABEL: @test3
entry:
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
; CHECK-LABEL: if.then:
; CHECK: getelementptr i8, {{.+}} 40
  %v1 = load i32, i32* %casted, align 4
  call void @foo(i32 %v1)
; CHECK-NOT: getelementptr i8, {{.+}}, 40
  %v2 = load i32, i32* %casted, align 4
  call void @foo(i32 %v2)
  br label %fallthrough

fallthrough:
  ret void
}

; Can we still sink addressing mode if there's a cold use of the
; address itself?  
define void @test4(i1 %cond, i64* %base) {
; CHECK-LABEL: @test4
entry:
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
; CHECK-LABEL: if.then:
; CHECK: getelementptr i8, {{.+}} 40
  %v1 = load i32, i32* %casted, align 4
  call void @foo(i32 %v1)
  %cmp = icmp eq i32 %v1, 0
  br i1 %cmp, label %rare.1, label %fallthrough

fallthrough:
  ret void

rare.1:
; CHECK-LABEL: rare.1:
; CHECK: getelementptr i8, {{.+}} 40
  call void @slowpath(i32 %v1, i32* %casted) cold
  br label %fallthrough
}

; Negative test - don't want to duplicate addressing into hot path
define void @test5(i1 %cond, i64* %base) {
; CHECK-LABEL: @test5
entry:
; CHECK: %addr = getelementptr
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
; CHECK-LABEL: if.then:
; CHECK-NOT: getelementptr i8, {{.+}} 40
  %v1 = load i32, i32* %casted, align 4
  call void @foo(i32 %v1)
  %cmp = icmp eq i32 %v1, 0
  br i1 %cmp, label %rare.1, label %fallthrough

fallthrough:
  ret void

rare.1:
  call void @slowpath(i32 %v1, i32* %casted) ;; NOT COLD
  br label %fallthrough
}

; Negative test - opt for size
define void @test6(i1 %cond, i64* %base) minsize {
; CHECK-LABEL: @test6
entry:
; CHECK: %addr = getelementptr
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
; CHECK-LABEL: if.then:
; CHECK-NOT: getelementptr i8, {{.+}} 40
  %v1 = load i32, i32* %casted, align 4
  call void @foo(i32 %v1)
  %cmp = icmp eq i32 %v1, 0
  br i1 %cmp, label %rare.1, label %fallthrough

fallthrough:
  ret void

rare.1:
  call void @slowpath(i32 %v1, i32* %casted) cold
  br label %fallthrough
}


; Make sure sinking two copies of addressing mode into different blocks works
; when there are cold paths for each.
define void @test7(i1 %cond, i64* %base) {
; CHECK-LABEL: @test7
entry:
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br i1 %cond, label %if.then, label %fallthrough

if.then:
; CHECK-LABEL: if.then:
; CHECK: getelementptr i8, {{.+}} 40
  %v1 = load i32, i32* %casted, align 4
  call void @foo(i32 %v1)
  %cmp = icmp eq i32 %v1, 0
  br i1 %cmp, label %rare.1, label %next

next:
; CHECK-LABEL: next:
; CHECK: getelementptr i8, {{.+}} 40
  %v2 = load i32, i32* %casted, align 4
  call void @foo(i32 %v2)
  %cmp2 = icmp eq i32 %v2, 0
  br i1 %cmp2, label %rare.1, label %fallthrough

fallthrough:
  ret void

rare.1:
; CHECK-LABEL: rare.1:
; CHECK: getelementptr i8, {{.+}} 40
  call void @slowpath(i32 %v1, i32* %casted) cold
  br label %next

rare.2:
; CHECK-LABEL: rare.2:
; CHECK: getelementptr i8, {{.+}} 40
  call void @slowpath(i32 %v2, i32* %casted) cold
  br label %fallthrough
}

declare void @slowpath(i32, i32*)

; Make sure we don't end up in an infinite loop after we fail to sink.
; CHECK-LABEL: define void @test8
; CHECK: %ptr = getelementptr i8, i8* %aFOO_load_ptr2int_2void, i32 undef
define void @test8() {
allocas:
  %aFOO_load = load float*, float** undef
  %aFOO_load_ptr2int = ptrtoint float* %aFOO_load to i64
  %aFOO_load_ptr2int_broadcast_init = insertelement <4 x i64> undef, i64 %aFOO_load_ptr2int, i32 0
  %aFOO_load_ptr2int_2void = inttoptr i64 %aFOO_load_ptr2int to i8*
  %ptr = getelementptr i8, i8* %aFOO_load_ptr2int_2void, i32 undef
  br label %load.i145

load.i145:
  %ptr.i143 = bitcast i8* %ptr to <4 x float>*
  %valall.i144 = load <4 x float>, <4 x float>* %ptr.i143, align 4
  %x_offset = getelementptr [1 x [2 x <4 x float>]], [1 x [2 x <4 x float>]]* @x, i32 0, i64 0
  br label %pl_loop.i.i122

pl_loop.i.i122:
  br label %pl_loop.i.i122
}

; Make sure we can sink address computation even
; if there is a cycle in phi nodes.
define void @test9(i1 %cond, i64* %base) {
; CHECK-LABEL: @test9
entry:
  %addr = getelementptr inbounds i64, i64* %base, i64 5
  %casted = bitcast i64* %addr to i32*
  br label %header

header:
  %iv = phi i32 [0, %entry], [%iv.inc, %backedge]
  %casted.loop = phi i32* [%casted, %entry], [%casted.merged, %backedge]
  br i1 %cond, label %if.then, label %backedge

if.then:
  call void @foo(i32 %iv)
  %addr.1 = getelementptr inbounds i64, i64* %base, i64 5
  %casted.1 = bitcast i64* %addr.1 to i32*
  br label %backedge

backedge:
; CHECK-LABEL: backedge:
; CHECK: getelementptr i8, {{.+}} 40
  %casted.merged = phi i32* [%casted.loop, %header], [%casted.1, %if.then]
  %v = load i32, i32* %casted.merged, align 4
  call void @foo(i32 %v)
  %iv.inc = add i32 %iv, 1
  %cmp = icmp slt i32 %iv.inc, 1000
  br i1 %cmp, label %header, label %exit

exit:
  ret void
}
