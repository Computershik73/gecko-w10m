/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implement shared vtbl methods. */

#include "xptcprivate.h"

/*
 * Windows on 32-bit ARM: AAPCS with the hard-float (VFP) variant, which is
 * what clang reports through __ARM_PCS_VFP for thumbv7-windows-msvc.
 *
 * The corresponding assembly (xptcstubs_asm_arm.asm) saves the argument
 * registers before calling here:
 *
 *   gprData  -> r1, r2, r3          (r0 holds 'self' and is passed separately)
 *   fprData  -> d0 .. d7            (also addressable as s0 .. s15)
 *   args     -> the caller's stack arguments, i.e. everything that did not
 *               fit in the registers above
 *
 * Argument allocation follows the "Procedure Call Standard for the ARM
 * Architecture":
 *
 *   - Integer and pointer arguments take the next free core register (NCRN),
 *     starting at r1 because r0 is 'this'. A 64-bit argument needs an
 *     even-numbered register pair, so r1 may be skipped. Once the core
 *     registers are exhausted every later integer argument goes on the stack;
 *     the core registers are never revisited.
 *   - Stack arguments are word aligned, and 8-byte aligned for 64-bit types.
 *   - Floating point arguments take VFP registers with back-filling: a float
 *     may land in a single-precision slot that an earlier double left empty.
 *     Once a floating point argument goes to the stack, no later one may use a
 *     VFP register.
 */

// Number of single-precision VFP slots usable for arguments (s0 - s15).
#define VFP_SINGLE_COUNT 16
// First core register number past the argument registers.
#define CORE_REG_END 4

namespace {

// Tracks the VFP argument slots, in units of single-precision registers.
struct VfpAllocator {
  // Next free single-precision slot.
  uint32_t nextSingle = 0;
  // Next free double-precision slot, expressed in single-precision units so
  // that it can be compared with nextSingle directly. Always even.
  uint32_t nextDouble = 0;
  // Set once an argument had to go to the stack.
  bool exhausted = false;

  // Returns the index of the single-precision slot to read, or -1 when the
  // argument belongs on the stack.
  int32_t takeSingle() {
    if (exhausted || nextSingle >= VFP_SINGLE_COUNT) {
      exhausted = true;
      return -1;
    }
    int32_t slot = int32_t(nextSingle);
    nextSingle++;
    if (nextSingle < nextDouble) {
      // We just filled a hole left by an earlier double; the next single
      // continues after that double.
      nextSingle = nextDouble;
    } else if (nextSingle > nextDouble) {
      nextDouble = nextSingle + (nextSingle & 1);
    }
    return slot;
  }

  // Returns the index of the double-precision slot to read (in units of
  // doubles), or -1 when the argument belongs on the stack.
  int32_t takeDouble() {
    if (exhausted || nextDouble + 2 > VFP_SINGLE_COUNT) {
      exhausted = true;
      return -1;
    }
    int32_t slot = int32_t(nextDouble / 2);
    nextDouble += 2;
    if (nextSingle < nextDouble - 2) {
      // A hole is left behind for a later float to back-fill; nextSingle stays
      // where it is.
    } else {
      nextSingle = nextDouble;
    }
    return slot;
  }
};

// Tracks the core registers and the stack.
struct CoreAllocator {
  uint32_t* gprData;  // r1, r2, r3
  uint32_t* stack;    // next stacked argument
  uint32_t next = 1;  // next core register number; r0 is 'this'

  explicit CoreAllocator(uint32_t* aGprData, uint32_t* aStack)
      : gprData(aGprData), stack(aStack) {}

  uint32_t* takeWord() {
    if (next < CORE_REG_END) {
      return &gprData[next++ - 1];
    }
    next = CORE_REG_END;
    return stack++;
  }

  uint32_t* takeDoubleWord() {
    // 64-bit values need an even-numbered core register pair.
    if (next & 1) {
      next++;
    }
    if (next + 1 < CORE_REG_END) {
      uint32_t* p = &gprData[next - 1];
      next += 2;
      return p;
    }
    next = CORE_REG_END;
    // ... and 8-byte alignment on the stack.
    if (uintptr_t(stack) & 4) {
      stack++;
    }
    uint32_t* p = stack;
    stack += 2;
    return p;
  }
};

}  // namespace

extern "C" nsresult PrepareAndDispatch(nsXPTCStubBase* self,
                                       uint32_t methodIndex, uint32_t* args,
                                       uint32_t* gprData, double* fprData) {
  nsXPTCMiniVariant paramBuffer[PARAM_BUFFER_COUNT];
  const nsXPTMethodInfo* info;

  NS_ASSERTION(self, "no self");

  self->mEntry->GetMethodInfo(uint16_t(methodIndex), &info);
  NS_ASSERTION(info, "no method info");

  uint32_t paramCount = info->ParamCount();
  const uint8_t indexOfJSContext = info->IndexOfJSContext();

  CoreAllocator core(gprData, args);
  VfpAllocator vfp;
  const float* fprSingles = reinterpret_cast<const float*>(fprData);

  for (uint32_t i = 0; i < paramCount; i++) {
    const nsXPTParamInfo& param = info->Param(i);
    const nsXPTType& type = param.GetType();
    nsXPTCMiniVariant* dp = &paramBuffer[i];

    if (i == indexOfJSContext) {
      // The JSContext argument is passed like any other pointer, but is not
      // part of the variant list; consume its slot and move on.
      core.takeWord();
    }

    if (param.IsOut() || !type.IsArithmetic()) {
      dp->val.p = *reinterpret_cast<void**>(core.takeWord());
      continue;
    }

    switch (type) {
      case nsXPTType::T_I8:
        dp->val.i8 = int8_t(*core.takeWord());
        break;
      case nsXPTType::T_I16:
        dp->val.i16 = int16_t(*core.takeWord());
        break;
      case nsXPTType::T_I32:
        dp->val.i32 = int32_t(*core.takeWord());
        break;
      case nsXPTType::T_I64:
        dp->val.i64 = *reinterpret_cast<int64_t*>(core.takeDoubleWord());
        break;
      case nsXPTType::T_U8:
        dp->val.u8 = uint8_t(*core.takeWord());
        break;
      case nsXPTType::T_U16:
        dp->val.u16 = uint16_t(*core.takeWord());
        break;
      case nsXPTType::T_U32:
        dp->val.u32 = *core.takeWord();
        break;
      case nsXPTType::T_U64:
        dp->val.u64 = *reinterpret_cast<uint64_t*>(core.takeDoubleWord());
        break;
      case nsXPTType::T_FLOAT: {
        int32_t slot = vfp.takeSingle();
        if (slot >= 0) {
          dp->val.f = fprSingles[slot];
        } else {
          dp->val.f = *reinterpret_cast<float*>(core.stack++);
        }
        break;
      }
      case nsXPTType::T_DOUBLE: {
        int32_t slot = vfp.takeDouble();
        if (slot >= 0) {
          dp->val.d = fprData[slot];
        } else {
          if (uintptr_t(core.stack) & 4) {
            core.stack++;
          }
          dp->val.d = *reinterpret_cast<double*>(core.stack);
          core.stack += 2;
        }
        break;
      }
      case nsXPTType::T_BOOL:
        dp->val.b = bool(uint8_t(*core.takeWord()));
        break;
      case nsXPTType::T_CHAR:
        dp->val.c = char(*core.takeWord());
        break;
      case nsXPTType::T_WCHAR:
        dp->val.wc = char16_t(*core.takeWord());
        break;
      default:
        NS_ERROR("bad type");
        break;
    }
  }

  nsresult result =
      self->mOuter->CallMethod(uint16_t(methodIndex), info, paramBuffer);

  return result;
}

// The StubN entry points live in xptcstubs_asm_arm.asm; only the sentinels are
// generated here.
#define STUB_ENTRY(n) /* defined in the assembly file */

#define SENTINEL_ENTRY(n)                        \
  nsresult nsXPTCStubBase::Sentinel##n() {       \
    NS_ERROR("nsXPTCStubBase::Sentinel called"); \
    return NS_ERROR_NOT_IMPLEMENTED;             \
  }

#include "xptcstubsdef.inc"
