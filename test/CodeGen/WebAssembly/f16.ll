; RUN: llc < %s -asm-verbose=false -disable-wasm-fallthrough-return-opt -wasm-keep-registers | FileCheck %s
; RUN: llc < %s -asm-verbose=false -disable-wasm-fallthrough-return-opt -wasm-keep-registers -fast-isel | FileCheck %s

; Test that f16 is expanded.

target datalayout = "e-m:e-p:32:32-i64:64-n32:64-S128"
target triple = "wasm32-unknown-unknown"

; CHECK-LABEL: demote:
; CHECK-NEXT: .functype demote (f32) -> (f32){{$}}
; CHECK-NEXT: local.get	$push[[L0:[0-9]+]]=, 0{{$}}
; CHECK-NEXT: i32.call	$push[[L1:[0-9]+]]=, __gnu_f2h_ieee@FUNCTION, $pop[[L0]]{{$}}
; CHECK-NEXT: f32.call	$push[[L2:[0-9]+]]=, __gnu_h2f_ieee@FUNCTION, $pop[[L1]]{{$}}
; CHECK-NEXT: return  	$pop[[L2]]{{$}}
define half @demote(float %f) {
    %t = fptrunc float %f to half
    ret half %t
}

; CHECK-LABEL: promote:
; CHECK-NEXT: .functype promote (f32) -> (f32){{$}}
; CHECK-NEXT: local.get	$push0=, 0{{$}}
; CHECK-NEXT: return  	$pop0{{$}}
define float @promote(half %f) {
    %t = fpext half %f to float
    ret float %t
}
