# RUN: llvm-mc %s -arch=mips -mcpu=mips32 -show-encoding |\
# RUN:   FileCheck %s -check-prefix=NORMAL

# RUN: llvm-mc %s -arch=mips -mcpu=mips64 -target-abi n32 -show-encoding |\
# RUN:   FileCheck %s -check-prefix=NORMAL

# RUN: llvm-mc %s -arch=mips64 -mcpu=mips64 -target-abi n64 -show-encoding |\
# RUN:   FileCheck %s -check-prefix=NORMAL

# RUN: llvm-mc %s -arch=mips -mcpu=mips32 -mattr=micromips -show-encoding |\
# RUN:   FileCheck %s -check-prefix=MICRO

# RUN: llvm-mc %s -arch=mips64 -mcpu=mips64 -target-abi n32 -mattr=micromips -show-encoding |\
# RUN:   FileCheck %s -check-prefix=MICRO

# RUN: llvm-mc %s -arch=mips64 -mcpu=mips64 -target-abi n64 -mattr=micromips -show-encoding |\
# RUN:   FileCheck %s -check-prefix=MICRO

# Repeat the tests using ELF output.

# RUN: llvm-mc %s -arch=mips -mcpu=mips32 -filetype=obj | \
# RUN:   llvm-objdump -d -r - | FileCheck %s -check-prefixes=ELF-O32
# RUN: llvm-mc %s -arch=mips64 -mcpu=mips64 -target-abi n32 -filetype=obj | \
# RUN:   llvm-objdump -d -r - | FileCheck %s -check-prefixes=ELF-NXX
# RUN: llvm-mc %s -arch=mips64 -mcpu=mips64 -target-abi n64 -filetype=obj | \
# RUN:   llvm-objdump -d -r - | FileCheck %s -check-prefixes=ELF-NXX

  .weak weak_label

  .text
  .option pic2

  .ent local_label
local_label:
  .frame  $sp, 0, $ra
  .set noreorder

  j local_label
  nop

# NORMAL: b      local_label      # encoding: [0x10,0x00,A,A]
# NORMAL:                         #   fixup A - offset: 0, value: local_label-4, kind: fixup_Mips_PC16

# MICRO:  b      local_label      # encoding: [0x94,0x00,A,A]
# MICRO:                          #   fixup A - offset: 0, value: local_label, kind: fixup_MICROMIPS_PC16_S1

# ELF-O32:      10 00 ff ff     b       0

# ELF-NXX:      10 00 ff ff     b       0

  j weak_label
  nop

# NORMAL: b      weak_label       # encoding: [0x10,0x00,A,A]
# NORMAL:                         #   fixup A - offset: 0, value: weak_label-4, kind: fixup_Mips_PC16

# MICRO:  b      weak_label       # encoding: [0x94,0x00,A,A]
# MICRO:                          #   fixup A - offset: 0, value: weak_label, kind: fixup_MICROMIPS_PC16_S1

# ELF-O32:      10 00 ff ff     b       0
# ELF-O32-NEXT:                 R_MIPS_PC16  weak_label

# ELF-NXX:      10 00 00 00     b       4
# ELF-NXX-NEXT:                 R_MIPS_PC16/R_MIPS_NONE/R_MIPS_NONE  weak_label

  j global_label
  nop

# NORMAL: b      global_label     # encoding: [0x10,0x00,A,A]
# NORMAL:                         #   fixup A - offset: 0, value: global_label-4, kind: fixup_Mips_PC16

# MICRO:  b      global_label     # encoding: [0x94,0x00,A,A]
# MICRO:                          #   fixup A - offset: 0, value: global_label, kind: fixup_MICROMIPS_PC16_S1

# ELF-O32:      10 00 ff ff     b       0
# ELF-O32-NEXT:         00000010:  R_MIPS_PC16  global_label

# ELF-NXX:      10 00 00 00     b       4
# ELF-NXX-NEXT:                 R_MIPS_PC16/R_MIPS_NONE/R_MIPS_NONE  global_label

  j .text
  nop

# NORMAL: b      .text            # encoding: [0x10,0x00,A,A]
# NORMAL:                         #   fixup A - offset: 0, value: .text-4, kind: fixup_Mips_PC16

# MICRO:  b      .text            # encoding: [0x94,0x00,A,A]
# MICRO:                          #   fixup A - offset: 0, value: .text, kind: fixup_MICROMIPS_PC16_S1

# ELF-O32:      10 00 ff f9 	b	-24 <local_label>
# ELF-O32-NEXT: 00 00 00 00 	nop

# ELF-NXX:      10 00 ff f9 	b	-24 <local_label>
# ELF-NXX-NEXT: 00 00 00 00 	nop

  j 1f
  nop

# NORMAL: b      {{.*}}tmp0{{.*}} # encoding: [0x10,0x00,A,A]
# NORMAL:                         #   fixup A - offset: 0, value: {{.*}}tmp0{{.*}}-4, kind: fixup_Mips_PC16

# MICRO:  b      {{.*}}tmp0{{.*}} # encoding: [0x94,0x00,A,A]
# MICRO:                          #   fixup A - offset: 0, value: {{.*}}tmp0{{.*}}, kind: fixup_MICROMIPS_PC16_S1

# ELF-O32:      10 00 00 04     b       20 <local_label+0x34>

# ELF-NXX:      10 00 00 04     b       20 <local_label+0x34>

  .local forward_local
  j forward_local
  nop

# NORMAL: b      forward_local    # encoding: [0x10,0x00,A,A]
# NORMAL:                         #   fixup A - offset: 0, value: forward_local-4, kind: fixup_Mips_PC16

# MICRO:  b      forward_local    # encoding: [0x94,0x00,A,A]
# MICRO:                          #   fixup A - offset: 0, value: forward_local, kind: fixup_MICROMIPS_PC16_S1

# ELF-O32:      10 00 00 04     b       20 <forward_local>

# ELF-NXX:      10 00 00 04     b       20 <forward_local>

  j 0x4

# NORMAL: b      4                # encoding: [0x10,0x00,0x00,0x01]

# MICRO:  b      4                # encoding: [0x94,0x00,0x00,0x02]

# ELF-O32:      10 00 00 01     b       8

# ELF-NXX:      10 00 00 01     b       8

  .end local_label

1:
  nop
  nop
forward_local:

