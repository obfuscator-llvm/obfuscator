//===-- X86ShuffleDecode.cpp - X86 shuffle decode logic -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Define several functions to decode x86 specific shuffle semantics into a
// generic vector mask.
//
//===----------------------------------------------------------------------===//

#include "X86ShuffleDecode.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/CodeGen/MachineValueType.h"

//===----------------------------------------------------------------------===//
//  Vector Mask Decoding
//===----------------------------------------------------------------------===//

namespace llvm {

void DecodeINSERTPSMask(unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  // Defaults the copying the dest value.
  ShuffleMask.push_back(0);
  ShuffleMask.push_back(1);
  ShuffleMask.push_back(2);
  ShuffleMask.push_back(3);

  // Decode the immediate.
  unsigned ZMask = Imm & 15;
  unsigned CountD = (Imm >> 4) & 3;
  unsigned CountS = (Imm >> 6) & 3;

  // CountS selects which input element to use.
  unsigned InVal = 4 + CountS;
  // CountD specifies which element of destination to update.
  ShuffleMask[CountD] = InVal;
  // ZMask zaps values, potentially overriding the CountD elt.
  if (ZMask & 1) ShuffleMask[0] = SM_SentinelZero;
  if (ZMask & 2) ShuffleMask[1] = SM_SentinelZero;
  if (ZMask & 4) ShuffleMask[2] = SM_SentinelZero;
  if (ZMask & 8) ShuffleMask[3] = SM_SentinelZero;
}

void DecodeInsertElementMask(MVT VT, unsigned Idx, unsigned Len,
                             SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();
  assert((Idx + Len) <= NumElts && "Insertion out of range");

  for (unsigned i = 0; i != NumElts; ++i)
    ShuffleMask.push_back(i);
  for (unsigned i = 0; i != Len; ++i)
    ShuffleMask[Idx + i] = NumElts + i;
}

// <3,1> or <6,7,2,3>
void DecodeMOVHLPSMask(unsigned NElts, SmallVectorImpl<int> &ShuffleMask) {
  for (unsigned i = NElts / 2; i != NElts; ++i)
    ShuffleMask.push_back(NElts + i);

  for (unsigned i = NElts / 2; i != NElts; ++i)
    ShuffleMask.push_back(i);
}

// <0,2> or <0,1,4,5>
void DecodeMOVLHPSMask(unsigned NElts, SmallVectorImpl<int> &ShuffleMask) {
  for (unsigned i = 0; i != NElts / 2; ++i)
    ShuffleMask.push_back(i);

  for (unsigned i = 0; i != NElts / 2; ++i)
    ShuffleMask.push_back(NElts + i);
}

void DecodeMOVSLDUPMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();
  for (int i = 0, e = NumElts / 2; i < e; ++i) {
    ShuffleMask.push_back(2 * i);
    ShuffleMask.push_back(2 * i);
  }
}

void DecodeMOVSHDUPMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();
  for (int i = 0, e = NumElts / 2; i < e; ++i) {
    ShuffleMask.push_back(2 * i + 1);
    ShuffleMask.push_back(2 * i + 1);
  }
}

void DecodeMOVDDUPMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned VectorSizeInBits = VT.getSizeInBits();
  unsigned ScalarSizeInBits = VT.getScalarSizeInBits();
  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumLanes = VectorSizeInBits / 128;
  unsigned NumLaneElts = NumElts / NumLanes;
  unsigned NumLaneSubElts = 64 / ScalarSizeInBits;

  for (unsigned l = 0; l < NumElts; l += NumLaneElts)
    for (unsigned i = 0; i < NumLaneElts; i += NumLaneSubElts)
      for (unsigned s = 0; s != NumLaneSubElts; s++)
        ShuffleMask.push_back(l + s);
}

void DecodePSLLDQMask(MVT VT, unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  unsigned VectorSizeInBits = VT.getSizeInBits();
  unsigned NumElts = VectorSizeInBits / 8;
  unsigned NumLanes = VectorSizeInBits / 128;
  unsigned NumLaneElts = NumElts / NumLanes;

  for (unsigned l = 0; l < NumElts; l += NumLaneElts)
    for (unsigned i = 0; i < NumLaneElts; ++i) {
      int M = SM_SentinelZero;
      if (i >= Imm) M = i - Imm + l;
      ShuffleMask.push_back(M);
    }
}

void DecodePSRLDQMask(MVT VT, unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  unsigned VectorSizeInBits = VT.getSizeInBits();
  unsigned NumElts = VectorSizeInBits / 8;
  unsigned NumLanes = VectorSizeInBits / 128;
  unsigned NumLaneElts = NumElts / NumLanes;

  for (unsigned l = 0; l < NumElts; l += NumLaneElts)
    for (unsigned i = 0; i < NumLaneElts; ++i) {
      unsigned Base = i + Imm;
      int M = Base + l;
      if (Base >= NumLaneElts) M = SM_SentinelZero;
      ShuffleMask.push_back(M);
    }
}

void DecodePALIGNRMask(MVT VT, unsigned Imm,
                       SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();
  unsigned Offset = Imm * (VT.getScalarSizeInBits() / 8);

  unsigned NumLanes = VT.getSizeInBits() / 128;
  unsigned NumLaneElts = NumElts / NumLanes;

  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = 0; i != NumLaneElts; ++i) {
      unsigned Base = i + Offset;
      // if i+offset is out of this lane then we actually need the other source
      if (Base >= NumLaneElts) Base += NumElts - NumLaneElts;
      ShuffleMask.push_back(Base + l);
    }
  }
}

void DecodeVALIGNMask(MVT VT, unsigned Imm,
                      SmallVectorImpl<int> &ShuffleMask) {
  int NumElts = VT.getVectorNumElements();
  // Not all bits of the immediate are used so mask it.
  assert(isPowerOf2_32(NumElts) && "NumElts should be power of 2");
  Imm = Imm & (NumElts - 1);
  for (int i = 0; i != NumElts; ++i)
    ShuffleMask.push_back(i + Imm);
}

/// DecodePSHUFMask - This decodes the shuffle masks for pshufw, pshufd, and vpermilp*.
/// VT indicates the type of the vector allowing it to handle different
/// datatypes and vector widths.
void DecodePSHUFMask(MVT VT, unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();

  unsigned NumLanes = VT.getSizeInBits() / 128;
  if (NumLanes == 0) NumLanes = 1;  // Handle MMX
  unsigned NumLaneElts = NumElts / NumLanes;

  unsigned NewImm = Imm;
  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = 0; i != NumLaneElts; ++i) {
      ShuffleMask.push_back(NewImm % NumLaneElts + l);
      NewImm /= NumLaneElts;
    }
    if (NumLaneElts == 4) NewImm = Imm; // reload imm
  }
}

void DecodePSHUFHWMask(MVT VT, unsigned Imm,
                       SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();

  for (unsigned l = 0; l != NumElts; l += 8) {
    unsigned NewImm = Imm;
    for (unsigned i = 0, e = 4; i != e; ++i) {
      ShuffleMask.push_back(l + i);
    }
    for (unsigned i = 4, e = 8; i != e; ++i) {
      ShuffleMask.push_back(l + 4 + (NewImm & 3));
      NewImm >>= 2;
    }
  }
}

void DecodePSHUFLWMask(MVT VT, unsigned Imm,
                       SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();

  for (unsigned l = 0; l != NumElts; l += 8) {
    unsigned NewImm = Imm;
    for (unsigned i = 0, e = 4; i != e; ++i) {
      ShuffleMask.push_back(l + (NewImm & 3));
      NewImm >>= 2;
    }
    for (unsigned i = 4, e = 8; i != e; ++i) {
      ShuffleMask.push_back(l + i);
    }
  }
}

void DecodePSWAPMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumHalfElts = NumElts / 2;

  for (unsigned l = 0; l != NumHalfElts; ++l)
    ShuffleMask.push_back(l + NumHalfElts);
  for (unsigned h = 0; h != NumHalfElts; ++h)
    ShuffleMask.push_back(h);
}

/// DecodeSHUFPMask - This decodes the shuffle masks for shufp*. VT indicates
/// the type of the vector allowing it to handle different datatypes and vector
/// widths.
void DecodeSHUFPMask(MVT VT, unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();

  unsigned NumLanes = VT.getSizeInBits() / 128;
  unsigned NumLaneElts = NumElts / NumLanes;

  unsigned NewImm = Imm;
  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    // each half of a lane comes from different source
    for (unsigned s = 0; s != NumElts * 2; s += NumElts) {
      for (unsigned i = 0; i != NumLaneElts / 2; ++i) {
        ShuffleMask.push_back(NewImm % NumLaneElts + s + l);
        NewImm /= NumLaneElts;
      }
    }
    if (NumLaneElts == 4) NewImm = Imm; // reload imm
  }
}

/// DecodeUNPCKHMask - This decodes the shuffle masks for unpckhps/unpckhpd
/// and punpckh*. VT indicates the type of the vector allowing it to handle
/// different datatypes and vector widths.
void DecodeUNPCKHMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();

  // Handle 128 and 256-bit vector lengths. AVX defines UNPCK* to operate
  // independently on 128-bit lanes.
  unsigned NumLanes = VT.getSizeInBits() / 128;
  if (NumLanes == 0) NumLanes = 1;  // Handle MMX
  unsigned NumLaneElts = NumElts / NumLanes;

  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = l + NumLaneElts / 2, e = l + NumLaneElts; i != e; ++i) {
      ShuffleMask.push_back(i);           // Reads from dest/src1
      ShuffleMask.push_back(i + NumElts); // Reads from src/src2
    }
  }
}

/// DecodeUNPCKLMask - This decodes the shuffle masks for unpcklps/unpcklpd
/// and punpckl*. VT indicates the type of the vector allowing it to handle
/// different datatypes and vector widths.
void DecodeUNPCKLMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();

  // Handle 128 and 256-bit vector lengths. AVX defines UNPCK* to operate
  // independently on 128-bit lanes.
  unsigned NumLanes = VT.getSizeInBits() / 128;
  if (NumLanes == 0 ) NumLanes = 1;  // Handle MMX
  unsigned NumLaneElts = NumElts / NumLanes;

  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = l, e = l + NumLaneElts / 2; i != e; ++i) {
      ShuffleMask.push_back(i);           // Reads from dest/src1
      ShuffleMask.push_back(i + NumElts); // Reads from src/src2
    }
  }
}

/// Decodes a broadcast of the first element of a vector.
void DecodeVectorBroadcast(MVT DstVT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = DstVT.getVectorNumElements();
  ShuffleMask.append(NumElts, 0);
}

/// Decodes a broadcast of a subvector to a larger vector type.
void DecodeSubVectorBroadcast(MVT DstVT, MVT SrcVT,
                              SmallVectorImpl<int> &ShuffleMask) {
  assert(SrcVT.getScalarType() == DstVT.getScalarType() &&
         "Non matching vector element types");
  unsigned NumElts = SrcVT.getVectorNumElements();
  unsigned Scale = DstVT.getSizeInBits() / SrcVT.getSizeInBits();

  for (unsigned i = 0; i != Scale; ++i)
    for (unsigned j = 0; j != NumElts; ++j)
      ShuffleMask.push_back(j);
}

/// \brief Decode a shuffle packed values at 128-bit granularity
/// (SHUFF32x4/SHUFF64x2/SHUFI32x4/SHUFI64x2)
/// immediate mask into a shuffle mask.
void decodeVSHUF64x2FamilyMask(MVT VT, unsigned Imm,
                        SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumLanes = VT.getSizeInBits() / 128;
  unsigned NumElementsInLane = 128 / VT.getScalarSizeInBits();
  unsigned ControlBitsMask = NumLanes - 1;
  unsigned NumControlBits  = NumLanes / 2;

  for (unsigned l = 0; l != NumLanes; ++l) {
    unsigned LaneMask = (Imm >> (l * NumControlBits)) & ControlBitsMask;
    // We actually need the other source.
    if (l >= NumLanes / 2)
      LaneMask += NumLanes;
    for (unsigned i = 0; i != NumElementsInLane; ++i)
      ShuffleMask.push_back(LaneMask * NumElementsInLane + i);
  }
}

void DecodeVPERM2X128Mask(MVT VT, unsigned Imm,
                          SmallVectorImpl<int> &ShuffleMask) {
  unsigned HalfSize = VT.getVectorNumElements() / 2;

  for (unsigned l = 0; l != 2; ++l) {
    unsigned HalfMask = Imm >> (l * 4);
    unsigned HalfBegin = (HalfMask & 0x3) * HalfSize;
    for (unsigned i = HalfBegin, e = HalfBegin + HalfSize; i != e; ++i)
      ShuffleMask.push_back(HalfMask & 8 ? SM_SentinelZero : (int)i);
  }
}

void DecodePSHUFBMask(ArrayRef<uint64_t> RawMask,
                      SmallVectorImpl<int> &ShuffleMask) {
  for (int i = 0, e = RawMask.size(); i < e; ++i) {
    uint64_t M = RawMask[i];
    if (M == (uint64_t)SM_SentinelUndef) {
      ShuffleMask.push_back(M);
      continue;
    }
    // For 256/512-bit vectors the base of the shuffle is the 128-bit
    // subvector we're inside.
    int Base = (i / 16) * 16;
    // If the high bit (7) of the byte is set, the element is zeroed.
    if (M & (1 << 7))
      ShuffleMask.push_back(SM_SentinelZero);
    else {
      // Only the least significant 4 bits of the byte are used.
      int Index = Base + (M & 0xf);
      ShuffleMask.push_back(Index);
    }
  }
}

void DecodeBLENDMask(MVT VT, unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  int ElementBits = VT.getScalarSizeInBits();
  int NumElements = VT.getVectorNumElements();
  for (int i = 0; i < NumElements; ++i) {
    // If there are more than 8 elements in the vector, then any immediate blend
    // mask applies to each 128-bit lane. There can never be more than
    // 8 elements in a 128-bit lane with an immediate blend.
    int Bit = NumElements > 8 ? i % (128 / ElementBits) : i;
    assert(Bit < 8 &&
           "Immediate blends only operate over 8 elements at a time!");
    ShuffleMask.push_back(((Imm >> Bit) & 1) ? NumElements + i : i);
  }
}

void DecodeVPPERMMask(ArrayRef<uint64_t> RawMask,
                      SmallVectorImpl<int> &ShuffleMask) {
  assert(RawMask.size() == 16 && "Illegal VPPERM shuffle mask size");

  // VPPERM Operation
  // Bits[4:0] - Byte Index (0 - 31)
  // Bits[7:5] - Permute Operation
  //
  // Permute Operation:
  // 0 - Source byte (no logical operation).
  // 1 - Invert source byte.
  // 2 - Bit reverse of source byte.
  // 3 - Bit reverse of inverted source byte.
  // 4 - 00h (zero - fill).
  // 5 - FFh (ones - fill).
  // 6 - Most significant bit of source byte replicated in all bit positions.
  // 7 - Invert most significant bit of source byte and replicate in all bit positions.
  for (int i = 0, e = RawMask.size(); i < e; ++i) {
    uint64_t M = RawMask[i];
    if (M == (uint64_t)SM_SentinelUndef) {
      ShuffleMask.push_back(M);
      continue;
    }

    uint64_t PermuteOp = (M >> 5) & 0x7;
    if (PermuteOp == 4) {
      ShuffleMask.push_back(SM_SentinelZero);
      continue;
    }
    if (PermuteOp != 0) {
      ShuffleMask.clear();
      return;
    }

    uint64_t Index = M & 0x1F;
    ShuffleMask.push_back((int)Index);
  }
}

/// DecodeVPERMMask - this decodes the shuffle masks for VPERMQ/VPERMPD.
void DecodeVPERMMask(MVT VT, unsigned Imm, SmallVectorImpl<int> &ShuffleMask) {
  assert((VT.is256BitVector() || VT.is512BitVector()) &&
         (VT.getScalarSizeInBits() == 64) && "Unexpected vector value type");
  unsigned NumElts = VT.getVectorNumElements();
  for (unsigned l = 0; l != NumElts; l += 4)
    for (unsigned i = 0; i != 4; ++i)
      ShuffleMask.push_back(l + ((Imm >> (2 * i)) & 3));
}

void DecodeZeroExtendMask(MVT SrcScalarVT, MVT DstVT, SmallVectorImpl<int> &Mask) {
  unsigned NumDstElts = DstVT.getVectorNumElements();
  unsigned SrcScalarBits = SrcScalarVT.getSizeInBits();
  unsigned DstScalarBits = DstVT.getScalarSizeInBits();
  unsigned Scale = DstScalarBits / SrcScalarBits;
  assert(SrcScalarBits < DstScalarBits &&
         "Expected zero extension mask to increase scalar size");

  for (unsigned i = 0; i != NumDstElts; i++) {
    Mask.push_back(i);
    for (unsigned j = 1; j != Scale; j++)
      Mask.push_back(SM_SentinelZero);
  }
}

void DecodeZeroMoveLowMask(MVT VT, SmallVectorImpl<int> &ShuffleMask) {
  unsigned NumElts = VT.getVectorNumElements();
  ShuffleMask.push_back(0);
  for (unsigned i = 1; i < NumElts; i++)
    ShuffleMask.push_back(SM_SentinelZero);
}

void DecodeScalarMoveMask(MVT VT, bool IsLoad, SmallVectorImpl<int> &Mask) {
  // First element comes from the first element of second source.
  // Remaining elements: Load zero extends / Move copies from first source.
  unsigned NumElts = VT.getVectorNumElements();
  Mask.push_back(NumElts);
  for (unsigned i = 1; i < NumElts; i++)
    Mask.push_back(IsLoad ? static_cast<int>(SM_SentinelZero) : i);
}

void DecodeEXTRQIMask(MVT VT, int Len, int Idx,
                      SmallVectorImpl<int> &ShuffleMask) {
  assert(VT.is128BitVector() && "Expected 128-bit vector");
  unsigned NumElts = VT.getVectorNumElements();
  unsigned EltSize = VT.getScalarSizeInBits();
  unsigned HalfElts = NumElts / 2;

  // Only the bottom 6 bits are valid for each immediate.
  Len &= 0x3F;
  Idx &= 0x3F;

  // We can only decode this bit extraction instruction as a shuffle if both the
  // length and index work with whole elements.
  if (0 != (Len % EltSize) || 0 != (Idx % EltSize))
    return;

  // A length of zero is equivalent to a bit length of 64.
  if (Len == 0)
    Len = 64;

  // If the length + index exceeds the bottom 64 bits the result is undefined.
  if ((Len + Idx) > 64) {
    ShuffleMask.append(NumElts, SM_SentinelUndef);
    return;
  }

  // Convert index and index to work with elements.
  Len /= EltSize;
  Idx /= EltSize;

  // EXTRQ: Extract Len elements starting from Idx. Zero pad the remaining
  // elements of the lower 64-bits. The upper 64-bits are undefined.
  for (int i = 0; i != Len; ++i)
    ShuffleMask.push_back(i + Idx);
  for (int i = Len; i != (int)HalfElts; ++i)
    ShuffleMask.push_back(SM_SentinelZero);
  for (int i = HalfElts; i != (int)NumElts; ++i)
    ShuffleMask.push_back(SM_SentinelUndef);
}

void DecodeINSERTQIMask(MVT VT, int Len, int Idx,
                        SmallVectorImpl<int> &ShuffleMask) {
  assert(VT.is128BitVector() && "Expected 128-bit vector");
  unsigned NumElts = VT.getVectorNumElements();
  unsigned EltSize = VT.getScalarSizeInBits();
  unsigned HalfElts = NumElts / 2;

  // Only the bottom 6 bits are valid for each immediate.
  Len &= 0x3F;
  Idx &= 0x3F;

  // We can only decode this bit insertion instruction as a shuffle if both the
  // length and index work with whole elements.
  if (0 != (Len % EltSize) || 0 != (Idx % EltSize))
    return;

  // A length of zero is equivalent to a bit length of 64.
  if (Len == 0)
    Len = 64;

  // If the length + index exceeds the bottom 64 bits the result is undefined.
  if ((Len + Idx) > 64) {
    ShuffleMask.append(NumElts, SM_SentinelUndef);
    return;
  }

  // Convert index and index to work with elements.
  Len /= EltSize;
  Idx /= EltSize;

  // INSERTQ: Extract lowest Len elements from lower half of second source and
  // insert over first source starting at Idx element. The upper 64-bits are
  // undefined.
  for (int i = 0; i != Idx; ++i)
    ShuffleMask.push_back(i);
  for (int i = 0; i != Len; ++i)
    ShuffleMask.push_back(i + NumElts);
  for (int i = Idx + Len; i != (int)HalfElts; ++i)
    ShuffleMask.push_back(i);
  for (int i = HalfElts; i != (int)NumElts; ++i)
    ShuffleMask.push_back(SM_SentinelUndef);
}

void DecodeVPERMILPMask(MVT VT, ArrayRef<uint64_t> RawMask,
                        SmallVectorImpl<int> &ShuffleMask) {
  unsigned VecSize = VT.getSizeInBits();
  unsigned EltSize = VT.getScalarSizeInBits();
  unsigned NumLanes = VecSize / 128;
  unsigned NumEltsPerLane = VT.getVectorNumElements() / NumLanes;
  assert((VecSize == 128 || VecSize == 256 || VecSize == 512) &&
         "Unexpected vector size");
  assert((EltSize == 32 || EltSize == 64) && "Unexpected element size");

  for (unsigned i = 0, e = RawMask.size(); i < e; ++i) {
    uint64_t M = RawMask[i];
    M = (EltSize == 64 ? ((M >> 1) & 0x1) : (M & 0x3));
    unsigned LaneOffset = i & ~(NumEltsPerLane - 1);
    ShuffleMask.push_back((int)(LaneOffset + M));
  }
}

void DecodeVPERMIL2PMask(MVT VT, unsigned M2Z, ArrayRef<uint64_t> RawMask,
                         SmallVectorImpl<int> &ShuffleMask) {
  unsigned VecSize = VT.getSizeInBits();
  unsigned EltSize = VT.getScalarSizeInBits();
  unsigned NumLanes = VecSize / 128;
  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumEltsPerLane = NumElts / NumLanes;
  assert((VecSize == 128 || VecSize == 256) && "Unexpected vector size");
  assert((EltSize == 32 || EltSize == 64) && "Unexpected element size");
  assert((NumElts == RawMask.size()) && "Unexpected mask size");

  for (unsigned i = 0, e = RawMask.size(); i < e; ++i) {
    // VPERMIL2 Operation.
    // Bits[3] - Match Bit.
    // Bits[2:1] - (Per Lane) PD Shuffle Mask.
    // Bits[2:0] - (Per Lane) PS Shuffle Mask.
    uint64_t Selector = RawMask[i];
    unsigned MatchBit = (Selector >> 3) & 0x1;

    // M2Z[0:1]     MatchBit
    //   0Xb           X        Source selected by Selector index.
    //   10b           0        Source selected by Selector index.
    //   10b           1        Zero.
    //   11b           0        Zero.
    //   11b           1        Source selected by Selector index.
    if ((M2Z & 0x2) != 0 && MatchBit != (M2Z & 0x1)) {
      ShuffleMask.push_back(SM_SentinelZero);
      continue;
    }

    int Index = i & ~(NumEltsPerLane - 1);
    if (EltSize == 64)
      Index += (Selector >> 1) & 0x1;
    else
      Index += Selector & 0x3;

    int Src = (Selector >> 2) & 0x1;
    Index += Src * NumElts;
    ShuffleMask.push_back(Index);
  }
}

void DecodeVPERMVMask(ArrayRef<uint64_t> RawMask,
                      SmallVectorImpl<int> &ShuffleMask) {
  uint64_t EltMaskSize = RawMask.size() - 1;
  for (auto M : RawMask) {
    M &= EltMaskSize;
    ShuffleMask.push_back((int)M);
  }
}

void DecodeVPERMV3Mask(ArrayRef<uint64_t> RawMask,
                      SmallVectorImpl<int> &ShuffleMask) {
  uint64_t EltMaskSize = (RawMask.size() * 2) - 1;
  for (auto M : RawMask) {
    M &= EltMaskSize;
    ShuffleMask.push_back((int)M);
  }
}

} // llvm namespace
