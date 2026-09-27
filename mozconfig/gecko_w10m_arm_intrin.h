/* gecko_w10m_arm_intrin.h
 *
 * Force-included (clang-cl /FI) for the arm-uwp target. VS2022's STL <bit>
 * (__msvc_bit_utils.hpp) calls the MSVC ARM intrinsics _CountLeadingZeros /
 * _CountLeadingZeros64, but for clang the STL routes to clang's <intrin.h>,
 * which does not declare them, and MSVC's own intrin0.inl.h skips its body
 * unless vcruntime.h has already set _VCRT_COMPILER_PREPROCESSOR (not the case
 * at force-include time). So provide these ARM intrinsics ourselves, mapped to
 * clang builtins. clang-cl treats the MSVC intrinsic names as ordinary
 * identifiers, so plain definitions satisfy the STL's use.
 */
#ifndef GECKO_W10M_ARM_INTRIN_H
#define GECKO_W10M_ARM_INTRIN_H

#if defined(_M_ARM) && defined(__clang__)

#  ifdef __cplusplus
extern "C" {
#  endif

static __inline unsigned int _CountLeadingZeros(unsigned long _Val) {
  return _Val ? (unsigned int)__builtin_clz((unsigned int)_Val) : 32u;
}

static __inline unsigned int _CountLeadingZeros64(unsigned __int64 _Val) {
  return _Val ? (unsigned int)__builtin_clzll(_Val) : 64u;
}

#  ifdef __cplusplus
}  // extern "C"
#  endif

#endif  // _M_ARM && __clang__

/* _tzset lives in the UCRT but its declaration is hidden behind the desktop
 * partition guard in <time.h>. The symbol is present in the UWP ucrt lib, so
 * declaring it lets the few callers (ICU's U_TZSET, TestingFunctions) build. */
#if defined(_M_ARM) && defined(__clang__)
#  ifdef __cplusplus
extern "C" {
#  endif
void _tzset(void);
#  ifdef __cplusplus
}
#  endif

// clang-cl on ARM emits an external reference to the deprecated MSVC compiler
// barrier _ReadWriteBarrier rather than lowering it inline; provide it.
static __inline void _ReadWriteBarrier(void) {
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
#endif

#endif  // GECKO_W10M_ARM_INTRIN_H
