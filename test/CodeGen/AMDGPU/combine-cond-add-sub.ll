; RUN: llc -march=amdgcn -verify-machineinstrs < %s | FileCheck -check-prefix=GCN %s
; RUN: llc -march=amdgcn -mcpu=gfx902  -verify-machineinstrs < %s | FileCheck -check-prefix=GFX9 %s

; GCN-LABEL: {{^}}add1:
; GCN: v_cmp_gt_u32_e{{32|64}} [[CC:[^,]+]], v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_addc_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, 0, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask

; GFX9-LABEL: {{^}}add1:
; GFX9: v_addc_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @add1(i32 addrspace(1)* nocapture %arg) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = zext i1 %cmp to i32
  %add = add i32 %v, %ext
  store i32 %add, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}add1_i16:
; GCN: v_cmp_gt_u32_e{{32|64}} [[CC:[^,]+]], v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_addc_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, 0, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask

; GFX9-LABEL: {{^}}add1_i16:
; GFX9: v_addc_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define i16 @add1_i16(i32 addrspace(1)* nocapture %arg, i16 addrspace(1)* nocapture %dst) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = zext i1 %cmp to i32
  %add = add i32 %v, %ext
  %trunc = trunc i32 %add to i16
  ret i16 %trunc
}

; GCN-LABEL: {{^}}sub1:
; GCN: v_cmp_gt_u32_e32 vcc, v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_subbrev_u32_e32 v{{[0-9]+}}, vcc, 0, v{{[0-9]+}}, vcc
; GCN-NOT: v_cndmask

; GFX9-LABEL: {{^}}sub1:
; GFX9: v_subbrev_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @sub1(i32 addrspace(1)* nocapture %arg) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = sext i1 %cmp to i32
  %add = add i32 %v, %ext
  store i32 %add, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}add_adde:
; GCN: v_cmp_gt_u32_e{{32|64}} [[CC:[^,]+]], v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_addc_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, v{{[0-9]+}}, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask
; GCN-NOT: v_add

; GFX9-LABEL: {{^}}add_adde:
; GFX9: v_addc_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @add_adde(i32 addrspace(1)* nocapture %arg, i32 %a) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = zext i1 %cmp to i32
  %adde = add i32 %v, %ext
  %add2 = add i32 %adde, %a
  store i32 %add2, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}adde_add:
; GCN: v_cmp_gt_u32_e{{32|64}} [[CC:[^,]+]], v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_addc_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, v{{[0-9]+}}, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask
; GCN-NOT: v_add

; GFX9-LABEL: {{^}}adde_add:
; GFX9: v_addc_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @adde_add(i32 addrspace(1)* nocapture %arg, i32 %a) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = zext i1 %cmp to i32
  %add = add i32 %v, %a
  %adde = add i32 %add, %ext
  store i32 %adde, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}sub_sube:
; GCN: v_cmp_gt_u32_e{{32|64}} [[CC:[^,]+]], v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_subb_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, v{{[0-9]+}}, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask
; GCN-NOT: v_sub

; GFX9-LABEL: {{^}}sub_sube:
; GFX9: v_subb_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @sub_sube(i32 addrspace(1)* nocapture %arg, i32 %a) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = sext i1 %cmp to i32
  %adde = add i32 %v, %ext
  %sub = sub i32 %adde, %a
  store i32 %sub, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}sube_sub:
; GCN: v_cmp_gt_u32_e{{32|64}} [[CC:[^,]+]], v{{[0-9]+}}, v{{[0-9]+}}
; GCN: v_subb_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, v{{[0-9]+}}, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask
; GCN-NOT: v_sub

; GFX9-LABEL: {{^}}sube_sub:
; GFX9: v_subb_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @sube_sub(i32 addrspace(1)* nocapture %arg, i32 %a) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = icmp ugt i32 %x, %y
  %ext = sext i1 %cmp to i32
  %sub = sub i32 %v, %a
  %adde = add i32 %sub, %ext
  store i32 %adde, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}zext_flclass:
; GCN: v_cmp_class_f32_e{{32|64}} [[CC:[^,]+]],
; GCN: v_addc_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, 0, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask

; GFX9-LABEL: {{^}}zext_flclass:
; GFX9: v_addc_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @zext_flclass(i32 addrspace(1)* nocapture %arg, float %x) {
bb:
  %id = tail call i32 @llvm.amdgcn.workitem.id.x()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %id
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = tail call zeroext i1 @llvm.amdgcn.class.f32(float %x, i32 608)
  %ext = zext i1 %cmp to i32
  %add = add i32 %v, %ext
  store i32 %add, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}sext_flclass:
; GCN: v_cmp_class_f32_e32 vcc,
; GCN: v_subbrev_u32_e32 v{{[0-9]+}}, vcc, 0, v{{[0-9]+}}, vcc
; GCN-NOT: v_cndmask

; GFX9-LABEL: {{^}}sext_flclass:
; GFX9: v_subbrev_co_u32_e32 v{{[0-9]+}}, vcc
define amdgpu_kernel void @sext_flclass(i32 addrspace(1)* nocapture %arg, float %x) {
bb:
  %id = tail call i32 @llvm.amdgcn.workitem.id.x()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %id
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp = tail call zeroext i1 @llvm.amdgcn.class.f32(float %x, i32 608)
  %ext = sext i1 %cmp to i32
  %add = add i32 %v, %ext
  store i32 %add, i32 addrspace(1)* %gep, align 4
  ret void
}

; GCN-LABEL: {{^}}add_and:
; GCN: s_and_b64 [[CC:[^,]+]],
; GCN: v_addc_u32_e{{32|64}} v{{[0-9]+}}, {{[^,]+}}, 0, v{{[0-9]+}}, [[CC]]
; GCN-NOT: v_cndmask

; GFX9-LABEL: {{^}}add_and:
; GFX9: v_addc_co_u32_e{{32|64}} v{{[0-9]+}}, vcc
define amdgpu_kernel void @add_and(i32 addrspace(1)* nocapture %arg) {
bb:
  %x = tail call i32 @llvm.amdgcn.workitem.id.x()
  %y = tail call i32 @llvm.amdgcn.workitem.id.y()
  %gep = getelementptr inbounds i32, i32 addrspace(1)* %arg, i32 %x
  %v = load i32, i32 addrspace(1)* %gep, align 4
  %cmp1 = icmp ugt i32 %x, %y
  %cmp2 = icmp ugt i32 %x, 1
  %cmp = and i1 %cmp1, %cmp2
  %ext = zext i1 %cmp to i32
  %add = add i32 %v, %ext
  store i32 %add, i32 addrspace(1)* %gep, align 4
  ret void
}

declare i1 @llvm.amdgcn.class.f32(float, i32) #0

declare i32 @llvm.amdgcn.workitem.id.x() #0

declare i32 @llvm.amdgcn.workitem.id.y() #0

attributes #0 = { nounwind readnone speculatable }
