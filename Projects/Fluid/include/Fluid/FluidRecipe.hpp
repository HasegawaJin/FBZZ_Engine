/// @file    FluidRecipe.hpp
/// @brief   流体エフェクトのレシピの設定型 (全体の設定 + 発生源・力の部品リスト + 見た目・出力)
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note ここに置くのは «設定の形» だけ。.fluid (TOML) の読み書き・Reflect・プリセットは
///       Engine の FluidRecipeCodec が担当する。ファイル形式を Fluid が知らないことで、
///       ソルバーは Math だけに依存したまま実行時からも呼べる。
///
/// @note 気体と液体を 1 つのレシピにするのは、解き方が違っても «発生源 → ソルバー → 1 コマの絵 →
///       アトラス → .mat» という流れが同じで、出力・見た目・素材化の設定を共有できるため。
///       種類ごとに別アセットにすると、焼き・Inspector・.mat 生成が 2 本ずつになる。
///
/// @note 部品 (発生源・力) を種類別のリストにしてノードグラフにしないのは、同じレシピを
///       CPU 2D / CPU 3D / 液体 PBF / GPU の 4 通りで解くため。部品の «種類» と «ソルバーの段»
///       (発生源 = 注入、力 = 外力) が 1 対 1 なら、部品を 1 つ足しても実装箇所は段ごとに 1 つで済む。
///       計算の順番まで自由にすると、それが 4 通り × 任意の順になって追いつかない。
/// @see Fluid/FluidOperatorEval.hpp 形と力の式の正本 (GPU は FluidGpuCommon.hlsli に 1:1 の写し)。
/// @see Docs/design/fluid-library.md
#pragma once

#include <Fluid/FluidBakeSettings.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::fluid {

enum class FluidKind : uint8_t {
    Gas = 0,  ///< @brief 格子で解く気体 (煙・炎・爆発・蒸気・砂煙・インク・霞・陽炎)
    Liquid,   ///< @brief 粒子で解く液体 (水しぶき・噴流・血・溶岩)
};

/// @brief 焼いた絵の作り方。ブレンドと alpha の意味もここで決まる。
enum class FluidShading : uint8_t {
    Smoke = 0,   ///< @brief 自己影つきの煙。Alpha ブレンド
    Fire,        ///< @brief 黒体放射の炎 + 煤。Premultiplied (芯は光り、煙は背景を隠す)
    Glow,        ///< @brief 密度をそのまま発光に。Additive (魔法の靄・霊気)
    Distortion,  ///< @brief 速度を RG の変位に。歪み (陽炎・衝撃波)
    Liquid,      ///< @brief メタボールの液面。法線とスペキュラを焼き込む。Alpha ブレンド
};

/// @brief 1 つのレシピに置ける部品の数。GPU の定数バッファに載る数に揃える (CPU だけ多く置けると焼き分けで絵が変わる)。
inline constexpr int kMaxFluidSources = 16;
inline constexpr int kMaxFluidForces = 8;
inline constexpr int kMaxFluidColliders = 8;
inline constexpr int kMaxFluidMotionKeys = 8;
/// @brief 量のエンベロープのキーの数。動きのキーと同じ数に揃える。
/// @note 定数バッファに載らないのに上限を決める理由: 倍率は刻みごとに CPU が畳んで 1 つの float で渡すため
///       GPU の枠は要らないが、「ファイルと AI から来る配列は上限で切る」規則を動きと違えると、
///       どちらが何個まで置けるか覚えていられなくなる。
inline constexpr int kMaxFluidAmountKeys = 8;

enum class FluidSourceShape : uint8_t {
    Sphere = 0,  ///< @brief size.x = 半径
    Box,         ///< @brief size = 各軸の半分の大きさ
    Cone,        ///< @brief 頂点が center、direction へ開く。size.x = 底の半径、size.y = 長さ (噴流・火炎放射)
    Ring,        ///< @brief direction を法線とする輪。size.x = 輪の半径、size.y = 管の太さ (半径) (衝撃波・煙の輪)
    /// @brief 画像の形に湧く (文字・ロゴ・魔法陣)。direction を法線とする板に texture を貼り、その濃さ (輝度 × α) で注ぐ。
    /// @brief size.x / size.y = 板の半幅 / 半高さ、size.z = 板の厚みの半分。軸の決め方は FluidTextureSourceBasis。
    Texture,
    /// @brief center を通り direction を軸とする線分に、半径ぶんの肉を付けた形 (腕・脚・棒・パイプ・武器)。
    /// @brief size.x = 半径、size.y = 芯の線分の半分の長さ (両端に半球が付くので、端から端までは 2 × (size.y + size.x))。
    /// @brief size.z は見ない。
    Capsule,
    /// @brief center を通り direction を軸とする平らな端の柱 (煙突・通気口から柱状に立ち上る煙)。
    /// @brief size.x = 半径、size.y = 半分の高さ。size.z は見ない。端の近くだけ重みがなめらかに 0 へ落ちる。
    Cylinder,
};

/// @brief 時刻 → 中心からのずれ。キーの間は直線でつなぐ。
struct FluidMotionKey {
    float time = 0.0f;
    math::Vector3 offset = { 0.0f, 0.0f, 0.0f };
};

/// @brief 部品の動き。キーが無ければ動かない。先頭より前・末尾より後は端のキーで止まる。
struct FluidMotion {
    /// @brief time の昇順。kMaxFluidMotionKeys まで。
    std::vector<FluidMotionKey> keys;
    /// @brief 動く速さを流速 (気体) / 撃ち出す速度 (液体) に足す。動く発生源が周りを引きずる。
    bool inheritVelocity = true;
};

/// @brief 時刻 → 量の倍率。キーの間は直線でつなぐ。
struct FluidAmountKey {
    float time = 0.0f;
    float scale = 1.0f;
};

/// @brief 部品が「どれだけ」出しているかの時間変化。キーが空なら常に倍率 1 (エンベロープが無いのと 1 ビットも
/// @brief 変わらない)。先頭より前・末尾より後は端のキーの値で止まる (外挿しない)。式は FluidOperatorEval.hpp の
/// @brief SampleFluidAmount。
/// @note 位置と velocity には掛けない理由: 「どこに居るか」は FluidMotion の担当。同じ動きを 2 つの表から
///       作れると、直すときにどちらを触るのか決まらない。ここは「勢い」だけを持つ。
/// @note 倍率 (絶対値でなく) にする理由: 部品の density / strength に掛けるので、基準の値を 1 か所で直せば
///       エンベロープの形を書き直さずに全体の強さを変えられる。
struct FluidAmount {
    /// @brief time の昇順。kMaxFluidAmountKeys まで。
    std::vector<FluidAmountKey> keys;
};

/// @brief 発生源。気体は格子へ密度・温度・燃料・流速を注ぎ、液体は粒子を撃ち出す (kind で読み替える)。
/// @brief 座標と大きさは「領域を各軸 [-1,1] に正規化した単位」。
/// @note 正規化単位にする理由: 焼く解像度 (64/128/256) を変えても同じ絵になるようにするため。
struct FluidSource {
    bool enabled = true;
    /// @brief 人と AI が見分けるための名前 (解き方には関係しない)。
    std::string name;
    FluidSourceShape shape = FluidSourceShape::Sphere;
    math::Vector3 center = { 0.0f, -0.6f, 0.0f };
    /// @brief 意味は shape ごと (FluidSourceShape を参照)。
    math::Vector3 size = { 0.18f, 0.18f, 0.18f };
    /// @brief Cone の開く向き / Ring・Texture の法線 / Capsule・Cylinder の軸。
    /// @brief 長さは問わない (0 なら Cone / Ring / Capsule / Cylinder は上向き、Texture は手前向き)。
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f };
    /// @brief Texture の画像 (PNG / TGA / JPG。Assets 相対・guid:・実パス)。濃さは輝度 × α。
    /// @brief Sprite 参照 (`<画像>::sprite::<ID>`) ならそのコマだけを切り抜く。
    std::string texture;

    /// @name 気体
    /// @{
    /// @brief 1 秒あたりに足す量。
    float density     = 2.0f;
    float temperature = 2.0f;
    float fuel        = 0.0f;
    /// @brief 注入量をノイズで揺らす [0,1]。完全に一様だと左右対称な「きのこ」しか出ない。
    float noise = 0.5f;
    /// @}

    /// @name 気体・液体の両方
    /// @{
    /// @brief 気体: 発生源の中の流速 (0 なら流速には触らない) / 液体: 撃ち出す速度 [領域単位/秒]。
    math::Vector3 velocity = { 0.0f, 0.0f, 0.0f };
    float startTime = 0.0f;
    /// @brief 気体: 0 以下は「最後まで出し続ける」/ 液体: 0 以下は startTime に一斉、正ならその期間に均して出す。
    float duration  = 0.0f;

    /// @brief 色の鍵 [0,1]。render.albedoRamp のどの色で描くか。気体は煙に乗って運ばれ (密度で重み付けした平均)、
    /// @brief 液体は粒子ごとに持つ。複数の発生源の煙が混ざると色も混ざる。useAlbedoRamp が false なら見た目に効かない。
    float colorKey = 0.0f;
    /// @}

    /// @name 液体
    /// @{
    /// @brief 撃ち出す速度の大きさに対するばらつき [0,1]。
    float spread = 0.5f;
    /// @brief 撃ち出す総数。
    int   count = 600;
    /// @}

    FluidMotion motion;
    /// @brief density / temperature / fuel に掛かる倍率 (気体のみ — 液体はこの 3 つを使わない)。
    FluidAmount amount;
};

/// @brief 力の種類。粒子の FlowField (FlowFieldType) と同じ語彙 (Curl は Noise。Baked は持たない)。
enum class FluidForceType : uint8_t { Wind = 0, Attract, Repulse, Vortex, Noise, Drag };

/// @brief 空間の場として流れへ加える力。式は FluidOperatorEval.hpp の FluidForceDelta。
struct FluidForce {
    bool enabled = true;
    std::string name;
    FluidForceType type = FluidForceType::Wind;
    math::Vector3 center = { 0.0f, 0.0f, 0.0f };
    /// @brief Wind: 向き / Vortex: 回転軸 (2D では常に画面の奥行き軸)。長さは問わない。
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    /// @brief 加速度 [領域単位/秒²]。Drag は減衰係数 [1/秒]。
    float strength = 2.0f;
    /// @brief 影響半径。0 以下は領域全体に一様。
    float radius = 0.0f;
    /// @brief influence = (1 - 距離/radius)^falloffPower。
    float falloffPower = 2.0f;
    /// @brief Noise: 細かさ (領域幅あたりの山の数) と、流れる速さ。
    float noiseFrequency = 3.0f;
    float noiseSpeed = 1.0f;
    float startTime = 0.0f;
    /// @brief 0 以下はずっと効く。
    float duration = 0.0f;
    FluidMotion motion;
    /// @brief strength に掛かる倍率。
    FluidAmount amount;
};

enum class FluidColliderShape : uint8_t {
    Sphere = 0,  ///< @brief size.x = 半径
    Box,         ///< @brief size = 各軸の半分 (軸に沿った箱。回転は持たない)
    Plane,       ///< @brief center を通り direction を法線とする面。法線の反対側がすべて固体 (壁・斜めの床)
    /// @brief center を通り direction を軸とする線分に、半径ぶんの肉を付けた形 (腕・脚・棒・パイプ)。
    /// @brief size.x = 半径、size.y = 芯の線分の半分の長さ (両端は半球)。size.z は見ない。
    Capsule,
    /// @brief center を通り direction を軸とする平らな端の柱。size.x = 半径、size.y = 半分の高さ。size.z は見ない。
    Cylinder,
};

/// @brief 流体が通り抜けない障害物。距離と法線の式は FluidOperatorEval.hpp の FluidColliderDistance / Normal。
/// @brief 気体: 中のセルは流れが止まり (動いていれば動きの速度で押し)、煙も入らない。
/// @brief 液体: 粒子を表面の外 (粒子半径ぶん) へ押し出し、表面に沿った速度を friction で落とす。
/// @brief 床 (gas.floor / liquid.floor) は今までどおり別に持つ (障害物に含めると既存の .fluid の結果が変わる)。
struct FluidCollider {
    bool enabled = true;
    std::string name;
    FluidColliderShape shape = FluidColliderShape::Sphere;
    math::Vector3 center = { 0.0f, 0.0f, 0.0f };
    math::Vector3 size = { 0.2f, 0.2f, 0.2f };
    /// @brief Plane の法線 (こちら側が流体) / Capsule・Cylinder の軸。長さは問わない (0 なら上向き)。
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f };
    /// @brief 液体: 表面に沿った速度を落とす強さ (liquid.floorFriction と同じ意味)。
    float friction = 0.4f;
    float startTime = 0.0f;
    /// @brief 0 以下はずっと居る。
    float duration = 0.0f;
    /// @brief inheritVelocity なら、動く障害物が流体を押しのける (気体は中の流速 = 動きの速度)。
    FluidMotion motion;
};

struct FluidGasSettings {
    /// @brief 2D ベイクの格子の 1 辺。0 は Auto (コマの解像度に合わせる。上限 256)。
    /// @note 既定を Auto にする理由: 格子がコマより粗いと、どれだけ大きく焼いても輪郭は
    ///       格子の粗さのままぼやける。コマを大きくしたら格子も付いてくるのが自然な期待。
    int   resolution = 0;
    /// @brief 上向きの加速度 = buoyancy × 温度 − weight × 密度 [領域単位/秒²]。
    float buoyancy = 1.5f;
    float weight   = 0.1f;
    /// @brief 渦度保存 (vorticity confinement)。数値拡散で消える細かい渦を戻す。
    float vorticity = 0.3f;
    /// @brief カールノイズで与える乱流の強さと細かさ。
    float turbulence      = 0.2f;
    float turbulenceScale = 3.0f;
    /// @brief 散逸 [1/秒]。exp(-rate × dt) で減る。
    float densityDissipation     = 0.2f;
    float temperatureDissipation = 1.0f;
    float velocityDamping        = 0.1f;
    /// @name 燃焼
    /// @{
    /// @brief 燃料が燃え始める温度。
    float ignitionTemperature = 0.3f;
    /// @brief 1 秒あたりに燃える燃料の割合。
    float burnRate = 4.0f;
    /// @brief 燃えた燃料 1 あたりの発熱・煤・膨張。膨張は圧力解法へ「湧き出し」として入り、爆風を作る。
    float burnHeat      = 3.0f;
    float burnSmoke     = 0.8f;
    float burnExpansion = 2.0f;
    /// @}
    /// @brief 一定の風 (加速度) [領域単位/秒²]。場所で変わる風は FluidForce (Wind + radius) で置く。
    math::Vector3 wind = { 0.0f, 0.0f, 0.0f };
    /// @brief 下端を床として閉じる (砂煙・煙だまり)。false なら全周が開いている。
    bool  floor = false;
    int   pressureIterations = 40;
    /// @brief MacCormack 移流。半ラグランジュだけだと煙の輪郭が 2〜3 コマでぼやける。
    bool  sharpAdvection = true;
    /// @brief 細部ノイズを流れに乗せて運ぶ周期 [秒]。この周期で座標を初期位置へ戻す。
    /// @brief 長いほどノイズが流れに引き伸ばされて筋っぽくなり、短いほど入れ替わりの «揺らぎ» が見える。
    float detailPeriod = 0.8f;
};

struct FluidLiquidSettings {
    int   maxParticles   = 4000;
    /// @brief 粒子の半径 [領域単位]。液面の細かさがこれで決まる。
    float particleRadius = 0.012f;
    float gravity        = 6.0f;
    /// @brief XSPH 粘性 [0,1]。上げると «とろみ» が出る (血・溶岩)。
    float viscosity      = 0.05f;
    /// @brief まとまり [0,1]。0 で飛沫が霧状に散り、1 で雫になって固まる。
    float cohesion       = 0.3f;
    int   solverIterations = 4;
    bool  floor         = true;
    float floorHeight   = -0.9f;
    float floorFriction = 0.4f;
    /// @brief 粒子の寿命 [秒]。0 は無限。正なら飛沫が細りながら消える。
    float particleLifetime = 0.0f;
};

inline constexpr int kFluidRampStops = 4;

struct FluidColorStop {
    /// @brief リニア (HDR 可)。
    math::Vector3 color = { 0.0f, 0.0f, 0.0f };
    float position = 0.0f;
};

/// @brief 4 点の折れ線グラデーション。position は昇順であること。
struct FluidColorRamp {
    std::array<FluidColorStop, kFluidRampStops> stops{};
};

struct FluidRenderSettings {
    FluidShading shading = FluidShading::Smoke;
    /// @brief 煙の地の色 (光が当たった側) と影の色。Glow では発光色。sRGB。
    math::Vector4 smokeColor  = { 0.62f, 0.62f, 0.64f, 1.0f };
    math::Vector4 shadowColor = { 0.10f, 0.09f, 0.09f, 1.0f };
    /// @brief 密度 → 光学的厚さの倍率。上げると濃く不透明になる。
    float opacity    = 4.0f;
    /// @brief 自己影の吸収係数。0 で影なし。
    float selfShadow = 3.0f;
    /// @brief 絵の中での光の向き (x: 右, y: 上)。
    math::Vector3 lightDirection = { -0.4f, 0.9f, 0.0f };
    /// @brief 格子より細かい起伏 (流れに乗せたノイズ) の強さと細かさ。0 で格子の解像度のまま。
    /// @brief 倍率は `1 ± detailStrength` (2D / 3D で同じ意味。1.0 = 密度 ±100 %)。
    /// @brief 0.375 で 0.62〜1.38 倍。1 に近づけるほど「消える所」と「倍になる所」が同時に出る。
    /// @note 格子を細かくするだけで済ませない理由: 解像度を倍にすると焼き時間は 8 倍になる。
    ///       細部は「流れに沿って動くこと」さえ守れば物理で解かなくても見分けが付かない。
    /// @note 2D と 3D で揃えている理由: 以前は 3D だけ「× 2・正規化なし」で、同じ値の効きが 1.9 倍違った。
    ///       正本は 2D = FluidBaker.cpp の DetailNoise、3D = VolumeRaymarch.hlsl の DetailFactor。
    float detailStrength = 0.375f;
    /// @note 7 で止める理由: 最小の特徴は `1 / (detailScale × 8.37)` (4 オクターブ目は基本の 8.37 倍)。
    ///       10 だと 0.0119 で、128³ のボクセル幅 0.0156 を下回り標本化できずちらつくだけになる。
    float detailScale    = 7.0f;
    /// @name 炎
    /// @{
    /// @brief 温度 1.0 を何ケルビンとみなすか。輝度は T^4 で増える。
    float fireKelvin    = 1500.0f;
    float fireIntensity = 1.0f;
    /// @brief 温度 (0〜1) → 発光の色を自分で決める。false なら shading から導く (Fire = 黒体 / Glow = 発光色)。
    /// @brief 2D の Fire / Glow と、3D (黒体放射を切っているとき) の両方が使う。
    bool useEmissionRamp = false;
    FluidColorRamp emissionRamp = { { FluidColorStop{ { 0.0f, 0.0f, 0.0f }, 0.0f },
                                      FluidColorStop{ { 0.5f, 0.05f, 0.0f }, 0.33f },
                                      FluidColorStop{ { 2.5f, 0.9f, 0.2f }, 0.66f },
                                      FluidColorStop{ { 6.0f, 5.0f, 4.0f }, 1.0f } } };
    /// @brief 色の鍵 (FluidSource::colorKey) → 散乱の色 (リニア)。2D の煙 / 液の地の色と、3D の Albedo Ramp の両方が使う。
    /// @brief false なら smokeColor / liquidColor の 1 色。
    bool useAlbedoRamp = false;
    FluidColorRamp albedoRamp = { { FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.0f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.33f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.66f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 1.0f } } };
    /// @}
    /// @name 液体
    /// @{
    math::Vector4 liquidColor = { 0.30f, 0.55f, 0.85f, 0.85f };
    /// @brief 描画の半径 (粒子半径に対する倍率) と、液面とみなす閾値。
    float liquidRadiusScale = 2.5f;
    float liquidThreshold   = 0.5f;
    float specular          = 0.8f;
    /// @brief 3D (Volume Flipbook Baker) の液面: 縁の柔らかさ・濃さ (大きいほど不透明)・艶・正面の反射率。
    float liquidSoftness   = 0.08f;
    float liquidExtinction = 60.0f;
    float liquidGloss      = 96.0f;
    float liquidFresnel    = 0.02f;
    /// @}
};

struct FluidOutputSettings {
    int   frameSize = 256;
    int   columns   = 8;
    int   rows      = 8;
    /// @brief 1 コマを何倍の解像度で描いてから縮めるか (1〜4)。液面の縁のギザギザと細かい煙の瞬きを消す。
    int   supersampling = 1;
    /// @brief 焼く時間 [秒] と、焼き始める前に回しておく時間。
    float duration  = 2.0f;
    float warmup    = 0.0f;
    /// @brief 1 コマあたりの最低ソルバー刻み数 (速い流れでは CFL で自動的に増える)。
    int   substeps  = 2;
    /// @brief 最後のコマから最初のコマへつながるよう、末尾を先頭へクロスフェードする (FPS 再生用)。
    bool  loop      = false;
    bool  motionVectors = true;
    /// @brief 気体のみ: 3D で解き直して 速度場 PNG を焼く (粒子を同じ流れに乗せる)。
    bool  vectorField = false;
    int   vectorFieldResolution = 32;
    /// @brief 速度場 PNG の半分の大きさ [m]。
    math::Vector3 vectorFieldExtents = { 2.0f, 2.0f, 2.0f };
};

struct FluidRecipe {
    FluidKind kind = FluidKind::Gas;
    uint32_t  seed = 1;
    FluidGasSettings gas;
    FluidLiquidSettings liquid;
    /// @brief 発生源 (kMaxFluidSources まで)。気体と液体で共通の部品で、kind が読み替える。
    std::vector<FluidSource> sources;
    /// @brief 力 (kMaxFluidForces まで)。気体・液体の両方に効く。
    std::vector<FluidForce> forces;
    /// @brief 障害物 (kMaxFluidColliders まで)。気体・液体の両方に効く。
    std::vector<FluidCollider> colliders;
    FluidRenderSettings render;
    FluidOutputSettings output;
    FluidBakeSettings bake;
};

/// @brief 2D ベイクで実際に使う格子の 1 辺 (Auto を解決した値)。
[[nodiscard]] int ResolveGasResolution(const FluidRecipe& recipe);

} // namespace fbzz::fluid
