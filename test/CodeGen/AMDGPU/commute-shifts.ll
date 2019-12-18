; RUN: llc -march=amdgcn -mcpu=verde -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=SI %s
; RUN: llc -march=amdgcn -mcpu=tonga -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=VI %s

; GCN-LABEL: {{^}}main:
; SI: v_lshl_b32_e32 v{{[0-9]+}}, 1, v{{[0-9]+}}
; VI: v_lshlrev_b32_e64 v{{[0-9]+}}, v{{[0-9]+}}, 1
define amdgpu_ps float @main(float %arg0, float %arg1) #0 {
bb:
  %tmp = fptosi float %arg0 to i32
  %tmp1 = call <4 x float> @llvm.amdgcn.image.load.1d.v4f32.i32(i32 15, i32 undef, <8 x i32> undef, i32 0, i32 0)
  %tmp2.f = extractelement <4 x float> %tmp1, i32 0
  %tmp2 = bitcast float %tmp2.f to i32
  %tmp3 = and i32 %tmp, 7
  %tmp4 = shl i32 1, %tmp3
  %tmp5 = and i32 %tmp2, %tmp4
  %tmp6 = icmp eq i32 %tmp5, 0
  %tmp7 = select i1 %tmp6, float 0.000000e+00, float %arg1
  %tmp8 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float undef, float %tmp7)
  %tmp9 = bitcast <2 x half> %tmp8 to float
  ret float %tmp9
}

declare <2 x half> @llvm.amdgcn.cvt.pkrtz(float, float) #1
declare <4 x float> @llvm.amdgcn.image.load.1d.v4f32.i32(i32, i32, <8 x i32>, i32, i32) #2

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }
