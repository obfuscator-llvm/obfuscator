; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mattr=+fma | FileCheck -check-prefix=FMA3 -check-prefix=FMA3_256 %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mattr=+fma,+avx512f | FileCheck -check-prefix=FMA3 -check-prefix=FMA3_512 %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mattr=+fma4 | FileCheck -check-prefix=FMA4 %s

; This test checks the fusing of MUL + ADDSUB to FMADDSUB.

define <2 x double> @mul_addsub_pd128(<2 x double> %A, <2 x double> %B,  <2 x double> %C) #0 {
; FMA3-LABEL: mul_addsub_pd128:
; FMA3:       # BB#0: # %entry
; FMA3-NEXT:  vfmaddsub213pd  %xmm2, %xmm1, %xmm0
; FMA3-NEXT:  retq
;
; FMA4-LABEL: mul_addsub_pd128:
; FMA4:       # BB#0: # %entry
; FMA4-NEXT:  vfmaddsubpd     %xmm2, %xmm1, %xmm0, %xmm0
; FMA4-NEXT:  retq
entry:
  %AB = fmul <2 x double> %A, %B
  %Sub = fsub <2 x double> %AB, %C
  %Add = fadd <2 x double> %AB, %C
  %Addsub = shufflevector <2 x double> %Sub, <2 x double> %Add, <2 x i32> <i32 0, i32 3>
  ret <2 x double> %Addsub
}

define <4 x float> @mul_addsub_ps128(<4 x float> %A, <4 x float> %B, <4 x float> %C) #0 {
; FMA3-LABEL: mul_addsub_ps128:
; FMA3:       # BB#0: # %entry
; FMA3-NEXT:  vfmaddsub213ps  %xmm2, %xmm1, %xmm0
; FMA3-NEXT:  retq
;
; FMA4-LABEL: mul_addsub_ps128:
; FMA4:       # BB#0: # %entry
; FMA4-NEXT:  vfmaddsubps     %xmm2, %xmm1, %xmm0, %xmm0
; FMA4-NEXT:  retq
entry:
  %AB = fmul <4 x float> %A, %B
  %Sub = fsub <4 x float> %AB, %C
  %Add = fadd <4 x float> %AB, %C
  %Addsub = shufflevector <4 x float> %Sub, <4 x float> %Add, <4 x i32> <i32 0, i32 5, i32 2, i32 7>
  ret <4 x float> %Addsub
}

define <4 x double> @mul_addsub_pd256(<4 x double> %A, <4 x double> %B, <4 x double> %C) #0 {
; FMA3-LABEL: mul_addsub_pd256:
; FMA3:       # BB#0: # %entry
; FMA3-NEXT:  vfmaddsub213pd  %ymm2, %ymm1, %ymm0
; FMA3-NEXT:  retq
;
; FMA4-LABEL: mul_addsub_pd256:
; FMA4:       # BB#0: # %entry
; FMA4-NEXT:  vfmaddsubpd     %ymm2, %ymm1, %ymm0, %ymm0
; FMA4-NEXT:  retq
entry:
  %AB = fmul <4 x double> %A, %B
  %Sub = fsub <4 x double> %AB, %C
  %Add = fadd <4 x double> %AB, %C
  %Addsub = shufflevector <4 x double> %Sub, <4 x double> %Add, <4 x i32> <i32 0, i32 5, i32 2, i32 7>
  ret <4 x double> %Addsub
}

define <8 x float> @mul_addsub_ps256(<8 x float> %A, <8 x float> %B, <8 x float> %C) #0 {
; FMA3-LABEL: mul_addsub_ps256:
; FMA3:       # BB#0: # %entry
; FMA3-NEXT:  vfmaddsub213ps  %ymm2, %ymm1, %ymm0
; FMA3-NEXT:  retq
;
; FMA4-LABEL: mul_addsub_ps256:
; FMA4:       # BB#0: # %entry
; FMA4-NEXT:  vfmaddsubps     %ymm2, %ymm1, %ymm0, %ymm0
; FMA4-NEXT:  retq
entry:
  %AB = fmul <8 x float> %A, %B
  %Sub = fsub <8 x float> %AB, %C
  %Add = fadd <8 x float> %AB, %C
  %Addsub = shufflevector <8 x float> %Sub, <8 x float> %Add, <8 x i32> <i32 0, i32 9, i32 2, i32 11, i32 4, i32 13, i32 6, i32 15>
  ret <8 x float> %Addsub
}

define <8 x double> @mul_addsub_pd512(<8 x double> %A, <8 x double> %B, <8 x double> %C) #0 {
; FMA3_256-LABEL: mul_addsub_pd512:
; FMA3_256:       # BB#0: # %entry
; FMA3_256-NEXT:  vfmaddsub213pd  %ymm4, %ymm2, %ymm0
; FMA3_256-NEXT:  vfmaddsub213pd  %ymm5, %ymm3, %ymm1
; FMA3_256-NEXT:  retq
;
; FMA3_512-LABEL: mul_addsub_pd512:
; FMA3_512:       # BB#0: # %entry
; FMA3_512-NEXT:  vfmaddsub213pd  %zmm2, %zmm1, %zmm0
; FMA3_512-NEXT:  retq
;
; FMA4-LABEL: mul_addsub_pd512:
; FMA4:       # BB#0: # %entry
; FMA4-NEXT:  vfmaddsubpd     %ymm4, %ymm2, %ymm0, %ymm0
; FMA4-NEXT:  vfmaddsubpd     %ymm5, %ymm3, %ymm1, %ymm1
; FMA4-NEXT:  retq
entry:
  %AB = fmul <8 x double> %A, %B
  %Sub = fsub <8 x double> %AB, %C
  %Add = fadd <8 x double> %AB, %C
  %Addsub = shufflevector <8 x double> %Sub, <8 x double> %Add, <8 x i32> <i32 0, i32 9, i32 2, i32 11, i32 4, i32 13, i32 6, i32 15>
  ret <8 x double> %Addsub
}

define <16 x float> @mul_addsub_ps512(<16 x float> %A, <16 x float> %B, <16 x float> %C) #0 {
; FMA3_256-LABEL: mul_addsub_ps512:
; FMA3_256:       # BB#0: # %entry
; FMA3_256-NEXT:  vfmaddsub213ps  %ymm4, %ymm2, %ymm0
; FMA3_256-NEXT:  vfmaddsub213ps  %ymm5, %ymm3, %ymm1
; FMA3_256-NEXT:  retq
;
; FMA3_512-LABEL: mul_addsub_ps512:
; FMA3_512:       # BB#0: # %entry
; FMA3_512-NEXT:  vfmaddsub213ps  %zmm2, %zmm1, %zmm0
; FMA3_512-NEXT:  retq
;
; FMA4-LABEL: mul_addsub_ps512:
; FMA4:       # BB#0: # %entry
; FMA4-NEXT:  vfmaddsubps     %ymm4, %ymm2, %ymm0, %ymm0
; FMA4-NEXT:  vfmaddsubps     %ymm5, %ymm3, %ymm1, %ymm1
; FMA4-NEXT:  retq
entry:
  %AB = fmul <16 x float> %A, %B
  %Sub = fsub <16 x float> %AB, %C
  %Add = fadd <16 x float> %AB, %C
  %Addsub = shufflevector <16 x float> %Sub, <16 x float> %Add, <16 x i32> <i32 0, i32 17, i32 2, i32 19, i32 4, i32 21, i32 6, i32 23, i32 8, i32 25, i32 10, i32 27, i32 12, i32 29, i32 14, i32 31>
  ret <16 x float> %Addsub
}

attributes #0 = { nounwind "unsafe-fp-math"="true" }
