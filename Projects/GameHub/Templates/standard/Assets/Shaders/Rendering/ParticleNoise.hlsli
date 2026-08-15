// FBZZ Engine
// Rendering/ParticleNoise.hlsli
// パーティクル系シェーダーが共有する 3D 値ノイズ
//
// WHY: 同じノイズを ParticleGpuSim.cs.hlsl (シミュレーション) と
//      Particle.hlsl / ParticleGPU.hlsl (ボリュメトリック煙の密度) の両方が使う。
//      各ファイルへ複製すると、片方の係数だけ直したときに
//      「シミュレーションの渦と見た目の濃淡がずれる」形で静かに壊れる。
//
// 式は ParticlePass.cpp の同名関数と一致させること (CPU/GPU で挙動を揃える)。

#ifndef FBZZ_PARTICLE_NOISE_INCLUDED
#define FBZZ_PARTICLE_NOISE_INCLUDED

// 整数ハッシュ (PCG 系)。格子点から再現可能な擬似乱数を作る。
uint PcgHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 格子点 (整数座標) → [-1, 1] の擬似乱数値
float LatticeValue(int3 c)
{
    uint h = PcgHash((uint)c.x * 73856093u ^ (uint)c.y * 19349663u ^ (uint)c.z * 83492791u);
    return (float)h * (2.0f / 4294967295.0f) - 1.0f;
}

// 3D 値ノイズ [-1, 1]。8 格子点を smoothstep 重みでトリリニア補間する。
float ValueNoise3D(float3 p)
{
    float3 f = floor(p);
    int3   c = (int3)f;
    float3 t = p - f;
    // smoothstep フェード: 格子境界で勾配を連続にする
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

// 複数オクターブを重ねた乱流。煙の内部にディテールの階層を作る。
float FbmNoise3D(float3 p, int octaves)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    [loop] for (int i = 0; i < octaves; ++i)
    {
        sum += ValueNoise3D(p * frequency) * amplitude;
        frequency *= 2.03f;   // 整数倍にすると格子が揃って縞が出るため僅かにずらす
        amplitude *= 0.5f;
    }
    return sum;
}

#endif // FBZZ_PARTICLE_NOISE_INCLUDED
