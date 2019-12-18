;; Check that runtime symbols get appropriate linkage.

; RUN: opt < %s -mtriple=x86_64-apple-macosx10.10.0 -instrprof -S | FileCheck %s --check-prefixes=COMMON,MACHO
; RUN: opt < %s -mtriple=x86_64-unknown-linux -instrprof -S | FileCheck %s --check-prefixes=COMMON,LINUX
; RUN: opt < %s  -mtriple=x86_64-pc-win32-coff -instrprof -S | FileCheck %s --check-prefixes=COMMON,COFF
; RUN: opt < %s -mtriple=x86_64-apple-macosx10.10.0 -passes=instrprof -S | FileCheck %s --check-prefixes=COMMON,MACHO
; RUN: opt < %s -mtriple=x86_64-unknown-linux -passes=instrprof -S | FileCheck %s --check-prefixes=COMMON,LINUX
; RUN: opt < %s  -mtriple=x86_64-pc-win32-coff -passes=instrprof -S | FileCheck %s --check-prefixes=COMMON,COFF

; MACHO: @__llvm_profile_runtime = external global i32
; LINUX-NOT: @__llvm_profile_runtime = external global i32

@__profn_foo = hidden constant [3 x i8] c"foo"
@__profn_foo_weak = weak hidden constant [8 x i8] c"foo_weak"
@"__profn_linkage.ll:foo_internal" = internal constant [23 x i8] c"linkage.ll:foo_internal"
@__profn_foo_inline = linkonce_odr hidden constant [10 x i8] c"foo_inline"
@__profn_foo_extern = linkonce_odr hidden constant [10 x i8] c"foo_extern"

; COMMON: @__profc_foo = hidden global
; COMMON: @__profd_foo = hidden global
define void @foo() {
  call void @llvm.instrprof.increment(i8* getelementptr inbounds ([3 x i8], [3 x i8]* @__profn_foo, i32 0, i32 0), i64 0, i32 1, i32 0)
  ret void
}

; COMMON: @__profc_foo_weak = weak hidden global
; COMMON: @__profd_foo_weak = weak hidden global
define weak void @foo_weak() {
  call void @llvm.instrprof.increment(i8* getelementptr inbounds ([8 x i8], [8 x i8]* @__profn_foo_weak, i32 0, i32 0), i64 0, i32 1, i32 0)
  ret void
}

; COMMON: @"__profc_linkage.ll:foo_internal" = internal global
; COMMON: @"__profd_linkage.ll:foo_internal" = internal global
define internal void @foo_internal() {
  call void @llvm.instrprof.increment(i8* getelementptr inbounds ([23 x i8], [23 x i8]* @"__profn_linkage.ll:foo_internal", i32 0, i32 0), i64 0, i32 1, i32 0)
  ret void
}

; COMMON: @__profc_foo_inline = linkonce_odr hidden global
; COFF-SAME: section ".lprfc", align 8
; COMMON: @__profd_foo_inline = linkonce_odr hidden global
; COFF-SAME: section ".lprfd", align 8
define linkonce_odr void @foo_inline() {
  call void @llvm.instrprof.increment(i8* getelementptr inbounds ([10 x i8], [10 x i8]* @__profn_foo_inline, i32 0, i32 0), i64 0, i32 1, i32 0)
  ret void
}

; LINUX: @__profc_foo_extern = linkonce_odr hidden global {{.*}}section "__llvm_prf_cnts", comdat($__profv_foo_extern), align 8
; LINUX: @__profd_foo_extern = linkonce_odr hidden global {{.*}}section "__llvm_prf_data", comdat($__profv_foo_extern), align 8
; MACHO: @__profc_foo_extern = linkonce_odr hidden global
; MACHO: @__profd_foo_extern = linkonce_odr hidden global
; COFF: @__profc_foo_extern = linkonce_odr hidden global {{.*}}section ".lprfc", comdat, align 8
; COFF: @__profd_foo_extern = linkonce_odr hidden global {{.*}}section ".lprfd", comdat($__profc_foo_extern), align 8
define available_externally void @foo_extern() {
  call void @llvm.instrprof.increment(i8* getelementptr inbounds ([10 x i8], [10 x i8]* @__profn_foo_extern, i32 0, i32 0), i64 0, i32 1, i32 0)
  ret void
}

declare void @llvm.instrprof.increment(i8*, i64, i32, i32)

; MACHO: define linkonce_odr hidden i32 @__llvm_profile_runtime_user() {{.*}} {
; MACHO:   %[[REG:.*]] = load i32, i32* @__llvm_profile_runtime
; MACHO:   ret i32 %[[REG]]
; MACHO: }
; COFF: define linkonce_odr hidden i32 @__llvm_profile_runtime_user() {{.*}} comdat {
; LINUX-NOT: define linkonce_odr hidden i32 @__llvm_profile_runtime_user() {{.*}} {
; LINUX-NOT:   %[[REG:.*]] = load i32, i32* @__llvm_profile_runtime
