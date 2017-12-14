; RUN: llc -verify-machineinstrs < %s -march=ppc32 -mtriple=powerpc-apple-darwin | \
; RUN:   grep "stw r3, 32751"
; RUN: llc -verify-machineinstrs < %s -march=ppc64 -mtriple=powerpc-apple-darwin | \
; RUN:   grep "stw r3, 32751"
; RUN: llc -verify-machineinstrs < %s -march=ppc64 -mtriple=powerpc-apple-darwin | \
; RUN:   grep "std r3, 9024"

define void @test() nounwind {
	store i32 0, i32* inttoptr (i64 48725999 to i32*)
	ret void
}

define void @test2() nounwind {
	store i64 0, i64* inttoptr (i64 74560 to i64*)
	ret void
}

