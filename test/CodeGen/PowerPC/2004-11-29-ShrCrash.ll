; RUN: llc -verify-machineinstrs < %s -march=ppc32
define void @test() {
	%tr1 = lshr i32 1, 0		; <i32> [#uses=0]
	ret void
}

