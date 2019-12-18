;RUN: llc < %s -march=amdgcn -mcpu=verde -verify-machineinstrs | FileCheck %s -check-prefix=CHECK -check-prefix=SICI
;RUN: llc < %s -march=amdgcn -mcpu=tonga -verify-machineinstrs | FileCheck %s -check-prefix=CHECK -check-prefix=VI

;CHECK-LABEL: {{^}}buffer_load:
;CHECK: buffer_load_dwordx4 v[0:3], off, s[0:3], 0
;CHECK: buffer_load_dwordx4 v[4:7], off, s[0:3], 0 glc
;CHECK: buffer_load_dwordx4 v[8:11], off, s[0:3], 0 slc
;CHECK: s_waitcnt
define amdgpu_ps {<4 x float>, <4 x float>, <4 x float>} @buffer_load(<4 x i32> inreg) {
main_body:
  %data = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 0, i32 0, i32 0)
  %data_glc = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 0, i32 0, i32 1)
  %data_slc = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 0, i32 0, i32 2)
  %r0 = insertvalue {<4 x float>, <4 x float>, <4 x float>} undef, <4 x float> %data, 0
  %r1 = insertvalue {<4 x float>, <4 x float>, <4 x float>} %r0, <4 x float> %data_glc, 1
  %r2 = insertvalue {<4 x float>, <4 x float>, <4 x float>} %r1, <4 x float> %data_slc, 2
  ret {<4 x float>, <4 x float>, <4 x float>} %r2
}

;CHECK-LABEL: {{^}}buffer_load_immoffs:
;CHECK: buffer_load_dwordx4 v[0:3], off, s[0:3], 0 offset:40
;CHECK: s_waitcnt
define amdgpu_ps <4 x float> @buffer_load_immoffs(<4 x i32> inreg) {
main_body:
  %data = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 40, i32 0, i32 0)
  ret <4 x float> %data
}

;CHECK-LABEL: {{^}}buffer_load_immoffs_large:
;CHECK: s_movk_i32 [[OFFSET:s[0-9]+]], 0x1ffc
;CHECK: buffer_load_dwordx4 v[0:3], off, s[0:3], [[OFFSET]] offset:4
;CHECK: s_waitcnt
define amdgpu_ps <4 x float> @buffer_load_immoffs_large(<4 x i32> inreg) {
main_body:
  %data = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 4, i32 8188, i32 0)
  ret <4 x float> %data
}

;CHECK-LABEL: {{^}}buffer_load_ofs:
;CHECK: buffer_load_dwordx4 v[0:3], v0, s[0:3], 0 offen
;CHECK: s_waitcnt
define amdgpu_ps <4 x float> @buffer_load_ofs(<4 x i32> inreg, i32) {
main_body:
  %data = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 %1, i32 0, i32 0)
  ret <4 x float> %data
}

;CHECK-LABEL: {{^}}buffer_load_ofs_imm:
;CHECK: buffer_load_dwordx4 v[0:3], v0, s[0:3], 0 offen offset:60
;CHECK: s_waitcnt
define amdgpu_ps <4 x float> @buffer_load_ofs_imm(<4 x i32> inreg, i32) {
main_body:
  %ofs = add i32 %1, 60
  %data = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 %ofs, i32 0, i32 0)
  ret <4 x float> %data
}

;CHECK-LABEL: {{^}}buffer_load_x1:
;CHECK: buffer_load_dword v0, v0, s[0:3], 0 offen
;CHECK: s_waitcnt
define amdgpu_ps float @buffer_load_x1(<4 x i32> inreg %rsrc, i32 %ofs) {
main_body:
  %data = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %ofs, i32 0, i32 0)
  ret float %data
}

;CHECK-LABEL: {{^}}buffer_load_x2:
;CHECK: buffer_load_dwordx2 v[0:1], v0, s[0:3], 0 offen
;CHECK: s_waitcnt
define amdgpu_ps <2 x float> @buffer_load_x2(<4 x i32> inreg %rsrc, i32 %ofs) {
main_body:
  %data = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 %ofs, i32 0, i32 0)
  ret <2 x float> %data
}

;CHECK-LABEL: {{^}}buffer_load_negative_offset:
;CHECK: v_add_{{[iu]}}32_e32 [[VOFS:v[0-9]+]], vcc, -16, v0
;CHECK: buffer_load_dwordx4 v[0:3], [[VOFS]], s[0:3], 0 offen
define amdgpu_ps <4 x float> @buffer_load_negative_offset(<4 x i32> inreg, i32 %ofs) {
main_body:
  %ofs.1 = add i32 %ofs, -16
  %data = call <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32> %0, i32 %ofs.1, i32 0, i32 0)
  ret <4 x float> %data
}

; SI won't merge ds memory operations, because of the signed offset bug, so
; we only have check lines for VI.
; CHECK-LABEL: buffer_load_mmo:
; VI: v_mov_b32_e32 [[ZERO:v[0-9]+]], 0
; VI: ds_write2_b32 v{{[0-9]+}}, [[ZERO]], [[ZERO]] offset1:4
define amdgpu_ps float @buffer_load_mmo(<4 x i32> inreg %rsrc, float addrspace(3)* %lds) {
entry:
  store float 0.0, float addrspace(3)* %lds
  %val = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 0, i32 0, i32 0)
  %tmp2 = getelementptr float, float addrspace(3)* %lds, i32 4
  store float 0.0, float addrspace(3)* %tmp2
  ret float %val
}

;CHECK-LABEL: {{^}}buffer_load_x1_offen_merged_and:
;CHECK-NEXT: %bb.
;CHECK-NEXT: buffer_load_dwordx4 v[{{[0-9]}}:{{[0-9]}}], v0, s[0:3], 0 offen offset:4
;CHECK-NEXT: buffer_load_dwordx2 v[{{[0-9]}}:{{[0-9]}}], v0, s[0:3], 0 offen offset:28
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x1_offen_merged_and(<4 x i32> inreg %rsrc, i32 %a) {
main_body:
  %a1 = add i32 %a, 4
  %a2 = add i32 %a, 8
  %a3 = add i32 %a, 12
  %a4 = add i32 %a, 16
  %a5 = add i32 %a, 28
  %a6 = add i32 %a, 32
  %r1 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a1, i32 0, i32 0)
  %r2 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a2, i32 0, i32 0)
  %r3 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a3, i32 0, i32 0)
  %r4 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a4, i32 0, i32 0)
  %r5 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a5, i32 0, i32 0)
  %r6 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a6, i32 0, i32 0)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r5, float %r6, float undef, float undef, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_x1_offen_merged_or:
;CHECK-NEXT: %bb.
;CHECK-NEXT: v_lshlrev_b32_e32 v{{[0-9]}}, 6, v0
;CHECK-NEXT: buffer_load_dwordx4 v[{{[0-9]}}:{{[0-9]}}], v{{[0-9]}}, s[0:3], 0 offen offset:4
;CHECK-NEXT: buffer_load_dwordx2 v[{{[0-9]}}:{{[0-9]}}], v{{[0-9]}}, s[0:3], 0 offen offset:28
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x1_offen_merged_or(<4 x i32> inreg %rsrc, i32 %inp) {
main_body:
  %a = shl i32 %inp, 6
  %a1 = or i32 %a, 4
  %a2 = or i32 %a, 8
  %a3 = or i32 %a, 12
  %a4 = or i32 %a, 16
  %a5 = or i32 %a, 28
  %a6 = or i32 %a, 32
  %r1 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a1, i32 0, i32 0)
  %r2 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a2, i32 0, i32 0)
  %r3 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a3, i32 0, i32 0)
  %r4 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a4, i32 0, i32 0)
  %r5 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a5, i32 0, i32 0)
  %r6 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a6, i32 0, i32 0)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r5, float %r6, float undef, float undef, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_x1_offen_merged_glc_slc:
;CHECK-NEXT: %bb.
;CHECK-NEXT: buffer_load_dwordx2 v[{{[0-9]}}:{{[0-9]}}], v0, s[0:3], 0 offen offset:4{{$}}
;CHECK-NEXT: buffer_load_dwordx2 v[{{[0-9]}}:{{[0-9]}}], v0, s[0:3], 0 offen offset:12 glc{{$}}
;CHECK-NEXT: buffer_load_dwordx2 v[{{[0-9]}}:{{[0-9]}}], v0, s[0:3], 0 offen offset:28 glc slc{{$}}
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x1_offen_merged_glc_slc(<4 x i32> inreg %rsrc, i32 %a) {
main_body:
  %a1 = add i32 %a, 4
  %a2 = add i32 %a, 8
  %a3 = add i32 %a, 12
  %a4 = add i32 %a, 16
  %a5 = add i32 %a, 28
  %a6 = add i32 %a, 32
  %r1 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a1, i32 0, i32 0)
  %r2 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a2, i32 0, i32 0)
  %r3 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a3, i32 0, i32 1)
  %r4 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a4, i32 0, i32 1)
  %r5 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a5, i32 0, i32 3)
  %r6 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 %a6, i32 0, i32 3)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r5, float %r6, float undef, float undef, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_x2_offen_merged_and:
;CHECK-NEXT: %bb.
;CHECK-NEXT: buffer_load_dwordx4 v[{{[0-9]}}:{{[0-9]}}], v0, s[0:3], 0 offen offset:4
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x2_offen_merged_and(<4 x i32> inreg %rsrc, i32 %a) {
main_body:
  %a1 = add i32 %a, 4
  %a2 = add i32 %a, 12
  %vr1 = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 %a1, i32 0, i32 0)
  %vr2 = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 %a2, i32 0, i32 0)
  %r1 = extractelement <2 x float> %vr1, i32 0
  %r2 = extractelement <2 x float> %vr1, i32 1
  %r3 = extractelement <2 x float> %vr2, i32 0
  %r4 = extractelement <2 x float> %vr2, i32 1
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_x2_offen_merged_or:
;CHECK-NEXT: %bb.
;CHECK-NEXT: v_lshlrev_b32_e32 v{{[0-9]}}, 4, v0
;CHECK-NEXT: buffer_load_dwordx4 v[{{[0-9]}}:{{[0-9]}}], v{{[0-9]}}, s[0:3], 0 offen offset:4
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x2_offen_merged_or(<4 x i32> inreg %rsrc, i32 %inp) {
main_body:
  %a = shl i32 %inp, 4
  %a1 = add i32 %a, 4
  %a2 = add i32 %a, 12
  %vr1 = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 %a1, i32 0, i32 0)
  %vr2 = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 %a2, i32 0, i32 0)
  %r1 = extractelement <2 x float> %vr1, i32 0
  %r2 = extractelement <2 x float> %vr1, i32 1
  %r3 = extractelement <2 x float> %vr2, i32 0
  %r4 = extractelement <2 x float> %vr2, i32 1
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_x1_offset_merged:
;CHECK-NEXT: %bb.
;CHECK-NEXT: buffer_load_dwordx4 v[{{[0-9]}}:{{[0-9]}}], off, s[0:3], 0 offset:4
;CHECK-NEXT: buffer_load_dwordx2 v[{{[0-9]}}:{{[0-9]}}], off, s[0:3], 0 offset:28
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x1_offset_merged(<4 x i32> inreg %rsrc) {
main_body:
  %r1 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 4, i32 0, i32 0)
  %r2 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 8, i32 0, i32 0)
  %r3 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 12, i32 0, i32 0)
  %r4 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 16, i32 0, i32 0)
  %r5 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 28, i32 0, i32 0)
  %r6 = call float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32> %rsrc, i32 32, i32 0, i32 0)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r5, float %r6, float undef, float undef, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_x2_offset_merged:
;CHECK-NEXT: %bb.
;CHECK-NEXT: buffer_load_dwordx4 v[{{[0-9]}}:{{[0-9]}}], off, s[0:3], 0 offset:4
;CHECK: s_waitcnt
define amdgpu_ps void @buffer_load_x2_offset_merged(<4 x i32> inreg %rsrc) {
main_body:
  %vr1 = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 4, i32 0, i32 0)
  %vr2 = call <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32> %rsrc, i32 12, i32 0, i32 0)
  %r1 = extractelement <2 x float> %vr1, i32 0
  %r2 = extractelement <2 x float> %vr1, i32 1
  %r3 = extractelement <2 x float> %vr2, i32 0
  %r4 = extractelement <2 x float> %vr2, i32 1
  call void @llvm.amdgcn.exp.f32(i32 0, i32 15, float %r1, float %r2, float %r3, float %r4, i1 true, i1 true)
  ret void
}

;CHECK-LABEL: {{^}}buffer_load_int:
;CHECK: buffer_load_dwordx4 v[0:3], off, s[0:3], 0
;CHECK: buffer_load_dwordx2 v[4:5], off, s[0:3], 0 glc
;CHECK: buffer_load_dword v6, off, s[0:3], 0 slc
;CHECK: s_waitcnt
define amdgpu_ps {<4 x float>, <2 x float>, float} @buffer_load_int(<4 x i32> inreg) {
main_body:
  %data = call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> %0, i32 0, i32 0, i32 0)
  %data_glc = call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32(<4 x i32> %0, i32 0, i32 0, i32 1)
  %data_slc = call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> %0, i32 0, i32 0, i32 2)
  %fdata = bitcast <4 x i32> %data to <4 x float>
  %fdata_glc = bitcast <2 x i32> %data_glc to <2 x float>
  %fdata_slc = bitcast i32 %data_slc to float
  %r0 = insertvalue {<4 x float>, <2 x float>, float} undef, <4 x float> %fdata, 0
  %r1 = insertvalue {<4 x float>, <2 x float>, float} %r0, <2 x float> %fdata_glc, 1
  %r2 = insertvalue {<4 x float>, <2 x float>, float} %r1, float %fdata_slc, 2
  ret {<4 x float>, <2 x float>, float} %r2
}

declare float @llvm.amdgcn.raw.buffer.load.f32(<4 x i32>, i32, i32, i32) #0
declare <2 x float> @llvm.amdgcn.raw.buffer.load.v2f32(<4 x i32>, i32, i32, i32) #0
declare <4 x float> @llvm.amdgcn.raw.buffer.load.v4f32(<4 x i32>, i32, i32, i32) #0
declare i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32>, i32, i32, i32) #0
declare <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32(<4 x i32>, i32, i32, i32) #0
declare <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32>, i32, i32, i32) #0
declare void @llvm.amdgcn.exp.f32(i32, i32, float, float, float, float, i1, i1) #0

attributes #0 = { nounwind readonly }
