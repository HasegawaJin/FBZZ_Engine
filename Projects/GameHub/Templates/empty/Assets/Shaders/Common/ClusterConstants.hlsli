// FBZZ Engine
// ClusterConstants.hlsli | Common
// クラスタライトカリング (Forward+ / Deferred+) の共通定義
//
// LAYOUT: Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の
//         PunctualLightGPU / ClusterConstantsCB と完全に一致させること。
//         片方だけ変えると、ライトが黙って別の位置・別の色で評価される。
#ifndef CLUSTER_CONSTANTS_HLSLI
#define CLUSTER_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

// ---- グリッド寸法 ---------------------------------------------------------
// WHY 解像度非依存の固定グリッドか: 画面ピクセル数からタイル数を決めると、
//     ビューポートをリサイズするたびにクラスタバッファを作り直すことになる。
//     縦横比 16:9 に合わせた固定分割にしておけば、確保は起動時の 1 回で済み、
//     エディタのパネル操作で毎フレーム再確保が走る事故も起きない。
#define FBZZ_CLUSTER_GRID_X 32
#define FBZZ_CLUSTER_GRID_Y 18
#define FBZZ_CLUSTER_GRID_Z 24
#define FBZZ_CLUSTER_COUNT  (FBZZ_CLUSTER_GRID_X * FBZZ_CLUSTER_GRID_Y * FBZZ_CLUSTER_GRID_Z)

// 1 クラスタが保持できるライト数の上限。あふれた分はライト番号の昇順で切り捨てる。
// WHY 昇順で切るか: 同じ顔ぶれが重なっている限り毎フレーム同じライトが残る。
//
// WHY 64 か: 32 では足りないシーンが実際に出た。屋内アリーナに range が部屋と同程度の
//     管ライトを数十本吊ると、ほぼ全クラスタが全ライトと重なる。上限に当たると番号の
//     大きいライトが黙って消え、しかもクラスタごとに重なる本数が 32 をまたぐ場所では
//     カメラの動きに合わせてライトが出入りする (点滅して見える)。
//     ループ回数は実際のライト本数で決まるので、上限を上げても本数の少ないシーンの
//     コストは変わらない。増えるのはインデックスバッファだけ (13824 クラスタ ×
//     (64+1) × 4B = 3.6MB、32 のときの倍)。
#define FBZZ_MAX_LIGHTS_PER_CLUSTER 64

// クラスタ 1 個分の uint 数。先頭がライト数、続けてライト番号が並ぶ。
// WHY offset テーブルを持たないか: 1 スレッド = 1 クラスタで書くとカウンタがスレッド内に
//     収まるので、可変長パッキングをやめて固定ストライドにできる。
//     結果 UAV が 1 本で済み、DX11 の u0〜u7 制約に余裕をもって収まる。
#define FBZZ_CLUSTER_STRIDE (FBZZ_MAX_LIGHTS_PER_CLUSTER + 1)

// 点光源 + スポットを統合した配列の上限。
#define FBZZ_MAX_PUNCTUAL_LIGHTS 256

// PunctualLight::type
#define FBZZ_LIGHT_TYPE_POINT 0
#define FBZZ_LIGHT_TYPE_SPOT  1
// 矩形の面光源。position が面の中心、direction が面の法線、tangent / bitangent が
// 面内の軸で、halfWidth / halfHeight がその半寸法。LTC で解析的に積分する。
#define FBZZ_LIGHT_TYPE_AREA  2
// 球の光源。position が中心、halfWidth が半径。
#define FBZZ_LIGHT_TYPE_SPHERE 3
// カプセルの光源 (蛍光灯・ネオン管)。position が中心、tangent が軸、
// halfWidth が半径、halfHeight が軸方向の半長。
#define FBZZ_LIGHT_TYPE_TUBE   4

// ---- ライト供給モード -----------------------------------------------------
// WHY 3 状態にするか: cbuffer が束縛されていないパスでは中身が全ゼロで読まれる。
//     ゼロ = LEGACY にしておけば、b9 と t29/t30 を渡していない既存パス
//     (Terrain / Water / Decal / Particle など) は今までと 1 ビットも変わらず動く。
//     おかげでパス単位で 1 つずつ移行でき、途中の状態でも常にビルドが通る。
#define FBZZ_LIGHT_MODE_LEGACY    0  // b3 の固定長 cbuffer (点 8 / スポット 4)
#define FBZZ_LIGHT_MODE_LINEAR    1  // StructuredBuffer を全数走査 (カリング無効・検証用)
#define FBZZ_LIGHT_MODE_CLUSTERED 2  // クラスタが持つライトだけ走査

// ---- ライト 1 本 (96 bytes) -----------------------------------------------
// WHY 点光源とスポットを 1 つの型へ統合するか: インデックス空間が 1 本になり、
//     カリング CS のループも PS のループも 1 重で済む (従来は 2 重ループだった)。
//     点光源では direction / innerCos / outerCos を使わない。
//
// shadowIndex / cookieIndex は「アトラスの何番目のスロットか」。-1 で無効。
// WHY ライト側に持たせるか: 影と Cookie を持てるライトは全体のごく一部なので、
//     全ライトぶんの行列を配るのではなく、持っているものだけが番号でスロットを指す。
//     PunctualShadowConstants (b12) 側の配列長がライト数と独立になり、
//     ライトを 256 本置いても影を持つのは 16 本まで、という運用ができる。
//
// tangent / bitangent / halfWidth / halfHeight は「大きさを持つ光源」が使う。
//   AREA   : tangent/bitangent = 面内の軸, halfWidth/halfHeight = 半寸法
//   SPHERE : halfWidth = 半径
//   TUBE   : tangent = 軸, halfWidth = 半径, halfHeight = 軸方向の半長
//   POINT / SPOT : halfWidth = 光源半径 (影のにじみ幅とハイライトの広がりにだけ効く)
// WHY 使わない型でも枠を持つか: 型ごとに構造体を分けるとインデックス空間が再び割れ、
//     クラスタカリングの CS と PS のループが型の数だけ増える。統合を保つ方を採る。
struct PunctualLight
{
    float3 position;   float range;
    float3 color;      float intensity;
    float3 direction;  float innerCos;   // Spot のみ
    float  outerCos;   uint  type;       int shadowIndex;  int cookieIndex;
    float3 tangent;    float halfWidth;  // Area のみ
    float3 bitangent;  float halfHeight; // Area のみ
};

// ---- 定数 (b9) ------------------------------------------------------------
cbuffer ClusterConstants : register(CB_CLUSTER)
{
    // 1 クラスタが覆う画面上のピクセル数 = screenSize / (GRID_X, GRID_Y)。
    // WHY CPU で割っておくか: PS 側は SV_POSITION.xy をこれで割るだけになり、
    //     PostProcConstants (b5) の screenSize に依存せずに済む。
    //     マテリアルパスは b5 を束縛しないので、これがないとタイル座標を出せない。
    float2 clusterTilePx;
    // 指数分割の係数。slice = floor(log(viewZ) * scale + bias)
    float  clusterSliceScale;
    float  clusterSliceBias;

    uint   clusterLightMode;    // FBZZ_LIGHT_MODE_* (未束縛時は 0 = LEGACY)
    uint   punctualLightCount;  // 有効なライト本数
    uint   clusterDebugMode;    // 0=通常, 1=クラスタあたりライト数のヒートマップ
    uint   _clusterPad0;
};

// ビュー空間深度からスライス番号を求める。
uint FBZZ_ClusterSliceFromViewZ(float viewZ)
{
    // log(0) と負値を踏まないよう下限で止める。near 面より手前は必ずスライス 0。
    const float z = max(viewZ, 1e-4f);
    const int   s = (int)floor(log(z) * clusterSliceScale + clusterSliceBias);
    return (uint)clamp(s, 0, FBZZ_CLUSTER_GRID_Z - 1);
}

// (x, y, z) から線形クラスタ番号へ。CS と PS で必ず同じ式を使うこと。
uint FBZZ_ClusterIndex(uint3 coord)
{
    return coord.x
         + coord.y * FBZZ_CLUSTER_GRID_X
         + coord.z * FBZZ_CLUSTER_GRID_X * FBZZ_CLUSTER_GRID_Y;
}

#endif // CLUSTER_CONSTANTS_HLSLI
