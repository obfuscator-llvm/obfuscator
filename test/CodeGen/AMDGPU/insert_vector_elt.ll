; RUN: llc -verify-machineinstrs -march=amdgcn -mattr=+max-private-element-size-16 < %s | FileCheck -check-prefix=GCN -check-prefix=SI -check-prefix=GCN-NO-TONGA %s
; RUN: llc -verify-machineinstrs -march=amdgcn -mcpu=tonga -mattr=-flat-for-global -mattr=+max-private-element-size-16 < %s | FileCheck -check-prefix=GCN -check-prefix=SI -check-prefix=GCN-TONGA %s

; FIXME: Broken on evergreen
; FIXME: For some reason the 8 and 16 vectors are being stored as
; individual elements instead of 128-bit stores.


; FIXME: Why is the constant moved into the intermediate register and
; not just directly into the vector component?

; GCN-LABEL: {{^}}insertelement_v4f32_0:
; GCN: s_load_dwordx4
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: s_mov_b32 [[CONSTREG:s[0-9]+]], 0x40a00000
; GCN-DAG: v_mov_b32_e32 v[[LOW_REG:[0-9]+]], [[CONSTREG]]
; GCN: buffer_store_dwordx4 v{{\[}}[[LOW_REG]]:
define amdgpu_kernel void @insertelement_v4f32_0(<4 x float> addrspace(1)* %out, <4 x float> %a) nounwind {
  %vecins = insertelement <4 x float> %a, float 5.000000e+00, i32 0
  store <4 x float> %vecins, <4 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v4f32_1:
define amdgpu_kernel void @insertelement_v4f32_1(<4 x float> addrspace(1)* %out, <4 x float> %a) nounwind {
  %vecins = insertelement <4 x float> %a, float 5.000000e+00, i32 1
  store <4 x float> %vecins, <4 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v4f32_2:
define amdgpu_kernel void @insertelement_v4f32_2(<4 x float> addrspace(1)* %out, <4 x float> %a) nounwind {
  %vecins = insertelement <4 x float> %a, float 5.000000e+00, i32 2
  store <4 x float> %vecins, <4 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v4f32_3:
define amdgpu_kernel void @insertelement_v4f32_3(<4 x float> addrspace(1)* %out, <4 x float> %a) nounwind {
  %vecins = insertelement <4 x float> %a, float 5.000000e+00, i32 3
  store <4 x float> %vecins, <4 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v4i32_0:
define amdgpu_kernel void @insertelement_v4i32_0(<4 x i32> addrspace(1)* %out, <4 x i32> %a) nounwind {
  %vecins = insertelement <4 x i32> %a, i32 999, i32 0
  store <4 x i32> %vecins, <4 x i32> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v3f32_1:
define amdgpu_kernel void @insertelement_v3f32_1(<3 x float> addrspace(1)* %out, <3 x float> %a) nounwind {
  %vecins = insertelement <3 x float> %a, float 5.000000e+00, i32 1
  store <3 x float> %vecins, <3 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v3f32_2:
define amdgpu_kernel void @insertelement_v3f32_2(<3 x float> addrspace(1)* %out, <3 x float> %a) nounwind {
  %vecins = insertelement <3 x float> %a, float 5.000000e+00, i32 2
  store <3 x float> %vecins, <3 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_v3f32_3:
define amdgpu_kernel void @insertelement_v3f32_3(<3 x float> addrspace(1)* %out, <3 x float> %a) nounwind {
  %vecins = insertelement <3 x float> %a, float 5.000000e+00, i32 3
  store <3 x float> %vecins, <3 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}insertelement_to_sgpr:
; GCN-NOT: v_readfirstlane
define amdgpu_ps <4 x float> @insertelement_to_sgpr() nounwind {
  %tmp = load <4 x i32>, <4 x i32> addrspace(2)* undef
  %tmp1 = insertelement <4 x i32> %tmp, i32 0, i32 0
  %tmp2 = call <4 x float> @llvm.amdgcn.image.gather4.lz.v4f32.v2f32.v8i32(<2 x float> undef, <8 x i32> undef, <4 x i32> undef, i32 1, i1 false, i1 false, i1 false, i1 false, i1 true)
  ret <4 x float> %tmp2
}

; GCN-LABEL: {{^}}dynamic_insertelement_v2f32:
; GCN: v_mov_b32_e32 [[CONST:v[0-9]+]], 0x40a00000
; GCN: v_movreld_b32_e32 v[[LOW_RESULT_REG:[0-9]+]], [[CONST]]
; GCN: buffer_store_dwordx2 {{v\[}}[[LOW_RESULT_REG]]:
define amdgpu_kernel void @dynamic_insertelement_v2f32(<2 x float> addrspace(1)* %out, <2 x float> %a, i32 %b) nounwind {
  %vecins = insertelement <2 x float> %a, float 5.000000e+00, i32 %b
  store <2 x float> %vecins, <2 x float> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v3f32:
; GCN: v_mov_b32_e32 [[CONST:v[0-9]+]], 0x40a00000
; GCN: v_movreld_b32_e32 v[[LOW_RESULT_REG:[0-9]+]], [[CONST]]
; GCN-DAG: buffer_store_dwordx2 {{v\[}}[[LOW_RESULT_REG]]:
; GCN-DAG: buffer_store_dword v
define amdgpu_kernel void @dynamic_insertelement_v3f32(<3 x float> addrspace(1)* %out, <3 x float> %a, i32 %b) nounwind {
  %vecins = insertelement <3 x float> %a, float 5.000000e+00, i32 %b
  store <3 x float> %vecins, <3 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v4f32:
; GCN: v_mov_b32_e32 [[CONST:v[0-9]+]], 0x40a00000
; GCN: v_movreld_b32_e32 v[[LOW_RESULT_REG:[0-9]+]], [[CONST]]
; GCN: buffer_store_dwordx4 {{v\[}}[[LOW_RESULT_REG]]:
define amdgpu_kernel void @dynamic_insertelement_v4f32(<4 x float> addrspace(1)* %out, <4 x float> %a, i32 %b) nounwind {
  %vecins = insertelement <4 x float> %a, float 5.000000e+00, i32 %b
  store <4 x float> %vecins, <4 x float> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v8f32:
; GCN: v_movreld_b32_e32 v{{[0-9]+}}, v{{[0-9]+}}
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
define amdgpu_kernel void @dynamic_insertelement_v8f32(<8 x float> addrspace(1)* %out, <8 x float> %a, i32 %b) nounwind {
  %vecins = insertelement <8 x float> %a, float 5.000000e+00, i32 %b
  store <8 x float> %vecins, <8 x float> addrspace(1)* %out, align 32
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v16f32:
; GCN: v_movreld_b32_e32 v{{[0-9]+}}, v{{[0-9]+}}
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
define amdgpu_kernel void @dynamic_insertelement_v16f32(<16 x float> addrspace(1)* %out, <16 x float> %a, i32 %b) nounwind {
  %vecins = insertelement <16 x float> %a, float 5.000000e+00, i32 %b
  store <16 x float> %vecins, <16 x float> addrspace(1)* %out, align 64
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v2i32:
; GCN: v_movreld_b32
; GCN: buffer_store_dwordx2
define amdgpu_kernel void @dynamic_insertelement_v2i32(<2 x i32> addrspace(1)* %out, <2 x i32> %a, i32 %b) nounwind {
  %vecins = insertelement <2 x i32> %a, i32 5, i32 %b
  store <2 x i32> %vecins, <2 x i32> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v3i32:
; GCN: v_movreld_b32_e32 v[[LOW_RESULT_REG:[0-9]+]], 5
; GCN-DAG: buffer_store_dwordx2 {{v\[}}[[LOW_RESULT_REG]]:
; GCN-DAG: buffer_store_dword v
define amdgpu_kernel void @dynamic_insertelement_v3i32(<3 x i32> addrspace(1)* %out, <3 x i32> %a, i32 %b) nounwind {
  %vecins = insertelement <3 x i32> %a, i32 5, i32 %b
  store <3 x i32> %vecins, <3 x i32> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v4i32:
; GCN: s_load_dword [[SVAL:s[0-9]+]], s{{\[[0-9]+:[0-9]+\]}}, {{0x12|0x48}}
; GCN: v_mov_b32_e32 [[VVAL:v[0-9]+]], [[SVAL]]
; GCN: v_movreld_b32_e32 v{{[0-9]+}}, [[VVAL]]
; GCN: buffer_store_dwordx4
define amdgpu_kernel void @dynamic_insertelement_v4i32(<4 x i32> addrspace(1)* %out, <4 x i32> %a, i32 %b, i32 %val) nounwind {
  %vecins = insertelement <4 x i32> %a, i32 %val, i32 %b
  store <4 x i32> %vecins, <4 x i32> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v8i32:
; GCN: v_movreld_b32
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
define amdgpu_kernel void @dynamic_insertelement_v8i32(<8 x i32> addrspace(1)* %out, <8 x i32> %a, i32 %b) nounwind {
  %vecins = insertelement <8 x i32> %a, i32 5, i32 %b
  store <8 x i32> %vecins, <8 x i32> addrspace(1)* %out, align 32
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v16i32:
; GCN: v_movreld_b32
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
define amdgpu_kernel void @dynamic_insertelement_v16i32(<16 x i32> addrspace(1)* %out, <16 x i32> %a, i32 %b) nounwind {
  %vecins = insertelement <16 x i32> %a, i32 5, i32 %b
  store <16 x i32> %vecins, <16 x i32> addrspace(1)* %out, align 64
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v2i16:
define amdgpu_kernel void @dynamic_insertelement_v2i16(<2 x i16> addrspace(1)* %out, <2 x i16> %a, i32 %b) nounwind {
  %vecins = insertelement <2 x i16> %a, i16 5, i32 %b
  store <2 x i16> %vecins, <2 x i16> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v3i16:
define amdgpu_kernel void @dynamic_insertelement_v3i16(<3 x i16> addrspace(1)* %out, <3 x i16> %a, i32 %b) nounwind {
  %vecins = insertelement <3 x i16> %a, i16 5, i32 %b
  store <3 x i16> %vecins, <3 x i16> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v4i16:
; GCN: buffer_load_ushort v{{[0-9]+}}, off
; GCN: buffer_load_ushort v{{[0-9]+}}, off
; GCN: buffer_load_ushort v{{[0-9]+}}, off
; GCN: buffer_load_ushort v{{[0-9]+}}, off

; GCN-DAG: v_mov_b32_e32 [[BASE_FI:v[0-9]+]], 8{{$}}
; GCN-DAG: s_and_b32 [[MASK_IDX:s[0-9]+]], s{{[0-9]+}}, 3{{$}}
; GCN-DAG: v_or_b32_e32 [[IDX:v[0-9]+]], [[MASK_IDX]], [[BASE_FI]]{{$}}

; GCN-DAG: buffer_store_short v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:14
; GCN-DAG: buffer_store_short v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:12
; GCN-DAG: buffer_store_short v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:10
; GCN-DAG: buffer_store_short v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:8
; GCN: buffer_store_short v{{[0-9]+}}, [[IDX]], s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offen{{$}}

; GCN: s_waitcnt

; GCN: buffer_load_dwordx2

; GCN: buffer_store_dwordx2 v{{\[[0-9]+:[0-9]+\]}}, off
define amdgpu_kernel void @dynamic_insertelement_v4i16(<4 x i16> addrspace(1)* %out, <4 x i16> %a, i32 %b) nounwind {
  %vecins = insertelement <4 x i16> %a, i16 5, i32 %b
  store <4 x i16> %vecins, <4 x i16> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v2i8:
; GCN: buffer_load_ubyte v{{[0-9]+}}, off
; GCN: buffer_load_ubyte v{{[0-9]+}}, off

; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:5
; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:4

; GCN: buffer_store_byte v{{[0-9]+}}, v{{[0-9]+}}, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offen{{$}}

; GCN-NO-TONGA: buffer_load_ubyte
; GCN-NO-TONGA: buffer_load_ubyte
; GCN-TONGA: buffer_load_ushort

; GCN: buffer_store_short v{{[0-9]+}}, off
define amdgpu_kernel void @dynamic_insertelement_v2i8(<2 x i8> addrspace(1)* %out, <2 x i8> %a, i32 %b) nounwind {
  %vecins = insertelement <2 x i8> %a, i8 5, i32 %b
  store <2 x i8> %vecins, <2 x i8> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v3i8:
; GCN: buffer_load_ubyte v{{[0-9]+}}, off
; GCN: buffer_load_ubyte v{{[0-9]+}}, off
; GCN: buffer_load_ubyte v{{[0-9]+}}, off

; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:4
; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:5
; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:6

; GCN-NO-TONGA: buffer_load_ubyte
; GCN-NO-TONGA: buffer_load_ubyte
; GCN-NO-TONGA: buffer_load_ubyte
; GCN-TONGA: buffer_load_ushort
; GCN-TONGA: buffer_load_ubyte

; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off
; GCN-DAG: buffer_store_short v{{[0-9]+}}, off
define amdgpu_kernel void @dynamic_insertelement_v3i8(<3 x i8> addrspace(1)* %out, <3 x i8> %a, i32 %b) nounwind {
  %vecins = insertelement <3 x i8> %a, i8 5, i32 %b
  store <3 x i8> %vecins, <3 x i8> addrspace(1)* %out, align 4
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v4i8:
; GCN: buffer_load_ubyte v{{[0-9]+}}, off
; GCN: buffer_load_ubyte v{{[0-9]+}}, off
; GCN: buffer_load_ubyte v{{[0-9]+}}, off
; GCN: buffer_load_ubyte v{{[0-9]+}}, off

; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:7
; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:6
; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:5
; GCN-DAG: buffer_store_byte v{{[0-9]+}}, off, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offset:4

; GCN: buffer_store_byte v{{[0-9]+}}, v{{[0-9]+}}, s{{\[[0-9]+:[0-9]+\]}}, s{{[0-9]+}} offen{{$}}

; GCN-NO-TONGA: buffer_load_ubyte
; GCN-NO-TONGA: buffer_load_ubyte
; GCN-NO-TONGA: buffer_load_ubyte
; GCN-NO-TONGA: buffer_load_ubyte
; GCN-TONGA: buffer_load_dword

; GCN: buffer_store_dword v{{[0-9]+}}, off
define amdgpu_kernel void @dynamic_insertelement_v4i8(<4 x i8> addrspace(1)* %out, <4 x i8> %a, i32 %b) nounwind {
  %vecins = insertelement <4 x i8> %a, i8 5, i32 %b
  store <4 x i8> %vecins, <4 x i8> addrspace(1)* %out, align 4
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v8i8:
define amdgpu_kernel void @dynamic_insertelement_v8i8(<8 x i8> addrspace(1)* %out, <8 x i8> %a, i32 %b) nounwind {
  %vecins = insertelement <8 x i8> %a, i8 5, i32 %b
  store <8 x i8> %vecins, <8 x i8> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v16i8:
define amdgpu_kernel void @dynamic_insertelement_v16i8(<16 x i8> addrspace(1)* %out, <16 x i8> %a, i32 %b) nounwind {
  %vecins = insertelement <16 x i8> %a, i8 5, i32 %b
  store <16 x i8> %vecins, <16 x i8> addrspace(1)* %out, align 16
  ret void
}

; This test requires handling INSERT_SUBREG in SIFixSGPRCopies.  Check that
; the compiler doesn't crash.
; GCN-LABEL: {{^}}insert_split_bb:
define amdgpu_kernel void @insert_split_bb(<2 x i32> addrspace(1)* %out, i32 addrspace(1)* %in, i32 %a, i32 %b) {
entry:
  %0 = insertelement <2 x i32> undef, i32 %a, i32 0
  %1 = icmp eq i32 %a, 0
  br i1 %1, label %if, label %else

if:
  %2 = load i32, i32 addrspace(1)* %in
  %3 = insertelement <2 x i32> %0, i32 %2, i32 1
  br label %endif

else:
  %4 = getelementptr i32, i32 addrspace(1)* %in, i32 1
  %5 = load i32, i32 addrspace(1)* %4
  %6 = insertelement <2 x i32> %0, i32 %5, i32 1
  br label %endif

endif:
  %7 = phi <2 x i32> [%3, %if], [%6, %else]
  store <2 x i32> %7, <2 x i32> addrspace(1)* %out
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v2f64:
; GCN-DAG: s_load_dwordx4 s{{\[}}[[A_ELT0:[0-9]+]]:[[A_ELT3:[0-9]+]]{{\]}}
; GCN-DAG: s_load_dword [[IDX:s[0-9]+]], s{{\[[0-9]+:[0-9]+\]}}, {{0x11|0x44}}{{$}}

; GCN-DAG: s_lshl_b32 [[SCALEDIDX:s[0-9]+]], [[IDX]], 1{{$}}

; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 v{{[0-9]+}}, s{{[0-9]+}}
; GCN-DAG: v_mov_b32_e32 [[ELT1:v[0-9]+]], 0x40200000

; GCN-DAG: s_mov_b32 m0, [[SCALEDIDX]]
; GCN: v_movreld_b32_e32 v{{[0-9]+}}, 0

; Increment to next element folded into base register, but FileCheck
; can't do math expressions

; FIXME: Should be able to manipulate m0 directly instead of s_lshl_b32 + copy to m0

; GCN: v_movreld_b32_e32 v{{[0-9]+}}, [[ELT1]]

; GCN: buffer_store_dwordx4
; GCN: s_endpgm
define amdgpu_kernel void @dynamic_insertelement_v2f64(<2 x double> addrspace(1)* %out, <2 x double> %a, i32 %b) nounwind {
  %vecins = insertelement <2 x double> %a, double 8.0, i32 %b
  store <2 x double> %vecins, <2 x double> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v2i64:

; GCN-DAG: v_movreld_b32_e32 v{{[0-9]+}}, 5
; GCN-DAG: v_movreld_b32_e32 v{{[0-9]+}}, 0

; GCN: buffer_store_dwordx4
; GCN: s_endpgm
define amdgpu_kernel void @dynamic_insertelement_v2i64(<2 x i64> addrspace(1)* %out, <2 x i64> %a, i32 %b) nounwind {
  %vecins = insertelement <2 x i64> %a, i64 5, i32 %b
  store <2 x i64> %vecins, <2 x i64> addrspace(1)* %out, align 8
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v3i64:
define amdgpu_kernel void @dynamic_insertelement_v3i64(<3 x i64> addrspace(1)* %out, <3 x i64> %a, i32 %b) nounwind {
  %vecins = insertelement <3 x i64> %a, i64 5, i32 %b
  store <3 x i64> %vecins, <3 x i64> addrspace(1)* %out, align 32
  ret void
}

; FIXME: Should be able to do without stack access. The used stack
; space is also 2x what should be required.

; GCN-LABEL: {{^}}dynamic_insertelement_v4f64:
; GCN: SCRATCH_RSRC_DWORD

; Stack store

; GCN-DAG: buffer_store_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:32{{$}}
; GCN-DAG: buffer_store_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:48{{$}}

; Write element
; GCN: buffer_store_dwordx2 v{{\[[0-9]+:[0-9]+\]}}, v{{[0-9]+}}, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offen{{$}}

; Stack reload
; GCN-DAG: buffer_load_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:32{{$}}
; GCN-DAG: buffer_load_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:48{{$}}

; Store result
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: s_endpgm
; GCN: ScratchSize: 64

define amdgpu_kernel void @dynamic_insertelement_v4f64(<4 x double> addrspace(1)* %out, <4 x double> %a, i32 %b) nounwind {
  %vecins = insertelement <4 x double> %a, double 8.0, i32 %b
  store <4 x double> %vecins, <4 x double> addrspace(1)* %out, align 16
  ret void
}

; GCN-LABEL: {{^}}dynamic_insertelement_v8f64:
; GCN-DAG: SCRATCH_RSRC_DWORD

; GCN-DAG: buffer_store_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:64{{$}}
; GCN-DAG: buffer_store_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:80{{$}}
; GCN-DAG: buffer_store_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:96{{$}}
; GCN-DAG: buffer_store_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:112{{$}}

; GCN: buffer_store_dwordx2 v{{\[[0-9]+:[0-9]+\]}}, v{{[0-9]+}}, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offen{{$}}

; GCN-DAG: buffer_load_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:64{{$}}
; GCN-DAG: buffer_load_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:80{{$}}
; GCN-DAG: buffer_load_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:96{{$}}
; GCN-DAG: buffer_load_dwordx4 v{{\[[0-9]+:[0-9]+\]}}, off, s{{\[[0-9]+:[0-9]+\]}}, {{s[0-9]+}} offset:112{{$}}

; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: buffer_store_dwordx4
; GCN: s_endpgm
; GCN: ScratchSize: 128
define amdgpu_kernel void @dynamic_insertelement_v8f64(<8 x double> addrspace(1)* %out, <8 x double> %a, i32 %b) #0 {
  %vecins = insertelement <8 x double> %a, double 8.0, i32 %b
  store <8 x double> %vecins, <8 x double> addrspace(1)* %out, align 16
  ret void
}

declare <4 x float> @llvm.amdgcn.image.gather4.lz.v4f32.v2f32.v8i32(<2 x float>, <8 x i32>, <4 x i32>, i32, i1, i1, i1, i1, i1) #1

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
