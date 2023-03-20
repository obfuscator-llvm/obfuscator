; RUN: opt -functionattrs -S < %s | FileCheck %s --check-prefix=FNATTR
; RUN: opt -attributor -attributor-disable=false -S < %s | FileCheck %s --check-prefix=ATTRIBUTOR
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

; Test cases designed for the nosync function attribute.
; FIXME's are used to indicate problems and missing attributes.

; struct RT {
;   char A;
;   int B[10][20];
;   char C;
; };
; struct ST {
;   int X;
;   double Y;
;   struct RT Z;
; };
;
; int *foo(struct ST *s) {
;   return &s[1].Z.B[5][13];
; }

; TEST 1
; non-convergent and readnone implies nosync
%struct.RT = type { i8, [10 x [20 x i32]], i8 }
%struct.ST = type { i32, double, %struct.RT }

; FNATTR: Function Attrs: norecurse nounwind optsize readnone ssp uwtable
; FNATTR-NEXT: define nonnull i32* @foo(%struct.ST* readnone %s)
; ATTRIBUTOR: Function Attrs: nofree nosync nounwind optsize readnone ssp uwtable
; ATTRIBUTOR-NEXT: define nonnull i32* @foo(%struct.ST* %s)
define i32* @foo(%struct.ST* %s) nounwind uwtable readnone optsize ssp {
entry:
  %arrayidx = getelementptr inbounds %struct.ST, %struct.ST* %s, i64 1, i32 2, i32 1, i64 5, i64 13
  ret i32* %arrayidx
}

; TEST 2
; atomic load with monotonic ordering
; int load_monotonic(_Atomic int *num) {
;   int n = atomic_load_explicit(num, memory_order_relaxed);
;   return n;
; }

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define i32 @load_monotonic(i32* nocapture readonly)
; ATTRIBUTOR: Function Attrs: nofree norecurse nosync nounwind uwtable
; ATTRIBUTOR-NEXT: define i32 @load_monotonic(i32* nocapture readonly)
define i32 @load_monotonic(i32* nocapture readonly) norecurse nounwind uwtable {
  %2 = load atomic i32, i32* %0 monotonic, align 4
  ret i32 %2
}


; TEST 3
; atomic store with monotonic ordering.
; void store_monotonic(_Atomic int *num) {
;   atomic_load_explicit(num, memory_order_relaxed);
; }

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define void @store_monotonic(i32* nocapture)
; ATTRIBUTOR: Function Attrs: nofree norecurse nosync nounwind uwtable
; ATTRIBUTOR-NEXT: define void @store_monotonic(i32* nocapture)
define void @store_monotonic(i32* nocapture) norecurse nounwind uwtable {
  store atomic i32 10, i32* %0 monotonic, align 4
  ret void
}

; TEST 4 - negative, should not deduce nosync
; atomic load with acquire ordering.
; int load_acquire(_Atomic int *num) {
;   int n = atomic_load_explicit(num, memory_order_acquire);
;   return n;
; }

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define i32 @load_acquire(i32* nocapture readonly)
; ATTRIBUTOR: Function Attrs: nofree norecurse nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define i32 @load_acquire(i32* nocapture readonly)
define i32 @load_acquire(i32* nocapture readonly) norecurse nounwind uwtable {
  %2 = load atomic i32, i32* %0 acquire, align 4
  ret i32 %2
}

; TEST 5 - negative, should not deduce nosync
; atomic load with release ordering
; void load_release(_Atomic int *num) {
;   atomic_store_explicit(num, 10, memory_order_release);
; }

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define void @load_release(i32* nocapture)
; ATTRIBUTOR: Function Attrs: nofree norecurse nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define void @load_release(i32* nocapture)
define void @load_release(i32* nocapture) norecurse nounwind uwtable {
  store atomic volatile i32 10, i32* %0 release, align 4
  ret void
}

; TEST 6 - negative volatile, relaxed atomic

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define void @load_volatile_release(i32* nocapture)
; ATTRIBUTOR: Function Attrs: nofree norecurse nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define void @load_volatile_release(i32* nocapture)
define void @load_volatile_release(i32* nocapture) norecurse nounwind uwtable {
  store atomic volatile i32 10, i32* %0 release, align 4
  ret void
}

; TEST 7 - negative, should not deduce nosync
; volatile store.
; void volatile_store(volatile int *num) {
;   *num = 14;
; }

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define void @volatile_store(i32*)
; ATTRIBUTOR: Function Attrs: nofree norecurse nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define void @volatile_store(i32*)
define void @volatile_store(i32*) norecurse nounwind uwtable {
  store volatile i32 14, i32* %0, align 4
  ret void
}

; TEST 8 - negative, should not deduce nosync
; volatile load.
; int volatile_load(volatile int *num) {
;   int n = *num;
;   return n;
; }

; FNATTR: Function Attrs: nofree norecurse nounwind uwtable
; FNATTR-NEXT: define i32 @volatile_load(i32*)
; ATTRIBUTOR: Function Attrs: nofree norecurse nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define i32 @volatile_load(i32*)
define i32 @volatile_load(i32*) norecurse nounwind uwtable {
  %2 = load volatile i32, i32* %0, align 4
  ret i32 %2
}

; TEST 9

; FNATTR: Function Attrs: noinline nosync nounwind uwtable
; FNATTR-NEXT: declare void @nosync_function()
; ATTRIBUTOR: Function Attrs: noinline nosync nounwind uwtable
; ATTRIBUTOR-NEXT: declare void @nosync_function()
declare void @nosync_function() noinline nounwind uwtable nosync

; FNATTR: Function Attrs: noinline nounwind uwtable
; FNATTR-NEXT: define void @call_nosync_function()
; ATTRIBUTOR: Function Attrs: noinline nosync nounwind uwtable
; ATTRIBUTOR-next: define void @call_nosync_function()
define void @call_nosync_function() nounwind uwtable noinline {
  tail call void @nosync_function() noinline nounwind uwtable
  ret void
}

; TEST 10 - negative, should not deduce nosync

; FNATTR: Function Attrs: noinline nounwind uwtable
; FNATTR-NEXT: declare void @might_sync()
; ATTRIBUTOR: Function Attrs: noinline nounwind uwtable
; ATTRIBUTOR-NEXT: declare void @might_sync()
declare void @might_sync() noinline nounwind uwtable

; FNATTR: Function Attrs: noinline nounwind uwtable
; FNATTR-NEXT: define void @call_might_sync()
; ATTRIBUTOR: Function Attrs: noinline nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define void @call_might_sync()
define void @call_might_sync() nounwind uwtable noinline {
  tail call void @might_sync() noinline nounwind uwtable
  ret void
}

; TEST 11 - negative, should not deduce nosync
; volatile operation in same scc. Call volatile_load defined in TEST 8.

; FNATTR: Function Attrs: nofree noinline nounwind uwtable
; FNATTR-NEXT: define i32 @scc1(i32*)
; ATTRIBUTOR: Function Attrs: nofree noinline nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define i32 @scc1(i32*)
define i32 @scc1(i32*) noinline nounwind uwtable {
  tail call void @scc2(i32* %0);
  %val = tail call i32 @volatile_load(i32* %0);
  ret i32 %val;
}

; FNATTR: Function Attrs: nofree noinline nounwind uwtable
; FNATTR-NEXT: define void @scc2(i32*)
; ATTRIBUTOR: Function Attrs: nofree noinline nounwind uwtable
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define void @scc2(i32*)
define void @scc2(i32*) noinline nounwind uwtable {
  tail call i32 @scc1(i32* %0);
  ret void;
}

; TEST 12 - fences, negative
;
; void foo1(int *a, std::atomic<bool> flag){
;   *a = 100;
;   atomic_thread_fence(std::memory_order_release);
;   flag.store(true, std::memory_order_relaxed);
; }
;
; void bar(int *a, std::atomic<bool> flag){
;   while(!flag.load(std::memory_order_relaxed))
;     ;
;
;   atomic_thread_fence(std::memory_order_acquire);
;   int b = *a;
; }

%"struct.std::atomic" = type { %"struct.std::__atomic_base" }
%"struct.std::__atomic_base" = type { i8 }

; FNATTR: Function Attrs: nofree norecurse nounwind
; FNATTR-NEXT: define void @foo1(i32* nocapture, %"struct.std::atomic"* nocapture)
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR: define void @foo1(i32*, %"struct.std::atomic"*)
define void @foo1(i32*, %"struct.std::atomic"*) {
  store i32 100, i32* %0, align 4
  fence release
  %3 = getelementptr inbounds %"struct.std::atomic", %"struct.std::atomic"* %1, i64 0, i32 0, i32 0
  store atomic i8 1, i8* %3 monotonic, align 1
  ret void
}

; FNATTR: Function Attrs: nofree norecurse nounwind
; FNATTR-NEXT: define void @bar(i32* nocapture readnone, %"struct.std::atomic"* nocapture readonly)
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR: define void @bar(i32*, %"struct.std::atomic"*)
define void @bar(i32 *, %"struct.std::atomic"*) {
  %3 = getelementptr inbounds %"struct.std::atomic", %"struct.std::atomic"* %1, i64 0, i32 0, i32 0
  br label %4

4:                                                ; preds = %4, %2
  %5 = load atomic i8, i8* %3  monotonic, align 1
  %6 = and i8 %5, 1
  %7 = icmp eq i8 %6, 0
  br i1 %7, label %4, label %8

8:                                                ; preds = %4
  fence acquire
  ret void
}

; TEST 13 - Fence syncscope("singlethread") seq_cst
; FNATTR: Function Attrs: nofree norecurse nounwind
; FNATTR-NEXT: define void @foo1_singlethread(i32* nocapture, %"struct.std::atomic"* nocapture)
; ATTRIBUTOR: Function Attrs: nofree nosync
; ATTRIBUTOR: define void @foo1_singlethread(i32*, %"struct.std::atomic"*)
define void @foo1_singlethread(i32*, %"struct.std::atomic"*) {
  store i32 100, i32* %0, align 4
  fence syncscope("singlethread") release
  %3 = getelementptr inbounds %"struct.std::atomic", %"struct.std::atomic"* %1, i64 0, i32 0, i32 0
  store atomic i8 1, i8* %3 monotonic, align 1
  ret void
}

; FNATTR: Function Attrs: nofree norecurse nounwind
; FNATTR-NEXT: define void @bar_singlethread(i32* nocapture readnone, %"struct.std::atomic"* nocapture readonly)
; ATTRIBUTOR: Function Attrs: nofree nosync
; ATTRIBUTOR: define void @bar_singlethread(i32*, %"struct.std::atomic"*)
define void @bar_singlethread(i32 *, %"struct.std::atomic"*) {
  %3 = getelementptr inbounds %"struct.std::atomic", %"struct.std::atomic"* %1, i64 0, i32 0, i32 0
  br label %4

4:                                                ; preds = %4, %2
  %5 = load atomic i8, i8* %3  monotonic, align 1
  %6 = and i8 %5, 1
  %7 = icmp eq i8 %6, 0
  br i1 %7, label %4, label %8

8:                                                ; preds = %4
  fence syncscope("singlethread") acquire
  ret void
}

declare void @llvm.memcpy(i8* %dest, i8* %src, i32 %len, i1 %isvolatile)
declare void @llvm.memset(i8* %dest, i8 %val, i32 %len, i1 %isvolatile)

; TEST 14 - negative, checking volatile intrinsics.

; ATTRIBUTOR: Function Attrs: nounwind
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define i32 @memcpy_volatile(i8* %ptr1, i8* %ptr2)
define i32 @memcpy_volatile(i8* %ptr1, i8* %ptr2) {
  call void @llvm.memcpy(i8* %ptr1, i8* %ptr2, i32 8, i1 1)
  ret i32 4
}

; TEST 15 - positive, non-volatile intrinsic.

; ATTRIBUTOR: Function Attrs: nosync
; ATTRIBUTOR-NEXT: define i32 @memset_non_volatile(i8* %ptr1, i8 %val)
define i32 @memset_non_volatile(i8* %ptr1, i8 %val) {
  call void @llvm.memset(i8* %ptr1, i8 %val, i32 8, i1 0)
  ret i32 4
}

; TEST 16 - negative, inline assembly.

; ATTRIBUTOR: define i32 @inline_asm_test(i32 %x)
define i32 @inline_asm_test(i32 %x) {
  call i32 asm "bswap $0", "=r,r"(i32 %x)
  ret i32 4
}

declare void @readnone_test() convergent readnone

; ATTRIBUTOR: define void @convergent_readnone()
; TEST 17 - negative. Convergent
define void @convergent_readnone(){
    call void @readnone_test()
    ret void
}

; ATTRIBUTOR: Function Attrs: nounwind
; ATTRIBUTOR-NEXT: declare void @llvm.x86.sse2.clflush(i8*)
declare void @llvm.x86.sse2.clflush(i8*)
@a = common global i32 0, align 4

; TEST 18 - negative. Synchronizing intrinsic

; ATTRIBUTOR: Function Attrs: nounwind
; ATTRIBUTOR-NOT: nosync
; ATTRIBUTOR-NEXT: define void @i_totally_sync()
define void @i_totally_sync() {
  tail call void @llvm.x86.sse2.clflush(i8* bitcast (i32* @a to i8*))
  ret void
}

declare float @llvm.cos(float %val) readnone

; TEST 19 - positive, readnone & non-convergent intrinsic.

; ATTRIBUTOR: Function Attrs: nosync nounwind
; ATTRIBUTOR-NEXT: define i32 @cos_test(float %x)
define i32 @cos_test(float %x) {
  call float @llvm.cos(float %x)
  ret i32 4
}
