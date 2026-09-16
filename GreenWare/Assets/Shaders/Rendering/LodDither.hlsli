/// @file LodDither.hlsli
/// @brief LOD 切り替えのディザクロスフェード
/// @author Hasegawa Jin
/// @date 2026-08-25
#ifndef LOD_DITHER_HLSLI
#define LOD_DITHER_HLSLI

// LOD が切り替わる瞬間、旧レベルは市松の片側だけを残して消え、新レベルは
// 残りの側だけを描いて現れる。2 つを足すと必ず画面が埋まるので穴が開かない。
// 中間状態のまだら模様は TAA が数フレームで溶かす。
//
// WHY アルファブレンドでないか: LOD の入れ替えは不透明パスの中で起きる。半透明にすると
//     描画順の管理と深度書き込みの制御が要り、ソートの都合で不透明の後ろへ回る。
//
// WHY Alpha-to-Coverage でないか: このエンジンは MSAA を使わない (全 RT が
//     SampleDesc.Count = 1)。サンプルが 1 枚では A2C は 2 値クリップに退化する。

// 4x4 Bayer 行列。値域は [0, 1)。
// 順序ディザ (青色ノイズや乱数でない) を選ぶのは TAA と噛み合わせるため —
// パターンが画面に固定されていれば、ジッターがサブピクセルを掃くだけで綺麗に平均が出る。
float FBZZ_Bayer4x4(float2 svPosition)
{
    static const float kBayer[16] = {
         0.0f /16.0f,  8.0f /16.0f,  2.0f /16.0f, 10.0f /16.0f,
        12.0f /16.0f,  4.0f /16.0f, 14.0f /16.0f,  6.0f /16.0f,
         3.0f /16.0f, 11.0f /16.0f,  1.0f /16.0f,  9.0f /16.0f,
        15.0f /16.0f,  7.0f /16.0f, 13.0f /16.0f,  5.0f /16.0f
    };
    const uint2 p = uint2(svPosition) & 3u;
    return kBayer[p.y * 4u + p.x];
}

/// LOD 遷移中の画素を捨てる。ピクセルシェーダーの先頭で呼ぶ。
///   lodDither: ObjectConstants.objectParams.x
///      0    — 遷移していない。全画素を残す
///      > 0  — 出現中。しきい値 lodDither 未満の画素だけ残す
///      < 0  — 退場中。しきい値 -lodDither 以上の画素だけ残す
///
/// 出現側と退場側で判定の向きが逆なのが肝。同じ向きにすると両者が同じ画素を
/// 描いて、残り半分がどちらにも描かれない穴になる。
void ApplyLodDither(float2 svPosition, float lodDither)
{
    if (lodDither == 0.0f)
        return;

    const float bayer = FBZZ_Bayer4x4(svPosition);
    clip(lodDither > 0.0f ? (lodDither - bayer) : (bayer + lodDither));
}

#endif // LOD_DITHER_HLSLI
