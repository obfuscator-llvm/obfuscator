; RUN: llc -verify-machineinstrs -mtriple=powerpc64-unknown-linux-gnu -O2 \
; RUN:   -ppc-gpr-icmps=all -ppc-asm-full-reg-names -mcpu=pwr8 < %s | FileCheck %s \
; RUN:  --implicit-check-not cmpw --implicit-check-not cmpd --implicit-check-not cmpl
; RUN: llc -verify-machineinstrs -mtriple=powerpc64le-unknown-linux-gnu -O2 \
; RUN:   -ppc-gpr-icmps=all -ppc-asm-full-reg-names -mcpu=pwr8 < %s | FileCheck %s \
; RUN:  --implicit-check-not cmpw --implicit-check-not cmpd --implicit-check-not cmpl

@glob = common local_unnamed_addr global i32 0, align 4

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeui(i32 zeroext %a, i32 zeroext %b) {
entry:
  %cmp = icmp uge i32 %a, %b
  %conv1 = zext i1 %cmp to i64
  ret i64 %conv1
; CHECK-LABEL: test_llgeui:
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK-NEXT: not [[REG2:r[0-9]+]], [[REG1]]
; CHECK-NEXT: rldicl r3, [[REG2]], 1, 63
; CHECK: blr
}

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeui_sext(i32 zeroext %a, i32 zeroext %b) {
entry:
  %cmp = icmp uge i32 %a, %b
  %conv1 = sext i1 %cmp to i64
  ret i64 %conv1
; CHECK-LABEL: @test_llgeui_sext
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK-NEXT: rldicl [[REG2:r[0-9]+]], [[REG1]], 1, 63
; CHECK-NEXT: addi [[REG3:r[0-9]+]], [[REG2]], -1
; CHECK-NEXT: blr    
}

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeui_z(i32 zeroext %a) {
entry:
  %cmp = icmp uge i32 %a, 0
  %conv1 = zext i1 %cmp to i64
  ret i64 %conv1
; CHECK-LABEL: @test_llgeui_z
; CHECK: li r3, 1
; CHECK: blr    
}

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeui_sext_z(i32 zeroext %a) {
entry:
  %cmp = icmp uge i32 %a, 0
  %conv1 = sext i1 %cmp to i64
  ret i64 %conv1
; CHECK-LABEL: @test_llgeui_sext_z
; CHECK: li r3, -1
; CHECK-NEXT: blr    
}

; Function Attrs: norecurse nounwind
define void @test_llgeui_store(i32 zeroext %a, i32 zeroext %b) {
entry:
  %cmp = icmp uge i32 %a, %b
  %conv = zext i1 %cmp to i32
  store i32 %conv, i32* @glob
  ret void
; CHECK_LABEL: test_igeuc_store:
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK: not [[REG2:r[0-9]+]], [[REG1]]
; CHECK-NEXT: rldicl r3, [[REG2]], 1, 63
; CHECK: blr
}

; Function Attrs: norecurse nounwind
define void @test_llgeui_sext_store(i32 zeroext %a, i32 zeroext %b) {
entry:
  %cmp = icmp uge i32 %a, %b
  %sub = sext i1 %cmp to i32
  store i32 %sub, i32* @glob
  ret void
; CHECK-LABEL: @test_llgeui_sext_store
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK: rldicl [[REG2:r[0-9]+]], [[REG1]], 1, 63
; CHECK: addi [[REG3:r[0-9]+]], [[REG2]], -1
; CHECK: stw  [[REG3]]
; CHECK: blr  
}

; Function Attrs: norecurse nounwind
define void @test_llgeui_z_store(i32 zeroext %a) {
entry:
  %cmp = icmp uge i32 %a, 0
  %sub = zext i1 %cmp to i32
  store i32 %sub, i32* @glob
  ret void
; CHECK-LABEL: @test_llgeui_z_store
; CHECK: li [[REG1:r[0-9]+]], 1
; CHECK: stw [[REG1]]
; CHECK: blr  
}

; Function Attrs: norecurse nounwind
define void @test_llgeui_sext_z_store(i32 zeroext %a) {
entry:
  %cmp = icmp uge i32 %a, 0
  %sub = sext i1 %cmp to i32
  store i32 %sub, i32* @glob
  ret void
; CHECK-LABEL: @test_llgeui_sext_z_store
; CHECK: li [[REG1:r[0-9]+]], -1
; CHECK: stw [[REG1]]
; CHECK: blr
}

