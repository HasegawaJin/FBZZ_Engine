/// @file    CloudShadow.hlsli
/// @brief   地表へ落とす雲影の値ノイズと FBM。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note CloudShadow.hlsli | Rendering
/// @note 雲シャドウ (Phase C) — ワールド XZ で手続き型 FBM を評価し、地表に落ちる雲の影を近似する。
/// @note 専用の CloudShadowMap テクスチャを焼かず、フラグメントのワールド座標から直接サンプルする解析版。
/// @note Shadow.hlsli の影係数に乗算して使う (全 Lit シェーダー共通)。
#ifndef CLOUD_SHADOW_HLSLI
#define CLOUD_SHADOW_HLSLI

#include "Common/Random.hlsli"

/// @note 2D 値ノイズ (bilinear) — Cloud.hlsli と同系統だが本ファイルで自己完結させる。
float CloudShadowValueNoise(float2 p)
{
    return ValueNoise2D(p);
}

/// @note FBM — 4 オクターブ
float CloudShadowFBM(float2 p)
{
    float  v = 0.0f, amp = 0.5f;
    float2 q = p;
    [unroll] for (int i = 0; i < 4; ++i) { v += amp * CloudShadowValueNoise(q); q *= 2.1f; amp *= 0.5f; }
    return v;
}

/// @note CloudShadowFactor — 地表のワールド座標に落ちる雲影の透過率 [1-strength .. 1] を返す。
/// @note worldXZ  : フラグメントのワールド XZ
/// @note strength : 影の濃さ (0=無効)
/// @note coverage : 雲量 [0,1] (大きいほど影が広い)
/// @note scale    : world→ノイズ UV スケール
/// @note windXZ   : 流れる方向, speed : 速さ, time : 時間
/// @note 戻り値 1.0 = 影なし。strength=0 のとき常に 1.0。
float CloudShadowFactor(float2 worldXZ, float strength, float coverage,
                        float scale, float2 windXZ, float speed, float time)
{
    if (strength <= 0.0f) return 1.0f;

    float2 uv      = worldXZ * scale + windXZ * (time * speed * 0.02f);
    float  density = CloudShadowFBM(uv);

    /// @note coverage を閾値に density を雲量へマップ。coverage が大きいほど低い density でも雲とみなす。
    float  threshold = lerp(0.85f, 0.30f, saturate(coverage));
    float  cloud     = saturate((density - threshold) * 4.0f);

    /// @note 雲が濃いほど暗く。最大で strength ぶん暗くする。
    return 1.0f - cloud * saturate(strength);
}

#endif ///< @note CLOUD_SHADOW_HLSLI
