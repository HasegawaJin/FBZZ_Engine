/// @file PunctualShadowConstants.hlsli
/// @brief Spot / Point シャドウと Cookie の定数バッファ (b12) の単一定義
/// @author Hasegawa Jin
/// @date 2026-08-25
///
/// @note Graphics/Pipeline/RenderConstants.hpp の
/// @note PunctualShadowConstantsCB と完全に一致させること (2800 bytes)。
/// @note C++ 側に static_assert を置いてある。
///
/// @note b4 (ShadowConstants) と分けるか:
/// @note b4 は Directional のカスケードを配る場所で、Terrain / Water / パーティクルを含む
/// @note 20 以上のシェーダーが同じレイアウトを読む。Spot / Point はまだ「対応済みのパスから
/// @note 順に増やしていく」段階なので、束縛していないパスが 0 埋め (= 影も Cookie も無し) の
/// @note まま素通りできる別スロットへ置く。punctualShadowCount == 0 が安全側になる。
#ifndef PUNCTUAL_SHADOW_CONSTANTS_HLSLI
#define PUNCTUAL_SHADOW_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

/// @note シャドウアトラスのタイル数。Spot は 1 枚、Point はキューブ 6 面ぶんを消費する。
/// @note 16 か: アトラスを 4x4 に切る前提。2048² なら 1 タイル 512² で、
/// @note Spot 16 個 / Point 2 個 + Spot 4 個 といった配分になる。屋内 1 部屋に
/// @note 必要な数としては十分で、これ以上はアトラス解像度を上げる方が筋がよい。
#define FBZZ_MAX_PUNCTUAL_SHADOWS 16

/// @note Cookie アトラスのタイル数。
#define FBZZ_MAX_LIGHT_COOKIES 8

/// @note legacyPunctualSlots 内でスポットが始まる位置。
/// @note Common/Constants.hlsli の MAX_POINT_LIGHTS と一致させること。
#define FBZZ_LEGACY_SPOT_SLOT_BASE 8

/// @note レガシー経路 (b3) が型を扱えない「大きさを持つ光源」(Area / Sphere / Tube) を、
/// @note この cbuffer 側で運ぶ本数。
/// @note 4 本か: 窓・蛍光灯・看板・裸電球といった用途で 1 部屋に数個置くもので、
/// @note 点光源のように何十個も撒くものではない。それ以上必要なら Forward+ へ移る。
#define FBZZ_MAX_LEGACY_SHAPED_LIGHTS 4
/// @note 1 本が占める float4 の数。
#define FBZZ_LEGACY_SHAPED_STRIDE 6

cbuffer PunctualShadowConstants : register(CB_PUNCTUAL_SHADOW)
{
    /// @note スロットごとのライト viewProjection。Spot は 1 本、Point は連続する 6 本
    /// @note (キューブ面 +X,-X,+Y,-Y,+Z,-Z の順) を占める。
    float4x4 punctualShadowVP[FBZZ_MAX_PUNCTUAL_SHADOWS];

    /// @note スロットごとのアトラス矩形。xy = UV オフセット, zw = UV スケール。
    float4   punctualShadowRect[FBZZ_MAX_PUNCTUAL_SHADOWS];

    /// @note x = NDC 深度バイアス, y = 影の濃さ [0,1], z = 半影の広がり (テクセル), w = 予備。
    ///
    /// @note z の由来: 光源半径 / タイルが覆う幅。大きい光源ほど PCF カーネルを広げて
    /// @note 縁を柔らかくする。0 で従来どおり punctualShadowPcf のカーネルだけを使う。
    /// @note PCSS にしないか: PCSS はブロッカー探索に 16 タップを追加で要し、
    /// @note ライト 1 本ごとに掛かる。Spot / Point は同時に何本も評価されるので、
    /// @note 「距離で半影が変わる」より「光源の大きさで半影が変わる」の方が
    /// @note 費用対効果が高い。遮蔽物との距離に応じた変化は Directional の PCSS が持つ。
    /// @note スロットごとにバイアスを持つか: Spot は透視投影なので、同じワールド距離の
    /// @note オフセットでも near / far と円錐角で NDC 換算値が変わる。ライトごとに
    /// @note range も角度も違う以上、1 つの値を共有すると必ずどこかで破綻する。
    float4   punctualShadowParams[FBZZ_MAX_PUNCTUAL_SHADOWS];

    /// @note Cookie の投影行列とアトラス矩形。シャドウとは独立に割り当てる。
    /// @note シャドウ行列を流用しないか: Cookie は影を落とさないライトにも付けられる。
    /// @note 同じスロット番号に縛ると「Cookie だけ欲しい」ライトがシャドウ枠を消費する。
    float4x4 lightCookieVP[FBZZ_MAX_LIGHT_COOKIES];
    float4   lightCookieRect[FBZZ_MAX_LIGHT_COOKIES];

    /// @note レガシー経路 (b3 の固定長配列) 用の付加情報。
    /// @note x = shadowIndex, y = cookieIndex, z = sourceRadius [m], w = 予備
    /// @note sourceRadius もここか: b3 の PointLightData は 32 バイトぴったりで詰まっており、
    /// @note 1 float も空いていない。番号を運ぶために既に使っているこの枠へ相乗りさせる。
    /// @note [0..7]  → pointLights[0..7]
    /// @note [8..11] → spotLights[0..3]
    ///
    /// @note b3 側に足さないか: LightConstants は Constants / Detail / Decal / Terrain /
    /// @note Water の 5 か所へ手書きで複製されている。1 フィールド足すだけで 5 か所を
    /// @note 揃えて直す必要があり、1 つ漏らすとそのシェーダーだけ全ライトが別オフセットを
    /// @note 読む (ライトの位置と色がずれる) という、最も気づきにくい壊れ方をする。
    /// @note 番号だけをこちら側の配列へ逃がせば、b3 は 1 ビットも変わらない。
    ///
    /// @note float4 で持つか: HLSL の cbuffer は配列要素を 16 バイト境界へ詰めるため、
    /// @note int の配列でも 1 要素 16 バイトを占める。ベクトル成分への動的インデックスは
    /// @note コンパイラによって扱いが揺れるので、素直に 1 要素 1 レジスタで置く。
    float4   legacyPunctualSlots[12];

    /// @note レガシー経路用の「大きさを持つ光源」の実体。1 本あたり 6 レジスタ:
    /// @note [0] xyz = position,  w = range
    /// @note [1] rgb = color,     w = intensity
    /// @note [2] xyz = direction (面の法線 / 未使用),  w = 予備
    /// @note [3] xyz = tangent (面内軸 / 管の軸),      w = halfWidth  (半径 / 半幅)
    /// @note [4] xyz = bitangent (面内軸 / 未使用),    w = halfHeight (半長 / 半高)
    /// @note [5] x = FBZZ_LIGHT_TYPE_*, y = 1 なら両面 (Area のみ),
    /// @note z = シャドウスロット番号 (-1 = 影なし), w = 予備
    ///
    /// @note 実体ごと載せるか: 番号だけ運べば済んだ影 / Cookie と違い、これらは
    /// @note b3 の PointLightData / SpotLightData に「型そのもの」が無い。
    /// @note 既定のパイプラインは Forward (= b3 経路) なので、ここを通さないと
    /// @note 「Forward+ に切り替えたときだけ光る」ことになる。
    float4   legacyShapedLight[FBZZ_MAX_LEGACY_SHAPED_LIGHTS * FBZZ_LEGACY_SHAPED_STRIDE];

    float2   punctualShadowTexel;  // 1.0 / シャドウアトラス解像度
    int      punctualShadowCount;  /// @note 有効スロット数。0 のとき影の評価を丸ごと飛ばす
    int      punctualShadowPcf;    /// @note PCF カーネル半径: 0=ハード, 1=3x3, 2=5x5

    float2   lightCookieTexel;       // 1.0 / Cookie アトラス解像度
    int      lightCookieCount;       /// @note 有効 Cookie 数。0 のとき Cookie の評価を飛ばす
    int      legacyShapedLightCount; /// @note レガシー経路で評価する「大きさを持つ光源」の本数

    float4   _punctualShadowPad0;
};

#endif
