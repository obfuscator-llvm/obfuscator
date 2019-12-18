; RUN: llvm-c-test --test-dibuilder | FileCheck %s

; CHECK: ; ModuleID = 'debuginfo.c'
; CHECK-NEXT: source_filename = "debuginfo.c"

; CHECK:      define i64 @foo(i64, i64, <10 x i64>) !dbg !20 {
; CHECK-NEXT: entry:
; CHECK-NEXT:   call void @llvm.dbg.declare(metadata i64 0, metadata !27, metadata !DIExpression()), !dbg !32
; CHECK-NEXT:   call void @llvm.dbg.declare(metadata i64 0, metadata !28, metadata !DIExpression()), !dbg !32
; CHECK-NEXT:   call void @llvm.dbg.declare(metadata i64 0, metadata !29, metadata !DIExpression()), !dbg !32
; CHECK:      vars:                                             ; No predecessors!
; CHECK-NEXT:   call void @llvm.dbg.value(metadata i64 0, metadata !30, metadata !DIExpression(DW_OP_constu, 0, DW_OP_stack_value)), !dbg !33
; CHECK-NEXT: }

; CHECK:      ; Function Attrs: nounwind readnone speculatable
; CHECK-NEXT: declare void @llvm.dbg.declare(metadata, metadata, metadata) #0

; CHECK:      ; Function Attrs: nounwind readnone speculatable
; CHECK-NEXT: declare void @llvm.dbg.value(metadata, metadata, metadata) #0

; CHECK:      attributes #0 = { nounwind readnone speculatable }

; CHECK:      !llvm.dbg.cu = !{!0}
; CHECK-NEXT: !FooType = !{!16}

; CHECK:      !0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "llvm-c-test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !2, globals: !3, imports: !12, splitDebugInlining: false)
; CHECK-NEXT: !1 = !DIFile(filename: "debuginfo.c", directory: ".")
; CHECK-NEXT: !2 = !{}
; CHECK-NEXT: !3 = !{!4, !8}
; CHECK-NEXT: !4 = !DIGlobalVariableExpression(var: !5, expr: !DIExpression(DW_OP_constu, 0, DW_OP_stack_value))
; CHECK-NEXT: !5 = distinct !DIGlobalVariable(name: "globalClass", scope: !6, file: !1, line: 1, type: !7, isLocal: true, isDefinition: true)
; CHECK-NEXT: !6 = !DIModule(scope: null, name: "llvm-c-test", includePath: "/test/include/llvm-c-test.h")
; CHECK-NEXT: !7 = !DICompositeType(tag: DW_TAG_structure_type, name: "TestClass", scope: !1, file: !1, line: 42, size: 64, flags: DIFlagObjcClassComplete, elements: !2)
; CHECK-NEXT: !8 = !DIGlobalVariableExpression(var: !9, expr: !DIExpression(DW_OP_constu, 0, DW_OP_stack_value))
; CHECK-NEXT: !9 = distinct !DIGlobalVariable(name: "global", scope: !6, file: !1, line: 1, type: !10, isLocal: true, isDefinition: true)
; CHECK-NEXT: !10 = !DIDerivedType(tag: DW_TAG_typedef, name: "int64_t", scope: !1, file: !1, line: 42, baseType: !11)
; CHECK-NEXT: !11 = !DIBasicType(name: "Int64", size: 64)
; CHECK-NEXT: !12 = !{!13, !15}
; CHECK-NEXT: !13 = !DIImportedEntity(tag: DW_TAG_imported_module, scope: !6, entity: !14, file: !1, line: 42)
; CHECK-NEXT: !14 = !DIModule(scope: null, name: "llvm-c-test-import", includePath: "/test/include/llvm-c-test-import.h")
; CHECK-NEXT: !15 = !DIImportedEntity(tag: DW_TAG_imported_module, scope: !6, entity: !13, file: !1, line: 42)
; CHECK-NEXT: !16 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !17, size: 192, dwarfAddressSpace: 0)
; CHECK-NEXT: !17 = !DICompositeType(tag: DW_TAG_structure_type, name: "MyStruct", scope: !18, file: !1, size: 192, elements: !19, runtimeLang: DW_LANG_C89, identifier: "MyStruct")
; CHECK-NEXT: !18 = !DINamespace(name: "NameSpace", scope: !6)
; CHECK-NEXT: !19 = !{!11, !11, !11}
; CHECK-NEXT: !20 = distinct !DISubprogram(name: "foo", linkageName: "foo", scope: !1, file: !1, line: 42, type: !21, scopeLine: 42, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition, unit: !0, retainedNodes: !26)
; CHECK-NEXT: !21 = !DISubroutineType(types: !22)
; CHECK-NEXT: !22 = !{!11, !11, !23}
; CHECK-NEXT: !23 = !DICompositeType(tag: DW_TAG_array_type, baseType: !11, size: 640, flags: DIFlagVector, elements: !24)
; CHECK-NEXT: !24 = !{!25}
; CHECK-NEXT: !25 = !DISubrange(count: 10)
; CHECK-NEXT: !26 = !{!27, !28, !29, !30}
; CHECK-NEXT: !27 = !DILocalVariable(name: "a", arg: 1, scope: !20, file: !1, line: 42, type: !11)
; CHECK-NEXT: !28 = !DILocalVariable(name: "b", arg: 2, scope: !20, file: !1, line: 42, type: !11)
; CHECK-NEXT: !29 = !DILocalVariable(name: "c", arg: 3, scope: !20, file: !1, line: 42, type: !23)
; CHECK-NEXT: !30 = !DILocalVariable(name: "d", scope: !31, file: !1, line: 43, type: !11)
; CHECK-NEXT: !31 = distinct !DILexicalBlock(scope: !20, file: !1, line: 42)
; CHECK-NEXT: !32 = !DILocation(line: 42, scope: !20)
; CHECK-NEXT: !33 = !DILocation(line: 43, scope: !20)
