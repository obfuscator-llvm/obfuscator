; RUN: llc -verify-machineinstrs -mcpu=pwr7 -mattr=+vsx < %s | FileCheck %s
; RUN: llc -verify-machineinstrs -mcpu=pwr7 -mattr=+vsx < %s | FileCheck \
; RUN:   -check-prefix=CHECK-REG %s
; RUN: llc -verify-machineinstrs -mcpu=pwr7 -mattr=+vsx -fast-isel -O0 < %s | \
; RUN:   FileCheck %s
; RUN: llc -verify-machineinstrs -mcpu=pwr7 -mattr=+vsx -fast-isel -O0 < %s | \
; RUN:   FileCheck -check-prefix=CHECK-FISL %s
; RUN: llc -verify-machineinstrs -mcpu=pwr9 < %s | FileCheck \
; RUN:   -check-prefix=CHECK-P9-REG %s
; RUN: llc -verify-machineinstrs -mcpu=pwr9 -fast-isel -O0 < %s | FileCheck \
; RUN:   -check-prefix=CHECK-P9-FISL %s
target datalayout = "E-m:e-i64:64-n32:64"
target triple = "powerpc64-unknown-linux-gnu"

define double @foo1(double %a) nounwind {
entry:
  call void asm sideeffect "", "~{f0},~{f1},~{f2},~{f3},~{f4},~{f5},~{f6},~{f7},~{f8},~{f9},~{f10},~{f11},~{f12},~{f13},~{f14},~{f15},~{f16},~{f17},~{f18},~{f19},~{f20},~{f21},~{f22},~{f23},~{f24},~{f25},~{f26},~{f27},~{f28},~{f29},~{f30},~{f31}"() nounwind
  br label %return

; CHECK-REG: @foo1
; CHECK-REG: xxlor [[R1:[0-9]+]], 1, 1
; CHECK-REG: xxlor 1, [[R1]], [[R1]]
; CHECK-REG: blr

; CHECK-FISL: @foo1
; CHECK-FISL: lis 3, -1
; CHECK-FISL: ori 3, 3, 65384
; CHECK-FISL: stxsdx 1, 1, 3
; CHECK-FISL: blr

; CHECK-P9-REG: @foo1
; CHECK-P9-REG: xxlor [[R1:[0-9]+]], 1, 1
; CHECK-P9-REG: xxlor 1, [[R1]], [[R1]]
; CHECK-P9-REG: blr

; CHECK-P9-FISL: @foo1
; CHECK-P9-FISL: stfd 31, -8(1)
; CHECK-P9-FISL: blr

return:                                           ; preds = %entry
  ret double %a
}

define double @foo2(double %a) nounwind {
entry:
  %b = fadd double %a, %a
  call void asm sideeffect "", "~{f0},~{f1},~{f2},~{f3},~{f4},~{f5},~{f6},~{f7},~{f8},~{f9},~{f10},~{f11},~{f12},~{f13},~{f14},~{f15},~{f16},~{f17},~{f18},~{f19},~{f20},~{f21},~{f22},~{f23},~{f24},~{f25},~{f26},~{f27},~{f28},~{f29},~{f30},~{f31}"() nounwind
  br label %return

; CHECK-REG: @foo2
; CHECK-REG: {{xxlor|xsadddp}} [[R1:[0-9]+]], 1, 1
; CHECK-REG: {{xxlor|xsadddp}} 1, [[R1]], [[R1]]
; CHECK-REG: blr

; CHECK-FISL: @foo2
; CHECK-FISL: xsadddp [[R1:[0-9]+]], 1, 1
; CHECK-FISL: stxsdx [[R1]], [[R1]], 3
; CHECK-FISL: lxsdx [[R1]], [[R1]], 3
; CHECK-FISL: blr

; CHECK-P9-REG: @foo2
; CHECK-P9-REG: {{xxlor|xsadddp}} [[R1:[0-9]+]], 1, 1
; CHECK-P9-REG: {{xxlor|xsadddp}} 1, [[R1]], [[R1]]
; CHECK-P9-REG: blr

; CHECK-P9-FISL: @foo2
; CHECK-P9-FISL: xsadddp [[R1:[0-9]+]], 1, 1
; CHECK-P9-FISL: stfd [[R1]], [[OFF:[0-9\-]+]](1)
; CHECK-P9-FISL: lfd [[R1]], [[OFF]](1)
; CHECK-P9-FISL: blr

return:                                           ; preds = %entry
  ret double %b
}

define double @foo3(double %a) nounwind {
entry:
  call void asm sideeffect "", "~{f0},~{f1},~{f2},~{f3},~{f4},~{f5},~{f6},~{f7},~{f8},~{f9},~{f10},~{f11},~{f12},~{f13},~{f14},~{f15},~{f16},~{f17},~{f18},~{f19},~{f20},~{f21},~{f22},~{f23},~{f24},~{f25},~{f26},~{f27},~{f28},~{f29},~{f30},~{f31},~{v0},~{v1},~{v2},~{v3},~{v4},~{v5},~{v6},~{v7},~{v8},~{v9},~{v10},~{v11},~{v12},~{v13},~{v14},~{v15},~{v16},~{v17},~{v18},~{v19},~{v20},~{v21},~{v22},~{v23},~{v24},~{v25},~{v26},~{v27},~{v28},~{v29},~{v30},~{v31}"() nounwind
  br label %return

; CHECK: @foo3
; CHECK: stxsdx 1,
; CHECK: lxsdx [[R1:[0-9]+]],
; CHECK: xsadddp 1, [[R1]], [[R1]]
; CHECK: blr

; CHECK-P9-REG-LABEL: foo3
; CHECK-P9-REG: stfd 1, [[OFF:[0-9\-]+]](1)
; CHECK-P9-REG: lfd [[FPR:[0-9]+]], [[OFF]](1)
; CHECK-P9-REG: xsadddp 1, [[FPR]], [[FPR]]

; CHECK-P9-FISL-LABEL: foo3
; CHECK-P9-FISL: stfd 1, [[OFF:[0-9\-]+]](1)
; CHECK-P9-FISL: lfd [[FPR:[0-9]+]], [[OFF]](1)
; CHECK-P9-FISL: xsadddp 1, [[FPR]], [[FPR]]
return:                                           ; preds = %entry
  %b = fadd double %a, %a
  ret double %b
}

