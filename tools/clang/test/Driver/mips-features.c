// Check handling MIPS specific features options.
//
// -mabicalls
// RUN: %clang -target mips-linux-gnu -### -c %s -mno-abicalls -mabicalls 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MABICALLS %s
// CHECK-MABICALLS: "-target-feature" "-noabicalls"
//
// -mno-abicalls
// RUN: %clang -target mips-linux-gnu -### -c %s -mabicalls -mno-abicalls 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MNOABICALLS %s
// CHECK-MNOABICALLS: "-target-feature" "+noabicalls"
//
// -mips16
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-mips16 -mips16 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MIPS16 %s
// CHECK-MIPS16: "-target-feature" "+mips16"
//
// -mno-mips16
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mips16 -mno-mips16 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMIPS16 %s
// CHECK-NOMIPS16: "-target-feature" "-mips16"
//
// -mmicromips
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-micromips -mmicromips 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MICROMIPS %s
// CHECK-MICROMIPS: "-target-feature" "+micromips"
//
// -mno-micromips
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mmicromips -mno-micromips 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMICROMIPS %s
// CHECK-NOMICROMIPS: "-target-feature" "-micromips"
//
// -mdsp
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-dsp -mdsp 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MDSP %s
// CHECK-MDSP: "-target-feature" "+dsp"
//
// -mno-dsp
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mdsp -mno-dsp 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMDSP %s
// CHECK-NOMDSP: "-target-feature" "-dsp"
//
// -mdspr2
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-dspr2 -mdspr2 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MDSPR2 %s
// CHECK-MDSPR2: "-target-feature" "+dspr2"
//
// -mno-dspr2
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mdspr2 -mno-dspr2 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMDSPR2 %s
// CHECK-NOMDSPR2: "-target-feature" "-dspr2"
//
// -mmsa
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-msa -mmsa 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MMSA %s
// CHECK-MMSA: "-target-feature" "+msa"
//
// -mno-msa
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mmsa -mno-msa 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMMSA %s
// CHECK-NOMMSA: "-target-feature" "-msa"
//
// -mmt
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-mt -mmt 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MMT %s
// CHECK-MMT: "-target-feature" "+mt"
//
// -mno-mt
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mmt -mno-mt 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMMT %s
// CHECK-NOMMT: "-target-feature" "-mt"
//
// -modd-spreg
// RUN: %clang -target mips-linux-gnu -### -c %s -mno-odd-spreg -modd-spreg 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MODDSPREG %s
// CHECK-MODDSPREG: "-target-feature" "-nooddspreg"
//
// -mno-odd-spreg
// RUN: %clang -target mips-linux-gnu -### -c %s -modd-spreg -mno-odd-spreg 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMODDSPREG %s
// CHECK-NOMODDSPREG: "-target-feature" "+nooddspreg"
//
// -mfpxx
// RUN: %clang -target mips-linux-gnu -### -c %s -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MFPXX %s
// CHECK-MFPXX: "-target-feature" "+fpxx"
// CHECK-MFPXX: "-target-feature" "+nooddspreg"
//
// -mfpxx -modd-spreg
// RUN: %clang -target mips-linux-gnu -### -c %s -mfpxx -modd-spreg 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MFPXX-ODDSPREG %s
// CHECK-MFPXX-ODDSPREG: "-target-feature" "+fpxx"
// CHECK-MFPXX-ODDSPREG: "-target-feature" "-nooddspreg"
//
// -mfp64
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mfp32 -mfp64 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MFP64 %s
// CHECK-MFP64: "-target-feature" "+fp64"
//
// -mfp32
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mfp64 -mfp32 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOMFP64 %s
// CHECK-NOMFP64: "-target-feature" "-fp64"
//
// -mnan=2008
// RUN: %clang -target mips-linux-gnu -march=mips32r3 -### -c %s \
// RUN:     -mnan=legacy -mnan=2008 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NAN2008 %s
// CHECK-NAN2008: "-target-feature" "+nan2008"
//
// -mnan=legacy
// RUN: %clang -target mips-linux-gnu -march=mips32r3 -### -c %s \
// RUN:     -mnan=2008 -mnan=legacy 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NANLEGACY %s
// CHECK-NANLEGACY: "-target-feature" "-nan2008"
//
// -mcompact-branches=never
// RUN: %clang -target mips-linux-gnu -march=mips32r6 -### -c %s \
// RUN:     -mcompact-branches=never 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-CBNEVER %s
// CHECK-CBNEVER: "-mllvm" "-mips-compact-branches=never"
//
// -mcompact-branches=optimal
// RUN: %clang -target mips-linux-gnu -march=mips32r6 -### -c %s \
// RUN:     -mcompact-branches=optimal 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-CBOPTIMAL %s
// CHECK-CBOPTIMAL: "-mllvm" "-mips-compact-branches=optimal"
//
// -mcompact-branches=always
// RUN: %clang -target mips-linux-gnu -march=mips32r6 -### -c %s \
// RUN:     -mcompact-branches=always 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-CBALWAYS %s
// CHECK-CBALWAYS: "-mllvm" "-mips-compact-branches=always"
//
// -mxgot
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-xgot -mxgot 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-XGOT %s
// CHECK-XGOT: "-mllvm" "-mxgot"
//
// -mno-xgot
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mxgot -mno-xgot 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOXGOT %s
// CHECK-NOXGOT-NOT: "-mllvm" "-mxgot"
//
// -mldc1-sdc1
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-ldc1-sdc1 -mldc1-sdc1 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-LDC1SDC1 %s
// CHECK-LDC1SDC1-NOT: "-mllvm" "-mno-ldc1-sdc1"
//
// -mno-ldc1-sdc1
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mldc1-sdc1 -mno-ldc1-sdc1 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOLDC1SDC1 %s
// CHECK-NOLDC1SDC1: "-mllvm" "-mno-ldc1-sdc1"
//
// -mcheck-zero-division
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mno-check-zero-division -mcheck-zero-division 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-ZERODIV %s
// CHECK-ZERODIV-NOT: "-mllvm" "-mno-check-zero-division"
//
// -mno-check-zero-division
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -mcheck-zero-division -mno-check-zero-division 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-NOZERODIV %s
// CHECK-NOZERODIV: "-mllvm" "-mno-check-zero-division"
//
// -G
// RUN: %clang -target mips-linux-gnu -### -c %s \
// RUN:     -G 16 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MIPS-G %s
// CHECK-MIPS-G: "-mllvm" "-mips-ssection-threshold=16"
//
// -msoft-float (unknown vendor)
// RUN: %clang -target mips-linux-gnu -### -c %s -msoft-float 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-SOFTFLOAT %s
// CHECK-SOFTFLOAT: "-target-feature" "+soft-float"
// CHECK-SOFTFLOAT-NOT: "-target-feature" "+fpxx"
//
// -msoft-float -mfpxx (unknown vendor)
// RUN: %clang -target mips-linux-gnu -### -c %s -msoft-float -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-SOFTFLOAT-FPXX %s
// CHECK-SOFTFLOAT-FPXX: "-target-feature" "+soft-float"
// CHECK-SOFTFLOAT-FPXX: "-target-feature" "+fpxx"
//
// -msoft-float (MTI)
// RUN: %clang -target mips-mti-linux-gnu -### -c %s -msoft-float 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MTI-SOFTFLOAT %s
// CHECK-MTI-SOFTFLOAT: "-target-feature" "+soft-float"
// CHECK-MTI-SOFTFLOAT-NOT: "-target-feature" "+fpxx"
//
// -msoft-float -mfpxx (MTI)
// RUN: %clang -target mips-mti-linux-gnu -### -c %s -msoft-float -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MTI-SOFTFLOAT-FPXX %s
// CHECK-MTI-SOFTFLOAT-FPXX: "-target-feature" "+soft-float"
// CHECK-MTI-SOFTFLOAT-FPXX: "-target-feature" "+fpxx"
//
// -msoft-float (IMG)
// RUN: %clang -target mips-img-linux-gnu -### -c %s -msoft-float 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-IMG-SOFTFLOAT %s
// CHECK-IMG-SOFTFLOAT: "-target-feature" "+soft-float"
// CHECK-IMG-SOFTFLOAT-NOT: "-target-feature" "+fpxx"
//
// -msoft-float -mfpxx (IMG)
// RUN: %clang -target mips-img-linux-gnu -### -c %s -msoft-float -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-IMG-SOFTFLOAT-FPXX %s
// CHECK-IMG-SOFTFLOAT-FPXX: "-target-feature" "+soft-float"
// CHECK-IMG-SOFTFLOAT-FPXX: "-target-feature" "+fpxx"
//
// -msingle-float (unknown vendor)
// RUN: %clang -target mips-linux-gnu -### -c %s -msingle-float 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-SINGLEFLOAT %s
// CHECK-SINGLEFLOAT: "-target-feature" "+single-float"
// CHECK-SINGLEFLOAT-NOT: "-target-feature" "+fpxx"
//
// -msingle-float -mfpxx (unknown vendor)
// RUN: %clang -target mips-linux-gnu -### -c %s -msingle-float -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-SINGLEFLOAT-FPXX %s
// CHECK-SINGLEFLOAT-FPXX: "-target-feature" "+single-float"
// CHECK-SINGLEFLOAT-FPXX: "-target-feature" "+fpxx"
//
// -msingle-float (MTI)
// RUN: %clang -target mips-mti-linux-gnu -### -c %s -msingle-float 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MTI-SINGLEFLOAT %s
// CHECK-MTI-SINGLEFLOAT: "-target-feature" "+single-float"
// CHECK-MTI-SINGLEFLOAT-NOT: "-target-feature" "+fpxx"
//
// -msingle-float -mfpxx (MTI)
// RUN: %clang -target mips-mti-linux-gnu -### -c %s -msingle-float -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-MTI-SINGLEFLOAT-FPXX %s
// CHECK-MTI-SINGLEFLOAT-FPXX: "-target-feature" "+single-float"
// CHECK-MTI-SINGLEFLOAT-FPXX: "-target-feature" "+fpxx"
//
// -msingle-float (IMG)
// RUN: %clang -target mips-img-linux-gnu -### -c %s -msingle-float 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-IMG-SINGLEFLOAT %s
// CHECK-IMG-SINGLEFLOAT: "-target-feature" "+single-float"
// CHECK-IMG-SINGLEFLOAT-NOT: "-target-feature" "+fpxx"
//
// -msingle-float -mfpxx (IMG)
// RUN: %clang -target mips-img-linux-gnu -### -c %s -msingle-float -mfpxx 2>&1 \
// RUN:   | FileCheck --check-prefix=CHECK-IMG-SINGLEFLOAT-FPXX %s
// CHECK-IMG-SINGLEFLOAT-FPXX: "-target-feature" "+single-float"
// CHECK-IMG-SINGLEFLOAT-FPXX: "-target-feature" "+fpxx"

// -mlong-call
// RUN: %clang -target mips-img-linux-gnu -### -c %s -mlong-calls 2>&1 \
// RUN:   | FileCheck --check-prefix=LONG-CALLS-ON %s
// RUN: %clang -target mips-img-linux-gnu -### -c %s -mno-long-calls 2>&1 \
// RUN:   | FileCheck --check-prefix=LONG-CALLS-OFF %s
// RUN: %clang -target mips-img-linux-gnu -### -c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LONG-CALLS-DEF %s
// LONG-CALLS-ON: "-target-feature" "+long-calls"
// LONG-CALLS-OFF: "-target-feature" "-long-calls"
// LONG-CALLS-DEF-NOT: "long-calls"
