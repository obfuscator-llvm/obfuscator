; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=i386-unknown-linux-gnu -mattr=avx512bw,avx512vl,avx512dq | FileCheck %s

define void @pr34605(i8* nocapture %s, i32 %p) {
; CHECK-LABEL: pr34605:
; CHECK:       # BB#0: # %entry
; CHECK-NEXT:    movl {{[0-9]+}}(%esp), %eax
; CHECK-NEXT:    vpbroadcastd {{[0-9]+}}(%esp), %zmm0
; CHECK-NEXT:    vpcmpeqd {{\.LCPI.*}}, %zmm0, %k0
; CHECK-NEXT:    vpcmpeqd {{\.LCPI.*}}, %zmm0, %k1
; CHECK-NEXT:    kunpckwd %k0, %k1, %k0
; CHECK-NEXT:    vpcmpeqd {{\.LCPI.*}}, %zmm0, %k1
; CHECK-NEXT:    vpcmpeqd {{\.LCPI.*}}, %zmm0, %k2
; CHECK-NEXT:    kunpckwd %k1, %k2, %k1
; CHECK-NEXT:    kunpckdq %k0, %k1, %k0
; CHECK-NEXT:    kxord %k0, %k0, %k1
; CHECK-NEXT:    movl $1, %ecx
; CHECK-NEXT:    kmovd %ecx, %k2
; CHECK-NEXT:    kunpckdq %k2, %k1, %k1
; CHECK-NEXT:    kandq %k1, %k0, %k1
; CHECK-NEXT:    vmovdqu8 {{\.LCPI.*}}, %zmm0 {%k1} {z}
; CHECK-NEXT:    vmovdqu8 %zmm0, (%eax)
; CHECK-NEXT:    vpxord %zmm0, %zmm0, %zmm0
; CHECK-NEXT:    vmovdqu32 %zmm0, 64(%eax)
; CHECK-NEXT:    vmovdqu32 %zmm0, 128(%eax)
; CHECK-NEXT:    vmovdqu32 %zmm0, 192(%eax)
; CHECK-NEXT:    vmovdqu32 %zmm0, 256(%eax)
; CHECK-NEXT:    vmovdqu32 %zmm0, 320(%eax)
; CHECK-NEXT:    vmovdqu32 %zmm0, 384(%eax)
; CHECK-NEXT:    vmovdqu32 %zmm0, 448(%eax)
; CHECK-NEXT:    vzeroupper
; CHECK-NEXT:    retl
entry:
  %broadcast.splatinsert = insertelement <64 x i32> undef, i32 %p, i32 0
  %broadcast.splat = shufflevector <64 x i32> %broadcast.splatinsert, <64 x i32> undef, <64 x i32> zeroinitializer
  %0 = icmp eq <64 x i32> %broadcast.splat, <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15, i32 16, i32 17, i32 18, i32 19, i32 20, i32 21, i32 22, i32 23, i32 24, i32 25, i32 26, i32 27, i32 28, i32 29, i32 30, i32 31, i32 32, i32 33, i32 34, i32 35, i32 36, i32 37, i32 38, i32 39, i32 40, i32 41, i32 42, i32 43, i32 44, i32 45, i32 46, i32 47, i32 48, i32 49, i32 50, i32 51, i32 52, i32 53, i32 54, i32 55, i32 56, i32 57, i32 58, i32 59, i32 60, i32 61, i32 62, i32 63>
  %1 = and <64 x i1> %0, <i1 true, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false, i1 false>
  %2 = zext <64 x i1> %1 to <64 x i8>
  %3 = bitcast i8* %s to <64 x i8>*
  store <64 x i8> %2, <64 x i8>* %3, align 1
  %4 = getelementptr inbounds i8, i8* %s, i32 64
  %5 = bitcast i8* %4 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %5, align 1
  %6 = getelementptr inbounds i8, i8* %s, i32 128
  %7 = bitcast i8* %6 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %7, align 1
  %8 = getelementptr inbounds i8, i8* %s, i32 192
  %9 = bitcast i8* %8 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %9, align 1
  %10 = getelementptr inbounds i8, i8* %s, i32 256
  %11 = bitcast i8* %10 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %11, align 1
  %12 = getelementptr inbounds i8, i8* %s, i32 320
  %13 = bitcast i8* %12 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %13, align 1
  %14 = getelementptr inbounds i8, i8* %s, i32 384
  %15 = bitcast i8* %14 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %15, align 1
  %16 = getelementptr inbounds i8, i8* %s, i32 448
  %17 = bitcast i8* %16 to <64 x i8>*
  store <64 x i8> zeroinitializer, <64 x i8>* %17, align 1
  ret void
}
