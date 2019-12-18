; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=fiji -verify-machineinstrs < %s | FileCheck -check-prefix=GCN %s

; Check that we properly realign the stack. While 4-byte access is all
; that is ever needed, some transformations rely on the known bits from the alignment of the pointer (e.g.


; 128 byte object
; 4 byte emergency stack slot
; = 144 bytes with padding between them

; GCN-LABEL: {{^}}needs_align16_default_stack_align:
; GCN: s_mov_b32 s5, s32
; GCN-NOT: s32

; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: v_or_b32_e32 v{{[0-9]+}}, 12
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen

; GCN-NOT: s32

; GCN: ; ScratchSize: 144
define void @needs_align16_default_stack_align(i32 %idx) #0 {
  %alloca.align16 = alloca [8 x <4 x i32>], align 16, addrspace(5)
  %gep0 = getelementptr inbounds [8 x <4 x i32>], [8 x <4 x i32>] addrspace(5)* %alloca.align16, i32 0, i32 %idx
  store volatile <4 x i32> <i32 1, i32 2, i32 3, i32 4>, <4 x i32> addrspace(5)* %gep0, align 16
  ret void
}

; GCN-LABEL: {{^}}needs_align16_stack_align4:
; GCN: s_add_u32 [[SCRATCH_REG:s[0-9]+]], s32, 0x3c0{{$}}
; GCN: s_and_b32 s5, s6, 0xfffffc00
; GCN: s_add_u32 s32, s32, 0x2800{{$}}

; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: v_or_b32_e32 v{{[0-9]+}}, 12
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen

; GCN: s_sub_u32 s32, s32, 0x2800

; GCN: ; ScratchSize: 160
define void @needs_align16_stack_align4(i32 %idx) #2 {
  %alloca.align16 = alloca [8 x <4 x i32>], align 16, addrspace(5)
  %gep0 = getelementptr inbounds [8 x <4 x i32>], [8 x <4 x i32>] addrspace(5)* %alloca.align16, i32 0, i32 %idx
  store volatile <4 x i32> <i32 1, i32 2, i32 3, i32 4>, <4 x i32> addrspace(5)* %gep0, align 16
  ret void
}

; GCN-LABEL: {{^}}needs_align32:
; GCN: s_add_u32 [[SCRATCH_REG:s[0-9]+]], s32, 0x7c0{{$}}
; GCN: s_and_b32 s5, s6, 0xfffff800
; GCN: s_add_u32 s32, s32, 0x3000{{$}}

; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: v_or_b32_e32 v{{[0-9]+}}, 12
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen

; GCN: s_sub_u32 s32, s32, 0x3000

; GCN: ; ScratchSize: 192
define void @needs_align32(i32 %idx) #0 {
  %alloca.align16 = alloca [8 x <4 x i32>], align 32, addrspace(5)
  %gep0 = getelementptr inbounds [8 x <4 x i32>], [8 x <4 x i32>] addrspace(5)* %alloca.align16, i32 0, i32 %idx
  store volatile <4 x i32> <i32 1, i32 2, i32 3, i32 4>, <4 x i32> addrspace(5)* %gep0, align 32
  ret void
}

; GCN-LABEL: {{^}}force_realign4:
; GCN: s_add_u32 [[SCRATCH_REG:s[0-9]+]], s32, 0xc0{{$}}
; GCN: s_and_b32 s5, s6, 0xffffff00
; GCN: s_add_u32 s32, s32, 0xd00{{$}}

; GCN: buffer_store_dword v{{[0-9]+}}, v{{[0-9]+}}, s[0:3], s4 offen
; GCN: s_sub_u32 s32, s32, 0xd00

; GCN: ; ScratchSize: 52
define void @force_realign4(i32 %idx) #1 {
  %alloca.align16 = alloca [8 x i32], align 4, addrspace(5)
  %gep0 = getelementptr inbounds [8 x i32], [8 x i32] addrspace(5)* %alloca.align16, i32 0, i32 %idx
  store volatile i32 3, i32 addrspace(5)* %gep0, align 4
  ret void
}

; GCN-LABEL: {{^}}kernel_call_align16_from_8:
; GCN: s_add_u32 s32, s8, 0x400{{$}}
; GCN-NOT: s32
; GCN: s_swappc_b64
define amdgpu_kernel void @kernel_call_align16_from_8() #0 {
  %alloca = alloca i32, align 4, addrspace(5)
  store volatile i32 2, i32 addrspace(5)* %alloca
  call void @needs_align16_default_stack_align(i32 1)
  ret void
}

; The call sequence should keep the stack on call aligned to 4
; GCN-LABEL: {{^}}kernel_call_align16_from_5:
; GCN: s_add_u32 s32, s8, 0x400
; GCN: s_swappc_b64
define amdgpu_kernel void @kernel_call_align16_from_5() {
  %alloca0 = alloca i8, align 1, addrspace(5)
  store volatile i8 2, i8  addrspace(5)* %alloca0

  call void @needs_align16_default_stack_align(i32 1)
  ret void
}

; GCN-LABEL: {{^}}kernel_call_align4_from_5:
; GCN: s_add_u32 s32, s8, 0x400
; GCN: s_swappc_b64
define amdgpu_kernel void @kernel_call_align4_from_5() {
  %alloca0 = alloca i8, align 1, addrspace(5)
  store volatile i8 2, i8  addrspace(5)* %alloca0

  call void @needs_align16_stack_align4(i32 1)
  ret void
}

attributes #0 = { noinline nounwind }
attributes #1 = { noinline nounwind "stackrealign" }
attributes #2 = { noinline nounwind alignstack=4 }
