; RUN: llc -mtriple wasm32-unknown-unknown-wasm -filetype=obj %s -o - | obj2yaml | FileCheck %s
; Verify that addresses of external functions generate correctly typed
; imports and relocations or type R_TABLE_INDEX_I32.

declare void @f1(i32) #1
@ptr_to_f1 = hidden global void (i32)* @f1, align 4

; CHECK:      --- !WASM
; CHECK-NEXT: FileHeader:      
; CHECK-NEXT:   Version:         0x00000001
; CHECK-NEXT: Sections:        
; CHECK-NEXT:   - Type:            TYPE
; CHECK-NEXT:     Signatures:      
; CHECK-NEXT:       - Index:           0
; CHECK-NEXT:         ReturnType:      NORESULT
; CHECK-NEXT:         ParamTypes:      
; CHECK-NEXT:           - I32
; CHECK:        - Type:            IMPORT
; CHECK-NEXT:     Imports:
; CHECK-NEXT:       - Module:          env
; CHECK-NEXT:         Field:           f1
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         SigIndex:        0
; CHECK:        - Type:            ELEM
; CHECK-NEXT:     Segments:
; CHECK-NEXT:       - Offset:
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           0
; CHECK-NEXT:         Functions:       [ 0 ]
; CHECK:        - Type:            DATA
; CHECK-NEXT:     Relocations:
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_TABLE_INDEX_I32
; CHECK-NEXT:         Index:           0
; CHECK-NEXT:         Offset:          0x00000006
