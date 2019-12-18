; RUN: llc -filetype=obj -wasm-keep-registers %s -o %t.o
; RUN: obj2yaml %t.o | FileCheck %s
; RUN: llvm-objdump -t %t.o | FileCheck --check-prefix=CHECK-SYMS %s

target triple = "wasm32-unknown-unknown"

; 'foo_alias()' is weak alias of function 'foo()'
; 'bar_alias' is weak alias of global variable 'bar'
; Generates two exports of the same function, one of them weak

@bar = global i32 7, align 8
@bar_alias = weak hidden alias i32, i32* @bar
@foo_alias = weak hidden alias i32 (), i32 ()* @foo

@direct_address = global i32()* @foo, align 8
@alias_address = global i32()* @foo_alias, align 8

define hidden i32 @foo() #0 {
entry:
  ret i32 0
}

define hidden i32 @call_direct() #0 {
entry:
  %call = call i32 @foo()
  ret i32 %call
}

define hidden i32 @call_alias() #0 {
entry:
  %call = call i32 @foo_alias()
  ret i32 %call
}

define hidden i32 @call_direct_ptr() #0 {
entry:
  %0 = load i32 ()*, i32 ()** @direct_address, align 8
  %call = call i32 %0()
  ret i32 %call
}

define hidden i32 @call_alias_ptr() #0 {
entry:
  %0 = load i32 ()*, i32 ()** @alias_address, align 8
  %call = call i32 %0()
  ret i32 %call
}

; CHECK:        - Type:            TYPE
; CHECK-NEXT:     Signatures:
; CHECK-NEXT:       - Index:           0
; CHECK-NEXT:         ReturnType:      I32
; CHECK-NEXT:         ParamTypes:
; CHECK-NEXT:   - Type:            IMPORT
; CHECK-NEXT:     Imports:
; CHECK-NEXT:       - Module:          env
; CHECK-NEXT:         Field:           __linear_memory
; CHECK-NEXT:         Kind:            MEMORY
; CHECK-NEXT:         Memory:
; CHECK-NEXT:           Initial:         0x00000001
; CHECK-NEXT:       - Module:          env
; CHECK-NEXT:         Field:           __indirect_function_table
; CHECK-NEXT:         Kind:            TABLE
; CHECK-NEXT:         Table:
; CHECK-NEXT:           ElemType:        FUNCREF
; CHECK-NEXT:           Limits:
; CHECK-NEXT:             Initial:         0x00000001
; CHECK-NEXT:   - Type:            FUNCTION
; CHECK-NEXT:     FunctionTypes:   [ 0, 0, 0, 0, 0 ]
; CHECK-NEXT:   - Type:            ELEM
; CHECK-NEXT:     Segments:
; CHECK-NEXT:       - Offset:
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           1
; CHECK-NEXT:         Functions:       [ 0 ]
; CHECK-NEXT:   - Type:            CODE
; CHECK-NEXT:     Relocations:
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_FUNCTION_INDEX_LEB
; CHECK-NEXT:         Index:           0
; CHECK-NEXT:         Offset:          0x00000009
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_FUNCTION_INDEX_LEB
; CHECK-NEXT:         Index:           3
; CHECK-NEXT:         Offset:          0x00000012
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_MEMORY_ADDR_LEB
; CHECK-NEXT:         Index:           5
; CHECK-NEXT:         Offset:          0x0000001E
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_TYPE_INDEX_LEB
; CHECK-NEXT:         Index:           0
; CHECK-NEXT:         Offset:          0x00000024
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_MEMORY_ADDR_LEB
; CHECK-NEXT:         Index:           7
; CHECK-NEXT:         Offset:          0x00000031
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_TYPE_INDEX_LEB
; CHECK-NEXT:         Index:           0
; CHECK-NEXT:         Offset:          0x00000037
; CHECK-NEXT:     Functions:
; CHECK-NEXT:       - Index:           0
; CHECK-NEXT:         Locals:
; CHECK-NEXT:         Body:            41000B
; CHECK-NEXT:       - Index:           1
; CHECK-NEXT:         Locals:
; CHECK-NEXT:         Body:            1080808080000B
; CHECK-NEXT:       - Index:           2
; CHECK-NEXT:         Locals:
; CHECK-NEXT:         Body:            1080808080000B
; CHECK-NEXT:       - Index:           3
; CHECK-NEXT:         Locals:
; CHECK-NEXT:         Body:            410028028880808000118080808000000B
; CHECK-NEXT:       - Index:           4
; CHECK-NEXT:         Locals:
; CHECK-NEXT:         Body:            410028029080808000118080808000000B
; CHECK-NEXT:   - Type:            DATA
; CHECK-NEXT:     Relocations:
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_TABLE_INDEX_I32
; CHECK-NEXT:         Index:           0
; CHECK-NEXT:         Offset:          0x0000000F
; CHECK-NEXT:       - Type:            R_WEBASSEMBLY_TABLE_INDEX_I32
; CHECK-NEXT:         Index:           3
; CHECK-NEXT:         Offset:          0x00000018
; CHECK-NEXT:     Segments:
; CHECK-NEXT:       - SectionOffset:   6
; CHECK-NEXT:         MemoryIndex:     0
; CHECK-NEXT:         Offset:
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           0
; CHECK-NEXT:         Content:         '07000000'
; CHECK-NEXT:       - SectionOffset:   15
; CHECK-NEXT:         MemoryIndex:     0
; CHECK-NEXT:         Offset:
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           8
; CHECK-NEXT:         Content:         '01000000'
; CHECK-NEXT:       - SectionOffset:   24
; CHECK-NEXT:         MemoryIndex:     0
; CHECK-NEXT:         Offset:
; CHECK-NEXT:           Opcode:          I32_CONST
; CHECK-NEXT:           Value:           16
; CHECK-NEXT:         Content:         '01000000'
; CHECK-NEXT:   - Type:            CUSTOM
; CHECK-NEXT:     Name:            linking
; CHECK-NEXT:     Version:         2
; CHECK-NEXT:     SymbolTable:
; CHECK-NEXT:       - Index:           0
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         Name:            foo
; CHECK-NEXT:         Flags:           [ VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Function:        0
; CHECK-NEXT:       - Index:           1
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         Name:            call_direct
; CHECK-NEXT:         Flags:           [ VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Function:        1
; CHECK-NEXT:       - Index:           2
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         Name:            call_alias
; CHECK-NEXT:         Flags:           [ VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Function:        2
; CHECK-NEXT:       - Index:           3
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         Name:            foo_alias
; CHECK-NEXT:         Flags:           [ BINDING_WEAK, VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Function:        0
; CHECK-NEXT:       - Index:           4
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         Name:            call_direct_ptr
; CHECK-NEXT:         Flags:           [ VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Function:        3
; CHECK-NEXT:       - Index:           5
; CHECK-NEXT:         Kind:            DATA
; CHECK-NEXT:         Name:            direct_address
; CHECK-NEXT:         Flags:           [  ]
; CHECK-NEXT:         Segment:         1
; CHECK-NEXT:         Size:            4
; CHECK-NEXT:       - Index:           6
; CHECK-NEXT:         Kind:            FUNCTION
; CHECK-NEXT:         Name:            call_alias_ptr
; CHECK-NEXT:         Flags:           [ VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Function:        4
; CHECK-NEXT:       - Index:           7
; CHECK-NEXT:         Kind:            DATA
; CHECK-NEXT:         Name:            alias_address
; CHECK-NEXT:         Flags:           [  ]
; CHECK-NEXT:         Segment:         2
; CHECK-NEXT:         Size:            4
; CHECK-NEXT:       - Index:           8
; CHECK-NEXT:         Kind:            DATA
; CHECK-NEXT:         Name:            bar
; CHECK-NEXT:         Flags:           [  ]
; CHECK-NEXT:         Segment:         0
; CHECK-NEXT:         Size:            4
; CHECK-NEXT:       - Index:           9
; CHECK-NEXT:         Kind:            DATA
; CHECK-NEXT:         Name:            bar_alias
; CHECK-NEXT:         Flags:           [ BINDING_WEAK, VISIBILITY_HIDDEN ]
; CHECK-NEXT:         Segment:         0
; CHECK-NEXT:         Size:            4
; CHECK-NEXT:     SegmentInfo:
; CHECK-NEXT:       - Index:           0
; CHECK-NEXT:         Name:            .data.bar
; CHECK-NEXT:         Alignment:       3
; CHECK-NEXT:         Flags:           [ ]
; CHECK-NEXT:       - Index:           1
; CHECK-NEXT:         Name:            .data.direct_address
; CHECK-NEXT:         Alignment:       3
; CHECK-NEXT:         Flags:           [ ]
; CHECK-NEXT:       - Index:           2
; CHECK-NEXT:         Name:            .data.alias_address
; CHECK-NEXT:         Alignment:       3
; CHECK-NEXT:         Flags:           [ ]
; CHECK-NEXT: ...

; CHECK-SYMS: SYMBOL TABLE:
; CHECK-SYMS-NEXT: 00000000 g     F CODE	.hidden foo
; CHECK-SYMS-NEXT: 00000001 g     F CODE	.hidden call_direct
; CHECK-SYMS-NEXT: 00000002 g     F CODE	.hidden call_alias
; CHECK-SYMS-NEXT: 00000000 gw    F CODE	.hidden foo_alias
; CHECK-SYMS-NEXT: 00000003 g     F CODE	.hidden call_direct_ptr
; CHECK-SYMS-NEXT: 00000008 g     O DATA	direct_address
; CHECK-SYMS-NEXT: 00000004 g     F CODE	.hidden call_alias_ptr
; CHECK-SYMS-NEXT: 00000010 g     O DATA	alias_address
; CHECK-SYMS-NEXT: 00000000 g     O DATA	bar
; CHECK-SYMS-NEXT: 00000000 gw    O DATA	.hidden bar_alias
