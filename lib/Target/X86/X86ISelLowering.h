//===-- X86ISelLowering.h - X86 DAG Lowering Interface ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces that X86 uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86ISELLOWERING_H
#define LLVM_LIB_TARGET_X86_X86ISELLOWERING_H

#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetOptions.h"

namespace llvm {
  class X86Subtarget;
  class X86TargetMachine;

  namespace X86ISD {
    // X86 Specific DAG Nodes
    enum NodeType : unsigned {
      // Start the numbering where the builtin ops leave off.
      FIRST_NUMBER = ISD::BUILTIN_OP_END,

      /// Bit scan forward.
      BSF,
      /// Bit scan reverse.
      BSR,

      /// Double shift instructions. These correspond to
      /// X86::SHLDxx and X86::SHRDxx instructions.
      SHLD,
      SHRD,

      /// Bitwise logical AND of floating point values. This corresponds
      /// to X86::ANDPS or X86::ANDPD.
      FAND,

      /// Bitwise logical OR of floating point values. This corresponds
      /// to X86::ORPS or X86::ORPD.
      FOR,

      /// Bitwise logical XOR of floating point values. This corresponds
      /// to X86::XORPS or X86::XORPD.
      FXOR,

      ///  Bitwise logical ANDNOT of floating point values. This
      /// corresponds to X86::ANDNPS or X86::ANDNPD.
      FANDN,

      /// These operations represent an abstract X86 call
      /// instruction, which includes a bunch of information.  In particular the
      /// operands of these node are:
      ///
      ///     #0 - The incoming token chain
      ///     #1 - The callee
      ///     #2 - The number of arg bytes the caller pushes on the stack.
      ///     #3 - The number of arg bytes the callee pops off the stack.
      ///     #4 - The value to pass in AL/AX/EAX (optional)
      ///     #5 - The value to pass in DL/DX/EDX (optional)
      ///
      /// The result values of these nodes are:
      ///
      ///     #0 - The outgoing token chain
      ///     #1 - The first register result value (optional)
      ///     #2 - The second register result value (optional)
      ///
      CALL,

      /// This operation implements the lowering for readcyclecounter.
      RDTSC_DAG,

      /// X86 Read Time-Stamp Counter and Processor ID.
      RDTSCP_DAG,

      /// X86 Read Performance Monitoring Counters.
      RDPMC_DAG,

      /// X86 compare and logical compare instructions.
      CMP, COMI, UCOMI,

      /// X86 bit-test instructions.
      BT,

      /// X86 SetCC. Operand 0 is condition code, and operand 1 is the EFLAGS
      /// operand, usually produced by a CMP instruction.
      SETCC,

      /// X86 Select
      SELECT, SELECTS,

      // Same as SETCC except it's materialized with a sbb and the value is all
      // one's or all zero's.
      SETCC_CARRY,  // R = carry_bit ? ~0 : 0

      /// X86 FP SETCC, implemented with CMP{cc}SS/CMP{cc}SD.
      /// Operands are two FP values to compare; result is a mask of
      /// 0s or 1s.  Generally DTRT for C/C++ with NaNs.
      FSETCC,

      /// X86 FP SETCC, similar to above, but with output as an i1 mask and
      /// with optional rounding mode.
      FSETCCM, FSETCCM_RND,

      /// X86 conditional moves. Operand 0 and operand 1 are the two values
      /// to select from. Operand 2 is the condition code, and operand 3 is the
      /// flag operand produced by a CMP or TEST instruction. It also writes a
      /// flag result.
      CMOV,

      /// X86 conditional branches. Operand 0 is the chain operand, operand 1
      /// is the block to branch if condition is true, operand 2 is the
      /// condition code, and operand 3 is the flag operand produced by a CMP
      /// or TEST instruction.
      BRCOND,

      /// Return with a flag operand. Operand 0 is the chain operand, operand
      /// 1 is the number of bytes of stack to pop.
      RET_FLAG,

      /// Return from interrupt. Operand 0 is the number of bytes to pop.
      IRET,

      /// Repeat fill, corresponds to X86::REP_STOSx.
      REP_STOS,

      /// Repeat move, corresponds to X86::REP_MOVSx.
      REP_MOVS,

      /// On Darwin, this node represents the result of the popl
      /// at function entry, used for PIC code.
      GlobalBaseReg,

      /// A wrapper node for TargetConstantPool, TargetJumpTable,
      /// TargetExternalSymbol, TargetGlobalAddress, TargetGlobalTLSAddress,
      /// MCSymbol and TargetBlockAddress.
      Wrapper,

      /// Special wrapper used under X86-64 PIC mode for RIP
      /// relative displacements.
      WrapperRIP,

      /// Copies a 64-bit value from the low word of an XMM vector
      /// to an MMX vector.
      MOVDQ2Q,

      /// Copies a 32-bit value from the low word of a MMX
      /// vector to a GPR.
      MMX_MOVD2W,

      /// Copies a GPR into the low 32-bit word of a MMX vector
      /// and zero out the high word.
      MMX_MOVW2D,

      /// Extract an 8-bit value from a vector and zero extend it to
      /// i32, corresponds to X86::PEXTRB.
      PEXTRB,

      /// Extract a 16-bit value from a vector and zero extend it to
      /// i32, corresponds to X86::PEXTRW.
      PEXTRW,

      /// Insert any element of a 4 x float vector into any element
      /// of a destination 4 x floatvector.
      INSERTPS,

      /// Insert the lower 8-bits of a 32-bit value to a vector,
      /// corresponds to X86::PINSRB.
      PINSRB,

      /// Insert the lower 16-bits of a 32-bit value to a vector,
      /// corresponds to X86::PINSRW.
      PINSRW,

      /// Shuffle 16 8-bit values within a vector.
      PSHUFB,

      /// Compute Sum of Absolute Differences.
      PSADBW,
      /// Compute Double Block Packed Sum-Absolute-Differences
      DBPSADBW,

      /// Bitwise Logical AND NOT of Packed FP values.
      ANDNP,

      /// Blend where the selector is an immediate.
      BLENDI,

      /// Dynamic (non-constant condition) vector blend where only the sign bits
      /// of the condition elements are used. This is used to enforce that the
      /// condition mask is not valid for generic VSELECT optimizations.
      SHRUNKBLEND,

      /// Combined add and sub on an FP vector.
      ADDSUB,

      //  FP vector ops with rounding mode.
      FADD_RND, FADDS_RND,
      FSUB_RND, FSUBS_RND,
      FMUL_RND, FMULS_RND,
      FDIV_RND, FDIVS_RND,
      FMAX_RND, FMAXS_RND,
      FMIN_RND, FMINS_RND,
      FSQRT_RND, FSQRTS_RND,

      // FP vector get exponent.
      FGETEXP_RND, FGETEXPS_RND,
      // Extract Normalized Mantissas.
      VGETMANT, VGETMANTS,
      // FP Scale.
      SCALEF,
      SCALEFS,

      // Integer add/sub with unsigned saturation.
      ADDUS,
      SUBUS,

      // Integer add/sub with signed saturation.
      ADDS,
      SUBS,

      // Unsigned Integer average.
      AVG,

      /// Integer horizontal add/sub.
      HADD,
      HSUB,

      /// Floating point horizontal add/sub.
      FHADD,
      FHSUB,

      // Detect Conflicts Within a Vector
      CONFLICT,

      /// Floating point max and min.
      FMAX, FMIN,

      /// Commutative FMIN and FMAX.
      FMAXC, FMINC,

      /// Scalar intrinsic floating point max and min.
      FMAXS, FMINS,

      /// Floating point reciprocal-sqrt and reciprocal approximation.
      /// Note that these typically require refinement
      /// in order to obtain suitable precision.
      FRSQRT, FRCP,
      FRSQRTS, FRCPS,

      // Thread Local Storage.
      TLSADDR,

      // Thread Local Storage. A call to get the start address
      // of the TLS block for the current module.
      TLSBASEADDR,

      // Thread Local Storage.  When calling to an OS provided
      // thunk at the address from an earlier relocation.
      TLSCALL,

      // Exception Handling helpers.
      EH_RETURN,

      // SjLj exception handling setjmp.
      EH_SJLJ_SETJMP,

      // SjLj exception handling longjmp.
      EH_SJLJ_LONGJMP,

      // SjLj exception handling dispatch.
      EH_SJLJ_SETUP_DISPATCH,

      /// Tail call return. See X86TargetLowering::LowerCall for
      /// the list of operands.
      TC_RETURN,

      // Vector move to low scalar and zero higher vector elements.
      VZEXT_MOVL,

      // Vector integer zero-extend.
      VZEXT,
      // Vector integer signed-extend.
      VSEXT,

      // Vector integer truncate.
      VTRUNC,
      // Vector integer truncate with unsigned/signed saturation.
      VTRUNCUS, VTRUNCS,

      // Vector FP extend.
      VFPEXT, VFPEXT_RND, VFPEXTS_RND,

      // Vector FP round.
      VFPROUND, VFPROUND_RND, VFPROUNDS_RND,

      // Convert a vector to mask, set bits base on MSB.
      CVT2MASK,

      // 128-bit vector logical left / right shift
      VSHLDQ, VSRLDQ,

      // Vector shift elements
      VSHL, VSRL, VSRA,

      // Vector variable shift right arithmetic.
      // Unlike ISD::SRA, in case shift count greater then element size
      // use sign bit to fill destination data element.
      VSRAV,

      // Vector shift elements by immediate
      VSHLI, VSRLI, VSRAI,

      // Shifts of mask registers.
      KSHIFTL, KSHIFTR,

      // Bit rotate by immediate
      VROTLI, VROTRI,

      // Vector packed double/float comparison.
      CMPP,

      // Vector integer comparisons.
      PCMPEQ, PCMPGT,
      // Vector integer comparisons, the result is in a mask vector.
      PCMPEQM, PCMPGTM,

      MULTISHIFT,

      /// Vector comparison generating mask bits for fp and
      /// integer signed and unsigned data types.
      CMPM,
      CMPMU,
      // Vector comparison with rounding mode for FP values
      CMPM_RND,

      // Arithmetic operations with FLAGS results.
      ADD, SUB, ADC, SBB, SMUL,
      INC, DEC, OR, XOR, AND,

      // Bit field extract.
      BEXTR,

      // LOW, HI, FLAGS = umul LHS, RHS.
      UMUL,

      // 8-bit SMUL/UMUL - AX, FLAGS = smul8/umul8 AL, RHS.
      SMUL8, UMUL8,

      // 8-bit divrem that zero-extend the high result (AH).
      UDIVREM8_ZEXT_HREG,
      SDIVREM8_SEXT_HREG,

      // X86-specific multiply by immediate.
      MUL_IMM,

      // Vector sign bit extraction.
      MOVMSK,

      // Vector bitwise comparisons.
      PTEST,

      // Vector packed fp sign bitwise comparisons.
      TESTP,

      // Vector "test" in AVX-512, the result is in a mask vector.
      TESTM,
      TESTNM,

      // OR/AND test for masks.
      KORTEST,
      KTEST,

      // Several flavors of instructions with vector shuffle behaviors.
      // Saturated signed/unnsigned packing.
      PACKSS,
      PACKUS,
      // Intra-lane alignr.
      PALIGNR,
      // AVX512 inter-lane alignr.
      VALIGN,
      PSHUFD,
      PSHUFHW,
      PSHUFLW,
      SHUFP,
      //Shuffle Packed Values at 128-bit granularity.
      SHUF128,
      MOVDDUP,
      MOVSHDUP,
      MOVSLDUP,
      MOVLHPS,
      MOVLHPD,
      MOVHLPS,
      MOVLPS,
      MOVLPD,
      MOVSD,
      MOVSS,
      UNPCKL,
      UNPCKH,
      VPERMILPV,
      VPERMILPI,
      VPERMI,
      VPERM2X128,

      // Variable Permute (VPERM).
      // Res = VPERMV MaskV, V0
      VPERMV,

      // 3-op Variable Permute (VPERMT2).
      // Res = VPERMV3 V0, MaskV, V1
      VPERMV3,

      // 3-op Variable Permute overwriting the index (VPERMI2).
      // Res = VPERMIV3 V0, MaskV, V1
      VPERMIV3,

      // Bitwise ternary logic.
      VPTERNLOG,
      // Fix Up Special Packed Float32/64 values.
      VFIXUPIMM,
      VFIXUPIMMS,
      // Range Restriction Calculation For Packed Pairs of Float32/64 values.
      VRANGE,
      // Reduce - Perform Reduction Transformation on scalar\packed FP.
      VREDUCE, VREDUCES,
      // RndScale - Round FP Values To Include A Given Number Of Fraction Bits.
      VRNDSCALE, VRNDSCALES,
      // Tests Types Of a FP Values for packed types.
      VFPCLASS,
      // Tests Types Of a FP Values for scalar types.
      VFPCLASSS,

      // Broadcast scalar to vector.
      VBROADCAST,
      // Broadcast mask to vector.
      VBROADCASTM,
      // Broadcast subvector to vector.
      SUBV_BROADCAST,

      // Extract vector element.
      VEXTRACT,

      /// SSE4A Extraction and Insertion.
      EXTRQI, INSERTQI,

      // XOP variable/immediate rotations.
      VPROT, VPROTI,
      // XOP arithmetic/logical shifts.
      VPSHA, VPSHL,
      // XOP signed/unsigned integer comparisons.
      VPCOM, VPCOMU,
      // XOP packed permute bytes.
      VPPERM,
      // XOP two source permutation.
      VPERMIL2,

      // Vector multiply packed unsigned doubleword integers.
      PMULUDQ,
      // Vector multiply packed signed doubleword integers.
      PMULDQ,
      // Vector Multiply Packed UnsignedIntegers with Round and Scale.
      MULHRS,

      // Multiply and Add Packed Integers.
      VPMADDUBSW, VPMADDWD,
      VPMADD52L, VPMADD52H,

      // FMA nodes.
      FMADD,
      FNMADD,
      FMSUB,
      FNMSUB,
      FMADDSUB,
      FMSUBADD,

      // FMA with rounding mode.
      FMADD_RND,
      FNMADD_RND,
      FMSUB_RND,
      FNMSUB_RND,
      FMADDSUB_RND,
      FMSUBADD_RND,

      // Scalar intrinsic FMA with rounding mode.
      // Two versions, passthru bits on op1 or op3.
      FMADDS1_RND, FMADDS3_RND,
      FNMADDS1_RND, FNMADDS3_RND,
      FMSUBS1_RND, FMSUBS3_RND,
      FNMSUBS1_RND, FNMSUBS3_RND,

      // Compress and expand.
      COMPRESS,
      EXPAND,

      // Convert Unsigned/Integer to Floating-Point Value with rounding mode.
      SINT_TO_FP_RND, UINT_TO_FP_RND,
      SCALAR_SINT_TO_FP_RND, SCALAR_UINT_TO_FP_RND,

      // Vector float/double to signed/unsigned integer.
      CVTP2SI, CVTP2UI, CVTP2SI_RND, CVTP2UI_RND,
      // Scalar float/double to signed/unsigned integer.
      CVTS2SI_RND, CVTS2UI_RND,

      // Vector float/double to signed/unsigned integer with truncation.
      CVTTP2SI, CVTTP2UI, CVTTP2SI_RND, CVTTP2UI_RND,
      // Scalar float/double to signed/unsigned integer with truncation.
      CVTTS2SI_RND, CVTTS2UI_RND,

      // Vector signed/unsigned integer to float/double.
      CVTSI2P, CVTUI2P,

      // Save xmm argument registers to the stack, according to %al. An operator
      // is needed so that this can be expanded with control flow.
      VASTART_SAVE_XMM_REGS,

      // Windows's _chkstk call to do stack probing.
      WIN_ALLOCA,

      // For allocating variable amounts of stack space when using
      // segmented stacks. Check if the current stacklet has enough space, and
      // falls back to heap allocation if not.
      SEG_ALLOCA,

      // Memory barriers.
      MEMBARRIER,
      MFENCE,

      // Store FP status word into i16 register.
      FNSTSW16r,

      // Store contents of %ah into %eflags.
      SAHF,

      // Get a random integer and indicate whether it is valid in CF.
      RDRAND,

      // Get a NIST SP800-90B & C compliant random integer and
      // indicate whether it is valid in CF.
      RDSEED,

      // SSE42 string comparisons.
      PCMPISTRI,
      PCMPESTRI,

      // Test if in transactional execution.
      XTEST,

      // ERI instructions.
      RSQRT28, RSQRT28S, RCP28, RCP28S, EXP2,

      // Conversions between float and half-float.
      CVTPS2PH, CVTPH2PS,

      // LWP insert record.
      LWPINS,

      // Compare and swap.
      LCMPXCHG_DAG = ISD::FIRST_TARGET_MEMORY_OPCODE,
      LCMPXCHG8_DAG,
      LCMPXCHG16_DAG,
      LCMPXCHG8_SAVE_EBX_DAG,
      LCMPXCHG16_SAVE_RBX_DAG,

      /// LOCK-prefixed arithmetic read-modify-write instructions.
      /// EFLAGS, OUTCHAIN = LADD(INCHAIN, PTR, RHS)
      LADD, LSUB, LOR, LXOR, LAND,

      // Load, scalar_to_vector, and zero extend.
      VZEXT_LOAD,

      // Store FP control world into i16 memory.
      FNSTCW16m,

      /// This instruction implements FP_TO_SINT with the
      /// integer destination in memory and a FP reg source.  This corresponds
      /// to the X86::FIST*m instructions and the rounding mode change stuff. It
      /// has two inputs (token chain and address) and two outputs (int value
      /// and token chain).
      FP_TO_INT16_IN_MEM,
      FP_TO_INT32_IN_MEM,
      FP_TO_INT64_IN_MEM,

      /// This instruction implements SINT_TO_FP with the
      /// integer source in memory and FP reg result.  This corresponds to the
      /// X86::FILD*m instructions. It has three inputs (token chain, address,
      /// and source type) and two outputs (FP value and token chain). FILD_FLAG
      /// also produces a flag).
      FILD,
      FILD_FLAG,

      /// This instruction implements an extending load to FP stack slots.
      /// This corresponds to the X86::FLD32m / X86::FLD64m. It takes a chain
      /// operand, ptr to load from, and a ValueType node indicating the type
      /// to load to.
      FLD,

      /// This instruction implements a truncating store to FP stack
      /// slots. This corresponds to the X86::FST32m / X86::FST64m. It takes a
      /// chain operand, value to store, address, and a ValueType to store it
      /// as.
      FST,

      /// This instruction grabs the address of the next argument
      /// from a va_list. (reads and modifies the va_list in memory)
      VAARG_64,

      // Vector truncating store with unsigned/signed saturation
      VTRUNCSTOREUS, VTRUNCSTORES,
      // Vector truncating masked store with unsigned/signed saturation
      VMTRUNCSTOREUS, VMTRUNCSTORES,

      // X86 specific gather
      MGATHER

      // WARNING: Do not add anything in the end unless you want the node to
      // have memop! In fact, starting from FIRST_TARGET_MEMORY_OPCODE all
      // opcodes will be thought as target memory ops!
    };
  } // end namespace X86ISD

  /// Define some predicates that are used for node matching.
  namespace X86 {
    /// Return true if the specified
    /// EXTRACT_SUBVECTOR operand specifies a vector extract that is
    /// suitable for input to VEXTRACTF128, VEXTRACTI128 instructions.
    bool isVEXTRACT128Index(SDNode *N);

    /// Return true if the specified
    /// INSERT_SUBVECTOR operand specifies a subvector insert that is
    /// suitable for input to VINSERTF128, VINSERTI128 instructions.
    bool isVINSERT128Index(SDNode *N);

    /// Return true if the specified
    /// EXTRACT_SUBVECTOR operand specifies a vector extract that is
    /// suitable for input to VEXTRACTF64X4, VEXTRACTI64X4 instructions.
    bool isVEXTRACT256Index(SDNode *N);

    /// Return true if the specified
    /// INSERT_SUBVECTOR operand specifies a subvector insert that is
    /// suitable for input to VINSERTF64X4, VINSERTI64X4 instructions.
    bool isVINSERT256Index(SDNode *N);

    /// Return the appropriate
    /// immediate to extract the specified EXTRACT_SUBVECTOR index
    /// with VEXTRACTF128, VEXTRACTI128 instructions.
    unsigned getExtractVEXTRACT128Immediate(SDNode *N);

    /// Return the appropriate
    /// immediate to insert at the specified INSERT_SUBVECTOR index
    /// with VINSERTF128, VINSERT128 instructions.
    unsigned getInsertVINSERT128Immediate(SDNode *N);

    /// Return the appropriate
    /// immediate to extract the specified EXTRACT_SUBVECTOR index
    /// with VEXTRACTF64X4, VEXTRACTI64x4 instructions.
    unsigned getExtractVEXTRACT256Immediate(SDNode *N);

    /// Return the appropriate
    /// immediate to insert at the specified INSERT_SUBVECTOR index
    /// with VINSERTF64x4, VINSERTI64x4 instructions.
    unsigned getInsertVINSERT256Immediate(SDNode *N);

    /// Returns true if Elt is a constant zero or floating point constant +0.0.
    bool isZeroNode(SDValue Elt);

    /// Returns true of the given offset can be
    /// fit into displacement field of the instruction.
    bool isOffsetSuitableForCodeModel(int64_t Offset, CodeModel::Model M,
                                      bool hasSymbolicDisplacement = true);

    /// Determines whether the callee is required to pop its
    /// own arguments. Callee pop is necessary to support tail calls.
    bool isCalleePop(CallingConv::ID CallingConv,
                     bool is64Bit, bool IsVarArg, bool GuaranteeTCO);

  } // end namespace X86

  //===--------------------------------------------------------------------===//
  //  X86 Implementation of the TargetLowering interface
  class X86TargetLowering final : public TargetLowering {
  public:
    explicit X86TargetLowering(const X86TargetMachine &TM,
                               const X86Subtarget &STI);

    unsigned getJumpTableEncoding() const override;
    bool useSoftFloat() const override;

    void markLibCallAttributes(MachineFunction *MF, unsigned CC,
                               ArgListTy &Args) const override;

    MVT getScalarShiftAmountTy(const DataLayout &, EVT) const override {
      return MVT::i8;
    }

    const MCExpr *
    LowerCustomJumpTableEntry(const MachineJumpTableInfo *MJTI,
                              const MachineBasicBlock *MBB, unsigned uid,
                              MCContext &Ctx) const override;

    /// Returns relocation base for the given PIC jumptable.
    SDValue getPICJumpTableRelocBase(SDValue Table,
                                     SelectionDAG &DAG) const override;
    const MCExpr *
    getPICJumpTableRelocBaseExpr(const MachineFunction *MF,
                                 unsigned JTI, MCContext &Ctx) const override;

    /// Return the desired alignment for ByVal aggregate
    /// function arguments in the caller parameter area. For X86, aggregates
    /// that contains are placed at 16-byte boundaries while the rest are at
    /// 4-byte boundaries.
    unsigned getByValTypeAlignment(Type *Ty,
                                   const DataLayout &DL) const override;

    /// Returns the target specific optimal type for load
    /// and store operations as a result of memset, memcpy, and memmove
    /// lowering. If DstAlign is zero that means it's safe to destination
    /// alignment can satisfy any constraint. Similarly if SrcAlign is zero it
    /// means there isn't a need to check it against alignment requirement,
    /// probably because the source does not need to be loaded. If 'IsMemset' is
    /// true, that means it's expanding a memset. If 'ZeroMemset' is true, that
    /// means it's a memset of zero. 'MemcpyStrSrc' indicates whether the memcpy
    /// source is constant so it does not need to be loaded.
    /// It returns EVT::Other if the type should be determined using generic
    /// target-independent logic.
    EVT getOptimalMemOpType(uint64_t Size, unsigned DstAlign, unsigned SrcAlign,
                            bool IsMemset, bool ZeroMemset, bool MemcpyStrSrc,
                            MachineFunction &MF) const override;

    /// Returns true if it's safe to use load / store of the
    /// specified type to expand memcpy / memset inline. This is mostly true
    /// for all types except for some special cases. For example, on X86
    /// targets without SSE2 f64 load / store are done with fldl / fstpl which
    /// also does type conversion. Note the specified type doesn't have to be
    /// legal as the hook is used before type legalization.
    bool isSafeMemOpType(MVT VT) const override;

    /// Returns true if the target allows unaligned memory accesses of the
    /// specified type. Returns whether it is "fast" in the last argument.
    bool allowsMisalignedMemoryAccesses(EVT VT, unsigned AS, unsigned Align,
                                       bool *Fast) const override;

    /// Provide custom lowering hooks for some operations.
    ///
    SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

    /// Places new result values for the node in Results (their number
    /// and types must exactly match those of the original return values of
    /// the node), or leaves Results empty, which indicates that the node is not
    /// to be custom lowered after all.
    void LowerOperationWrapper(SDNode *N,
                               SmallVectorImpl<SDValue> &Results,
                               SelectionDAG &DAG) const override;

    /// Replace the results of node with an illegal result
    /// type with new values built out of custom code.
    ///
    void ReplaceNodeResults(SDNode *N, SmallVectorImpl<SDValue>&Results,
                            SelectionDAG &DAG) const override;

    SDValue PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const override;

    // Return true if it is profitable to combine a BUILD_VECTOR to a TRUNCATE
    // for given operand and result types.
    // Example of such a combine:
    // v4i32 build_vector((extract_elt V, 0),
    //                    (extract_elt V, 2),
    //                    (extract_elt V, 4),
    //                    (extract_elt V, 6))
    //  -->
    // v4i32 truncate (bitcast V to v4i64)
    bool isDesirableToCombineBuildVectorToTruncate() const override {
      return true;
    }

    /// Return true if the target has native support for
    /// the specified value type and it is 'desirable' to use the type for the
    /// given node type. e.g. On x86 i16 is legal, but undesirable since i16
    /// instruction encodings are longer and some i16 instructions are slow.
    bool isTypeDesirableForOp(unsigned Opc, EVT VT) const override;

    /// Return true if the target has native support for the
    /// specified value type and it is 'desirable' to use the type. e.g. On x86
    /// i16 is legal, but undesirable since i16 instruction encodings are longer
    /// and some i16 instructions are slow.
    bool IsDesirableToPromoteOp(SDValue Op, EVT &PVT) const override;

    MachineBasicBlock *
    EmitInstrWithCustomInserter(MachineInstr &MI,
                                MachineBasicBlock *MBB) const override;

    /// This method returns the name of a target specific DAG node.
    const char *getTargetNodeName(unsigned Opcode) const override;

    bool isCheapToSpeculateCttz() const override;

    bool isCheapToSpeculateCtlz() const override;

    bool isCtlzFast() const override;

    bool hasBitPreservingFPLogic(EVT VT) const override {
      return VT == MVT::f32 || VT == MVT::f64 || VT.isVector();
    }

    bool isMultiStoresCheaperThanBitsMerge(EVT LTy, EVT HTy) const override {
      // If the pair to store is a mixture of float and int values, we will
      // save two bitwise instructions and one float-to-int instruction and
      // increase one store instruction. There is potentially a more
      // significant benefit because it avoids the float->int domain switch
      // for input value. So It is more likely a win.
      if ((LTy.isFloatingPoint() && HTy.isInteger()) ||
          (LTy.isInteger() && HTy.isFloatingPoint()))
        return true;
      // If the pair only contains int values, we will save two bitwise
      // instructions and increase one store instruction (costing one more
      // store buffer). Since the benefit is more blurred so we leave
      // such pair out until we get testcase to prove it is a win.
      return false;
    }

    bool isMaskAndCmp0FoldingBeneficial(const Instruction &AndI) const override;

    bool hasAndNotCompare(SDValue Y) const override;

    bool convertSetCCLogicToBitwiseLogic(EVT VT) const override {
      return VT.isScalarInteger();
    }

    /// Vector-sized comparisons are fast using PCMPEQ + PMOVMSK or PTEST.
    MVT hasFastEqualityCompare(unsigned NumBits) const override;

    /// Return the value type to use for ISD::SETCC.
    EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                           EVT VT) const override;

    /// Determine which of the bits specified in Mask are known to be either
    /// zero or one and return them in the KnownZero/KnownOne bitsets.
    void computeKnownBitsForTargetNode(const SDValue Op,
                                       KnownBits &Known,
                                       const APInt &DemandedElts,
                                       const SelectionDAG &DAG,
                                       unsigned Depth = 0) const override;

    /// Determine the number of bits in the operation that are sign bits.
    unsigned ComputeNumSignBitsForTargetNode(SDValue Op,
                                             const APInt &DemandedElts,
                                             const SelectionDAG &DAG,
                                             unsigned Depth) const override;

    bool isGAPlusOffset(SDNode *N, const GlobalValue* &GA,
                        int64_t &Offset) const override;

    SDValue getReturnAddressFrameIndex(SelectionDAG &DAG) const;

    bool ExpandInlineAsm(CallInst *CI) const override;

    ConstraintType getConstraintType(StringRef Constraint) const override;

    /// Examine constraint string and operand type and determine a weight value.
    /// The operand object must already have been set up with the operand type.
    ConstraintWeight
      getSingleConstraintMatchWeight(AsmOperandInfo &info,
                                     const char *constraint) const override;

    const char *LowerXConstraint(EVT ConstraintVT) const override;

    /// Lower the specified operand into the Ops vector. If it is invalid, don't
    /// add anything to Ops. If hasMemory is true it means one of the asm
    /// constraint of the inline asm instruction being processed is 'm'.
    void LowerAsmOperandForConstraint(SDValue Op,
                                      std::string &Constraint,
                                      std::vector<SDValue> &Ops,
                                      SelectionDAG &DAG) const override;

    unsigned
    getInlineAsmMemConstraint(StringRef ConstraintCode) const override {
      if (ConstraintCode == "i")
        return InlineAsm::Constraint_i;
      else if (ConstraintCode == "o")
        return InlineAsm::Constraint_o;
      else if (ConstraintCode == "v")
        return InlineAsm::Constraint_v;
      else if (ConstraintCode == "X")
        return InlineAsm::Constraint_X;
      return TargetLowering::getInlineAsmMemConstraint(ConstraintCode);
    }

    /// Given a physical register constraint
    /// (e.g. {edx}), return the register number and the register class for the
    /// register.  This should only be used for C_Register constraints.  On
    /// error, this returns a register number of 0.
    std::pair<unsigned, const TargetRegisterClass *>
    getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                 StringRef Constraint, MVT VT) const override;

    /// Return true if the addressing mode represented
    /// by AM is legal for this target, for a load/store of the specified type.
    bool isLegalAddressingMode(const DataLayout &DL, const AddrMode &AM,
                               Type *Ty, unsigned AS) const override;

    /// Return true if the specified immediate is legal
    /// icmp immediate, that is the target has icmp instructions which can
    /// compare a register against the immediate without having to materialize
    /// the immediate into a register.
    bool isLegalICmpImmediate(int64_t Imm) const override;

    /// Return true if the specified immediate is legal
    /// add immediate, that is the target has add instructions which can
    /// add a register and the immediate without having to materialize
    /// the immediate into a register.
    bool isLegalAddImmediate(int64_t Imm) const override;

    /// \brief Return the cost of the scaling factor used in the addressing
    /// mode represented by AM for this target, for a load/store
    /// of the specified type.
    /// If the AM is supported, the return value must be >= 0.
    /// If the AM is not supported, it returns a negative value.
    int getScalingFactorCost(const DataLayout &DL, const AddrMode &AM, Type *Ty,
                             unsigned AS) const override;

    bool isVectorShiftByScalarCheap(Type *Ty) const override;

    /// Return true if it's free to truncate a value of
    /// type Ty1 to type Ty2. e.g. On x86 it's free to truncate a i32 value in
    /// register EAX to i16 by referencing its sub-register AX.
    bool isTruncateFree(Type *Ty1, Type *Ty2) const override;
    bool isTruncateFree(EVT VT1, EVT VT2) const override;

    bool allowTruncateForTailCall(Type *Ty1, Type *Ty2) const override;

    /// Return true if any actual instruction that defines a
    /// value of type Ty1 implicit zero-extends the value to Ty2 in the result
    /// register. This does not necessarily include registers defined in
    /// unknown ways, such as incoming arguments, or copies from unknown
    /// virtual registers. Also, if isTruncateFree(Ty2, Ty1) is true, this
    /// does not necessarily apply to truncate instructions. e.g. on x86-64,
    /// all instructions that define 32-bit values implicit zero-extend the
    /// result out to 64 bits.
    bool isZExtFree(Type *Ty1, Type *Ty2) const override;
    bool isZExtFree(EVT VT1, EVT VT2) const override;
    bool isZExtFree(SDValue Val, EVT VT2) const override;

    /// Return true if folding a vector load into ExtVal (a sign, zero, or any
    /// extend node) is profitable.
    bool isVectorLoadExtDesirable(SDValue) const override;

    /// Return true if an FMA operation is faster than a pair of fmul and fadd
    /// instructions. fmuladd intrinsics will be expanded to FMAs when this
    /// method returns true, otherwise fmuladd is expanded to fmul + fadd.
    bool isFMAFasterThanFMulAndFAdd(EVT VT) const override;

    /// Return true if it's profitable to narrow
    /// operations of type VT1 to VT2. e.g. on x86, it's profitable to narrow
    /// from i32 to i8 but not from i32 to i16.
    bool isNarrowingProfitable(EVT VT1, EVT VT2) const override;

    /// Given an intrinsic, checks if on the target the intrinsic will need to map
    /// to a MemIntrinsicNode (touches memory). If this is the case, it returns
    /// true and stores the intrinsic information into the IntrinsicInfo that was
    /// passed to the function.
    bool getTgtMemIntrinsic(IntrinsicInfo &Info, const CallInst &I,
                            unsigned Intrinsic) const override;

    /// Returns true if the target can instruction select the
    /// specified FP immediate natively. If false, the legalizer will
    /// materialize the FP immediate as a load from a constant pool.
    bool isFPImmLegal(const APFloat &Imm, EVT VT) const override;

    /// Targets can use this to indicate that they only support *some*
    /// VECTOR_SHUFFLE operations, those with specific masks. By default, if a
    /// target supports the VECTOR_SHUFFLE node, all mask values are assumed to
    /// be legal.
    bool isShuffleMaskLegal(const SmallVectorImpl<int> &Mask,
                            EVT VT) const override;

    /// Similar to isShuffleMaskLegal. This is used by Targets can use this to
    /// indicate if there is a suitable VECTOR_SHUFFLE that can be used to
    /// replace a VAND with a constant pool entry.
    bool isVectorClearMaskLegal(const SmallVectorImpl<int> &Mask,
                                EVT VT) const override;

    /// If true, then instruction selection should
    /// seek to shrink the FP constant of the specified type to a smaller type
    /// in order to save space and / or reduce runtime.
    bool ShouldShrinkFPConstant(EVT VT) const override {
      // Don't shrink FP constpool if SSE2 is available since cvtss2sd is more
      // expensive than a straight movsd. On the other hand, it's important to
      // shrink long double fp constant since fldt is very slow.
      return !X86ScalarSSEf64 || VT == MVT::f80;
    }

    /// Return true if we believe it is correct and profitable to reduce the
    /// load node to a smaller type.
    bool shouldReduceLoadWidth(SDNode *Load, ISD::LoadExtType ExtTy,
                               EVT NewVT) const override;

    /// Return true if the specified scalar FP type is computed in an SSE
    /// register, not on the X87 floating point stack.
    bool isScalarFPTypeInSSEReg(EVT VT) const {
      return (VT == MVT::f64 && X86ScalarSSEf64) || // f64 is when SSE2
             (VT == MVT::f32 && X86ScalarSSEf32);   // f32 is when SSE1
    }

    /// \brief Returns true if it is beneficial to convert a load of a constant
    /// to just the constant itself.
    bool shouldConvertConstantLoadToIntImm(const APInt &Imm,
                                           Type *Ty) const override;

    bool convertSelectOfConstantsToMath() const override {
      return true;
    }

    /// Return true if EXTRACT_SUBVECTOR is cheap for this result type
    /// with this index.
    bool isExtractSubvectorCheap(EVT ResVT, unsigned Index) const override;

    /// Intel processors have a unified instruction and data cache
    const char * getClearCacheBuiltinName() const override {
      return nullptr; // nothing to do, move along.
    }

    unsigned getRegisterByName(const char* RegName, EVT VT,
                               SelectionDAG &DAG) const override;

    /// If a physical register, this returns the register that receives the
    /// exception address on entry to an EH pad.
    unsigned
    getExceptionPointerRegister(const Constant *PersonalityFn) const override;

    /// If a physical register, this returns the register that receives the
    /// exception typeid on entry to a landing pad.
    unsigned
    getExceptionSelectorRegister(const Constant *PersonalityFn) const override;

    virtual bool needsFixedCatchObjects() const override;

    /// This method returns a target specific FastISel object,
    /// or null if the target does not support "fast" ISel.
    FastISel *createFastISel(FunctionLoweringInfo &funcInfo,
                             const TargetLibraryInfo *libInfo) const override;

    /// If the target has a standard location for the stack protector cookie,
    /// returns the address of that location. Otherwise, returns nullptr.
    Value *getIRStackGuard(IRBuilder<> &IRB) const override;

    bool useLoadStackGuardNode() const override;
    void insertSSPDeclarations(Module &M) const override;
    Value *getSDagStackGuard(const Module &M) const override;
    Value *getSSPStackGuardCheck(const Module &M) const override;

    /// Return true if the target stores SafeStack pointer at a fixed offset in
    /// some non-standard address space, and populates the address space and
    /// offset as appropriate.
    Value *getSafeStackPointerLocation(IRBuilder<> &IRB) const override;

    SDValue BuildFILD(SDValue Op, EVT SrcVT, SDValue Chain, SDValue StackSlot,
                      SelectionDAG &DAG) const;

    bool isNoopAddrSpaceCast(unsigned SrcAS, unsigned DestAS) const override;

    /// \brief Customize the preferred legalization strategy for certain types.
    LegalizeTypeAction getPreferredVectorAction(EVT VT) const override;

    bool isIntDivCheap(EVT VT, AttributeList Attr) const override;

    bool supportSwiftError() const override;

    StringRef getStackProbeSymbolName(MachineFunction &MF) const override;

    unsigned getMaxSupportedInterleaveFactor() const override { return 4; }

    /// \brief Lower interleaved load(s) into target specific
    /// instructions/intrinsics.
    bool lowerInterleavedLoad(LoadInst *LI,
                              ArrayRef<ShuffleVectorInst *> Shuffles,
                              ArrayRef<unsigned> Indices,
                              unsigned Factor) const override;

    /// \brief Lower interleaved store(s) into target specific
    /// instructions/intrinsics.
    bool lowerInterleavedStore(StoreInst *SI, ShuffleVectorInst *SVI,
                               unsigned Factor) const override;


    void finalizeLowering(MachineFunction &MF) const override;

  protected:
    std::pair<const TargetRegisterClass *, uint8_t>
    findRepresentativeClass(const TargetRegisterInfo *TRI,
                            MVT VT) const override;

  private:
    /// Keep a reference to the X86Subtarget around so that we can
    /// make the right decision when generating code for different targets.
    const X86Subtarget &Subtarget;

    /// Select between SSE or x87 floating point ops.
    /// When SSE is available, use it for f32 operations.
    /// When SSE2 is available, use it for f64 operations.
    bool X86ScalarSSEf32;
    bool X86ScalarSSEf64;

    /// A list of legal FP immediates.
    std::vector<APFloat> LegalFPImmediates;

    /// Indicate that this x86 target can instruction
    /// select the specified FP immediate natively.
    void addLegalFPImmediate(const APFloat& Imm) {
      LegalFPImmediates.push_back(Imm);
    }

    SDValue LowerCallResult(SDValue Chain, SDValue InFlag,
                            CallingConv::ID CallConv, bool isVarArg,
                            const SmallVectorImpl<ISD::InputArg> &Ins,
                            const SDLoc &dl, SelectionDAG &DAG,
                            SmallVectorImpl<SDValue> &InVals,
                            uint32_t *RegMask) const;
    SDValue LowerMemArgument(SDValue Chain, CallingConv::ID CallConv,
                             const SmallVectorImpl<ISD::InputArg> &ArgInfo,
                             const SDLoc &dl, SelectionDAG &DAG,
                             const CCValAssign &VA, MachineFrameInfo &MFI,
                             unsigned i) const;
    SDValue LowerMemOpCallTo(SDValue Chain, SDValue StackPtr, SDValue Arg,
                             const SDLoc &dl, SelectionDAG &DAG,
                             const CCValAssign &VA,
                             ISD::ArgFlagsTy Flags) const;

    // Call lowering helpers.

    /// Check whether the call is eligible for tail call optimization. Targets
    /// that want to do tail call optimization should implement this function.
    bool IsEligibleForTailCallOptimization(SDValue Callee,
                                           CallingConv::ID CalleeCC,
                                           bool isVarArg,
                                           bool isCalleeStructRet,
                                           bool isCallerStructRet,
                                           Type *RetTy,
                                    const SmallVectorImpl<ISD::OutputArg> &Outs,
                                    const SmallVectorImpl<SDValue> &OutVals,
                                    const SmallVectorImpl<ISD::InputArg> &Ins,
                                           SelectionDAG& DAG) const;
    SDValue EmitTailCallLoadRetAddr(SelectionDAG &DAG, SDValue &OutRetAddr,
                                    SDValue Chain, bool IsTailCall,
                                    bool Is64Bit, int FPDiff,
                                    const SDLoc &dl) const;

    unsigned GetAlignedArgumentStackSize(unsigned StackSize,
                                         SelectionDAG &DAG) const;

    unsigned getAddressSpace(void) const;

    std::pair<SDValue,SDValue> FP_TO_INTHelper(SDValue Op, SelectionDAG &DAG,
                                               bool isSigned,
                                               bool isReplace) const;

    SDValue LowerBUILD_VECTOR(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerBUILD_VECTORvXi1(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerVSELECT(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerEXTRACT_VECTOR_ELT(SDValue Op, SelectionDAG &DAG) const;
    SDValue ExtractBitFromMaskVector(SDValue Op, SelectionDAG &DAG) const;
    SDValue InsertBitToMaskVector(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerINSERT_VECTOR_ELT(SDValue Op, SelectionDAG &DAG) const;

    unsigned getGlobalWrapperKind(const GlobalValue *GV = nullptr) const;
    SDValue LowerConstantPool(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerGlobalAddress(const GlobalValue *GV, const SDLoc &dl,
                               int64_t Offset, SelectionDAG &DAG) const;
    SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerGlobalTLSAddress(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerExternalSymbol(SDValue Op, SelectionDAG &DAG) const;

    SDValue LowerSINT_TO_FP(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerUINT_TO_FP(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerUINT_TO_FP_i64(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerUINT_TO_FP_i32(SDValue Op, SelectionDAG &DAG) const;
    SDValue lowerUINT_TO_FP_vec(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerTRUNCATE(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerFP_TO_INT(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerToBT(SDValue And, ISD::CondCode CC, const SDLoc &dl,
                      SelectionDAG &DAG) const;
    SDValue LowerSETCC(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerSETCCCARRY(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerSELECT(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerBRCOND(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerJumpTable(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerDYNAMIC_STACKALLOC(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerVAARG(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerADDROFRETURNADDR(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerFRAME_TO_ARGS_OFFSET(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerEH_RETURN(SDValue Op, SelectionDAG &DAG) const;
    SDValue lowerEH_SJLJ_SETJMP(SDValue Op, SelectionDAG &DAG) const;
    SDValue lowerEH_SJLJ_LONGJMP(SDValue Op, SelectionDAG &DAG) const;
    SDValue lowerEH_SJLJ_SETUP_DISPATCH(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerINIT_TRAMPOLINE(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerFLT_ROUNDS_(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerWin64_i128OP(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerGC_TRANSITION_START(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerGC_TRANSITION_END(SDValue Op, SelectionDAG &DAG) const;

    SDValue
    LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
                         const SmallVectorImpl<ISD::InputArg> &Ins,
                         const SDLoc &dl, SelectionDAG &DAG,
                         SmallVectorImpl<SDValue> &InVals) const override;
    SDValue LowerCall(CallLoweringInfo &CLI,
                      SmallVectorImpl<SDValue> &InVals) const override;

    SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
                        const SmallVectorImpl<ISD::OutputArg> &Outs,
                        const SmallVectorImpl<SDValue> &OutVals,
                        const SDLoc &dl, SelectionDAG &DAG) const override;

    bool supportSplitCSR(MachineFunction *MF) const override {
      return MF->getFunction()->getCallingConv() == CallingConv::CXX_FAST_TLS &&
          MF->getFunction()->hasFnAttribute(Attribute::NoUnwind);
    }
    void initializeSplitCSR(MachineBasicBlock *Entry) const override;
    void insertCopiesSplitCSR(
      MachineBasicBlock *Entry,
      const SmallVectorImpl<MachineBasicBlock *> &Exits) const override;

    bool isUsedByReturnOnly(SDNode *N, SDValue &Chain) const override;

    bool mayBeEmittedAsTailCall(const CallInst *CI) const override;

    EVT getTypeForExtReturn(LLVMContext &Context, EVT VT,
                            ISD::NodeType ExtendKind) const override;

    bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                        bool isVarArg,
                        const SmallVectorImpl<ISD::OutputArg> &Outs,
                        LLVMContext &Context) const override;

    const MCPhysReg *getScratchRegisters(CallingConv::ID CC) const override;

    TargetLoweringBase::AtomicExpansionKind
    shouldExpandAtomicLoadInIR(LoadInst *SI) const override;
    bool shouldExpandAtomicStoreInIR(StoreInst *SI) const override;
    TargetLoweringBase::AtomicExpansionKind
    shouldExpandAtomicRMWInIR(AtomicRMWInst *AI) const override;

    LoadInst *
    lowerIdempotentRMWIntoFencedLoad(AtomicRMWInst *AI) const override;

    bool needsCmpXchgNb(Type *MemType) const;

    void SetupEntryBlockForSjLj(MachineInstr &MI, MachineBasicBlock *MBB,
                                MachineBasicBlock *DispatchBB, int FI) const;

    // Utility function to emit the low-level va_arg code for X86-64.
    MachineBasicBlock *
    EmitVAARG64WithCustomInserter(MachineInstr &MI,
                                  MachineBasicBlock *MBB) const;

    /// Utility function to emit the xmm reg save portion of va_start.
    MachineBasicBlock *
    EmitVAStartSaveXMMRegsWithCustomInserter(MachineInstr &BInstr,
                                             MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredSelect(MachineInstr &I,
                                         MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredAtomicFP(MachineInstr &I,
                                           MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredCatchRet(MachineInstr &MI,
                                           MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredCatchPad(MachineInstr &MI,
                                           MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredSegAlloca(MachineInstr &MI,
                                            MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredTLSAddr(MachineInstr &MI,
                                          MachineBasicBlock *BB) const;

    MachineBasicBlock *EmitLoweredTLSCall(MachineInstr &MI,
                                          MachineBasicBlock *BB) const;

    MachineBasicBlock *emitEHSjLjSetJmp(MachineInstr &MI,
                                        MachineBasicBlock *MBB) const;

    MachineBasicBlock *emitEHSjLjLongJmp(MachineInstr &MI,
                                         MachineBasicBlock *MBB) const;

    MachineBasicBlock *emitFMA3Instr(MachineInstr &MI,
                                     MachineBasicBlock *MBB) const;

    MachineBasicBlock *EmitSjLjDispatchBlock(MachineInstr &MI,
                                             MachineBasicBlock *MBB) const;

    /// Emit nodes that will be selected as "test Op0,Op0", or something
    /// equivalent, for use with the given x86 condition code.
    SDValue EmitTest(SDValue Op0, unsigned X86CC, const SDLoc &dl,
                     SelectionDAG &DAG) const;

    /// Emit nodes that will be selected as "cmp Op0,Op1", or something
    /// equivalent, for use with the given x86 condition code.
    SDValue EmitCmp(SDValue Op0, SDValue Op1, unsigned X86CC, const SDLoc &dl,
                    SelectionDAG &DAG) const;

    /// Convert a comparison if required by the subtarget.
    SDValue ConvertCmpIfNecessary(SDValue Cmp, SelectionDAG &DAG) const;

    /// Check if replacement of SQRT with RSQRT should be disabled.
    bool isFsqrtCheap(SDValue Operand, SelectionDAG &DAG) const override;

    /// Use rsqrt* to speed up sqrt calculations.
    SDValue getSqrtEstimate(SDValue Operand, SelectionDAG &DAG, int Enabled,
                            int &RefinementSteps, bool &UseOneConstNR,
                            bool Reciprocal) const override;

    /// Use rcp* to speed up fdiv calculations.
    SDValue getRecipEstimate(SDValue Operand, SelectionDAG &DAG, int Enabled,
                             int &RefinementSteps) const override;

    /// Reassociate floating point divisions into multiply by reciprocal.
    unsigned combineRepeatedFPDivisors() const override;
  };

  namespace X86 {
    FastISel *createFastISel(FunctionLoweringInfo &funcInfo,
                             const TargetLibraryInfo *libInfo);
  } // end namespace X86

  // Base class for all X86 non-masked store operations.
  class X86StoreSDNode : public MemSDNode {
  public:
    X86StoreSDNode(unsigned Opcode, unsigned Order, const DebugLoc &dl,
                   SDVTList VTs, EVT MemVT,
                   MachineMemOperand *MMO)
      :MemSDNode(Opcode, Order, dl, VTs, MemVT, MMO) {}
    const SDValue &getValue() const { return getOperand(1); }
    const SDValue &getBasePtr() const { return getOperand(2); }

    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::VTRUNCSTORES ||
        N->getOpcode() == X86ISD::VTRUNCSTOREUS;
    }
  };

  // Base class for all X86 masked store operations.
  // The class has the same order of operands as MaskedStoreSDNode for
  // convenience.
  class X86MaskedStoreSDNode : public MemSDNode {
  public:
    X86MaskedStoreSDNode(unsigned Opcode, unsigned Order,
                         const DebugLoc &dl, SDVTList VTs, EVT MemVT,
                         MachineMemOperand *MMO)
      : MemSDNode(Opcode, Order, dl, VTs, MemVT, MMO) {}

    const SDValue &getBasePtr() const { return getOperand(1); }
    const SDValue &getMask()    const { return getOperand(2); }
    const SDValue &getValue()   const { return getOperand(3); }

    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::VMTRUNCSTORES ||
        N->getOpcode() == X86ISD::VMTRUNCSTOREUS;
    }
  };

  // X86 Truncating Store with Signed saturation.
  class TruncSStoreSDNode : public X86StoreSDNode {
  public:
    TruncSStoreSDNode(unsigned Order, const DebugLoc &dl,
                        SDVTList VTs, EVT MemVT, MachineMemOperand *MMO)
      : X86StoreSDNode(X86ISD::VTRUNCSTORES, Order, dl, VTs, MemVT, MMO) {}

    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::VTRUNCSTORES;
    }
  };

  // X86 Truncating Store with Unsigned saturation.
  class TruncUSStoreSDNode : public X86StoreSDNode {
  public:
    TruncUSStoreSDNode(unsigned Order, const DebugLoc &dl,
                      SDVTList VTs, EVT MemVT, MachineMemOperand *MMO)
      : X86StoreSDNode(X86ISD::VTRUNCSTOREUS, Order, dl, VTs, MemVT, MMO) {}

    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::VTRUNCSTOREUS;
    }
  };

  // X86 Truncating Masked Store with Signed saturation.
  class MaskedTruncSStoreSDNode : public X86MaskedStoreSDNode {
  public:
    MaskedTruncSStoreSDNode(unsigned Order,
                         const DebugLoc &dl, SDVTList VTs, EVT MemVT,
                         MachineMemOperand *MMO)
      : X86MaskedStoreSDNode(X86ISD::VMTRUNCSTORES, Order, dl, VTs, MemVT, MMO) {}

    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::VMTRUNCSTORES;
    }
  };

  // X86 Truncating Masked Store with Unsigned saturation.
  class MaskedTruncUSStoreSDNode : public X86MaskedStoreSDNode {
  public:
    MaskedTruncUSStoreSDNode(unsigned Order,
                            const DebugLoc &dl, SDVTList VTs, EVT MemVT,
                            MachineMemOperand *MMO)
      : X86MaskedStoreSDNode(X86ISD::VMTRUNCSTOREUS, Order, dl, VTs, MemVT, MMO) {}

    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::VMTRUNCSTOREUS;
    }
  };

  // X86 specific Gather node.
  class X86MaskedGatherSDNode : public MaskedGatherScatterSDNode {
  public:
    X86MaskedGatherSDNode(unsigned Order,
                          const DebugLoc &dl, SDVTList VTs, EVT MemVT,
                          MachineMemOperand *MMO)
      : MaskedGatherScatterSDNode(X86ISD::MGATHER, Order, dl, VTs, MemVT, MMO)
    {}
    static bool classof(const SDNode *N) {
      return N->getOpcode() == X86ISD::MGATHER;
    }
  };

} // end namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86ISELLOWERING_H
