; RUN: llc -mtriple wasm32-unknown-unknown-wasm -filetype=obj %s -o - | obj2yaml | FileCheck %s

@.str1 = private unnamed_addr constant [6 x i8] c"hello\00", align 1
@.str2 = private unnamed_addr constant [6 x i8] c"world\00", align 1

@a = global i8* getelementptr inbounds ([6 x i8], [6 x i8]* @.str1, i32 0, i32 0), align 8
@b = global i8* getelementptr inbounds ([6 x i8], [6 x i8]* @.str2, i32 0, i32 0), align 8


; CHECK:        - Type:            GLOBAL
; CHECK-NEXT:     Globals:         
; CHECK-NEXT:       - Type:            I32
; CHECK-NEXT:         Mutable:         false
; CHECK-NEXT:         InitExpr:        
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           0
; CHECK-NEXT:       - Type:            I32
; CHECK-NEXT:         Mutable:         false
; CHECK-NEXT:         InitExpr:        
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           6
; CHECK-NEXT:       - Type:            I32
; CHECK-NEXT:         Mutable:         false
; CHECK-NEXT:         InitExpr:        
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           16
; CHECK-NEXT:       - Type:            I32
; CHECK-NEXT:         Mutable:         false
; CHECK-NEXT:         InitExpr:        
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           24
; CHECK-NEXT:   - Type:            EXPORT
; CHECK-NEXT:     Exports:         
; CHECK-NEXT:       - Name:            a
; CHECK-NEXT:         Kind:            GLOBAL
; CHECK-NEXT:         Index:           2
; CHECK-NEXT:       - Name:            b
; CHECK-NEXT:         Kind:            GLOBAL
; CHECK-NEXT:         Index:           3
; CHECK-NEXT:   - Type:            DATA
; CHECK-NEXT:     Relocations:     
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_GLOBAL_ADDR_I32
; CHECK-NEXT:         Index:           0
; CHECK-NEXT:         Offset:          0x00000016
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_GLOBAL_ADDR_I32
; CHECK-NEXT:         Index:           1
; CHECK-NEXT:         Offset:          0x0000001E
; CHECK-NEXT:     Segments:        
; CHECK-NEXT:       - SectionOffset:   6
; CHECK-NEXT:         MemoryIndex:     0
; CHECK-NEXT:         Offset:          
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           0
; CHECK-NEXT:         Content:         68656C6C6F00776F726C640000000000000000000000000006000000
; CHECK-NEXT:   - Type:            CUSTOM
; CHECK-NEXT:     Name:            linking
; CHECK-NEXT:     DataSize:        28
; CHECK-NEXT:     DataAlignment:   8
; CHECK-NEXT:     SymbolInfo:      
; CHECK-NEXT: ...
