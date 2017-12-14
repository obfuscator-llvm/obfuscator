; RUN: not llc -O0 -global-isel -verify-machineinstrs %s -o - 2>&1 | FileCheck %s --check-prefix=ERROR
; RUN: llc -O0 -global-isel -global-isel-abort=0 -verify-machineinstrs %s -o - 2>&1 | FileCheck %s --check-prefix=FALLBACK
; RUN: llc -O0 -global-isel -global-isel-abort=2 -pass-remarks-missed='gisel*' -verify-machineinstrs %s -o %t.out 2> %t.err
; RUN: FileCheck %s --check-prefix=FALLBACK-WITH-REPORT-OUT < %t.out
; RUN: FileCheck %s --check-prefix=FALLBACK-WITH-REPORT-ERR < %t.err
; This file checks that the fallback path to selection dag works.
; The test is fragile in the sense that it must be updated to expose
; something that fails with global-isel.
; When we cannot produce a test case anymore, that means we can remove
; the fallback path.

target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "aarch64--"

; We use __fixunstfti as the common denominator for __fixunstfti on Linux and
; ___fixunstfti on iOS
; ERROR: unable to lower arguments: i128 (i128)* (in function: ABIi128)
; FALLBACK: ldr q0,
; FALLBACK-NEXT: bl __fixunstfti
;
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to lower arguments: i128 (i128)* (in function: ABIi128)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for ABIi128
; FALLBACK-WITH-REPORT-OUT-LABEL: ABIi128:
; FALLBACK-WITH-REPORT-OUT: ldr q0,
; FALLBACK-WITH-REPORT-OUT-NEXT: bl __fixunstfti
define i128 @ABIi128(i128 %arg1) {
  %farg1 =       bitcast i128 %arg1 to fp128
  %res = fptoui fp128 %farg1 to i128
  ret i128 %res
}

; It happens that we don't handle ConstantArray instances yet during
; translation. Any other constant would be fine too.

; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to translate constant: [1 x double] (in function: constant)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for constant
; FALLBACK-WITH-REPORT-OUT-LABEL: constant:
; FALLBACK-WITH-REPORT-OUT: fmov d0, #1.0
define [1 x double] @constant() {
  ret [1 x double] [double 1.0]
}

  ; The key problem here is that we may fail to create an MBB referenced by a
  ; PHI. If so, we cannot complete the G_PHI and mustn't try or bad things
  ; happen.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: cannot select: G_STORE %vreg5, %vreg2; mem:ST4[%addr] GPR:%vreg5,%vreg2 (in function: pending_phis)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for pending_phis
; FALLBACK-WITH-REPORT-OUT-LABEL: pending_phis:
define i32 @pending_phis(i1 %tst, i32 %val, i32* %addr) {
  br i1 %tst, label %true, label %false

end:
  %res = phi i32 [%val, %true], [42, %false]
  ret i32 %res

true:
  store atomic i32 42, i32* %addr seq_cst, align 4
  br label %end

false:
  br label %end

}

  ; General legalizer inability to handle types whose size wasn't a power of 2.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to legalize instruction: %vreg1<def>(s42) = G_LOAD %vreg0; mem:LD6[%addr](align=8) (in function: odd_type)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for odd_type
; FALLBACK-WITH-REPORT-OUT-LABEL: odd_type:
define void @odd_type(i42* %addr) {
  %val42 = load i42, i42* %addr
  ret void
}

; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to legalize instruction: %vreg1<def>(<7 x s32>) = G_LOAD %vreg0; mem:LD28[%addr](align=32) (in function: odd_vector)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for odd_vector
; FALLBACK-WITH-REPORT-OUT-LABEL: odd_vector:
define void @odd_vector(<7 x i32>* %addr) {
  %vec = load <7 x i32>, <7 x i32>* %addr
  ret void
}

  ; RegBankSelect crashed when given invalid mappings, and AArch64's
  ; implementation produce valid-but-nonsense mappings for G_SEQUENCE.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to map instruction
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for sequence_mapping
; FALLBACK-WITH-REPORT-OUT-LABEL: sequence_mapping:
define void @sequence_mapping([2 x i64] %in) {
  ret void
}

  ; Legalizer was asserting when it enountered an unexpected default action.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to map instruction
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for legal_default
; FALLBACK-WITH-REPORT-LABEL: legal_default:
define void @legal_default([8 x i8] %in) {
  insertvalue { [4 x i8], [8 x i8], [4 x i8] } undef, [8 x i8] %in, 1
  ret void
}

  ; AArch64 was asserting instead of returning an invalid mapping for unknown
  ; sizes.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to translate instruction: ret: '  ret i128 undef' (in function: sequence_sizes)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for sequence_sizes
; FALLBACK-WITH-REPORT-LABEL: sequence_sizes:
define i128 @sequence_sizes([8 x i8] %in) {
  ret i128 undef
}

; Just to make sure we don't accidentally emit a normal load/store.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: cannot select: %vreg2<def>(s64) = G_LOAD %vreg0; mem:LD8[%addr] GPR:%vreg2,%vreg0 (in function: atomic_ops)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for atomic_ops
; FALLBACK-WITH-REPORT-LABEL: atomic_ops:
define i64 @atomic_ops(i64* %addr) {
  store atomic i64 0, i64* %addr unordered, align 8
  %res = load atomic i64, i64* %addr seq_cst, align 8
  ret i64 %res
}

; Make sure we don't mess up metadata arguments.
declare void @llvm.write_register.i64(metadata, i64)

; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to translate instruction: call: ' call void @llvm.write_register.i64(metadata !0, i64 0)' (in function: test_write_register_intrin)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for test_write_register_intrin
; FALLBACK-WITH-REPORT-LABEL: test_write_register_intrin:
define void @test_write_register_intrin() {
  call void @llvm.write_register.i64(metadata !{!"sp"}, i64 0)
  ret void
}

@_ZTIi = external global i8*
declare i32 @__gxx_personality_v0(...)

; Check that we fallback on invoke translation failures.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to translate instruction: invoke: '  invoke void %callee(i128 0)
; FALLBACK-WITH-REPORT-NEXT:   to label %continue unwind label %broken' (in function: invoke_weird_type)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for invoke_weird_type
; FALLBACK-WITH-REPORT-OUT-LABEL: invoke_weird_type:
define void @invoke_weird_type(void(i128)* %callee) personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
  invoke void %callee(i128 0)
    to label %continue unwind label %broken

broken:
  landingpad { i8*, i32 } catch i8* bitcast(i8** @_ZTIi to i8*)
  ret void

continue:
  ret void
}

; Check that we fallback on invoke translation failures.
; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to legalize instruction: %vreg0<def>(s128) = G_FCONSTANT quad 2
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for test_quad_dump
; FALLBACK-WITH-REPORT-OUT-LABEL: test_quad_dump:
define fp128 @test_quad_dump() {
  ret fp128 0xL00000000000000004000000000000000
}

; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to legalize instruction: %vreg0<def>(p0) = G_EXTRACT_VECTOR_ELT %vreg1, %vreg2; (in function: vector_of_pointers_extractelement)
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for vector_of_pointers_extractelement
; FALLBACK-WITH-REPORT-OUT-LABEL: vector_of_pointers_extractelement:
@var = global <2 x i16*> zeroinitializer
define void @vector_of_pointers_extractelement() {
  br label %end

block:
  %dummy = extractelement <2 x i16*> %vec, i32 0
  ret void

end:
  %vec = load <2 x i16*>, <2 x i16*>* undef
  br label %block
}

; FALLBACK-WITH-REPORT-ERR: remark: <unknown>:0:0: unable to legalize instruction: %vreg0<def>(<2 x p0>) = G_INSERT_VECTOR_ELT %vreg1, %vreg2, %vreg3; (in function: vector_of_pointers_insertelement
; FALLBACK-WITH-REPORT-ERR: warning: Instruction selection used fallback path for vector_of_pointers_insertelement
; FALLBACK-WITH-REPORT-OUT-LABEL: vector_of_pointers_insertelement:
define void @vector_of_pointers_insertelement() {
  br label %end

block:
  %dummy = insertelement <2 x i16*> %vec, i16* null, i32 0
  ret void

end:
  %vec = load <2 x i16*>, <2 x i16*>* undef
  br label %block
}
