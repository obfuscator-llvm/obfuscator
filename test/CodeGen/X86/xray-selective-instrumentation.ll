; RUN: llc -verify-machineinstrs -mcpu=nehalem < %s | grep xray_sled_

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128"
target triple = "x86_64-apple-darwin8"

define i32 @foo() nounwind uwtable "xray-instruction-threshold"="1" {
entry:
  ret i32 0
}
