; RUN: llc -march=amdgcn -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=GFX8 -check-prefix=NOAUTO %s
; RUN: llc -march=amdgcn -mattr=+auto-waitcnt-before-barrier -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=GFX8 -check-prefix=AUTO %s
; RUN: llc -march=amdgcn -mcpu=gfx900 -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=GFX9 -check-prefix=NOAUTO %s
; RUN: llc -march=amdgcn -mcpu=gfx900 -mattr=+auto-waitcnt-before-barrier -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=GFX9 -check-prefix=AUTO %s

; GCN-LABEL: {{^}}test_barrier:
; GFX8: buffer_store_dword
; GFX9: flat_store_dword
; NOAUTO: s_waitcnt
; AUTO-NOT: s_waitcnt
; GCN: s_barrier
define amdgpu_kernel void @test_barrier(i32 addrspace(1)* %out, i32 %size) #0 {
entry:
  %tmp = call i32 @llvm.amdgcn.workitem.id.x()
  %tmp1 = getelementptr i32, i32 addrspace(1)* %out, i32 %tmp
  store i32 %tmp, i32 addrspace(1)* %tmp1
  call void @llvm.amdgcn.s.barrier()
  %tmp3 = sub i32 %size, 1
  %tmp4 = sub i32 %tmp3, %tmp
  %tmp5 = getelementptr i32, i32 addrspace(1)* %out, i32 %tmp4
  %tmp6 = load i32, i32 addrspace(1)* %tmp5
  store i32 %tmp6, i32 addrspace(1)* %tmp1
  ret void
}

declare void @llvm.amdgcn.s.barrier() #1
declare i32 @llvm.amdgcn.workitem.id.x() #2

attributes #0 = { nounwind }
attributes #1 = { convergent nounwind }
attributes #2 = { nounwind readnone }
