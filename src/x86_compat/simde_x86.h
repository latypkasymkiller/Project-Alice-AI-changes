// simde_x86.h (Android/ARM)
//
// Os headers desta pasta (immintrin.h, x86intrin.h, ...) so entram no build
// Android. Eles substituem os headers de intrinsecas x86 do compilador e
// redirecionam tudo para o SIMDe, que implementa _mm_* / __m128 etc. com NEON.
// Com SIMDE_ENABLE_NATIVE_ALIASES os nomes originais (_mm_add_ps, __m128i...)
// continuam funcionando, entao o DataContainer e o jogo compilam sem mudanca.

#pragma once

#ifndef SIMDE_ENABLE_NATIVE_ALIASES
#define SIMDE_ENABLE_NATIVE_ALIASES
#endif

#include <simde/x86/avx2.h>
#include <simde/x86/sse4.2.h>
#include <simde/x86/fma.h>
#include <simde/x86/clmul.h>

// __lzcnt/_tzcnt_u32 etc. (LZCNT/BMI) nao fazem parte do SIMDe
#include <stdint.h>
static inline uint32_t __lzcnt(uint32_t v) { return v == 0 ? 32u : (uint32_t)__builtin_clz(v); }
static inline uint64_t __lzcnt64(uint64_t v) { return v == 0 ? 64u : (uint64_t)__builtin_clzll(v); }
static inline uint32_t _tzcnt_u32(uint32_t v) { return v == 0 ? 32u : (uint32_t)__builtin_ctz(v); }
static inline uint64_t _tzcnt_u64(uint64_t v) { return v == 0 ? 64u : (uint64_t)__builtin_ctzll(v); }
static inline uint32_t _lzcnt_u32(uint32_t v) { return __lzcnt(v); }
static inline uint64_t _lzcnt_u64(uint64_t v) { return __lzcnt64(v); }
