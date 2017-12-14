; RUN: llc -filetype=obj %s -o - | obj2yaml | FileCheck %s
; PR33624

source_filename = "ws.c"
target datalayout = "e-m:e-p:32:32-i64:64-n32:64-S128"
target triple = "wasm32-unknown-unknown-wasm"

%struct.bd = type { i8 }

@gBd = hidden global [2 x %struct.bd] [%struct.bd { i8 1 }, %struct.bd { i8 2 }], align 1

; CHECK:  - Type:            DATA
; CHECK:        Content:         '0102'
; CHECK:    DataSize:        2
