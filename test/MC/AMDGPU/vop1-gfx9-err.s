// RUN: not llvm-mc -arch=amdgcn -mcpu=gfx901 -show-encoding %s 2>&1 | FileCheck -check-prefix=GCN %s
// RUN: not llvm-mc -arch=amdgcn -mcpu=tonga -show-encoding %s 2>&1 | FileCheck -check-prefix=GCN %s
// RUN: not llvm-mc -arch=amdgcn -mcpu=hawaii -show-encoding %s 2>&1 | FileCheck -check-prefix=GCN %s

v_swap_b32 v1, 1
// GCN: :16: error: invalid operand for instruction

v_swap_b32 v1, s0
// GCN: :16: error: invalid operand for instruction

// FIXME: Better error for it requiring VOP1 encoding
v_swap_b32_e64 v1, v2
// GCN: :1: error: unrecognized instruction mnemonic

v_swap_b32 v1, v2, v1
// GCN: :20: error: invalid operand for instruction

v_swap_b32 v1, v2, v2
// GCN: :20: error: invalid operand for instruction

v_swap_b32 v1, v2, v2, v2
// GCN: :20: error: invalid operand for instruction

v_swap_codegen_pseudo_b32 v1, v2
// GCN: :1: error: unrecognized instruction mnemonic
