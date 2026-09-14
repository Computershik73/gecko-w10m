/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Platform specific code to invoke XPCOM methods on native objects.
 *
 * Windows on 32-bit ARM. The calling convention is AAPCS with the hard-float
 * (VFP) variant -- clang reports it as __ARM_PCS_VFP for thumbv7-windows-msvc
 * -- so this is the same code as the __ARM_PCS_VFP half of
 * xpcom/reflect/xptcall/md/unix/xptcinvoke_arm.cpp, which that file guards
 * behind a Linux/iOS-only #error. Keep the two in sync.
 */

#include "xptcprivate.h"

#include "mozilla/Compiler.h"

#if !defined(_M_ARM) || !defined(_WIN32)
#  error "This code is for 32-bit ARM Windows only."
#endif

#if !defined(__ARM_PCS_VFP)
#  error "Windows on ARM32 uses the hard-float AAPCS variant."
#endif


/* "Procedure Call Standard for the ARM Architecture" document, sections
 * "5.5 Parameter Passing" and "6.1.2 Procedure Calling" contain all the
 * needed information.
 *
 * http://infocenter.arm.com/help/topic/com.arm.doc.ihi0042d/IHI0042D_aapcs.pdf
 */

#if defined(__thumb__) && !defined(__thumb2__)
#error "Thumb1 is not supported"
#endif

#ifndef __ARMEL__
#error "Only little endian compatibility was tested"
#endif

/*
 * Allocation of integer function arguments initially to registers r1-r3
 * and then to stack. Handling of 'this' argument which goes to r0 registers
 * is handled separately and does not belong to these two inline functions.
 *
 * The doubleword arguments are allocated to even:odd
 * register pairs or get aligned at 8-byte boundary on stack. The "holes"
 * which may appear as a result of this realignment remain unused.
 *
 * 'ireg_args'  - pointer to the current position in the buffer,
 *                corresponding to the register arguments
 * 'stack_args' - pointer to the current position in the buffer,
 *                corresponding to the arguments on stack
 * 'end'        - pointer to the end of the registers argument
 *                buffer (it is guaranteed to be 8-bytes aligned)
 */

static inline void copy_word(uint32_t* &ireg_args,
                             uint32_t* &stack_args,
                             uint32_t* end,
                             uint32_t  data)
{
  if (ireg_args < end) {
    *ireg_args = data;
    ireg_args++;
  } else {
    *stack_args = data;
    stack_args++;
  }
}

static inline void copy_dword(uint32_t* &ireg_args,
                              uint32_t* &stack_args,
                              uint32_t* end,
                              uint64_t  data)
{
  if (ireg_args + 1 < end) {
    if ((uint32_t)ireg_args & 4) {
      ireg_args++;
    }
    *(uint64_t *)ireg_args = data;
    ireg_args += 2;
  } else {
    ireg_args = end;
    if ((uint32_t)stack_args & 4) {
      stack_args++;
    }
    *(uint64_t *)stack_args = data;
    stack_args += 2;
  }
}

/*
 * Allocation of floating point arguments to VFP registers (s0-s15, d0-d7).
 *
 * Unlike integer registers allocation, "back-filling" needs to be
 * supported. For example, the third floating point argument in the
 * following function is going to be allocated to s1 register, back-filling
 * the "hole":
 *    void f(float s0, double d1, float s1)
 *
 * Refer to the "Procedure Call Standard for the ARM Architecture" document
 * for more details.
 *
 * 'vfp_s_args' - pointer to the current position in the buffer with
 *                the next unallocated single precision register
 * 'vfp_d_args' - pointer to the current position in the buffer with
 *                the next unallocated double precision register,
 *                it has the same value as 'vfp_s_args' when back-filling
 *                is not used
 * 'end'        - pointer to the end of the vfp registers argument
 *                buffer (it is guaranteed to be 8-bytes aligned)
 *
 * Mozilla bugtracker has a test program attached which be used for
 * experimenting with VFP registers allocation code and testing its
 * correctness:
 * https://bugzilla.mozilla.org/show_bug.cgi?id=601914#c19
 */

static inline bool copy_vfp_single(float* &vfp_s_args, double* &vfp_d_args,
                                   float* end, float data)
{
  if (vfp_s_args >= end)
    return false;

  *vfp_s_args = data;
  vfp_s_args++;
  if (vfp_s_args < (float *)vfp_d_args) {
    // It was the case of back-filling, now the next free single precision
    // register should overlap with the next free double precision register
    vfp_s_args = (float *)vfp_d_args;
  } else if (vfp_s_args > (float *)vfp_d_args) {
    // also update the pointer to the next free double precision register
    vfp_d_args++;
  }
  return true;
}

static inline bool copy_vfp_double(float* &vfp_s_args, double* &vfp_d_args,
                                   float* end, double data)
{
  if (vfp_d_args >= (double *)end) {
    // The back-filling continues only so long as no VFP CPRC has been
    // allocated to a slot on the stack. Basically no VFP registers can
    // be allocated after this point.
    vfp_s_args = end;
    return false;
  }

  if (vfp_s_args == (float *)vfp_d_args) {
    // also update the pointer to the next free single precision register
    vfp_s_args += 2;
  }
  *vfp_d_args = data;
  vfp_d_args++;
  return true;
}

static void
invoke_copy_to_stack(uint32_t* stk, uint32_t *end,
                     uint32_t paramCount, nsXPTCVariant* s)
{
  uint32_t *ireg_args  = end - 3;
  float    *vfp_s_args = (float *)end;
  double   *vfp_d_args = (double *)end;
  float    *vfp_end    = vfp_s_args + 16;

  for (uint32_t i = 0; i < paramCount; i++, s++) {
    if (s->IsIndirect()) {
      copy_word(ireg_args, stk, end, (uint32_t)&s->val);
      continue;
    }
    // According to the ARM EABI, integral types that are smaller than a word
    // are to be sign/zero-extended to a full word and treated as 4-byte values
    switch (s->type)
    {
      case nsXPTType::T_FLOAT:
        if (!copy_vfp_single(vfp_s_args, vfp_d_args, vfp_end, s->val.f)) {
          copy_word(end, stk, end, reinterpret_cast<uint32_t&>(s->val.f));
        }
        break;
      case nsXPTType::T_DOUBLE:
        if (!copy_vfp_double(vfp_s_args, vfp_d_args, vfp_end, s->val.d)) {
          copy_dword(end, stk, end, reinterpret_cast<uint64_t&>(s->val.d));
        }
        break;
      case nsXPTType::T_I8:  copy_word(ireg_args, stk, end, s->val.i8);   break;
      case nsXPTType::T_I16: copy_word(ireg_args, stk, end, s->val.i16);  break;
      case nsXPTType::T_I32: copy_word(ireg_args, stk, end, s->val.i32);  break;
      case nsXPTType::T_I64: copy_dword(ireg_args, stk, end, s->val.i64); break;
      case nsXPTType::T_U8:  copy_word(ireg_args, stk, end, s->val.u8);   break;
      case nsXPTType::T_U16: copy_word(ireg_args, stk, end, s->val.u16);  break;
      case nsXPTType::T_U32: copy_word(ireg_args, stk, end, s->val.u32);  break;
      case nsXPTType::T_U64: copy_dword(ireg_args, stk, end, s->val.u64); break;
      case nsXPTType::T_BOOL: copy_word(ireg_args, stk, end, s->val.b);   break;
      case nsXPTType::T_CHAR: copy_word(ireg_args, stk, end, s->val.c);   break;
      case nsXPTType::T_WCHAR: copy_word(ireg_args, stk, end, s->val.wc); break;
      default:
        // all the others are plain pointer types
        copy_word(ireg_args, stk, end, reinterpret_cast<uint32_t>(s->val.p));
        break;
    }
  }
}

typedef uint32_t (*vtable_func)(nsISupports *, uint32_t, uint32_t, uint32_t);

EXPORT_XPCOM_API(nsresult)
NS_InvokeByIndex(nsISupports* that, uint32_t methodIndex,
                   uint32_t paramCount, nsXPTCVariant* params)
{
  vtable_func *vtable = *reinterpret_cast<vtable_func **>(that);
  vtable_func func = vtable[methodIndex];
  // 'register uint32_t result asm("r0")' could be used here, but it does not
  //  seem to be reliable in all cases: http://gcc.gnu.org/PR46164
  nsresult result;
  asm (
    "mov    r3, sp\n"
    "mov    %[stack_space_size], %[param_count_plus_2], lsl #3\n"
    "tst    r3, #4\n" /* check stack alignment */

    "add    %[stack_space_size], #(4 * 16)\n" /* space for VFP registers */
    "mov    r3, %[params]\n"

    "it     ne\n"
    "addne  %[stack_space_size], %[stack_space_size], #4\n"
    "sub    r0, sp, %[stack_space_size]\n" /* allocate space on stack */

    "sub    r2, %[param_count_plus_2], #2\n"
    "mov    sp, r0\n"

    "add    r1, r0, %[param_count_plus_2], lsl #3\n"
    "blx    %[invoke_copy_to_stack]\n"

    "add    ip, sp, %[param_count_plus_2], lsl #3\n"
    "mov    r0, %[that]\n"
    "ldmdb  ip, {r1, r2, r3}\n"
    "vldm   ip, {d0, d1, d2, d3, d4, d5, d6, d7}\n"
    "blx    %[func]\n"

    "add    sp, sp, %[stack_space_size]\n" /* cleanup stack */
    "mov    %[stack_space_size], r0\n" /* it's actually 'result' variable */
    : [stack_space_size]     "=&r" (result)
    : [func]                 "r"   (func),
      [that]                 "r"   (that),
      [params]               "r"   (params),
      [param_count_plus_2]   "r"   (paramCount + 2),
      [invoke_copy_to_stack] "r"   (invoke_copy_to_stack)
    : "cc", "memory",
      // Mark all the scratch registers as clobbered because they may be
      // modified by the functions, called from this inline assembly block
      "r0", "r1", "r2", "r3", "ip", "lr",
      "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7",
      // Also unconditionally mark d16-d31 registers as clobbered even though
      // they actually don't exist in vfpv2 and vfpv3-d16 variants. There is
      // no way to identify VFP variant using preprocessor at the momemnt
      // (see http://gcc.gnu.org/PR46128 for more details), but fortunately
      // current versions of gcc do not seem to complain about these registers
      // even when this code is compiled with '-mfpu=vfpv3-d16' option.
      // If gcc becomes more strict in the future and/or provides a way to
      // identify VFP variant, the following d16-d31 registers list needs
      // to be wrapped into some #ifdef
      "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
      "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"
  );
  return result;
}
