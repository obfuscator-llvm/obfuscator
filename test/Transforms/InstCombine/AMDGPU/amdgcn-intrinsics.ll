; RUN: opt -instcombine -S < %s | FileCheck %s

; --------------------------------------------------------------------
; llvm.amdgcn.rcp
; --------------------------------------------------------------------

declare float @llvm.amdgcn.rcp.f32(float) nounwind readnone
declare double @llvm.amdgcn.rcp.f64(double) nounwind readnone

; CHECK-LABEL: @test_constant_fold_rcp_f32_undef
; CHECK-NEXT: ret float undef
define float @test_constant_fold_rcp_f32_undef() nounwind {
  %val = call float @llvm.amdgcn.rcp.f32(float undef) nounwind readnone
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_rcp_f32_1
; CHECK-NEXT: ret float 1.000000e+00
define float @test_constant_fold_rcp_f32_1() nounwind {
  %val = call float @llvm.amdgcn.rcp.f32(float 1.0) nounwind readnone
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_rcp_f64_1
; CHECK-NEXT:  ret double 1.000000e+00
define double @test_constant_fold_rcp_f64_1() nounwind {
  %val = call double @llvm.amdgcn.rcp.f64(double 1.0) nounwind readnone
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_rcp_f32_half
; CHECK-NEXT: ret float 2.000000e+00
define float @test_constant_fold_rcp_f32_half() nounwind {
  %val = call float @llvm.amdgcn.rcp.f32(float 0.5) nounwind readnone
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_rcp_f64_half
; CHECK-NEXT:  ret double 2.000000e+00
define double @test_constant_fold_rcp_f64_half() nounwind {
  %val = call double @llvm.amdgcn.rcp.f64(double 0.5) nounwind readnone
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_rcp_f32_43
; CHECK-NEXT: call float @llvm.amdgcn.rcp.f32(float 4.300000e+01)
define float @test_constant_fold_rcp_f32_43() nounwind {
 %val = call float @llvm.amdgcn.rcp.f32(float 4.300000e+01) nounwind readnone
 ret float %val
}

; CHECK-LABEL: @test_constant_fold_rcp_f64_43
; CHECK-NEXT: call double @llvm.amdgcn.rcp.f64(double 4.300000e+01)
define double @test_constant_fold_rcp_f64_43() nounwind {
  %val = call double @llvm.amdgcn.rcp.f64(double 4.300000e+01) nounwind readnone
  ret double %val
}

; --------------------------------------------------------------------
; llvm.amdgcn.rsq
; --------------------------------------------------------------------

declare float @llvm.amdgcn.rsq.f32(float) nounwind readnone

; CHECK-LABEL: @test_constant_fold_rsq_f32_undef
; CHECK-NEXT: ret float undef
define float @test_constant_fold_rsq_f32_undef() nounwind {
  %val = call float @llvm.amdgcn.rsq.f32(float undef) nounwind readnone
  ret float %val
}

; --------------------------------------------------------------------
; llvm.amdgcn.frexp.mant
; --------------------------------------------------------------------

declare float @llvm.amdgcn.frexp.mant.f32(float) nounwind readnone
declare double @llvm.amdgcn.frexp.mant.f64(double) nounwind readnone


; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_undef(
; CHECK-NEXT: ret float undef
define float @test_constant_fold_frexp_mant_f32_undef() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float undef)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_undef(
; CHECK-NEXT:  ret double undef
define double @test_constant_fold_frexp_mant_f64_undef() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double undef)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_0(
; CHECK-NEXT: ret float 0.000000e+00
define float @test_constant_fold_frexp_mant_f32_0() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 0.0)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_0(
; CHECK-NEXT:  ret double 0.000000e+00
define double @test_constant_fold_frexp_mant_f64_0() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 0.0)
  ret double %val
}


; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_n0(
; CHECK-NEXT: ret float -0.000000e+00
define float @test_constant_fold_frexp_mant_f32_n0() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float -0.0)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_n0(
; CHECK-NEXT:  ret double -0.000000e+00
define double @test_constant_fold_frexp_mant_f64_n0() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double -0.0)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_1(
; CHECK-NEXT: ret float 5.000000e-01
define float @test_constant_fold_frexp_mant_f32_1() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 1.0)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_1(
; CHECK-NEXT:  ret double 5.000000e-01
define double @test_constant_fold_frexp_mant_f64_1() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 1.0)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_n1(
; CHECK-NEXT: ret float -5.000000e-01
define float @test_constant_fold_frexp_mant_f32_n1() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float -1.0)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_n1(
; CHECK-NEXT:  ret double -5.000000e-01
define double @test_constant_fold_frexp_mant_f64_n1() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double -1.0)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_nan(
; CHECK-NEXT: ret float 0x7FF8000000000000
define float @test_constant_fold_frexp_mant_f32_nan() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 0x7FF8000000000000)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_nan(
; CHECK-NEXT:  ret double 0x7FF8000000000000
define double @test_constant_fold_frexp_mant_f64_nan() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 0x7FF8000000000000)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_inf(
; CHECK-NEXT: ret float 0x7FF0000000000000
define float @test_constant_fold_frexp_mant_f32_inf() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 0x7FF0000000000000)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_inf(
; CHECK-NEXT:  ret double 0x7FF0000000000000
define double @test_constant_fold_frexp_mant_f64_inf() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 0x7FF0000000000000)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_ninf(
; CHECK-NEXT: ret float 0xFFF0000000000000
define float @test_constant_fold_frexp_mant_f32_ninf() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 0xFFF0000000000000)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_ninf(
; CHECK-NEXT:  ret double 0xFFF0000000000000
define double @test_constant_fold_frexp_mant_f64_ninf() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 0xFFF0000000000000)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_max_num(
; CHECK-NEXT: ret float 0x3FEFFFFFE0000000
define float @test_constant_fold_frexp_mant_f32_max_num() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 0x47EFFFFFE0000000)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_max_num(
; CHECK-NEXT:  ret double 0x3FEFFFFFFFFFFFFF
define double @test_constant_fold_frexp_mant_f64_max_num() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 0x7FEFFFFFFFFFFFFF)
  ret double %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f32_min_num(
; CHECK-NEXT: ret float 5.000000e-01
define float @test_constant_fold_frexp_mant_f32_min_num() nounwind {
  %val = call float @llvm.amdgcn.frexp.mant.f32(float 0x36A0000000000000)
  ret float %val
}

; CHECK-LABEL: @test_constant_fold_frexp_mant_f64_min_num(
; CHECK-NEXT:  ret double 5.000000e-01
define double @test_constant_fold_frexp_mant_f64_min_num() nounwind {
  %val = call double @llvm.amdgcn.frexp.mant.f64(double 4.940656e-324)
  ret double %val
}


; --------------------------------------------------------------------
; llvm.amdgcn.frexp.exp
; --------------------------------------------------------------------

declare i32 @llvm.amdgcn.frexp.exp.f32(float) nounwind readnone
declare i32 @llvm.amdgcn.frexp.exp.f64(double) nounwind readnone

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_undef(
; CHECK-NEXT: ret i32 undef
define i32 @test_constant_fold_frexp_exp_f32_undef() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float undef)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_undef(
; CHECK-NEXT:  ret i32 undef
define i32 @test_constant_fold_frexp_exp_f64_undef() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double undef)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_0(
; CHECK-NEXT: ret i32 0
define i32 @test_constant_fold_frexp_exp_f32_0() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_0(
; CHECK-NEXT:  ret i32 0
define i32 @test_constant_fold_frexp_exp_f64_0() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 0.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_n0(
; CHECK-NEXT: ret i32 0
define i32 @test_constant_fold_frexp_exp_f32_n0() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float -0.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_n0(
; CHECK-NEXT:  ret i32 0
define i32 @test_constant_fold_frexp_exp_f64_n0() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double -0.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_1024(
; CHECK-NEXT: ret i32 11
define i32 @test_constant_fold_frexp_exp_f32_1024() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 1024.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_1024(
; CHECK-NEXT:  ret i32 11
define i32 @test_constant_fold_frexp_exp_f64_1024() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 1024.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_n1024(
; CHECK-NEXT: ret i32 11
define i32 @test_constant_fold_frexp_exp_f32_n1024() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float -1024.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_n1024(
; CHECK-NEXT:  ret i32 11
define i32 @test_constant_fold_frexp_exp_f64_n1024() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double -1024.0)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_1_1024(
; CHECK-NEXT: ret i32 -9
define i32 @test_constant_fold_frexp_exp_f32_1_1024() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0.0009765625)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_1_1024(
; CHECK-NEXT:  ret i32 -9
define i32 @test_constant_fold_frexp_exp_f64_1_1024() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 0.0009765625)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_nan(
; CHECK-NEXT: ret i32 0
define i32 @test_constant_fold_frexp_exp_f32_nan() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0x7FF8000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_nan(
; CHECK-NEXT:  ret i32 0
define i32 @test_constant_fold_frexp_exp_f64_nan() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 0x7FF8000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_inf(
; CHECK-NEXT: ret i32 0
define i32 @test_constant_fold_frexp_exp_f32_inf() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0x7FF0000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_inf(
; CHECK-NEXT:  ret i32 0
define i32 @test_constant_fold_frexp_exp_f64_inf() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 0x7FF0000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_ninf(
; CHECK-NEXT: ret i32 0
define i32 @test_constant_fold_frexp_exp_f32_ninf() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0xFFF0000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_ninf(
; CHECK-NEXT:  ret i32 0
define i32 @test_constant_fold_frexp_exp_f64_ninf() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 0xFFF0000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_max_num(
; CHECK-NEXT: ret i32 128
define i32 @test_constant_fold_frexp_exp_f32_max_num() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0x47EFFFFFE0000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_max_num(
; CHECK-NEXT:  ret i32 1024
define i32 @test_constant_fold_frexp_exp_f64_max_num() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 0x7FEFFFFFFFFFFFFF)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f32_min_num(
; CHECK-NEXT: ret i32 -148
define i32 @test_constant_fold_frexp_exp_f32_min_num() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f32(float 0x36A0000000000000)
  ret i32 %val
}

; CHECK-LABEL: @test_constant_fold_frexp_exp_f64_min_num(
; CHECK-NEXT:  ret i32 -1073
define i32 @test_constant_fold_frexp_exp_f64_min_num() nounwind {
  %val = call i32 @llvm.amdgcn.frexp.exp.f64(double 4.940656e-324)
  ret i32 %val
}

; --------------------------------------------------------------------
; llvm.amdgcn.class
; --------------------------------------------------------------------

declare i1 @llvm.amdgcn.class.f32(float, i32) nounwind readnone
declare i1 @llvm.amdgcn.class.f64(double, i32) nounwind readnone

; CHECK-LABEL: @test_class_undef_mask_f32(
; CHECK: ret i1 false
define i1 @test_class_undef_mask_f32(float %x) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 undef)
  ret i1 %val
}

; CHECK-LABEL: @test_class_over_max_mask_f32(
; CHECK: %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 1)
define i1 @test_class_over_max_mask_f32(float %x) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 1025)
  ret i1 %val
}

; CHECK-LABEL: @test_class_no_mask_f32(
; CHECK: ret i1 false
define i1 @test_class_no_mask_f32(float %x) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 0)
  ret i1 %val
}

; CHECK-LABEL: @test_class_full_mask_f32(
; CHECK: ret i1 true
define i1 @test_class_full_mask_f32(float %x) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 1023)
  ret i1 %val
}

; CHECK-LABEL: @test_class_undef_no_mask_f32(
; CHECK: ret i1 false
define i1 @test_class_undef_no_mask_f32() nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float undef, i32 0)
  ret i1 %val
}

; CHECK-LABEL: @test_class_undef_full_mask_f32(
; CHECK: ret i1 true
define i1 @test_class_undef_full_mask_f32() nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float undef, i32 1023)
  ret i1 %val
}

; CHECK-LABEL: @test_class_undef_val_f32(
; CHECK: ret i1 undef
define i1 @test_class_undef_val_f32() nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float undef, i32 4)
  ret i1 %val
}

; CHECK-LABEL: @test_class_undef_undef_f32(
; CHECK: ret i1 undef
define i1 @test_class_undef_undef_f32() nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float undef, i32 undef)
  ret i1 %val
}

; CHECK-LABEL: @test_class_var_mask_f32(
; CHECK: %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 %mask)
define i1 @test_class_var_mask_f32(float %x, i32 %mask) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 %mask)
  ret i1 %val
}

; CHECK-LABEL: @test_class_isnan_f32(
; CHECK: %val = fcmp uno float %x, 0.000000e+00
define i1 @test_class_isnan_f32(float %x) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 3)
  ret i1 %val
}

; CHECK-LABEL: @test_class_is_p0_n0_f32(
; CHECK: %val = fcmp oeq float %x, 0.000000e+00
define i1 @test_class_is_p0_n0_f32(float %x) nounwind {
  %val = call i1 @llvm.amdgcn.class.f32(float %x, i32 96)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_snan_test_snan_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_snan_test_snan_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF0000000000001, i32 1)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_qnan_test_qnan_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_qnan_test_qnan_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF8000000000000, i32 2)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_qnan_test_snan_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_qnan_test_snan_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF8000000000000, i32 1)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_ninf_test_ninf_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_ninf_test_ninf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0xFFF0000000000000, i32 4)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_pinf_test_ninf_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_pinf_test_ninf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF0000000000000, i32 4)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_qnan_test_ninf_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_qnan_test_ninf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF8000000000000, i32 4)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_snan_test_ninf_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_snan_test_ninf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF0000000000001, i32 4)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_nnormal_test_nnormal_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_nnormal_test_nnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double -1.0, i32 8)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_pnormal_test_nnormal_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_pnormal_test_nnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 1.0, i32 8)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_nsubnormal_test_nsubnormal_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_nsubnormal_test_nsubnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x800fffffffffffff, i32 16)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_psubnormal_test_nsubnormal_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_psubnormal_test_nsubnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x000fffffffffffff, i32 16)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_nzero_test_nzero_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_nzero_test_nzero_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double -0.0, i32 32)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_pzero_test_nzero_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_pzero_test_nzero_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0.0, i32 32)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_pzero_test_pzero_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_pzero_test_pzero_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0.0, i32 64)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_nzero_test_pzero_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_nzero_test_pzero_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double -0.0, i32 64)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_psubnormal_test_psubnormal_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_psubnormal_test_psubnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x000fffffffffffff, i32 128)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_nsubnormal_test_psubnormal_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_nsubnormal_test_psubnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x800fffffffffffff, i32 128)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_pnormal_test_pnormal_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_pnormal_test_pnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 1.0, i32 256)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_nnormal_test_pnormal_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_nnormal_test_pnormal_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double -1.0, i32 256)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_pinf_test_pinf_f64(
; CHECK: ret i1 true
define i1 @test_constant_class_pinf_test_pinf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF0000000000000, i32 512)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_ninf_test_pinf_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_ninf_test_pinf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0xFFF0000000000000, i32 512)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_qnan_test_pinf_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_qnan_test_pinf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF8000000000000, i32 512)
  ret i1 %val
}

; CHECK-LABEL: @test_constant_class_snan_test_pinf_f64(
; CHECK: ret i1 false
define i1 @test_constant_class_snan_test_pinf_f64() nounwind {
  %val = call i1 @llvm.amdgcn.class.f64(double 0x7FF0000000000001, i32 512)
  ret i1 %val
}

; CHECK-LABEL: @test_class_is_snan_nnan_src(
; CHECK-NEXT: ret i1 false
define i1 @test_class_is_snan_nnan_src(float %x) {
  %nnan = fadd nnan float %x, 1.0
  %class = call i1 @llvm.amdgcn.class.f32(float %nnan, i32 1)
  ret i1 %class
}

; CHECK-LABEL: @test_class_is_qnan_nnan_src(
; CHECK-NEXT: ret i1 false
define i1 @test_class_is_qnan_nnan_src(float %x) {
  %nnan = fadd nnan float %x, 1.0
  %class = call i1 @llvm.amdgcn.class.f32(float %nnan, i32 2)
  ret i1 %class
}

; CHECK-LABEL: @test_class_is_nan_nnan_src(
; CHECK-NEXT: ret i1 false
define i1 @test_class_is_nan_nnan_src(float %x) {
  %nnan = fadd nnan float %x, 1.0
  %class = call i1 @llvm.amdgcn.class.f32(float %nnan, i32 3)
  ret i1 %class
}

; CHECK-LABEL: @test_class_is_nan_other_nnan_src(
; CHECK-NEXT: %nnan = fadd nnan float %x, 1.000000e+00
; CHECK-NEXT: %class = call i1 @llvm.amdgcn.class.f32(float %nnan, i32 264)
define i1 @test_class_is_nan_other_nnan_src(float %x) {
  %nnan = fadd nnan float %x, 1.0
  %class = call i1 @llvm.amdgcn.class.f32(float %nnan, i32 267)
  ret i1 %class
}

; --------------------------------------------------------------------
; llvm.amdgcn.cos
; --------------------------------------------------------------------
declare float @llvm.amdgcn.cos.f32(float) nounwind readnone
declare float @llvm.fabs.f32(float) nounwind readnone

; CHECK-LABEL: @cos_fneg_f32(
; CHECK: %cos = call float @llvm.amdgcn.cos.f32(float %x)
; CHECK-NEXT: ret float %cos
define float @cos_fneg_f32(float %x) {
  %x.fneg = fsub float -0.0, %x
  %cos = call float @llvm.amdgcn.cos.f32(float %x.fneg)
  ret float %cos
}

; CHECK-LABEL: @cos_fabs_f32(
; CHECK-NEXT: %cos = call float @llvm.amdgcn.cos.f32(float %x)
; CHECK-NEXT: ret float %cos
define float @cos_fabs_f32(float %x) {
  %x.fabs = call float @llvm.fabs.f32(float %x)
  %cos = call float @llvm.amdgcn.cos.f32(float %x.fabs)
  ret float %cos
}

; CHECK-LABEL: @cos_fabs_fneg_f32(
; CHECK-NEXT: %cos = call float @llvm.amdgcn.cos.f32(float %x)
; CHECK-NEXT: ret float %cos
define float @cos_fabs_fneg_f32(float %x) {
  %x.fabs = call float @llvm.fabs.f32(float %x)
  %x.fabs.fneg = fsub float -0.0, %x.fabs
  %cos = call float @llvm.amdgcn.cos.f32(float %x.fabs.fneg)
  ret float %cos
}

; --------------------------------------------------------------------
; llvm.amdgcn.cvt.pkrtz
; --------------------------------------------------------------------

declare <2 x half> @llvm.amdgcn.cvt.pkrtz(float, float) nounwind readnone

; CHECK-LABEL: @vars_lhs_cvt_pkrtz(
; CHECK: %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float %y)
define <2 x half> @vars_lhs_cvt_pkrtz(float %x, float %y) {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float %y)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @constant_lhs_cvt_pkrtz(
; CHECK: %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float 0.000000e+00, float %y)
define <2 x half> @constant_lhs_cvt_pkrtz(float %y) {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float 0.0, float %y)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @constant_rhs_cvt_pkrtz(
; CHECK: %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float 0.000000e+00)
define <2 x half> @constant_rhs_cvt_pkrtz(float %x) {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float 0.0)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @undef_lhs_cvt_pkrtz(
; CHECK: %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float undef, float %y)
define <2 x half> @undef_lhs_cvt_pkrtz(float %y) {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float undef, float %y)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @undef_rhs_cvt_pkrtz(
; CHECK: %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float undef)
define <2 x half> @undef_rhs_cvt_pkrtz(float %x) {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %x, float undef)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @undef_cvt_pkrtz(
; CHECK: ret <2 x half> undef
define <2 x half> @undef_cvt_pkrtz() {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float undef, float undef)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @constant_splat0_cvt_pkrtz(
; CHECK: ret <2 x half> zeroinitializer
define <2 x half> @constant_splat0_cvt_pkrtz() {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float 0.0, float 0.0)
  ret <2 x half> %cvt
}

; CHECK-LABEL: @constant_cvt_pkrtz(
; CHECK: ret <2 x half> <half 0xH4000, half 0xH4400>
define <2 x half> @constant_cvt_pkrtz() {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float 2.0, float 4.0)
  ret <2 x half> %cvt
}

; Test constant values where rtz changes result
; CHECK-LABEL: @constant_rtz_pkrtz(
; CHECK: ret <2 x half> <half 0xH7BFF, half 0xH7BFF>
define <2 x half> @constant_rtz_pkrtz() {
  %cvt = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float 65535.0, float 65535.0)
  ret <2 x half> %cvt
}

; --------------------------------------------------------------------
; llvm.amdgcn.cvt.pknorm.i16
; --------------------------------------------------------------------

declare <2 x i16> @llvm.amdgcn.cvt.pknorm.i16(float, float) nounwind readnone

; CHECK-LABEL: @undef_lhs_cvt_pknorm_i16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.i16(float undef, float %y)
define <2 x i16> @undef_lhs_cvt_pknorm_i16(float %y) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.i16(float undef, float %y)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_rhs_cvt_pknorm_i16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.i16(float %x, float undef)
define <2 x i16> @undef_rhs_cvt_pknorm_i16(float %x) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.i16(float %x, float undef)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_cvt_pknorm_i16(
; CHECK: ret <2 x i16> undef
define <2 x i16> @undef_cvt_pknorm_i16() {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.i16(float undef, float undef)
  ret <2 x i16> %cvt
}

; --------------------------------------------------------------------
; llvm.amdgcn.cvt.pknorm.u16
; --------------------------------------------------------------------

declare <2 x i16> @llvm.amdgcn.cvt.pknorm.u16(float, float) nounwind readnone

; CHECK-LABEL: @undef_lhs_cvt_pknorm_u16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.u16(float undef, float %y)
define <2 x i16> @undef_lhs_cvt_pknorm_u16(float %y) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.u16(float undef, float %y)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_rhs_cvt_pknorm_u16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.u16(float %x, float undef)
define <2 x i16> @undef_rhs_cvt_pknorm_u16(float %x) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.u16(float %x, float undef)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_cvt_pknorm_u16(
; CHECK: ret <2 x i16> undef
define <2 x i16> @undef_cvt_pknorm_u16() {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pknorm.u16(float undef, float undef)
  ret <2 x i16> %cvt
}

; --------------------------------------------------------------------
; llvm.amdgcn.cvt.pk.i16
; --------------------------------------------------------------------

declare <2 x i16> @llvm.amdgcn.cvt.pk.i16(i32, i32) nounwind readnone

; CHECK-LABEL: @undef_lhs_cvt_pk_i16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.i16(i32 undef, i32 %y)
define <2 x i16> @undef_lhs_cvt_pk_i16(i32 %y) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.i16(i32 undef, i32 %y)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_rhs_cvt_pk_i16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.i16(i32 %x, i32 undef)
define <2 x i16> @undef_rhs_cvt_pk_i16(i32 %x) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.i16(i32 %x, i32 undef)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_cvt_pk_i16(
; CHECK: ret <2 x i16> undef
define <2 x i16> @undef_cvt_pk_i16() {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.i16(i32 undef, i32 undef)
  ret <2 x i16> %cvt
}

; --------------------------------------------------------------------
; llvm.amdgcn.cvt.pk.u16
; --------------------------------------------------------------------

declare <2 x i16> @llvm.amdgcn.cvt.pk.u16(i32, i32) nounwind readnone

; CHECK-LABEL: @undef_lhs_cvt_pk_u16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.u16(i32 undef, i32 %y)
define <2 x i16> @undef_lhs_cvt_pk_u16(i32 %y) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.u16(i32 undef, i32 %y)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_rhs_cvt_pk_u16(
; CHECK: %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.u16(i32 %x, i32 undef)
define <2 x i16> @undef_rhs_cvt_pk_u16(i32 %x) {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.u16(i32 %x, i32 undef)
  ret <2 x i16> %cvt
}

; CHECK-LABEL: @undef_cvt_pk_u16(
; CHECK: ret <2 x i16> undef
define <2 x i16> @undef_cvt_pk_u16() {
  %cvt = call <2 x i16> @llvm.amdgcn.cvt.pk.u16(i32 undef, i32 undef)
  ret <2 x i16> %cvt
}

; --------------------------------------------------------------------
; llvm.amdgcn.ubfe
; --------------------------------------------------------------------

declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) nounwind readnone
declare i64 @llvm.amdgcn.ubfe.i64(i64, i32, i32) nounwind readnone

; CHECK-LABEL: @ubfe_var_i32(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 %width)
define i32 @ubfe_var_i32(i32 %src, i32 %offset, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_clear_high_bits_constant_offset_i32(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 5, i32 %width)
define i32 @ubfe_clear_high_bits_constant_offset_i32(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 133, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_clear_high_bits_constant_width_i32(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 5)
define i32 @ubfe_clear_high_bits_constant_width_i32(i32 %src, i32 %offset) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 133)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_width_0(
; CHECK-NEXT: ret i32 0
define i32 @ubfe_width_0(i32 %src, i32 %offset) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 0)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_width_31(
; CHECK: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 31)
define i32 @ubfe_width_31(i32 %src, i32 %offset) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 31)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_width_32(
; CHECK-NEXT: ret i32 0
define i32 @ubfe_width_32(i32 %src, i32 %offset) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 32)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_width_33(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 1)
define i32 @ubfe_width_33(i32 %src, i32 %offset) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 33)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_33(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 1, i32 %width)
define i32 @ubfe_offset_33(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 33, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_0(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 0, i32 %width)
define i32 @ubfe_offset_0(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 0, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_32(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 0, i32 %width)
define i32 @ubfe_offset_32(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 32, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_31(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 31, i32 %width)
define i32 @ubfe_offset_31(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 31, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_0_width_0(
; CHECK-NEXT: ret i32 0
define i32 @ubfe_offset_0_width_0(i32 %src) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 0, i32 0)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_0_width_3(
; CHECK-NEXT: and i32 %src, 7
; CHECK-NEXT: ret
define i32 @ubfe_offset_0_width_3(i32 %src) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 0, i32 3)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_3_width_1(
; CHECK-NEXT: %1 = lshr i32 %src, 3
; CHECK-NEXT: and i32 %1, 1
; CHECK-NEXT: ret i32
define i32 @ubfe_offset_3_width_1(i32 %src) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 3, i32 1)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_3_width_4(
; CHECK-NEXT: %1 = lshr i32 %src, 3
; CHECK-NEXT: and i32 %1, 15
; CHECK-NEXT: ret i32
define i32 @ubfe_offset_3_width_4(i32 %src) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 3, i32 4)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_0_0_0(
; CHECK-NEXT: ret i32 0
define i32 @ubfe_0_0_0() {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 0, i32 0, i32 0)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_neg1_5_7(
; CHECK-NEXT: ret i32 127
define i32 @ubfe_neg1_5_7() {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 -1, i32 5, i32 7)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_undef_src_i32(
; CHECK-NEXT: ret i32 undef
define i32 @ubfe_undef_src_i32(i32 %offset, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 undef, i32 %offset, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_undef_offset_i32(
; CHECK: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 undef, i32 %width)
define i32 @ubfe_undef_offset_i32(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 undef, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_undef_width_i32(
; CHECK: %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 undef)
define i32 @ubfe_undef_width_i32(i32 %src, i32 %offset) {
  %bfe = call i32 @llvm.amdgcn.ubfe.i32(i32 %src, i32 %offset, i32 undef)
  ret i32 %bfe
}

; CHECK-LABEL: @ubfe_offset_33_width_4_i64(
; CHECK-NEXT: %1 = lshr i64 %src, 33
; CHECK-NEXT: %bfe = and i64 %1, 15
define i64 @ubfe_offset_33_width_4_i64(i64 %src) {
  %bfe = call i64 @llvm.amdgcn.ubfe.i64(i64 %src, i32 33, i32 4)
  ret i64 %bfe
}

; CHECK-LABEL: @ubfe_offset_0_i64(
; CHECK-NEXT: %bfe = call i64 @llvm.amdgcn.ubfe.i64(i64 %src, i32 0, i32 %width)
define i64 @ubfe_offset_0_i64(i64 %src, i32 %width) {
  %bfe = call i64 @llvm.amdgcn.ubfe.i64(i64 %src, i32 0, i32 %width)
  ret i64 %bfe
}

; CHECK-LABEL: @ubfe_offset_32_width_32_i64(
; CHECK-NEXT: %bfe = lshr i64 %src, 32
; CHECK-NEXT: ret i64 %bfe
define i64 @ubfe_offset_32_width_32_i64(i64 %src) {
  %bfe = call i64 @llvm.amdgcn.ubfe.i64(i64 %src, i32 32, i32 32)
  ret i64 %bfe
}

; --------------------------------------------------------------------
; llvm.amdgcn.sbfe
; --------------------------------------------------------------------

declare i32 @llvm.amdgcn.sbfe.i32(i32, i32, i32) nounwind readnone
declare i64 @llvm.amdgcn.sbfe.i64(i64, i32, i32) nounwind readnone

; CHECK-LABEL: @sbfe_offset_31(
; CHECK-NEXT: %bfe = call i32 @llvm.amdgcn.sbfe.i32(i32 %src, i32 31, i32 %width)
define i32 @sbfe_offset_31(i32 %src, i32 %width) {
  %bfe = call i32 @llvm.amdgcn.sbfe.i32(i32 %src, i32 31, i32 %width)
  ret i32 %bfe
}

; CHECK-LABEL: @sbfe_neg1_5_7(
; CHECK-NEXT: ret i32 -1
define i32 @sbfe_neg1_5_7() {
  %bfe = call i32 @llvm.amdgcn.sbfe.i32(i32 -1, i32 5, i32 7)
  ret i32 %bfe
}

; CHECK-LABEL: @sbfe_offset_32_width_32_i64(
; CHECK-NEXT: %bfe = ashr i64 %src, 32
; CHECK-NEXT: ret i64 %bfe
define i64 @sbfe_offset_32_width_32_i64(i64 %src) {
  %bfe = call i64 @llvm.amdgcn.sbfe.i64(i64 %src, i32 32, i32 32)
  ret i64 %bfe
}

; --------------------------------------------------------------------
; llvm.amdgcn.exp
; --------------------------------------------------------------------

declare void @llvm.amdgcn.exp.f32(i32, i32, float, float, float, float, i1, i1) nounwind inaccessiblememonly

; Make sure no crashing on invalid variable params
; CHECK-LABEL: @exp_invalid_inputs(
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 %en, float 1.000000e+00, float 2.000000e+00, float 5.000000e-01, float 4.000000e+00, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 %tgt, i32 15, float 1.000000e+00, float 2.000000e+00, float 5.000000e-01, float 4.000000e+00, i1 true, i1 false)
define void @exp_invalid_inputs(i32 %tgt, i32 %en) {
  call void @llvm.amdgcn.exp.f32(i32 0, i32 %en, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 %tgt, i32 15, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  ret void
}

; CHECK-LABEL: @exp_disabled_inputs_to_undef(
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 1, float 1.000000e+00, float undef, float undef, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 2, float undef, float 2.000000e+00, float undef, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 4, float undef, float undef, float 5.000000e-01, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 8, float undef, float undef, float undef, float 4.000000e+00, i1 true, i1 false)

; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 1, float %x, float undef, float undef, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 2, float undef, float %y, float undef, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 4, float undef, float undef, float %z, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 8, float undef, float undef, float undef, float %w, i1 true, i1 false)

; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 0, float undef, float undef, float undef, float undef, i1 true, i1 false)

; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 3, float 1.000000e+00, float 2.000000e+00, float undef, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 5, float 1.000000e+00, float undef, float 5.000000e-01, float undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 9, float 1.000000e+00, float undef, float undef, float 4.000000e+00, i1 false, i1 false)
; CHECK: call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float 1.000000e+00, float 2.000000e+00, float 5.000000e-01, float 4.000000e+00, i1 false, i1 false)
define void @exp_disabled_inputs_to_undef(float %x, float %y, float %z, float %w) {
  ; enable src0..src3 constants
  call void @llvm.amdgcn.exp.f32(i32 0, i32 1, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 2, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 4, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 8, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)

  ; enable src0..src3 variables
  call void @llvm.amdgcn.exp.f32(i32 0, i32 1, float %x, float %y, float %z, float %w, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 2, float %x, float %y, float %z, float %w, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 4, float %x, float %y, float %z, float %w, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 8, float %x, float %y, float %z, float %w, i1 true, i1 false)

  ; enable none
  call void @llvm.amdgcn.exp.f32(i32 0, i32 0, float %x, float %y, float %z, float %w, i1 true, i1 false)

  ; enable different source combinations
  call void @llvm.amdgcn.exp.f32(i32 0, i32 3, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 5, float 1.0, float 2.0, float 0.5, float 4.0, i1 true, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 9, float 1.0, float 2.0, float 0.5, float 4.0, i1 false, i1 false)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float 1.0, float 2.0, float 0.5, float 4.0, i1 false, i1 false)

  ret void
}

; --------------------------------------------------------------------
; llvm.amdgcn.exp.compr
; --------------------------------------------------------------------

declare void @llvm.amdgcn.exp.compr.v2f16(i32, i32, <2 x half>, <2 x half>, i1, i1) nounwind inaccessiblememonly

; CHECK-LABEL: @exp_compr_invalid_inputs(
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 %en, <2 x half> <half 0xH3C00, half 0xH4000>, <2 x half> <half 0xH3800, half 0xH4400>, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 %tgt, i32 5, <2 x half> <half 0xH3C00, half 0xH4000>, <2 x half> <half 0xH3800, half 0xH4400>, i1 true, i1 false)
define void @exp_compr_invalid_inputs(i32 %tgt, i32 %en) {
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 %en, <2 x half> <half 1.0, half 2.0>, <2 x half> <half 0.5, half 4.0>, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 %tgt, i32 5, <2 x half> <half 1.0, half 2.0>, <2 x half> <half 0.5, half 4.0>, i1 true, i1 false)
  ret void
}

; CHECK-LABEL: @exp_compr_disabled_inputs_to_undef(
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 0, <2 x half> undef, <2 x half> undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 1, <2 x half> <half 0xH3C00, half 0xH4000>, <2 x half> undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 2, <2 x half> <half 0xH3C00, half 0xH4000>, <2 x half> undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 3, <2 x half> <half 0xH3C00, half 0xH4000>, <2 x half> undef, i1 true, i1 false)

; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 0, <2 x half> undef, <2 x half> undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 1, <2 x half> %xy, <2 x half> undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 2, <2 x half> %xy, <2 x half> undef, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 3, <2 x half> %xy, <2 x half> undef, i1 true, i1 false)

; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 12, <2 x half> undef, <2 x half> %zw, i1 true, i1 false)
; CHECK: call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 15, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)
define void @exp_compr_disabled_inputs_to_undef(<2 x half> %xy, <2 x half> %zw) {
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 0, <2 x half> <half 1.0, half 2.0>, <2 x half> <half 0.5, half 4.0>, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 1, <2 x half> <half 1.0, half 2.0>, <2 x half> <half 0.5, half 4.0>, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 2, <2 x half> <half 1.0, half 2.0>, <2 x half> <half 0.5, half 4.0>, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 3, <2 x half> <half 1.0, half 2.0>, <2 x half> <half 0.5, half 4.0>, i1 true, i1 false)

  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 0, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 1, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 2, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 3, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)

  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 12, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)
  call void @llvm.amdgcn.exp.compr.v2f16(i32 0, i32 15, <2 x half> %xy, <2 x half> %zw, i1 true, i1 false)
  ret void
}

; --------------------------------------------------------------------
; llvm.amdgcn.fmed3
; --------------------------------------------------------------------

declare float @llvm.amdgcn.fmed3.f32(float, float, float) nounwind readnone

; CHECK-LABEL: @fmed3_f32(
; CHECK: %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float %z)
define float @fmed3_f32(float %x, float %y, float %z) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float %z)
  ret float %med3
}

; CHECK-LABEL: @fmed3_canonicalize_x_c0_c1_f32(
; CHECK: call float @llvm.amdgcn.fmed3.f32(float %x, float 0.000000e+00, float 1.000000e+00)
define float @fmed3_canonicalize_x_c0_c1_f32(float %x) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float 0.0, float 1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_canonicalize_c0_x_c1_f32(
; CHECK: call float @llvm.amdgcn.fmed3.f32(float %x, float 0.000000e+00, float 1.000000e+00)
define float @fmed3_canonicalize_c0_x_c1_f32(float %x) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0.0, float %x, float 1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_canonicalize_c0_c1_x_f32(
; CHECK: call float @llvm.amdgcn.fmed3.f32(float %x, float 0.000000e+00, float 1.000000e+00)
define float @fmed3_canonicalize_c0_c1_x_f32(float %x) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0.0, float 1.0, float %x)
  ret float %med3
}

; CHECK-LABEL: @fmed3_canonicalize_x_y_c_f32(
; CHECK: call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float 1.000000e+00)
define float @fmed3_canonicalize_x_y_c_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float 1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_canonicalize_x_c_y_f32(
; CHECK: %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float 1.000000e+00)
define float @fmed3_canonicalize_x_c_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float 1.0, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_canonicalize_c_x_y_f32(
; CHECK: call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float 1.000000e+00)
define float @fmed3_canonicalize_c_x_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 1.0, float %x, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_undef_x_y_f32(
; CHECK: call float @llvm.minnum.f32(float %x, float %y)
define float @fmed3_undef_x_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float undef, float %x, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_fmf_undef_x_y_f32(
; CHECK: call nnan float @llvm.minnum.f32(float %x, float %y)
define float @fmed3_fmf_undef_x_y_f32(float %x, float %y) {
  %med3 = call nnan float @llvm.amdgcn.fmed3.f32(float undef, float %x, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_x_undef_y_f32(
; CHECK: call float @llvm.minnum.f32(float %x, float %y)
define float @fmed3_x_undef_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float undef, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_x_y_undef_f32(
; CHECK: call float @llvm.maxnum.f32(float %x, float %y)
define float @fmed3_x_y_undef_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float undef)
  ret float %med3
}

; CHECK-LABEL: @fmed3_qnan0_x_y_f32(
; CHECK: call float @llvm.minnum.f32(float %x, float %y)
define float @fmed3_qnan0_x_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0x7FF8000000000000, float %x, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_x_qnan0_y_f32(
; CHECK: call float @llvm.minnum.f32(float %x, float %y)
define float @fmed3_x_qnan0_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float 0x7FF8000000000000, float %y)
  ret float %med3
}

; CHECK-LABEL: @fmed3_x_y_qnan0_f32(
; CHECK: call float @llvm.maxnum.f32(float %x, float %y)
define float @fmed3_x_y_qnan0_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float %y, float 0x7FF8000000000000)
  ret float %med3
}

; CHECK-LABEL: @fmed3_qnan1_x_y_f32(
; CHECK: call float @llvm.minnum.f32(float %x, float %y)
define float @fmed3_qnan1_x_y_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0x7FF8000100000000, float %x, float %y)
  ret float %med3
}

; This can return any of the qnans.
; CHECK-LABEL: @fmed3_qnan0_qnan1_qnan2_f32(
; CHECK: ret float 0x7FF8030000000000
define float @fmed3_qnan0_qnan1_qnan2_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0x7FF8000100000000, float 0x7FF8002000000000, float 0x7FF8030000000000)
  ret float %med3
}

; CHECK-LABEL: @fmed3_constant_src0_0_f32(
; CHECK: ret float 5.000000e-01
define float @fmed3_constant_src0_0_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0.5, float -1.0, float 4.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_constant_src0_1_f32(
; CHECK: ret float 5.000000e-01
define float @fmed3_constant_src0_1_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0.5, float 4.0, float -1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_constant_src1_0_f32(
; CHECK: ret float 5.000000e-01
define float @fmed3_constant_src1_0_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float -1.0, float 0.5, float 4.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_constant_src1_1_f32(
; CHECK: ret float 5.000000e-01
define float @fmed3_constant_src1_1_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 4.0, float 0.5, float -1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_constant_src2_0_f32(
; CHECK: ret float 5.000000e-01
define float @fmed3_constant_src2_0_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float -1.0, float 4.0, float 0.5)
  ret float %med3
}

; CHECK-LABEL: @fmed3_constant_src2_1_f32(
; CHECK: ret float 5.000000e-01
define float @fmed3_constant_src2_1_f32(float %x, float %y) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 4.0, float -1.0, float 0.5)
  ret float %med3
}

; CHECK-LABEL: @fmed3_x_qnan0_qnan1_f32(
; CHECK: ret float %x
define float @fmed3_x_qnan0_qnan1_f32(float %x) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float %x, float 0x7FF8001000000000, float 0x7FF8002000000000)
  ret float %med3
}

; CHECK-LABEL: @fmed3_qnan0_x_qnan1_f32(
; CHECK: ret float %x
define float @fmed3_qnan0_x_qnan1_f32(float %x) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0x7FF8001000000000, float %x, float 0x7FF8002000000000)
  ret float %med3
}

; CHECK-LABEL: @fmed3_qnan0_qnan1_x_f32(
; CHECK: ret float %x
define float @fmed3_qnan0_qnan1_x_f32(float %x) {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0x7FF8001000000000, float 0x7FF8002000000000, float %x)
  ret float %med3
}

; CHECK-LABEL: @fmed3_nan_0_1_f32(
; CHECK: ret float 0.0
define float @fmed3_nan_0_1_f32() {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float 0x7FF8001000000000, float 0.0, float 1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_0_nan_1_f32(
; CHECK: ret float 0.0
define float @fmed3_0_nan_1_f32() {
  %med = call float @llvm.amdgcn.fmed3.f32(float 0.0, float 0x7FF8001000000000, float 1.0)
  ret float %med
}

; CHECK-LABEL: @fmed3_0_1_nan_f32(
; CHECK: ret float 1.0
define float @fmed3_0_1_nan_f32() {
  %med = call float @llvm.amdgcn.fmed3.f32(float 0.0, float 1.0, float 0x7FF8001000000000)
  ret float %med
}

; CHECK-LABEL: @fmed3_undef_0_1_f32(
; CHECK: ret float 0.0
define float @fmed3_undef_0_1_f32() {
  %med3 = call float @llvm.amdgcn.fmed3.f32(float undef, float 0.0, float 1.0)
  ret float %med3
}

; CHECK-LABEL: @fmed3_0_undef_1_f32(
; CHECK: ret float 0.0
define float @fmed3_0_undef_1_f32() {
  %med = call float @llvm.amdgcn.fmed3.f32(float 0.0, float undef, float 1.0)
  ret float %med
}

; CHECK-LABEL: @fmed3_0_1_undef_f32(
; CHECK: ret float 1.0
define float @fmed3_0_1_undef_f32() {
  %med = call float @llvm.amdgcn.fmed3.f32(float 0.0, float 1.0, float undef)
  ret float %med
}

; --------------------------------------------------------------------
; llvm.amdgcn.icmp
; --------------------------------------------------------------------

declare i64 @llvm.amdgcn.icmp.i32(i32, i32, i32) nounwind readnone convergent
declare i64 @llvm.amdgcn.icmp.i64(i64, i64, i32) nounwind readnone convergent
declare i64 @llvm.amdgcn.icmp.i1(i1, i1, i32) nounwind readnone convergent

; Make sure there's no crash for invalid input
; CHECK-LABEL: @invalid_nonconstant_icmp_code(
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 %c)
define i64 @invalid_nonconstant_icmp_code(i32 %a, i32 %b, i32 %c) {
  %result = call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 %c)
  ret i64 %result
}

; CHECK-LABEL: @invalid_icmp_code(
; CHECK: %under = call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 31)
; CHECK: %over = call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 42)
define i64 @invalid_icmp_code(i32 %a, i32 %b) {
  %under = call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 31)
  %over = call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 42)
  %or = or i64 %under, %over
  ret i64 %or
}

; CHECK-LABEL: @icmp_constant_inputs_false(
; CHECK: ret i64 0
define i64 @icmp_constant_inputs_false() {
  %result = call i64 @llvm.amdgcn.icmp.i32(i32 9, i32 8, i32 32)
  ret i64 %result
}

; CHECK-LABEL: @icmp_constant_inputs_true(
; CHECK: %result = call i64 @llvm.read_register.i64(metadata !0) #5
define i64 @icmp_constant_inputs_true() {
  %result = call i64 @llvm.amdgcn.icmp.i32(i32 9, i32 8, i32 34)
  ret i64 %result
}

; CHECK-LABEL: @icmp_constant_to_rhs_slt(
; CHECK: %result = call i64 @llvm.amdgcn.icmp.i32(i32 %x, i32 9, i32 38)
define i64 @icmp_constant_to_rhs_slt(i32 %x) {
  %result = call i64 @llvm.amdgcn.icmp.i32(i32 9, i32 %x, i32 40)
  ret i64 %result
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_eq_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 32)
define i64 @fold_icmp_ne_0_zext_icmp_eq_i32(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_ne_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 33)
define i64 @fold_icmp_ne_0_zext_icmp_ne_i32(i32 %a, i32 %b) {
  %cmp = icmp ne i32 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_sle_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 41)
define i64 @fold_icmp_ne_0_zext_icmp_sle_i32(i32 %a, i32 %b) {
  %cmp = icmp sle i32 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_ugt_i64(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i64(i64 %a, i64 %b, i32 34)
define i64 @fold_icmp_ne_0_zext_icmp_ugt_i64(i64 %a, i64 %b) {
  %cmp = icmp ugt i64 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_ult_swap_i64(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i64(i64 %a, i64 %b, i32 34)
define i64 @fold_icmp_ne_0_zext_icmp_ult_swap_i64(i64 %a, i64 %b) {
  %cmp = icmp ugt i64 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 0, i32 %zext.cmp, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_fcmp_oeq_f32(
; CHECK-NEXT: call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 1)
define i64 @fold_icmp_ne_0_zext_fcmp_oeq_f32(float %a, float %b) {
  %cmp = fcmp oeq float %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_fcmp_une_f32(
; CHECK-NEXT: call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 14)
define i64 @fold_icmp_ne_0_zext_fcmp_une_f32(float %a, float %b) {
  %cmp = fcmp une float %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_fcmp_olt_f64(
; CHECK-NEXT: call i64 @llvm.amdgcn.fcmp.f64(double %a, double %b, i32 4)
define i64 @fold_icmp_ne_0_zext_fcmp_olt_f64(double %a, double %b) {
  %cmp = fcmp olt double %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_sext_icmp_ne_0_i32(
; CHECK: %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 32)
define i64 @fold_icmp_sext_icmp_ne_0_i32(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %sext.cmp = sext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_eq_0_zext_icmp_eq_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 33)
define i64 @fold_icmp_eq_0_zext_icmp_eq_i32(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_eq_0_zext_icmp_slt_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 39)
define i64 @fold_icmp_eq_0_zext_icmp_slt_i32(i32 %a, i32 %b) {
  %cmp = icmp slt i32 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_eq_0_zext_fcmp_oeq_f32(
; CHECK-NEXT: call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 14)
define i64 @fold_icmp_eq_0_zext_fcmp_oeq_f32(float %a, float %b) {
  %cmp = fcmp oeq float %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_eq_0_zext_fcmp_ule_f32(
; CHECK-NEXT: call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 2)
define i64 @fold_icmp_eq_0_zext_fcmp_ule_f32(float %a, float %b) {
  %cmp = fcmp ule float %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_eq_0_zext_fcmp_ogt_f32(
; CHECK-NEXT: call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 13)
define i64 @fold_icmp_eq_0_zext_fcmp_ogt_f32(float %a, float %b) {
  %cmp = fcmp ogt float %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_zext_icmp_eq_1_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 32)
define i64 @fold_icmp_zext_icmp_eq_1_i32(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_zext_argi1_eq_1_i32(
; CHECK: %zext.cond = zext i1 %cond to i32
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cond, i32 0, i32 33)
define i64 @fold_icmp_zext_argi1_eq_1_i32(i1 %cond) {
  %zext.cond = zext i1 %cond to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cond, i32 1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_zext_argi1_eq_neg1_i32(
; CHECK: %zext.cond = zext i1 %cond to i32
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cond, i32 -1, i32 32)
define i64 @fold_icmp_zext_argi1_eq_neg1_i32(i1 %cond) {
  %zext.cond = zext i1 %cond to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cond, i32 -1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_sext_argi1_eq_1_i32(
; CHECK: %sext.cond = sext i1 %cond to i32
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cond, i32 1, i32 32)
define i64 @fold_icmp_sext_argi1_eq_1_i32(i1 %cond) {
  %sext.cond = sext i1 %cond to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cond, i32 1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_sext_argi1_eq_neg1_i32(
; CHECK: %sext.cond = sext i1 %cond to i32
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cond, i32 0, i32 33)
define i64 @fold_icmp_sext_argi1_eq_neg1_i32(i1 %cond) {
  %sext.cond = sext i1 %cond to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cond, i32 -1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_sext_argi1_eq_neg1_i64(
; CHECK: %sext.cond = sext i1 %cond to i64
; CHECK: call i64 @llvm.amdgcn.icmp.i64(i64 %sext.cond, i64 0, i32 33)
define i64 @fold_icmp_sext_argi1_eq_neg1_i64(i1 %cond) {
  %sext.cond = sext i1 %cond to i64
  %mask = call i64 @llvm.amdgcn.icmp.i64(i64 %sext.cond, i64 -1, i32 32)
  ret i64 %mask
}

; TODO: Should be able to fold to false
; CHECK-LABEL: @fold_icmp_sext_icmp_eq_1_i32(
; CHECK: %cmp = icmp eq i32 %a, %b
; CHECK: %sext.cmp = sext i1 %cmp to i32
; CHECK: %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cmp, i32 1, i32 32)
define i64 @fold_icmp_sext_icmp_eq_1_i32(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %sext.cmp = sext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cmp, i32 1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_sext_icmp_eq_neg1_i32(
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 32)
define i64 @fold_icmp_sext_icmp_eq_neg1_i32(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %sext.cmp = sext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cmp, i32 -1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_sext_icmp_sge_neg1_i32(
; CHECK: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 39)
define i64 @fold_icmp_sext_icmp_sge_neg1_i32(i32 %a, i32 %b) {
  %cmp = icmp sge i32 %a, %b
  %sext.cmp = sext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %sext.cmp, i32 -1, i32 32)
  ret i64 %mask
}

; CHECK-LABEL: @fold_not_icmp_ne_0_zext_icmp_sle_i32(
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i32(i32 %a, i32 %b, i32 38)
define i64 @fold_not_icmp_ne_0_zext_icmp_sle_i32(i32 %a, i32 %b) {
  %cmp = icmp sle i32 %a, %b
  %not = xor i1 %cmp, true
  %zext.cmp = zext i1 %not to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_eq_i4(
; CHECK-NEXT:    [[TMP1:%.*]] = zext i4 [[A:%.*]] to i16
; CHECK-NEXT:    [[TMP2:%.*]] = zext i4 [[B:%.*]] to i16
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 [[TMP1]], i16 [[TMP2]], i32 32)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_eq_i4(i4 %a, i4 %b) {
  %cmp = icmp eq i4 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_eq_i8(
; CHECK-NEXT:    [[TMP1:%.*]] = zext i8 [[A:%.*]] to i16
; CHECK-NEXT:    [[TMP2:%.*]] = zext i8 [[B:%.*]] to i16
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 [[TMP1]], i16 [[TMP2]], i32 32)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_eq_i8(i8 %a, i8 %b) {
  %cmp = icmp eq i8 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_eq_i16(
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 %a, i16 %b, i32 32)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_eq_i16(i16 %a, i16 %b) {
  %cmp = icmp eq i16 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_eq_i36(
; CHECK-NEXT:    [[TMP1:%.*]] = zext i36 [[A:%.*]] to i64
; CHECK-NEXT:    [[TMP2:%.*]] = zext i36 [[B:%.*]] to i64
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i64(i64 [[TMP1]], i64 [[TMP2]], i32 32)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_eq_i36(i36 %a, i36 %b) {
  %cmp = icmp eq i36 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_eq_i128(
; CHECK-NEXT:    [[CMP:%.*]] = icmp eq i128 [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[ZEXT_CMP:%.*]] = zext i1 [[CMP]] to i32
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i32(i32 [[ZEXT_CMP]], i32 0, i32 33)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_eq_i128(i128 %a, i128 %b) {
  %cmp = icmp eq i128 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_fcmp_oeq_f16(
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.fcmp.f16(half [[A:%.*]], half [[B:%.*]], i32 1)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_fcmp_oeq_f16(half %a, half %b) {
  %cmp = fcmp oeq half %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_fcmp_oeq_f128(
; CHECK-NEXT:    [[CMP:%.*]] = fcmp oeq fp128 [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[ZEXT_CMP:%.*]] = zext i1 [[CMP]] to i32
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i32(i32 [[ZEXT_CMP]], i32 0, i32 33)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_fcmp_oeq_f128(fp128 %a, fp128 %b) {
;
  %cmp = fcmp oeq fp128 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_slt_i4(
; CHECK-NEXT:    [[TMP1:%.*]] = sext i4 [[A:%.*]] to i16
; CHECK-NEXT:    [[TMP2:%.*]] = sext i4 [[B:%.*]] to i16
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 [[TMP1]], i16 [[TMP2]], i32 40)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_slt_i4(i4 %a, i4 %b) {
  %cmp = icmp slt i4 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_slt_i8(
; CHECK-NEXT:    [[TMP1:%.*]] = sext i8 [[A:%.*]] to i16
; CHECK-NEXT:    [[TMP2:%.*]] = sext i8 [[B:%.*]] to i16
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 [[TMP1]], i16 [[TMP2]], i32 40)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_slt_i8(i8 %a, i8 %b) {
  %cmp = icmp slt i8 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_slt_i16(
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 %a, i16 %b, i32 40)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_slt_i16(i16 %a, i16 %b) {
  %cmp = icmp slt i16 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_ult_i4(
; CHECK-NEXT:    [[TMP1:%.*]] = zext i4 [[A:%.*]] to i16
; CHECK-NEXT:    [[TMP2:%.*]] = zext i4 [[B:%.*]] to i16
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 [[TMP1]], i16 [[TMP2]], i32 36)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_ult_i4(i4 %a, i4 %b) {
  %cmp = icmp ult i4 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_ult_i8(
; CHECK-NEXT:    [[TMP1:%.*]] = zext i8 [[A:%.*]] to i16
; CHECK-NEXT:    [[TMP2:%.*]] = zext i8 [[B:%.*]] to i16
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 [[TMP1]], i16 [[TMP2]], i32 36)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_ult_i8(i8 %a, i8 %b) {
  %cmp = icmp ult i8 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_ne_0_zext_icmp_ult_i16(
; CHECK-NEXT:    [[MASK:%.*]] = call i64 @llvm.amdgcn.icmp.i16(i16 %a, i16 %b, i32 36)
; CHECK-NEXT:    ret i64 [[MASK]]
define i64 @fold_icmp_ne_0_zext_icmp_ult_i16(i16 %a, i16 %b) {
  %cmp = icmp ult i16 %a, %b
  %zext.cmp = zext i1 %cmp to i32
  %mask = call i64 @llvm.amdgcn.icmp.i32(i32 %zext.cmp, i32 0, i32 33)
  ret i64 %mask
}

; 1-bit NE comparisons

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_eq_i1(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_eq_i1(i32 %a, i32 %b) {
  %cmp = icmp eq i32 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_ne_i1(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_ne_i1(i32 %a, i32 %b) {
  %cmp = icmp ne i32 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_sle_i1(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_sle_i1(i32 %a, i32 %b) {
  %cmp = icmp sle i32 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_ugt_i64(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_ugt_i64(i64 %a, i64 %b) {
  %cmp = icmp ugt i64 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_ult_swap_i64(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_ult_swap_i64(i64 %a, i64 %b) {
  %cmp = icmp ugt i64 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 false, i1 %cmp, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_fcmp_oeq_f32(
; CHECK-NEXT: fcmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_fcmp_oeq_f32(float %a, float %b) {
  %cmp = fcmp oeq float %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_fcmp_une_f32(
; CHECK-NEXT: fcmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_fcmp_une_f32(float %a, float %b) {
  %cmp = fcmp une float %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_fcmp_olt_f64(
; CHECK-NEXT: fcmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_fcmp_olt_f64(double %a, double %b) {
  %cmp = fcmp olt double %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_eq_i4(
; CHECK-NEXT: icmp
; CHECK: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_eq_i4(i4 %a, i4 %b) {
  %cmp = icmp eq i4 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_eq_i8(
; CHECK-NEXT: icmp
; CHECK: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_eq_i8(i8 %a, i8 %b) {
  %cmp = icmp eq i8 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_eq_i16(
; CHECK-NEXT: icmp
; CHECK: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_eq_i16(i16 %a, i16 %b) {
  %cmp = icmp eq i16 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_eq_i36(
; CHECK-NEXT: icmp
; CHECK: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_eq_i36(i36 %a, i36 %b) {
  %cmp = icmp eq i36 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_eq_i128(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_eq_i128(i128 %a, i128 %b) {
  %cmp = icmp eq i128 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_fcmp_oeq_f16(
; CHECK-NEXT: fcmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_fcmp_oeq_f16(half %a, half %b) {
  %cmp = fcmp oeq half %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_fcmp_oeq_f128(
; CHECK-NEXT: fcmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_fcmp_oeq_f128(fp128 %a, fp128 %b) {
;
  %cmp = fcmp oeq fp128 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_slt_i4(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_slt_i4(i4 %a, i4 %b) {
  %cmp = icmp slt i4 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_slt_i8(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_slt_i8(i8 %a, i8 %b) {
  %cmp = icmp slt i8 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_slt_i16(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_slt_i16(i16 %a, i16 %b) {
  %cmp = icmp slt i16 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_ult_i4(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_ult_i4(i4 %a, i4 %b) {
  %cmp = icmp ult i4 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_ult_i8(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_ult_i8(i8 %a, i8 %b) {
  %cmp = icmp ult i8 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; CHECK-LABEL: @fold_icmp_i1_ne_0_icmp_ult_i16(
; CHECK-NEXT: icmp
; CHECK-NEXT: call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
define i64 @fold_icmp_i1_ne_0_icmp_ult_i16(i16 %a, i16 %b) {
  %cmp = icmp ult i16 %a, %b
  %mask = call i64 @llvm.amdgcn.icmp.i1(i1 %cmp, i1 false, i32 33)
  ret i64 %mask
}

; --------------------------------------------------------------------
; llvm.amdgcn.fcmp
; --------------------------------------------------------------------

declare i64 @llvm.amdgcn.fcmp.f32(float, float, i32) nounwind readnone convergent

; Make sure there's no crash for invalid input
; CHECK-LABEL: @invalid_nonconstant_fcmp_code(
; CHECK: call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 %c)
define i64 @invalid_nonconstant_fcmp_code(float %a, float %b, i32 %c) {
  %result = call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 %c)
  ret i64 %result
}

; CHECK-LABEL: @invalid_fcmp_code(
; CHECK: %under = call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 -1)
; CHECK: %over = call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 16)
define i64 @invalid_fcmp_code(float %a, float %b) {
  %under = call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 -1)
  %over = call i64 @llvm.amdgcn.fcmp.f32(float %a, float %b, i32 16)
  %or = or i64 %under, %over
  ret i64 %or
}

; CHECK-LABEL: @fcmp_constant_inputs_false(
; CHECK: ret i64 0
define i64 @fcmp_constant_inputs_false() {
  %result = call i64 @llvm.amdgcn.fcmp.f32(float 2.0, float 4.0, i32 1)
  ret i64 %result
}

; CHECK-LABEL: @fcmp_constant_inputs_true(
; CHECK: %result = call i64 @llvm.read_register.i64(metadata !0) #5
define i64 @fcmp_constant_inputs_true() {
  %result = call i64 @llvm.amdgcn.fcmp.f32(float 2.0, float 4.0, i32 4)
  ret i64 %result
}

; CHECK-LABEL: @fcmp_constant_to_rhs_olt(
; CHECK: %result = call i64 @llvm.amdgcn.fcmp.f32(float %x, float 4.000000e+00, i32 2)
define i64 @fcmp_constant_to_rhs_olt(float %x) {
  %result = call i64 @llvm.amdgcn.fcmp.f32(float 4.0, float %x, i32 4)
  ret i64 %result
}

; --------------------------------------------------------------------
; llvm.amdgcn.wqm.vote
; --------------------------------------------------------------------

declare i1 @llvm.amdgcn.wqm.vote(i1)

; CHECK-LABEL: @wqm_vote_true(
; CHECK: ret float 1.000000e+00
define float @wqm_vote_true() {
main_body:
  %w = call i1 @llvm.amdgcn.wqm.vote(i1 true)
  %r = select i1 %w, float 1.0, float 0.0
  ret float %r
}

; CHECK-LABEL: @wqm_vote_false(
; CHECK: ret float 0.000000e+00
define float @wqm_vote_false() {
main_body:
  %w = call i1 @llvm.amdgcn.wqm.vote(i1 false)
  %r = select i1 %w, float 1.0, float 0.0
  ret float %r
}

; CHECK-LABEL: @wqm_vote_undef(
; CHECK: ret float 0.000000e+00
define float @wqm_vote_undef() {
main_body:
  %w = call i1 @llvm.amdgcn.wqm.vote(i1 undef)
  %r = select i1 %w, float 1.0, float 0.0
  ret float %r
}

; --------------------------------------------------------------------
; llvm.amdgcn.kill
; --------------------------------------------------------------------

declare void @llvm.amdgcn.kill(i1)

; CHECK-LABEL: @kill_true() {
; CHECK-NEXT: ret void
; CHECK-NEXT: }
define void @kill_true() {
  call void @llvm.amdgcn.kill(i1 true)
  ret void
}

; --------------------------------------------------------------------
; llvm.amdgcn.update.dpp.i32
; --------------------------------------------------------------------

declare i32 @llvm.amdgcn.update.dpp.i32(i32, i32, i32, i32, i32, i1)

; CHECK-LABEL: {{^}}define amdgpu_kernel void @update_dpp_no_combine(
; CHECK: @llvm.amdgcn.update.dpp.i32(i32 %in1, i32 %in2, i32 1, i32 1, i32 1, i1 false)
define amdgpu_kernel void @update_dpp_no_combine(i32 addrspace(1)* %out, i32 %in1, i32 %in2) {
  %tmp0 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %in1, i32 %in2, i32 1, i32 1, i32 1, i1 0)
  store i32 %tmp0, i32 addrspace(1)* %out
  ret void
}

; CHECK-LABEL: {{^}}define amdgpu_kernel void @update_dpp_drop_old(
; CHECK: @llvm.amdgcn.update.dpp.i32(i32 undef, i32 %in2, i32 3, i32 15, i32 15, i1 true)
define amdgpu_kernel void @update_dpp_drop_old(i32 addrspace(1)* %out, i32 %in1, i32 %in2) {
  %tmp0 = call i32 @llvm.amdgcn.update.dpp.i32(i32 %in1, i32 %in2, i32 3, i32 15, i32 15, i1 1)
  store i32 %tmp0, i32 addrspace(1)* %out
  ret void
}

; CHECK-LABEL: {{^}}define amdgpu_kernel void @update_dpp_undef_old(
; CHECK: @llvm.amdgcn.update.dpp.i32(i32 undef, i32 %in1, i32 4, i32 15, i32 15, i1 true)
define amdgpu_kernel void @update_dpp_undef_old(i32 addrspace(1)* %out, i32 %in1) {
  %tmp0 = call i32 @llvm.amdgcn.update.dpp.i32(i32 undef, i32 %in1, i32 4, i32 15, i32 15, i1 1)
  store i32 %tmp0, i32 addrspace(1)* %out
  ret void
}

; CHECK: attributes #5 = { convergent }
