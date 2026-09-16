/// @file    ArcNoise.hlsli
/// @brief   放電の «不規則さ» を作る 1 次元ノイズ。ElectricArc と Beam が共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY テクスチャを持たせないか:
///   要るのは «帯の長さ方向にだけ変化する乱れ» で、2 次元の情報が無い。
///   1 次元の値ノイズなら数命令で済み、素材の割り当て漏れで放電が死ぬこともない。
///
/// WHY 共有するか:
///   放電リボン (ElectricArc) と照射ビーム (Beam) は «同じ電気» の別の出方なので、
///   乱れの質が違うと同じ武器から出ているように見えない。式を 1 箇所に置く。
#ifndef FBZZ_ARC_NOISE_HLSLI
#define FBZZ_ARC_NOISE_HLSLI

// 1 次元ハッシュ。
// NOTE: x を大きくすると frac(sin()) が精度を失って縞へ潰れる。位相を渡す側で
//       適当な周期に巻き取ること (ElectricArcBundle は 1024 で fmod している)。
float ArcHash(float n)
{
    return frac(sin(n * 12.9898f) * 43758.5453f);
}

// 折れ線補間の値ノイズ。稲妻の「太さのムラ」に使う。
float ArcNoise(float x)
{
    float i = floor(x);
    float f = frac(x);
    f = f * f * (3.0f - 2.0f * f);
    return lerp(ArcHash(i), ArcHash(i + 1.0f), f);
}

// 3 オクターブ。1 オクターブだと波打つだけで、放電の不規則さにならない。
float ArcFbm(float x)
{
    return ArcNoise(x) * 0.55f
         + ArcNoise(x * 2.3f) * 0.30f
         + ArcNoise(x * 5.1f) * 0.15f;
}

#endif // FBZZ_ARC_NOISE_HLSLI
