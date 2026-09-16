// FBZZ Engine
// UIShading.hlsli | UI
// UI マテリアルが使う形状・階調のプリミティブ集。
//
// WHY 距離場を基本にするか:
//   角丸・枠線・影・グロウは UI の見た目の大半を占めるが、テクスチャで作ると
//   角ごとに画像を用意することになり、半径を変えるたびに再書き出しが要る。
//   矩形からの符号付き距離が 1 本あれば、半径も枠幅も影の広がりも
//   すべて同じ距離に対するしきい値でしかなくなり、数値だけで調整できる。
//
// WHY アンチエイリアスを fwidth で取るか:
//   UI は解像度も Canvas スケールも実行時に変わる。固定幅でぼかすと、
//   4K では硬く、縮小時にはボケる。画面 1 ピクセルぶんを微分から測れば
//   どの解像度でも同じ締まり方になる。
#ifndef UI_SHADING_HLSLI
#define UI_SHADING_HLSLI

// ── 符号付き距離 (すべてピクセル単位、負 = 内側) ────────────────────────

// 角丸矩形。p は矩形中心を原点とするピクセル座標、halfSize は半寸法。
// radius は左上/右上/右下/左下 の順。
// NOTE: Canvas は原点が左上で y が下向き。つまり p.y > 0 は「下半分」を指す。
//       3D 側の感覚で上下を取り違えると、角丸が対角に入れ替わって出る。
float UI_SdRoundedBox(float2 p, float2 halfSize, float4 radius)
{
    // 象限ごとに使う半径を選ぶ。分岐ではなく lerp で選ぶのは、
    // 角の境界で半径が不連続に切り替わると距離場が裂けるため。
    radius.xy = (p.x > 0.0f) ? radius.yz : radius.xw;
    radius.x  = (p.y > 0.0f) ? radius.y  : radius.x;
    // 半径は辺の長さの半分を超えられない。超えると角同士が食い合って
    // 距離場が内側で反転する。
    radius.x  = min(radius.x, min(halfSize.x, halfSize.y));

    float2 q = abs(p) - halfSize + radius.x;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - radius.x;
}

// 全角同じ半径の簡易版。
float UI_SdRoundedBox(float2 p, float2 halfSize, float radius)
{
    return UI_SdRoundedBox(p, halfSize, float4(radius, radius, radius, radius));
}

float UI_SdCircle(float2 p, float radius)
{
    return length(p) - radius;
}

// ── 距離 → カバレッジ ────────────────────────────────────────────────

// 距離場を画面 1 ピクセル幅のアンチエイリアス済みカバレッジへ落とす。
// distance < 0 (内側) で 1、> 0 (外側) で 0。
float UI_Coverage(float distance)
{
    // 平坦部では fwidth が 0 になりゼロ除算する。下限を入れて saturate に任せる。
    float width = max(fwidth(distance), 1e-4f);
    return saturate(0.5f - distance / width);
}

// 指定幅のソフトエッジ。width をピクセルで与えると、そのぶんだけぼける。
// 影のように「意図的に広くぼかしたい」ときはこちら。
float UI_CoverageSoft(float distance, float width)
{
    return saturate(0.5f - distance / max(width, 1e-4f));
}

// 距離場から幅 thickness の枠だけを取り出す。輪郭の内側へ描く。
float UI_Border(float distance, float thickness)
{
    // 外側の縁と、thickness ぶん内側の縁の差分が枠になる。
    return saturate(UI_Coverage(distance) - UI_Coverage(distance + thickness));
}

// ドロップシャドウのカバレッジ。offset は影をずらすピクセル量。
float UI_Shadow(float2 p, float2 halfSize, float4 radius, float2 offset, float softness)
{
    float distance = UI_SdRoundedBox(p - offset, halfSize, radius);
    return UI_CoverageSoft(distance, max(softness, 1e-4f));
}

// ── 階調 ────────────────────────────────────────────────────────────

// 方向つき線形グラデーション。angle はラジアン (0 = 左→右)。
float UI_LinearGradient(float2 localUv, float angle)
{
    float2 dir = float2(cos(angle), sin(angle));
    // localUv を中心原点へ寄せてから射影する。回しても中心が動かない。
    return saturate(dot(localUv - 0.5f, dir) + 0.5f);
}

float UI_RadialGradient(float2 localUv, float2 center, float radius)
{
    return saturate(length(localUv - center) / max(radius, 1e-4f));
}

// ── バンディング対策 ────────────────────────────────────────────────

// 順序付きディザ。8bit のバックバッファへ長いグラデーションを出すと
// 必ず縞が見える。量子化の 1 段ぶん未満のノイズを足して縞を溶かす。
// WHY 乱数でなく Bayer 行列か: フレームごとに変わる乱数だと静止した UI が
//     ざらついて見える。位置だけで決まる固定パターンなら止まって見える。
float UI_DitherMask(float2 screenPos)
{
    float2 cell = floor(fmod(screenPos, 4.0f));
    // 4x4 Bayer を多項式で展開したもの (テーブル参照を避ける)。
    float index = cell.y * 4.0f + cell.x;
    float bayer = frac(index * (1.0f / 16.0f) + index * index * (1.0f / 256.0f) * 4.0f);
    return bayer - 0.5f;
}

// 8bit 出力を想定した 1/255 未満の揺らぎ。
float3 UI_Dither(float3 color, float2 screenPos)
{
    return color + UI_DitherMask(screenPos) * (1.0f / 255.0f);
}

// ── 合成 ────────────────────────────────────────────────────────────

// 事前乗算していない色どうしの通常合成 (src over dst)。
float4 UI_Over(float4 src, float4 dst)
{
    float outAlpha = src.a + dst.a * (1.0f - src.a);
    if (outAlpha <= 1e-5f) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 outColor = (src.rgb * src.a + dst.rgb * dst.a * (1.0f - src.a)) / outAlpha;
    return float4(outColor, outAlpha);
}

#endif // UI_SHADING_HLSLI
