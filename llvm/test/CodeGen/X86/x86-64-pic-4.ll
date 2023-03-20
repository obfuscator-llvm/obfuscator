; RUN: llc < %s -mtriple=x86_64-pc-linux -relocation-model=pic -o %t1
; RUN: grep "movq	a@GOTPCREL(%rip)," %t1

@a = global i32 0

define i32 @get_a() {
entry:
	%tmp1 = load i32, i32* @a, align 4
	ret i32 %tmp1
}
