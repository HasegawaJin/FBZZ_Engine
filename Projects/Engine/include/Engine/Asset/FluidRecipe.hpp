/// @file    FluidRecipe.hpp
/// @brief   .fluid — 流体エフェクトのレシピ (全体の設定 + 発生源・力の部品リスト + 見た目・出力)
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY レシピをアセットにするか:
///   焼いたフリップブック / Motion Vector / .vfield は «結果» でしかなく、浮力を 1 つ
///   変えて焼き直すには元の設定が要る。レシピを残しておけば、同じ流体から解像度違い・
///   コマ数違いを何度でも焼き直せる。ランタイムはレシピを読まない (焼いた結果だけを使う)。
///
/// WHY 気体と液体を 1 つのレシピにするか:
///   解き方は違っても «発生源 → ソルバー → 1 コマの絵 → アトラス → .mat» という流れは同じで、
///   出力・見た目・素材化の設定は共有できる。種類ごとに別アセットにすると、焼き・Inspector・
///   .mat 生成が 2 本ずつになる。
///
/// WHY 部品 (発生源・力) を種類別のリストにするか (ノードグラフにしないか):
///   同じレシピを CPU 2D / CPU 3D / 液体 PBF / GPU の 4 通りで解く。部品の «種類» と «ソルバーの段»
///   (発生源 = 注入、力 = 外力) が 1 対 1 なら、部品を 1 つ足しても実装箇所は段ごとに 1 つで済む。
///   計算の順番まで自由にすると、それが 4 通り × 任意の順になって追いつかない。
///   形と力の式は FluidOperatorEval.hpp が正本 (GPU は FluidGpuCommon.hlsli に 1:1 の写し)。
#pragma once

#include <Engine/Asset/FluidBakeSettings.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene { struct IReflector; }

namespace fbzz::asset {

enum class FluidKind : uint8_t {
    Gas = 0,  ///< 格子で解く気体 (煙・炎・爆発・蒸気・砂煙・インク・霞・陽炎)
    Liquid,   ///< 粒子で解く液体 (水しぶき・噴流・血・溶岩)
};

/// 焼いた絵の作り方。ブレンドと alpha の意味もここで決まる。
enum class FluidShading : uint8_t {
    Smoke = 0,   ///< 自己影つきの煙。Alpha ブレンド
    Fire,        ///< 黒体放射の炎 + 煤。Premultiplied (芯は光り、煙は背景を隠す)
    Glow,        ///< 密度をそのまま発光に。Additive (魔法の靄・霊気)
    Distortion,  ///< 速度を RG の変位に。歪み (陽炎・衝撃波)
    Liquid,      ///< メタボールの液面。法線とスペキュラを焼き込む。Alpha ブレンド
};

/// 1 つのレシピに置ける部品の数。GPU の定数バッファに載る数に揃える (CPU だけ多く置けると焼き分けで絵が変わる)。
inline constexpr int kMaxFluidSources = 16;
inline constexpr int kMaxFluidForces = 8;
inline constexpr int kMaxFluidColliders = 8;
inline constexpr int kMaxFluidMotionKeys = 8;
/// 量のエンベロープのキーの数。動きのキーと同じ数に揃える。
/// WHY 定数バッファに載らないのに上限を決めるか: 倍率は刻みごとに CPU が畳んで 1 つの float で渡すので
///     GPU の枠は要らないが、«ファイルと AI から来る配列は上限で切る» 規則を動きと違えると、
///     どちらが何個まで置けるかを覚えていられなくなる。
inline constexpr int kMaxFluidAmountKeys = 8;

enum class FluidSourceShape : uint8_t {
    Sphere = 0,  ///< size.x = 半径
    Box,         ///< size = 各軸の半分の大きさ
    Cone,        ///< 頂点が center、direction へ開く。size.x = 底の半径、size.y = 長さ (噴流・火炎放射)
    Ring,        ///< direction を法線とする輪。size.x = 輪の半径、size.y = 管の太さ (半径) (衝撃波・煙の輪)
    /// 画像の形に湧く (文字・ロゴ・魔法陣)。direction を法線とする板に texture を貼り、その濃さ (輝度 × α) で注ぐ。
    /// size.x / size.y = 板の半幅 / 半高さ、size.z = 板の厚みの半分。軸の決め方は FluidTextureSourceBasis。
    Texture,
    /// center を通り direction を軸とする線分に、半径ぶんの肉を付けた形 (腕・脚・棒・パイプ・武器)。
    /// size.x = 半径、size.y = 芯の線分の半分の長さ (両端に半球が付くので、端から端までは 2 × (size.y + size.x))。
    /// size.z は見ない。
    Capsule,
    /// center を通り direction を軸とする平らな端の柱 (煙突・通気口から柱状に立ち上る煙)。
    /// size.x = 半径、size.y = 半分の高さ。size.z は見ない。端の近くだけ重みがなめらかに 0 へ落ちる。
    Cylinder,
};

/// 時刻 → 中心からのずれ。キーの間は直線でつなぐ。
struct FluidMotionKey {
    float time = 0.0f;
    math::Vector3 offset = { 0.0f, 0.0f, 0.0f };
};

/// 部品の動き。キーが無ければ動かない。先頭より前・末尾より後は端のキーで止まる。
struct FluidMotion {
    /// time の昇順。kMaxFluidMotionKeys まで。
    std::vector<FluidMotionKey> keys;
    /// 動く速さを流速 (気体) / 撃ち出す速度 (液体) に足す。動く発生源が周りを引きずる。
    bool inheritVelocity = true;
};

/// 時刻 → 量の倍率。キーの間は直線でつなぐ。
struct FluidAmountKey {
    float time = 0.0f;
    float scale = 1.0f;
};

/// 部品が «どれだけ» 出しているかの時間変化。キーが空なら常に倍率 1 (エンベロープが無いのと 1 ビットも
/// 変わらない)。先頭より前・末尾より後は端のキーの値で止まる (外挿しない)。式は FluidOperatorEval.hpp の
/// SampleFluidAmount。
///
/// WHY 位置と velocity には掛けないか: «どこに居るか» は FluidMotion の担当。同じ動きを 2 つの表から
///     作れると、直すときにどちらを触るのかが決まらない。ここは «勢い» だけを持つ。
/// WHY 倍率 (絶対値でなく) か: 部品の density / strength に掛けるので、基準の値を 1 か所で直せば
///     エンベロープの形を書き直さずに全体の強さを変えられる。
struct FluidAmount {
    /// time の昇順。kMaxFluidAmountKeys まで。
    std::vector<FluidAmountKey> keys;
};

/// 発生源。気体は格子へ密度・温度・燃料・流速を注ぎ、液体は粒子を撃ち出す (kind で読み替える)。
/// 座標と大きさは «領域を各軸 [-1,1] に正規化した単位»。
/// WHY 正規化単位か: 焼く解像度 (64 / 128 / 256) を変えても同じ絵になるようにする。
struct FluidSource {
    bool enabled = true;
    /// 人と AI が見分けるための名前 (解き方には関係しない)。
    std::string name;
    FluidSourceShape shape = FluidSourceShape::Sphere;
    math::Vector3 center = { 0.0f, -0.6f, 0.0f };
    /// 意味は shape ごと (FluidSourceShape を参照)。
    math::Vector3 size = { 0.18f, 0.18f, 0.18f };
    /// Cone の開く向き / Ring・Texture の法線 / Capsule・Cylinder の軸。
    /// 長さは問わない (0 なら Cone / Ring / Capsule / Cylinder は上向き、Texture は手前向き)。
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f };
    /// Texture の画像 (PNG / TGA / JPG。Assets 相対・guid:・実パス)。濃さは輝度 × α。
    /// Sprite 参照 ("<画像>::sprite::<ID>") ならそのコマだけを切り抜く。
    std::string texture;

    // ── 気体 ──
    /// 1 秒あたりに足す量。
    float density     = 2.0f;
    float temperature = 2.0f;
    float fuel        = 0.0f;
    /// 注入量をノイズで揺らす [0,1]。完全に一様だと左右対称な «きのこ» しか出ない。
    float noise = 0.5f;

    // ── 気体・液体の両方 ──
    /// 気体: 発生源の中の流速 (0 なら流速には触らない) / 液体: 撃ち出す速度 [領域単位/秒]。
    math::Vector3 velocity = { 0.0f, 0.0f, 0.0f };
    float startTime = 0.0f;
    /// 気体: 0 以下は «最後まで出し続ける» / 液体: 0 以下は startTime に一斉、正ならその期間に均して出す。
    float duration  = 0.0f;

    /// 色の鍵 [0,1]。render.albedoRamp のどの色で描くか。気体は煙に乗って運ばれ (密度で重み付けした平均)、
    /// 液体は粒子ごとに持つ。複数の発生源の煙が混ざると色も混ざる。useAlbedoRamp が false なら見た目に効かない。
    float colorKey = 0.0f;

    // ── 液体 ──
    /// 撃ち出す速度の大きさに対するばらつき [0,1]。
    float spread = 0.5f;
    /// 撃ち出す総数。
    int   count = 600;

    FluidMotion motion;
    /// density / temperature / fuel に掛かる倍率 (気体のみ — 液体はこの 3 つを使わない)。
    FluidAmount amount;
};

/// 力の種類。粒子の ForceField (ForceFieldType) と同じ語彙 (Turbulence は Noise。VectorField は持たない)。
enum class FluidForceType : uint8_t { Wind = 0, Attract, Repulse, Vortex, Noise, Drag };

/// 空間の場として流れへ加える力。式は FluidOperatorEval.hpp の FluidForceDelta。
struct FluidForce {
    bool enabled = true;
    std::string name;
    FluidForceType type = FluidForceType::Wind;
    math::Vector3 center = { 0.0f, 0.0f, 0.0f };
    /// Wind: 向き / Vortex: 回転軸 (2D では常に画面の奥行き軸)。長さは問わない。
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    /// 加速度 [領域単位/秒²]。Drag は減衰係数 [1/秒]。
    float strength = 2.0f;
    /// 影響半径。0 以下は領域全体に一様。
    float radius = 0.0f;
    /// influence = (1 - 距離/radius)^falloffPower。
    float falloffPower = 2.0f;
    /// Noise: 細かさ (領域幅あたりの山の数) と、流れる速さ。
    float noiseFrequency = 3.0f;
    float noiseSpeed = 1.0f;
    float startTime = 0.0f;
    /// 0 以下はずっと効く。
    float duration = 0.0f;
    FluidMotion motion;
    /// strength に掛かる倍率。
    FluidAmount amount;
};

enum class FluidColliderShape : uint8_t {
    Sphere = 0,  ///< size.x = 半径
    Box,         ///< size = 各軸の半分 (軸に沿った箱。回転は持たない)
    Plane,       ///< center を通り direction を法線とする面。法線の反対側がすべて固体 (壁・斜めの床)
    /// center を通り direction を軸とする線分に、半径ぶんの肉を付けた形 (腕・脚・棒・パイプ)。
    /// size.x = 半径、size.y = 芯の線分の半分の長さ (両端は半球)。size.z は見ない。
    Capsule,
    /// center を通り direction を軸とする平らな端の柱。size.x = 半径、size.y = 半分の高さ。size.z は見ない。
    Cylinder,
};

/// 流体が通り抜けない障害物。距離と法線の式は FluidOperatorEval.hpp の FluidColliderDistance / Normal。
/// 気体: 中のセルは流れが止まり (動いていれば動きの速度で押し)、煙も入らない。
/// 液体: 粒子を表面の外 (粒子半径ぶん) へ押し出し、表面に沿った速度を friction で落とす。
/// 床 (gas.floor / liquid.floor) は今までどおり別に持つ (障害物に含めると既存の .fluid の結果が変わる)。
struct FluidCollider {
    bool enabled = true;
    std::string name;
    FluidColliderShape shape = FluidColliderShape::Sphere;
    math::Vector3 center = { 0.0f, 0.0f, 0.0f };
    math::Vector3 size = { 0.2f, 0.2f, 0.2f };
    /// Plane の法線 (こちら側が流体) / Capsule・Cylinder の軸。長さは問わない (0 なら上向き)。
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f };
    /// 液体: 表面に沿った速度を落とす強さ (liquid.floorFriction と同じ意味)。
    float friction = 0.4f;
    float startTime = 0.0f;
    /// 0 以下はずっと居る。
    float duration = 0.0f;
    /// inheritVelocity なら、動く障害物が流体を押しのける (気体は中の流速 = 動きの速度)。
    FluidMotion motion;
};

struct FluidGasSettings {
    /// 2D ベイクの格子の 1 辺。0 は Auto (コマの解像度に合わせる。上限 256)。
    /// WHY 既定を Auto にするか: 格子がコマより粗いと、どれだけ大きく焼いても輪郭は
    ///     格子の粗さのままぼやける。コマを大きくしたら格子も付いてくるのが自然な期待。
    int   resolution = 0;
    /// 上向きの加速度 = buoyancy × 温度 − weight × 密度 [領域単位/秒²]。
    float buoyancy = 1.5f;
    float weight   = 0.1f;
    /// 渦度保存 (vorticity confinement)。数値拡散で消える細かい渦を戻す。
    float vorticity = 0.3f;
    /// カールノイズで与える乱流の強さと細かさ。
    float turbulence      = 0.2f;
    float turbulenceScale = 3.0f;
    /// 散逸 [1/秒]。exp(-rate × dt) で減る。
    float densityDissipation     = 0.2f;
    float temperatureDissipation = 1.0f;
    float velocityDamping        = 0.1f;
    // ── 燃焼 ──
    /// 燃料が燃え始める温度。
    float ignitionTemperature = 0.3f;
    /// 1 秒あたりに燃える燃料の割合。
    float burnRate = 4.0f;
    /// 燃えた燃料 1 あたりの発熱・煤・膨張。膨張は圧力解法へ «湧き出し» として入り、爆風を作る。
    float burnHeat      = 3.0f;
    float burnSmoke     = 0.8f;
    float burnExpansion = 2.0f;
    /// 一定の風 (加速度) [領域単位/秒²]。場所で変わる風は FluidForce (Wind + radius) で置く。
    math::Vector3 wind = { 0.0f, 0.0f, 0.0f };
    /// 下端を床として閉じる (砂煙・煙だまり)。false なら全周が開いている。
    bool  floor = false;
    int   pressureIterations = 40;
    /// MacCormack 移流。半ラグランジュだけだと煙の輪郭が 2〜3 コマでぼやける。
    bool  sharpAdvection = true;
    /// 細部ノイズを流れに乗せて運ぶ周期 [秒]。この周期で座標を初期位置へ戻す。
    /// 長いほどノイズが流れに引き伸ばされて筋っぽくなり、短いほど入れ替わりの «揺らぎ» が見える。
    float detailPeriod = 0.8f;
};

struct FluidLiquidSettings {
    int   maxParticles   = 4000;
    /// 粒子の半径 [領域単位]。液面の細かさがこれで決まる。
    float particleRadius = 0.012f;
    float gravity        = 6.0f;
    /// XSPH 粘性 [0,1]。上げると «とろみ» が出る (血・溶岩)。
    float viscosity      = 0.05f;
    /// まとまり [0,1]。0 で飛沫が霧状に散り、1 で雫になって固まる。
    float cohesion       = 0.3f;
    int   solverIterations = 4;
    bool  floor         = true;
    float floorHeight   = -0.9f;
    float floorFriction = 0.4f;
    /// 粒子の寿命 [秒]。0 は無限。正なら飛沫が細りながら消える。
    float particleLifetime = 0.0f;
};

inline constexpr int kFluidRampStops = 4;

struct FluidColorStop {
    /// リニア (HDR 可)。
    math::Vector3 color = { 0.0f, 0.0f, 0.0f };
    float position = 0.0f;
};

/// 4 点の折れ線グラデーション。position は昇順であること。
struct FluidColorRamp {
    std::array<FluidColorStop, kFluidRampStops> stops{};
};

struct FluidRenderSettings {
    FluidShading shading = FluidShading::Smoke;
    /// 煙の地の色 (光が当たった側) と影の色。Glow では発光色。sRGB。
    math::Vector4 smokeColor  = { 0.62f, 0.62f, 0.64f, 1.0f };
    math::Vector4 shadowColor = { 0.10f, 0.09f, 0.09f, 1.0f };
    /// 密度 → 光学的厚さの倍率。上げると濃く不透明になる。
    float opacity    = 4.0f;
    /// 自己影の吸収係数。0 で影なし。
    float selfShadow = 3.0f;
    /// 絵の中での光の向き (x: 右, y: 上)。
    math::Vector3 lightDirection = { -0.4f, 0.9f, 0.0f };
    /// 格子より細かい起伏 (流れに乗せたノイズ) の強さと細かさ。0 で格子の解像度のまま。
    /// WHY 格子を細かくするだけで済ませないか: 解像度を倍にすると焼き時間は 8 倍になる。
    ///     細部は «流れに沿って動くこと» さえ守れば物理で解かなくても見分けが付かない。
    /// 倍率は `1 ± detailStrength` (2D / 3D で同じ意味。1.0 = 密度 ±100 %)。
    /// 0.375 で 0.62〜1.38 倍。1 に近づけるほど «消える所» と «倍になる所» が同時に出る。
    ///
    /// WHY 2D と 3D で揃えているか: 以前は 3D だけ «× 2 / 正規化なし» で、同じ値の効きが
    ///     1.9 倍違った。同じ項目名で意味が違うと、片方で詰めた値がもう片方で通じない。
    ///     正本は 2D = FluidBaker.cpp の DetailNoise、3D = VolumeRaymarch.hlsl の DetailFactor。
    float detailStrength = 0.375f;
    /// WHY 7 で止めるか: 最小の特徴は `1 / (detailScale × 8.37)` (4 オクターブ目は基本の 8.37 倍)。
    ///     10 だと 0.0119 で、128³ のボクセル幅 0.0156 を下回る = 標本化できずちらつくだけになる。
    float detailScale    = 7.0f;
    // ── 炎 ──
    /// 温度 1.0 を何ケルビンとみなすか。輝度は T^4 で増える。
    float fireKelvin    = 1500.0f;
    float fireIntensity = 1.0f;
    /// 温度 (0〜1) → 発光の色を自分で決める。false なら shading から導く (Fire = 黒体 / Glow = 発光色)。
    /// 2D の Fire / Glow と、3D (黒体放射を切っているとき) の両方が使う。
    bool useEmissionRamp = false;
    FluidColorRamp emissionRamp = { { FluidColorStop{ { 0.0f, 0.0f, 0.0f }, 0.0f },
                                      FluidColorStop{ { 0.5f, 0.05f, 0.0f }, 0.33f },
                                      FluidColorStop{ { 2.5f, 0.9f, 0.2f }, 0.66f },
                                      FluidColorStop{ { 6.0f, 5.0f, 4.0f }, 1.0f } } };
    /// 色の鍵 (FluidSource::colorKey) → 散乱の色 (リニア)。2D の煙 / 液の地の色と、3D の Albedo Ramp の両方が使う。
    /// false なら smokeColor / liquidColor の 1 色。
    bool useAlbedoRamp = false;
    FluidColorRamp albedoRamp = { { FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.0f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.33f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.66f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 1.0f } } };
    // ── 液体 ──
    math::Vector4 liquidColor = { 0.30f, 0.55f, 0.85f, 0.85f };
    /// 描画の半径 (粒子半径に対する倍率) と、液面とみなす閾値。
    float liquidRadiusScale = 2.5f;
    float liquidThreshold   = 0.5f;
    float specular          = 0.8f;
    /// 3D (Volume Flipbook Baker) の液面: 縁の柔らかさ・濃さ (大きいほど不透明)・艶・正面の反射率。
    float liquidSoftness   = 0.08f;
    float liquidExtinction = 60.0f;
    float liquidGloss      = 96.0f;
    float liquidFresnel    = 0.02f;
};

struct FluidOutputSettings {
    int   frameSize = 256;
    int   columns   = 8;
    int   rows      = 8;
    /// 1 コマを何倍の解像度で描いてから縮めるか (1〜4)。液面の縁のギザギザと細かい煙の瞬きを消す。
    int   supersampling = 1;
    /// 焼く時間 [秒] と、焼き始める前に回しておく時間。
    float duration  = 2.0f;
    float warmup    = 0.0f;
    /// 1 コマあたりの最低ソルバー刻み数 (速い流れでは CFL で自動的に増える)。
    int   substeps  = 2;
    /// 最後のコマから最初のコマへつながるよう、末尾を先頭へクロスフェードする (FPS 再生用)。
    bool  loop      = false;
    bool  motionVectors = true;
    /// 気体のみ: 3D で解き直して .vfield を焼く (粒子を同じ流れに乗せる)。
    bool  vectorField = false;
    int   vectorFieldResolution = 32;
    /// .vfield の半分の大きさ [m]。
    math::Vector3 vectorFieldExtents = { 2.0f, 2.0f, 2.0f };
};

struct FluidRecipe {
    FluidKind kind = FluidKind::Gas;
    uint32_t  seed = 1;
    FluidGasSettings gas;
    FluidLiquidSettings liquid;
    /// 発生源 (kMaxFluidSources まで)。気体と液体で共通の部品で、kind が読み替える。
    std::vector<FluidSource> sources;
    /// 力 (kMaxFluidForces まで)。気体・液体の両方に効く。
    std::vector<FluidForce> forces;
    /// 障害物 (kMaxFluidColliders まで)。気体・液体の両方に効く。
    std::vector<FluidCollider> colliders;
    FluidRenderSettings render;
    FluidOutputSettings output;
    FluidBakeSettings bake;
};

/// WHY 放電 (PlasmaBurst / ArcHaze) が «雷» そのものでないか:
///   稲妻の線は VFX Line (手続きメッシュ) の担当で、格子で解くと線が数セルに潰れて消える。
///   流体が受け持つのは線の «周りの空気» — 弾けた瞬間の球と、後に残る靄。
enum class FluidPreset : uint8_t {
    Smoke = 0, Fire, Explosion, Steam, DustBurst, Ink, MagicWisp, HeatHaze,
    WaterSplash, WaterJet, BloodBurst, LavaBlob, PlasmaBurst, ArcHaze,
    GroundRing, ColdMist, ChargeVortex, EmberBurst, SigilFlare,
    Count
};

[[nodiscard]] const char* FluidPresetName(FluidPreset preset);

/// 焼けば «それらしい絵» が出る出発点。中身はユーザーが組むのと同じ部品 (sources / forces) で書いてある。
[[nodiscard]] FluidRecipe MakeFluidPreset(FluidPreset preset);

/// 2D ベイクで実際に使う格子の 1 辺 (Auto を解決した値)。
[[nodiscard]] int ResolveGasResolution(const FluidRecipe& recipe);

/// .fluid (TOML) の読み書き。欠けたキーは既定値のまま。例外は使わない。
/// 保存は version 2 (部品は [[source]] / [[force]])。version 1 の gas_source / liquid_emitter は
/// 読むときに今の kind の側だけを sources へ移す。
[[nodiscard]] bool LoadFluidRecipe(const std::string& absPath, FluidRecipe& outRecipe,
                                   std::string* outError = nullptr);
[[nodiscard]] bool SaveFluidRecipe(const std::string& absPath, const FluidRecipe& recipe);

/// AI (JSON)・スキーマ問い合わせ用の項目一覧。フィールド名は .fluid (TOML) のキーと同じ
/// (部品の配列は "source" / "force"、動きは "motion"・量のエンベロープは "amount" の中の "key" 配列)。
/// WHY TOML の読み書きをこれで置き換えないか: TOML は enum を文字列で書いており、IReflector 経由の
///     書き方と一致しない。既存ファイルを壊さないよう TOML は手書きのまま残し、両者の一致はテスト
///     (FluidRecipeBakeTests) で縛る。
void ReflectFluidRecipe(FluidRecipe& recipe, scene::IReflector& r);

} // namespace fbzz::asset
