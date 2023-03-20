; RUN: llc -verify-machineinstrs -mtriple=powerpc64-unknown-linux-gnu -O2 \
; RUN:   -ppc-gpr-icmps=all -ppc-asm-full-reg-names -mcpu=pwr8 < %s | FileCheck %s \
; RUN:  --implicit-check-not cmpw --implicit-check-not cmpd --implicit-check-not cmpl
; RUN: llc -verify-machineinstrs -mtriple=powerpc64le-unknown-linux-gnu -O2 \
; RUN:   -ppc-gpr-icmps=all -ppc-asm-full-reg-names -mcpu=pwr8 < %s | FileCheck %s \
; RUN:  --implicit-check-not cmpw --implicit-check-not cmpd --implicit-check-not cmpl

@glob = common local_unnamed_addr global i16 0, align 2

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeus(i16 zeroext %a, i16 zeroext %b) {
entry:
  %cmp = icmp uge i16 %a, %b
  %conv3 = zext i1 %cmp to i64
  ret i64 %conv3
; CHECK-LABEL: test_llgeus:
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK-NEXT: not [[REG2:r[0-9]+]], [[REG1]]
; CHECK-NEXT: rldicl r3, [[REG2]], 1, 63
; CHECK: blr
}

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeus_sext(i16 zeroext %a, i16 zeroext %b) {
entry:
  %cmp = icmp uge i16 %a, %b
  %conv3 = sext i1 %cmp to i64
  ret i64 %conv3
; CHECK-LABEL: @test_llgeus_sext
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK-NEXT: rldicl [[REG2:r[0-9]+]], [[REG1]], 1, 63
; CHECK-NEXT: addi [[REG3:r[0-9]+]], [[REG2]], -1
; CHECK-NEXT: blr
}

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeus_z(i16 zeroext %a) {
entry:
  %cmp = icmp uge i16 %a, 0
  %conv1 = zext i1 %cmp to i64
  ret i64 %conv1
; CHECK-LABEL: @test_llgeus_z
; CHECK: li r3, 1
; CHECK-NEXT: blr
}

; Function Attrs: norecurse nounwind readnone
define i64 @test_llgeus_sext_z(i16 zeroext %a) {
entry:
  %cmp = icmp uge i16 %a, 0
  %conv1 = sext i1 %cmp to i64
  ret i64 %conv1
; CHECK-LABEL: @test_llgeus_sext_z
; CHECK: li r3, -1
; CHECK: blr
}

; Function Attrs: norecurse nounwind
define void @test_llgeus_store(i16 zeroext %a, i16 zeroext %b) {
entry:
  %cmp = icmp uge i16 %a, %b
  %conv3 = zext i1 %cmp to i16
  store i16 %conv3, i16* @glob
  ret void
; CHECK_LABEL: test_llgeus_store:
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK: not [[REG2:r[0-9]+]], [[REG1]]
; CHECK-NEXT: rldicl r3, [[REG2]], 1, 63
; CHECK: blr
}

; Function Attrs: norecurse nounwind
define void @test_llgeus_sext_store(i16 zeroext %a, i16 zeroext %b) {
entry:
  %cmp = icmp uge i16 %a, %b
  %conv3 = sext i1 %cmp to i16
  store i16 %conv3, i16* @glob
  ret void
; CHECK-LABEL: @test_llgeus_sext_store
; CHECK: sub [[REG1:r[0-9]+]], r3, r4
; CHECK: rldicl [[REG2:r[0-9]+]], [[REG1]], 1, 63
; CHECK: addi [[REG3:r[0-9]+]], [[REG2]], -1
; CHECK: sth  [[REG3]]
; CHECK: blr    
}

; Function Attrs: norecurse nounwind
define void @test_llgeus_z_store(i16 zeroext %a) {
entry:
  %cmp = icmp uge i16 %a, 0
  %conv1 = zext i1 %cmp to i16
  store i16 %conv1, i16* @glob
  ret void
; CHECK-LABEL: @test_llgeus_z_store
; CHECK: li [[REG1:r[0-9]+]], 1
; CHECK: sth [[REG1]]
; CHECK: blr  
}

; Function Attrs: norecurse nounwind
define void @test_llgeus_sext_z_store(i16 zeroext %a) {
entry:
  %cmp = icmp uge i16 %a, 0
  %conv1 = sext i1 %cmp to i16
  store i16 %conv1, i16* @glob
  ret void
; CHECK-LABEL: @test_llgeus_sext_z_store
; CHECK: li [[REG1:r[0-9]+]], -1
; CHECK: sth [[REG1]]
; CHECK: blr  
}

