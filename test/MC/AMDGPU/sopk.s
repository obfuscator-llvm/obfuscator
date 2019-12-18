// RUN: not llvm-mc -arch=amdgcn -show-encoding %s | FileCheck --check-prefix=GCN --check-prefix=SICI %s
// RUN: not llvm-mc -arch=amdgcn -mcpu=tahiti -show-encoding %s | FileCheck --check-prefix=GCN --check-prefix=SICI %s
// RUN: not llvm-mc -arch=amdgcn -mcpu=fiji -show-encoding %s | FileCheck --check-prefix=GCN --check-prefix=VI9 --check-prefix=VI %s
// RUN: llvm-mc -arch=amdgcn -mcpu=gfx900 -show-encoding %s | FileCheck --check-prefix=GCN --check-prefix=VI9 --check-prefix=GFX9 %s

// RUN: not llvm-mc -arch=amdgcn %s 2>&1 | FileCheck -check-prefix=NOSICIVI %s
// RUN: not llvm-mc -arch=amdgcn -mcpu=tahiti %s 2>&1 | FileCheck -check-prefix=NOSICIVI -check-prefix=NOSI %s
// RUN: not llvm-mc -arch=amdgcn -mcpu=fiji %s 2>&1 | FileCheck -check-prefix=NOSICIVI -check-prefix=NOVI %s

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

s_movk_i32 s2, 0x6
// GCN: s_movk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb0]

s_cmovk_i32 s2, 0x6
// SICI: s_cmovk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb1]
// VI9:  s_cmovk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb0]

s_cmpk_eq_i32 s2, 0x6
// SICI: s_cmpk_eq_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb1]
// VI9:  s_cmpk_eq_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb1]

s_cmpk_lg_i32 s2, 0x6
// SICI: s_cmpk_lg_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb2]
// VI9:  s_cmpk_lg_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb1]

s_cmpk_gt_i32 s2, 0x6
// SICI: s_cmpk_gt_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb2]
// VI9:  s_cmpk_gt_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb2]

s_cmpk_ge_i32 s2, 0x6
// SICI: s_cmpk_ge_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb3]
// VI9:  s_cmpk_ge_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb2]

s_cmpk_lt_i32 s2, 0x6
// SICI: s_cmpk_lt_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb3]
// VI9:  s_cmpk_lt_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb3]

s_cmpk_le_i32 s2, 0x6
// SICI: s_cmpk_le_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb4]
// VI9:  s_cmpk_le_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb3]

s_cmpk_eq_u32 s2, 0x6
// SICI: s_cmpk_eq_u32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb4]
// VI9:  s_cmpk_eq_u32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb4]

s_cmpk_lg_u32 s2, 0x6
// SICI: s_cmpk_lg_u32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb5]
// VI9:  s_cmpk_lg_u32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb4]

s_cmpk_gt_u32 s2, 0x6
// SICI: s_cmpk_gt_u32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb5]
// VI9:  s_cmpk_gt_u32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb5]

s_cmpk_ge_u32 s2, 0x6
// SICI: s_cmpk_ge_u32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb6]
// VI9:  s_cmpk_ge_u32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb5]

s_cmpk_lt_u32 s2, 0x6
// SICI: s_cmpk_lt_u32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb6]
// VI9:  s_cmpk_lt_u32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb6]

s_cmpk_le_u32 s2, 0x6
// SICI: s_cmpk_le_u32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb7]
// VI9:  s_cmpk_le_u32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb6]

s_cmpk_le_u32 s2, 0xFFFF
// SICI: s_cmpk_le_u32 s2, 0xffff ; encoding: [0xff,0xff,0x02,0xb7]
// VI9:  s_cmpk_le_u32 s2, 0xffff ; encoding: [0xff,0xff,0x82,0xb6]

s_addk_i32 s2, 0x6
// SICI: s_addk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb7]
// VI9:  s_addk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb7]

s_mulk_i32 s2, 0x6
// SICI: s_mulk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x02,0xb8]
// VI9:  s_mulk_i32 s2, 0x6 ; encoding: [0x06,0x00,0x82,0xb7]

s_mulk_i32 s2, -1
// SICI: s_mulk_i32 s2, 0xffff ; encoding: [0xff,0xff,0x02,0xb8]
// VI9:  s_mulk_i32 s2, 0xffff ; encoding: [0xff,0xff,0x82,0xb7]

s_mulk_i32 s2, 0xFFFF
// SICI: s_mulk_i32 s2, 0xffff ; encoding: [0xff,0xff,0x02,0xb8]
// VI9:  s_mulk_i32 s2, 0xffff ; encoding: [0xff,0xff,0x82,0xb7]

s_cbranch_i_fork s[2:3], 0x6
// SICI: s_cbranch_i_fork s[2:3], 0x6 ; encoding: [0x06,0x00,0x82,0xb8]
// VI9:  s_cbranch_i_fork s[2:3], 0x6 ; encoding: [0x06,0x00,0x02,0xb8]

// raw number mapped to known HW register
s_getreg_b32 s2, 0x6
// SICI: s_getreg_b32 s2, hwreg(HW_REG_LDS_ALLOC, 0, 1) ; encoding: [0x06,0x00,0x02,0xb9]
// VI9:  s_getreg_b32 s2, hwreg(HW_REG_LDS_ALLOC, 0, 1) ; encoding: [0x06,0x00,0x82,0xb8]

// HW register identifier, non-default offset/width
s_getreg_b32 s2, hwreg(HW_REG_GPR_ALLOC, 1, 31)
// SICI: s_getreg_b32 s2, hwreg(HW_REG_GPR_ALLOC, 1, 31) ; encoding: [0x45,0xf0,0x02,0xb9]
// VI9:  s_getreg_b32 s2, hwreg(HW_REG_GPR_ALLOC, 1, 31) ; encoding: [0x45,0xf0,0x82,0xb8]

// HW register code of unknown HW register, non-default offset/width
s_getreg_b32 s2, hwreg(51, 1, 31)
// SICI: s_getreg_b32 s2, hwreg(51, 1, 31) ; encoding: [0x73,0xf0,0x02,0xb9]
// VI9:  s_getreg_b32 s2, hwreg(51, 1, 31) ; encoding: [0x73,0xf0,0x82,0xb8]

// HW register code of unknown HW register, default offset/width
s_getreg_b32 s2, hwreg(51)
// SICI: s_getreg_b32 s2, hwreg(51) ; encoding: [0x33,0xf8,0x02,0xb9]
// VI9:  s_getreg_b32 s2, hwreg(51) ; encoding: [0x33,0xf8,0x82,0xb8]

// HW register code of unknown HW register, valid symbolic name range but no name available
s_getreg_b32 s2, hwreg(10)
// SICI: s_getreg_b32 s2, hwreg(10) ; encoding: [0x0a,0xf8,0x02,0xb9]
// VI9:  s_getreg_b32 s2, hwreg(10) ; encoding: [0x0a,0xf8,0x82,0xb8]

// HW_REG_SH_MEM_BASES valid starting from GFX9
s_getreg_b32 s2, hwreg(15)
// SICI: s_getreg_b32 s2, hwreg(15) ; encoding: [0x0f,0xf8,0x02,0xb9]
// VI:   s_getreg_b32 s2, hwreg(15) ; encoding: [0x0f,0xf8,0x82,0xb8]
// GFX9: s_getreg_b32 s2, hwreg(HW_REG_SH_MEM_BASES) ; encoding: [0x0f,0xf8,0x82,0xb8]

// raw number mapped to known HW register
s_setreg_b32 0x6, s2
// SICI: s_setreg_b32 hwreg(HW_REG_LDS_ALLOC, 0, 1), s2 ; encoding: [0x06,0x00,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(HW_REG_LDS_ALLOC, 0, 1), s2 ; encoding: [0x06,0x00,0x02,0xb9]

// raw number mapped to unknown HW register
s_setreg_b32 0x33, s2
// SICI: s_setreg_b32 hwreg(51, 0, 1), s2 ; encoding: [0x33,0x00,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(51, 0, 1), s2 ; encoding: [0x33,0x00,0x02,0xb9]

// raw number mapped to known HW register, default offset/width
s_setreg_b32 0xf803, s2
// SICI: s_setreg_b32 hwreg(HW_REG_TRAPSTS), s2       ; encoding: [0x03,0xf8,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(HW_REG_TRAPSTS), s2       ; encoding: [0x03,0xf8,0x02,0xb9]

// HW register identifier, default offset/width implied
s_setreg_b32 hwreg(HW_REG_HW_ID), s2
// SICI: s_setreg_b32 hwreg(HW_REG_HW_ID), s2       ; encoding: [0x04,0xf8,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(HW_REG_HW_ID), s2       ; encoding: [0x04,0xf8,0x02,0xb9]

// HW register identifier, non-default offset/width
s_setreg_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), s2
// SICI: s_setreg_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), s2       ; encoding: [0x45,0xf0,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), s2       ; encoding: [0x45,0xf0,0x02,0xb9]

// HW register code of unknown HW register, valid symbolic name range but no name available
s_setreg_b32 hwreg(10), s2
// SICI: s_setreg_b32 hwreg(10), s2      ; encoding: [0x0a,0xf8,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(10), s2      ; encoding: [0x0a,0xf8,0x02,0xb9]

// HW_REG_SH_MEM_BASES valid starting from GFX9
s_setreg_b32 hwreg(15), s2
// SICI: s_setreg_b32 hwreg(15), s2      ; encoding: [0x0f,0xf8,0x82,0xb9]
// VI:   s_setreg_b32 hwreg(15), s2      ; encoding: [0x0f,0xf8,0x02,0xb9]
// GFX9: s_setreg_b32 hwreg(HW_REG_SH_MEM_BASES), s2 ; encoding: [0x0f,0xf8,0x02,0xb9]

// HW register code, non-default offset/width
s_setreg_b32 hwreg(5, 1, 31), s2
// SICI: s_setreg_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), s2       ; encoding: [0x45,0xf0,0x82,0xb9]
// VI9:  s_setreg_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), s2       ; encoding: [0x45,0xf0,0x02,0xb9]

// raw number mapped to known HW register
s_setreg_imm32_b32 0x6, 0xff
// SICI: s_setreg_imm32_b32 hwreg(HW_REG_LDS_ALLOC, 0, 1), 0xff ; encoding: [0x06,0x00,0x80,0xba,0xff,0x00,0x00,0x00]
// VI9:  s_setreg_imm32_b32 hwreg(HW_REG_LDS_ALLOC, 0, 1), 0xff ; encoding: [0x06,0x00,0x00,0xba,0xff,0x00,0x00,0x00]

// HW register identifier, non-default offset/width
s_setreg_imm32_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), 0xff
// SICI: s_setreg_imm32_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), 0xff ; encoding: [0x45,0xf0,0x80,0xba,0xff,0x00,0x00,0x00]
// VI9:  s_setreg_imm32_b32 hwreg(HW_REG_GPR_ALLOC, 1, 31), 0xff ; encoding: [0x45,0xf0,0x00,0xba,0xff,0x00,0x00,0x00]

s_endpgm_ordered_ps_done
// GFX9:     s_endpgm_ordered_ps_done ; encoding: [0x00,0x00,0x9e,0xbf]
// NOSICIVI: error: instruction not supported on this GPU

s_call_b64 s[12:13], 12609
// GFX9:     s_call_b64 s[12:13], 0x3141 ; encoding: [0x41,0x31,0x8c,0xba]
// NOSICIVI: error: instruction not supported on this GPU

s_call_b64 s[100:101], 12609
// GFX9:     s_call_b64 s[100:101], 0x3141 ; encoding: [0x41,0x31,0xe4,0xba]
// NOSICIVI: error: instruction not supported on this GPU

s_call_b64 s[10:11], 49617
// GFX9:     s_call_b64 s[10:11], 0xc1d1 ; encoding: [0xd1,0xc1,0x8a,0xba]
// NOSICIVI: error: instruction not supported on this GPU
