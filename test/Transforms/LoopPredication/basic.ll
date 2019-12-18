; RUN: opt -S -loop-predication < %s 2>&1 | FileCheck %s
; RUN: opt -S -passes='require<scalar-evolution>,loop(loop-predication)' < %s 2>&1 | FileCheck %s

declare void @llvm.experimental.guard(i1, ...)

define i32 @unsigned_loop_0_to_n_ult_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @unsigned_loop_0_to_n_ult_check
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp ule i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @unsigned_loop_0_to_n_ule_latch_ult_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @unsigned_loop_0_to_n_ule_latch_ult_check
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp ult i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ule i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @unsigned_loop_0_to_n_ugt_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @unsigned_loop_0_to_n_ugt_check
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp ule i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ugt i32 %length, %i
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_ult_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_ult_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp sle i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_ult_check_length_range_known(i32* %array, i32* %length.ptr, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_ult_check_length_range_known
entry:
  %tmp5 = icmp sle i32 %n, 0
  %length = load i32, i32* %length.ptr, !range !{i32 1, i32 2147483648}
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp sle i32 %n, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 true, [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_inverse_latch_predicate(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_inverse_latch_predicate
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp slt i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp sgt i32 %i.next, %n
  br i1 %continue, label %exit, label %loop

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_sle_latch_ult_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_sle_latch_ult_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp slt i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp sle i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_preincrement_latch_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_preincrement_latch_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[length_minus_1:[^ ]+]] = add i32 %length, -1
; CHECK-NEXT: [[limit_check:[^ ]+]] = icmp sle i32 %n, [[length_minus_1]]
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add i32 %i, 1
  %continue = icmp slt i32 %i, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_preincrement_latch_check_postincrement_guard_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_preincrement_latch_check_postincrement_guard_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[length_minus_2:[^ ]+]] = add i32 %length, -2
; CHECK-NEXT: [[limit_check:[^ ]+]] = icmp sle i32 %n, [[length_minus_2]]
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 1, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]

  %i.next = add i32 %i, 1
  %within.bounds = icmp ult i32 %i.next, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %continue = icmp slt i32 %i, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_sle_latch_offset_ult_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_sle_latch_offset_ult_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[length_minus_1:[^ ]+]] = add i32 %length, -1
; CHECK-NEXT: [[limit_check:[^ ]+]] = icmp slt i32 %n, [[length_minus_1]]
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 1, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %i.offset = add i32 %i, 1
  %within.bounds = icmp ult i32 %i.offset, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add i32 %i, 1
  %continue = icmp sle i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_offset_sle_latch_offset_ult_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_offset_sle_latch_offset_ult_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp slt i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 1, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %i.offset = add i32 %i, 1
  %within.bounds = icmp ult i32 %i.offset, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add i32 %i, 1
  %i.next.offset = add i32 %i.next, 1
  %continue = icmp sle i32 %i.next.offset, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @unsupported_latch_pred_loop_0_to_n(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @unsupported_latch_pred_loop_0_to_n
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %within.bounds = icmp ult i32 %i, %length
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nsw i32 %i, 1
  %continue = icmp ne i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_unsupported_iv_step(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_unsupported_iv_step
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %within.bounds = icmp ult i32 %i, %length
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nsw i32 %i, 2
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_equal_iv_range_check(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_equal_iv_range_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp sle i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %j = phi i32 [ %j.next, %loop ], [ 0, %loop.preheader ]

  %within.bounds = icmp ult i32 %j, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %j.next = add nsw i32 %j, 1
  %i.next = add nsw i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_start_to_n_offset_iv_range_check(i32* %array, i32 %start.i,
                                                         i32 %start.j, i32 %length,
                                                         i32 %n) {
; CHECK-LABEL: @signed_loop_start_to_n_offset_iv_range_check
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[length_plus_start_i:[^ ]+]] = add i32 %length, %start.i
; CHECK-NEXT: [[limit:[^ ]+]] = sub i32 [[length_plus_start_i]], %start.j
; CHECK-NEXT: [[limit_check:[^ ]+]] = icmp sle i32 %n, [[limit]]
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 %start.j, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ %start.i, %loop.preheader ]
  %j = phi i32 [ %j.next, %loop ], [ %start.j, %loop.preheader ]

  %within.bounds = icmp ult i32 %j, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %j.next = add i32 %j, 1
  %i.next = add i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_different_iv_types(i32* %array, i16 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_different_iv_types
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %within.bounds = icmp ult i16 %j, %length
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %j = phi i16 [ %j.next, %loop ], [ 0, %loop.preheader ]

  %within.bounds = icmp ult i16 %j, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %j.next = add i16 %j, 1
  %i.next = add i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_0_to_n_different_iv_strides(i32* %array, i32 %length, i32 %n) {
; CHECK-LABEL: @signed_loop_0_to_n_different_iv_strides
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %within.bounds = icmp ult i32 %j, %length
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %j = phi i32 [ %j.next, %loop ], [ 0, %loop.preheader ]

  %within.bounds = icmp ult i32 %j, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %j.next = add nsw i32 %j, 2
  %i.next = add nsw i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @two_range_checks(i32* %array.1, i32 %length.1,
                             i32* %array.2, i32 %length.2, i32 %n) {
; CHECK-LABEL: @two_range_checks
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check_1:[^ ]+]] = icmp ule i32 %n, %length.{{1|2}}
; CHECK-NEXT: [[first_iteration_check_1:[^ ]+]] = icmp ult i32 0, %length.{{1|2}}
; CHECK-NEXT: [[wide_cond_1:[^ ]+]] = and i1 [[first_iteration_check_1]], [[limit_check_1]]
; CHECK-NEXT: [[limit_check_2:[^ ]+]] = icmp ule i32 %n, %length.{{1|2}}
; CHECK-NEXT: [[first_iteration_check_2:[^ ]+]] = icmp ult i32 0, %length.{{1|2}}
; CHECK-NEXT: [[wide_cond_2:[^ ]+]] = and i1 [[first_iteration_check_2]], [[limit_check_2]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: [[wide_cond:[^ ]+]] = and i1 [[wide_cond_1]], [[wide_cond_2]]
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds.1 = icmp ult i32 %i, %length.1
  %within.bounds.2 = icmp ult i32 %i, %length.2
  %within.bounds = and i1 %within.bounds.1, %within.bounds.2
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.1.i.ptr = getelementptr inbounds i32, i32* %array.1, i64 %i.i64
  %array.1.i = load i32, i32* %array.1.i.ptr, align 4
  %loop.acc.1 = add i32 %loop.acc, %array.1.i

  %array.2.i.ptr = getelementptr inbounds i32, i32* %array.2, i64 %i.i64
  %array.2.i = load i32, i32* %array.2.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc.1, %array.2.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @three_range_checks(i32* %array.1, i32 %length.1,
                               i32* %array.2, i32 %length.2,
                               i32* %array.3, i32 %length.3, i32 %n) {
; CHECK-LABEL: @three_range_checks
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check_1:[^ ]+]] = icmp ule i32 %n, %length.{{1|2|3}}
; CHECK-NEXT: [[first_iteration_check_1:[^ ]+]] = icmp ult i32 0, %length.{{1|2|3}}
; CHECK-NEXT: [[wide_cond_1:[^ ]+]] = and i1 [[first_iteration_check_1]], [[limit_check_1]]
; CHECK-NEXT: [[limit_check_2:[^ ]+]] = icmp ule i32 %n, %length.{{1|2|3}}
; CHECK-NEXT: [[first_iteration_check_2:[^ ]+]] = icmp ult i32 0, %length.{{1|2|3}}
; CHECK-NEXT: [[wide_cond_2:[^ ]+]] = and i1 [[first_iteration_check_2]], [[limit_check_2]]
; CHECK-NEXT: [[limit_check_3:[^ ]+]] = icmp ule i32 %n, %length.{{1|2|3}}
; CHECK-NEXT: [[first_iteration_check_3:[^ ]+]] = icmp ult i32 0, %length.{{1|2|3}}
; CHECK-NEXT: [[wide_cond_3:[^ ]+]] = and i1 [[first_iteration_check_3]], [[limit_check_3]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: [[wide_cond_and:[^ ]+]] = and i1 [[wide_cond_1]], [[wide_cond_2]]
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[wide_cond_and]], [[wide_cond_3]]
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds.1 = icmp ult i32 %i, %length.1
  %within.bounds.2 = icmp ult i32 %i, %length.2
  %within.bounds.3 = icmp ult i32 %i, %length.3
  %within.bounds.1.and.2 = and i1 %within.bounds.1, %within.bounds.2
  %within.bounds = and i1 %within.bounds.1.and.2, %within.bounds.3
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.1.i.ptr = getelementptr inbounds i32, i32* %array.1, i64 %i.i64
  %array.1.i = load i32, i32* %array.1.i.ptr, align 4
  %loop.acc.1 = add i32 %loop.acc, %array.1.i

  %array.2.i.ptr = getelementptr inbounds i32, i32* %array.2, i64 %i.i64
  %array.2.i = load i32, i32* %array.2.i.ptr, align 4
  %loop.acc.2 = add i32 %loop.acc.1, %array.2.i

  %array.3.i.ptr = getelementptr inbounds i32, i32* %array.3, i64 %i.i64
  %array.3.i = load i32, i32* %array.3.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc.2, %array.3.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @three_guards(i32* %array.1, i32 %length.1,
                         i32* %array.2, i32 %length.2,
                         i32* %array.3, i32 %length.3, i32 %n) {
; CHECK-LABEL: @three_guards
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check_1:[^ ]+]] = icmp ule i32 %n, %length.{{1|2|3}}
; CHECK-NEXT: [[first_iteration_check_1:[^ ]+]] = icmp ult i32 0, %length.{{1|2|3}}
; CHECK-NEXT: [[wide_cond_1:[^ ]+]] = and i1 [[first_iteration_check_1]], [[limit_check_1]]
; CHECK-NEXT: [[limit_check_2:[^ ]+]] = icmp ule i32 %n, %length.{{1|2|3}}
; CHECK-NEXT: [[first_iteration_check_2:[^ ]+]] = icmp ult i32 0, %length.{{1|2|3}}
; CHECK-NEXT: [[wide_cond_2:[^ ]+]] = and i1 [[first_iteration_check_2]], [[limit_check_2]]
; CHECK-NEXT: [[limit_check_3:[^ ]+]] = icmp ule i32 %n, %length.{{1|2|3}}
; CHECK-NEXT: [[first_iteration_check_3:[^ ]+]] = icmp ult i32 0, %length.{{1|2|3}}
; CHECK-NEXT: [[wide_cond_3:[^ ]+]] = and i1 [[first_iteration_check_3]], [[limit_check_3]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond_1]], i32 9) [ "deopt"() ]
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond_2]], i32 9) [ "deopt"() ]
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond_3]], i32 9) [ "deopt"() ]

  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]

  %within.bounds.1 = icmp ult i32 %i, %length.1
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds.1, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.1.i.ptr = getelementptr inbounds i32, i32* %array.1, i64 %i.i64
  %array.1.i = load i32, i32* %array.1.i.ptr, align 4
  %loop.acc.1 = add i32 %loop.acc, %array.1.i

  %within.bounds.2 = icmp ult i32 %i, %length.2
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds.2, i32 9) [ "deopt"() ]

  %array.2.i.ptr = getelementptr inbounds i32, i32* %array.2, i64 %i.i64
  %array.2.i = load i32, i32* %array.2.i.ptr, align 4
  %loop.acc.2 = add i32 %loop.acc.1, %array.2.i

  %within.bounds.3 = icmp ult i32 %i, %length.3
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds.3, i32 9) [ "deopt"() ]

  %array.3.i.ptr = getelementptr inbounds i32, i32* %array.3, i64 %i.i64
  %array.3.i = load i32, i32* %array.3.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc.2, %array.3.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @unsigned_loop_0_to_n_unrelated_condition(i32* %array, i32 %length, i32 %n, i32 %x) {
; CHECK-LABEL: @unsigned_loop_0_to_n_unrelated_condition
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[limit_check:[^ ]+]] = icmp ule i32 %n, %length
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, %length
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %unrelated.cond = icmp ult i32 %x, %length
; CHECK: [[guard_cond:[^ ]+]] = and i1 %unrelated.cond, [[wide_cond]]
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 [[guard_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  %unrelated.cond = icmp ult i32 %x, %length
  %guard.cond = and i1 %within.bounds, %unrelated.cond
  call void (i1, ...) @llvm.experimental.guard(i1 %guard.cond, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

; Don't change the guard condition if there were no widened subconditions
define i32 @test_no_widened_conditions(i32* %array, i32 %length, i32 %n, i32 %x1, i32 %x2, i32 %x3) {
; CHECK-LABEL: @test_no_widened_conditions
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %unrelated.cond.1 = icmp eq i32 %x1, %i
; CHECK-NEXT: %unrelated.cond.2 = icmp eq i32 %x2, %i
; CHECK-NEXT: %unrelated.cond.3 = icmp eq i32 %x3, %i
; CHECK-NEXT: %unrelated.cond.and.1 = and i1 %unrelated.cond.1, %unrelated.cond.2
; CHECK-NEXT: %guard.cond = and i1 %unrelated.cond.and.1, %unrelated.cond.3
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %guard.cond, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %unrelated.cond.1 = icmp eq i32 %x1, %i
  %unrelated.cond.2 = icmp eq i32 %x2, %i
  %unrelated.cond.3 = icmp eq i32 %x3, %i
  %unrelated.cond.and.1 = and i1 %unrelated.cond.1, %unrelated.cond.2
  %guard.cond = and i1 %unrelated.cond.and.1, %unrelated.cond.3

  call void (i1, ...) @llvm.experimental.guard(i1 %guard.cond, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_start_to_n_loop_variant_bound(i32* %array, i32 %x, i32 %start, i32 %n) {
; CHECK-LABEL: @signed_loop_start_to_n_loop_variant_bound
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %bound = add i32 %i, %x
; CHECK-NEXT: %within.bounds = icmp ult i32 %i, %bound
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ %start, %loop.preheader ]
  %bound = add i32 %i, %x
  %within.bounds = icmp ult i32 %i, %bound
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nsw i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @signed_loop_start_to_n_non_monotonic_predicate(i32* %array, i32 %x, i32 %start, i32 %n) {
; CHECK-LABEL: @signed_loop_start_to_n_non_monotonic_predicate
entry:
  %tmp5 = icmp sle i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: %guard.cond = icmp eq i32 %i, %x
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %guard.cond, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ %start, %loop.preheader ]
  %guard.cond = icmp eq i32 %i, %x
  call void (i1, ...) @llvm.experimental.guard(i1 %guard.cond, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nsw i32 %i, 1
  %continue = icmp slt i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @unsigned_loop_0_to_n_hoist_length(i32* %array, i16 %length.i16, i32 %n) {
; CHECK-LABEL: @unsigned_loop_0_to_n_hoist_length
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK: [[length:[^ ]+]] = zext i16 %length.i16 to i32
; CHECK-NEXT: [[limit_check:[^ ]+]] = icmp ule i32 %n, [[length]]
; CHECK-NEXT: [[first_iteration_check:[^ ]+]] = icmp ult i32 0, [[length]]
; CHECK-NEXT: [[wide_cond:[^ ]+]] = and i1 [[first_iteration_check]], [[limit_check]]
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK: call void (i1, ...) @llvm.experimental.guard(i1 [[wide_cond]], i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %length = zext i16 %length.i16 to i32
  %within.bounds = icmp ult i32 %i, %length
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}

define i32 @unsigned_loop_0_to_n_cant_hoist_length(i32* %array, i32 %length, i32 %divider, i32 %n) {
; CHECK-LABEL: @unsigned_loop_0_to_n_cant_hoist_length
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
; CHECK: loop.preheader:
; CHECK-NEXT: br label %loop
  br label %loop

loop:
; CHECK: loop:
; CHECK-NEXT: %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
; CHECK-NEXT: %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
; CHECK-NEXT: %length.udiv = udiv i32 %length, %divider
; CHECK-NEXT: %within.bounds = icmp ult i32 %i, %length.udiv
; CHECK-NEXT: call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %length.udiv = udiv i32 %length, %divider
  %within.bounds = icmp ult i32 %i, %length.udiv
  call void (i1, ...) @llvm.experimental.guard(i1 %within.bounds, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}
