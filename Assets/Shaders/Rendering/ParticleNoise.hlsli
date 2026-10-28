/// @file    ParticleNoise.hlsli
/// @brief   粒子と流体が共有する三次元の値ノイズと Curl Noise。
/// @author  Hasegawa Jin
/// @date    2026-08-12

/// @note Rendering/ParticleNoise.hlsli
/// @note パーティクル系シェーダーが共有する 3D 値ノイズ

/// @note 同じノイズを ParticleGpuSim.cs.hlsl (シミュレーション) と
/// @note Particle.hlsl / ParticleGPU.hlsl (ボリュメトリック煙の密度) の両方が使う。
/// @note 各ファイルへ複製すると、片方の係数だけ直したときに
/// @note 「シミュレーションの渦と見た目の濃淡がずれる」形で静かに壊れる。

/// @note 式は ParticlePass.cpp の同名関数と一致させること (CPU/GPU で挙動を揃える)。

#ifndef FBZZ_PARTICLE_NOISE_INCLUDED
#define FBZZ_PARTICLE_NOISE_INCLUDED

/// @note 整数ハッシュ (PCG 系)。格子点から再現可能な擬似乱数を作る。
uint PcgHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// @note 格子点 (整数座標) → [-1, 1] の擬似乱数値
float LatticeValue(int3 c)
{
    uint h = PcgHash((uint)c.x * 73856093u ^ (uint)c.y * 19349663u ^ (uint)c.z * 83492791u);
    return (float)h * (2.0f / 4294967295.0f) - 1.0f;
}

/// @note 3D 値ノイズ [-1, 1]。8 格子点を smoothstep 重みでトリリニア補間する。
float ValueNoise3D(float3 p)
{
    float3 f = floor(p);
    int3   c = (int3)f;
    float3 t = p - f;
    /// @note smoothstep フェード: 格子境界で勾配を連続にする
    t = t * t * (3.0f - 2.0f * t);
    float c000 = LatticeValue(c + int3(0, 0, 0));
    float c100 = LatticeValue(c + int3(1, 0, 0));
    float c010 = LatticeValue(c + int3(0, 1, 0));
    float c110 = LatticeValue(c + int3(1, 1, 0));
    float c001 = LatticeValue(c + int3(0, 0, 1));
    float c101 = LatticeValue(c + int3(1, 0, 1));
    float c011 = LatticeValue(c + int3(0, 1, 1));
    float c111 = LatticeValue(c + int3(1, 1, 1));
    float x00 = lerp(c000, c100, t.x);
    float x10 = lerp(c010, c110, t.x);
    float x01 = lerp(c001, c101, t.x);
    float x11 = lerp(c011, c111, t.x);
    float y0  = lerp(x00, x10, t.y);
    float y1  = lerp(x01, x11, t.y);
    return lerp(y0, y1, t.z);
}

/// @note 複数オクターブを重ねた乱流。煙の内部にディテールの階層を作る。
float FbmNoise3D(float3 p, int octaves)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    [loop] for (int i = 0; i < octaves; ++i)
    {
        sum += ValueNoise3D(p * frequency) * amplitude;
        frequency *= 2.03f;   ///< @note 整数倍にすると格子が揃って縞が出るため僅かにずらす
        amplitude *= 0.5f;
    }
    return sum;
}

/// @note 同じ値ノイズ・offset・中心差分の刻みを液体と気体で使う。
float3 CurlNoise3D(float3 p)
{
    const float3 p1 = p + 31.341f;
    const float3 p2 = p - 47.853f;
    const float3 p3 = p + 12.793f;
    const float eps = 0.25f;
    const float invTwoEps = 1.0f / (2.0f * eps);
    const float3 dx = float3(eps, 0.0f, 0.0f);
    const float3 dy = float3(0.0f, eps, 0.0f);
    const float3 dz = float3(0.0f, 0.0f, eps);
    const float dp1dy = (ValueNoise3D(p1 + dy) - ValueNoise3D(p1 - dy)) * invTwoEps;
    const float dp1dz = (ValueNoise3D(p1 + dz) - ValueNoise3D(p1 - dz)) * invTwoEps;
    const float dp2dx = (ValueNoise3D(p2 + dx) - ValueNoise3D(p2 - dx)) * invTwoEps;
    const float dp2dz = (ValueNoise3D(p2 + dz) - ValueNoise3D(p2 - dz)) * invTwoEps;
    const float dp3dx = (ValueNoise3D(p3 + dx) - ValueNoise3D(p3 - dx)) * invTwoEps;
    const float dp3dy = (ValueNoise3D(p3 + dy) - ValueNoise3D(p3 - dy)) * invTwoEps;
    return float3(dp3dy - dp2dz, dp1dz - dp3dx, dp2dx - dp1dy);
}

#endif ///< @note FBZZ_PARTICLE_NOISE_INCLUDED
