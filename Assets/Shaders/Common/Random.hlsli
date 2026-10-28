/// @file    Random.hlsli
/// @brief   ハッシュと二次元の値ノイズを共有する。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note Random.hlsli | Common
/// @note ハッシュ・疑似乱数 (SSAO / TAA のノイズ生成用)
#ifndef RANDOM_HLSLI
#define RANDOM_HLSLI

/// @note ---- 整数ハッシュ (Wang hash) -------------------------------------------

uint Hash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}

/// @note uint → [0, 1)
float HashToFloat(uint s) { return float(Hash(s)) * (1.0f / 4294967296.0f); }

/// @note ---- 2D ハッシュ ---------------------------------------------------------

/// @note UV 座標から [0, 1) のスカラー (sin ベース、軽量だが品質は低い)
float Hash2D(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

/// @note UV 座標から [0, 1) の 2 成分ベクトル (SSAO カーネル回転用)
float2 Hash2D2(float2 p)
{
    float2 k = float2(dot(p, float2(127.1f, 311.7f)),
                      dot(p, float2(269.5f, 183.3f)));
    return frac(sin(k) * 43758.5453f);
}

/// @note ---- フレームインデックス付きハッシュ (TAA 用) ---------------------------

/// @note pixel + frame の組み合わせでフレームごとにパターンを変化させる
float TemporalHash(uint2 pixel, uint frame)
{
    return HashToFloat(Hash(pixel.x + Hash(pixel.y + Hash(frame))));
}

/// @note 既存 Hash2D と三次フェードを使い、雲と雲影の値ノイズを揃える。
float ValueNoise2D(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);

    float a = Hash2D(i);
    float b = Hash2D(i + float2(1.0f, 0.0f));
    float c = Hash2D(i + float2(0.0f, 1.0f));
    float d = Hash2D(i + float2(1.0f, 1.0f));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

#endif ///< @note RANDOM_HLSLI
