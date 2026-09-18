/// @file    Simd.hpp
/// @brief   Math の実装が内部で使う SSE4.1 の薄い道具。公開 API の引数・戻り値には出さない。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 命令セットは SSE4.1 固定で FMA は使わない。積和は mul + add の 2 命令にし、スカラー版と加算の順序を揃える。
/// @see Docs/design/math-simd.md
/// @see https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html Intel Intrinsics Guide
#pragma once

#include <smmintrin.h>

namespace fbzz::math::simd {

using Vec = __m128;

/// @note 保存用の型は 16B 整列を約束しないので、常に非整列ロード・ストアを使う。
inline Vec  Load4(const float* p)   { return _mm_loadu_ps(p); }
inline void Store4(float* p, Vec v) { _mm_storeu_ps(p, v); }

/// @brief v の成分 i を 4 レーンへ複製する。
template <int I>
inline Vec SplatLane(Vec v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(I, I, I, I)); }

/// @return a * b + c。FMA ではなく 2 命令で、途中で 1 度丸める。
inline Vec MulAdd(Vec a, Vec b, Vec c) { return _mm_add_ps(_mm_mul_ps(a, b), c); }

/// @brief 4 本の行を転置する (行 i の成分 j と行 j の成分 i を入れ替える)。
inline void Transpose4x4(Vec& r0, Vec& r1, Vec& r2, Vec& r3) { _MM_TRANSPOSE4_PS(r0, r1, r2, r3); }

} // namespace fbzz::math::simd
