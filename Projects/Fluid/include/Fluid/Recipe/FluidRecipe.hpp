/// @file    FluidRecipe.hpp
/// @brief   流体エフェクトのレシピの設定型 (全体の設定 + 発生源・力の部品リスト + 見た目・出力)
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note ここに置くのは «設定の形» だけ。.fluid (TOML) の読み書き・Reflect・プリセットは
/// @note Engine の FluidRecipeCodec が担当する。ファイル形式を Fluid が知らないことで、
/// @note ソルバーは Math だけに依存したまま実行時からも呼べる。

/// @note 気体と液体を 1 つのレシピにするのは、解き方が違っても «発生源 → ソルバー → 1 コマの絵 →
/// @note アトラス → .mat» という流れが同じで、出力・見た目・素材化の設定を共有できるため。
/// @note 種類ごとに別アセットにすると、焼き・Inspector・.mat 生成が 2 本ずつになる。

/// @note 部品 (発生源・力) を種類別のリストにしてノードグラフにしないのは、同じレシピを
/// @note CPU 2D / CPU 3D / 液体 PBF / GPU の 4 通りで解くため。部品の «種類» と «ソルバーの段»
/// @note (発生源 = 注入、力 = 外力) が 1 対 1 なら、部品を 1 つ足しても実装箇所は段ごとに 1 つで済む。
/// @note 計算の順番まで自由にすると、それが 4 通り × 任意の順になって追いつかない。
/// @see Fluid/Operators/FluidOperatorEval.hpp 形と力の式の正本 (GPU は FluidGpuCommon.hlsli に 1:1 の写し)。
/// @see Docs/design/fluid-library.md
#pragma once
#include <Fluid/Api.hpp>

#include <Fluid/Recipe/FluidBakeSettings.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
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
/// @note GPU の枠は要らないが、「ファイルと AI から来る配列は上限で切る」規則を動きと違えると、
/// @note どちらが何個まで置けるか覚えていられなくなる。
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
    float time = 0.0f; ///< シミュレーション開始を原点とする移動キーの時刻 [秒]。
    math::Vector3 offset = { 0.0f, 0.0f, 0.0f }; ///< 操作の基準中心からの局所変位。
};

/// @brief 部品の動き。キーが無ければ動かない。先頭より前・末尾より後は端のキーで止まる。
struct FluidMotion {
    std::vector<FluidMotionKey> keys; ///< time の昇順。kMaxFluidMotionKeys まで。
    bool inheritVelocity = true; ///< 動く速さを流速 (気体) / 撃ち出す速度 (液体) に足す。動く発生源が周りを引きずる。
};

/// @brief 時刻 → 量の倍率。キーの間は直線でつなぐ。
struct FluidAmountKey {
    float time = 0.0f; ///< シミュレーション開始を原点とする量のキーの時刻 [秒]。
    float scale = 1.0f; ///< 基準量へ掛ける倍率。
};

/// @brief 部品が「どれだけ」出しているかの時間変化。キーが空なら常に倍率 1 (エンベロープが無いのと 1 ビットも
/// @brief 変わらない)。先頭より前・末尾より後は端のキーの値で止まる (外挿しない)。式は FluidOperatorEval.hpp の
/// @brief SampleFluidAmount。
/// @note 位置と velocity には掛けない理由: 「どこに居るか」は FluidMotion の担当。同じ動きを 2 つの表から
/// @note 作れると、直すときにどちらを触るのか決まらない。ここは「勢い」だけを持つ。
/// @note 倍率 (絶対値でなく) にする理由: 部品の density / strength に掛けるので、基準の値を 1 か所で直せば
/// @note エンベロープの形を書き直さずに全体の強さを変えられる。
struct FluidAmount {
    std::vector<FluidAmountKey> keys; ///< time の昇順。kMaxFluidAmountKeys まで。
};

/// @brief 発生源。気体は格子へ密度・温度・燃料・流速を注ぎ、液体は粒子を撃ち出す (kind で読み替える)。
/// @brief 座標と大きさは「領域を各軸 [-1,1] に正規化した単位」。
/// @note 正規化単位にする理由: 焼く解像度 (64/128/256) を変えても同じ絵になるようにするため。
struct FluidSource {
    bool enabled = true; ///< 発生源を刻みの対象に含めるかどうか。
    std::string name; ///< 人と AI が見分けるための名前 (解き方には関係しない)。
    FluidSourceShape shape = FluidSourceShape::Sphere; ///< 注入範囲の境界を決める幾何形状。
    math::Vector3 center = { 0.0f, -0.6f, 0.0f }; ///< 移動キーを加える前の、局所座標での発生源の中心。
    math::Vector3 size = { 0.18f, 0.18f, 0.18f }; ///< 意味は shape ごと (FluidSourceShape を参照)。
    /// @brief Cone の開く向き / Ring・Texture の法線 / Capsule・Cylinder の軸。
    /// @brief 長さは問わない (0 なら Cone / Ring / Capsule / Cylinder は上向き、Texture は手前向き)。
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f }; ///< 円錐・カプセル・円柱の軸、またはリング・テクスチャ面の法線。
    /// @brief Texture の画像 (PNG / TGA / JPG。Assets 相対・guid:・実パス)。濃さは輝度 × α。
    /// @brief Sprite 参照 (`<画像>::sprite::<ID>`) ならそのコマだけを切り抜く。
    std::string texture; ///< テクスチャ形状の注入マスクに使う画像の参照パス。

    /// @name 気体
    /// @{
    /// @brief 1 秒あたりに足す量。
    float density     = 2.0f; ///< 単位時間当たりの煙の密度の注入量。
    float temperature = 2.0f; ///< 単位時間当たりの温度の注入量。
    float fuel        = 0.0f; ///< 単位時間当たりの燃料の注入量。
    float noise = 0.5f; ///< 注入量をノイズで揺らす [0,1]。完全に一様だと左右対称な「きのこ」しか出ない。
    /// @}

    /// @name 気体・液体の両方
    /// @{
    /// @brief 気体: 発生源の中の流速 (0 なら流速には触らない) / 液体: 撃ち出す速度 [領域単位/秒]。
    math::Vector3 velocity = { 0.0f, 0.0f, 0.0f }; ///< 気体の局所目標速度、または液体粒子の局所射出速度 [領域単位/秒]。
    float startTime = 0.0f; ///< シミュレーション開始から注入開始までの時間 [秒]。
    float duration  = 0.0f; ///< 気体: 0 以下は「最後まで出し続ける」/ 液体: 0 以下は startTime に一斉、正ならその期間に均して出す。

    /// @brief 色の鍵 [0,1]。render.albedoRamp のどの色で描くか。気体は煙に乗って運ばれ (密度で重み付けした平均)、
    /// @brief 液体は粒子ごとに持つ。複数の発生源の煙が混ざると色も混ざる。useAlbedoRamp が false なら見た目に効かない。
    float colorKey = 0.0f; ///< この発生源が注入する、色ランプ参照用のキー値。
    /// @}

    /// @name 液体
    /// @{
    /// @brief 撃ち出す速度の大きさに対するばらつき [0,1]。
    float spread = 0.5f; ///< 液体粒子の射出速度に加えるばらつき。速度の大きさに掛ける 0～1 の値。
    int   count = 600; ///< 撃ち出す総数。
    /// @}

    FluidMotion motion; ///< 発生源の中心を時間に沿って動かす設定。
    FluidAmount amount; ///< density / temperature / fuel に掛かる倍率 (気体のみ — 液体はこの 3 つを使わない)。
};

/// @brief 力の種類。粒子の FlowField (FlowFieldType) と同じ語彙 (Curl は Noise。Baked は持たない)。
enum class FluidForceType : uint8_t { Wind = 0, Attract, Repulse, Vortex, Noise, Drag };

/// @brief 空間の場として流れへ加える力。式は FluidOperatorEval.hpp の FluidForceDelta。
struct FluidForce {
    bool enabled = true; ///< 外力を刻みの対象に含めるかどうか。
    std::string name; ///< レシピ内で外力を見分ける表示名。
    FluidForceType type = FluidForceType::Wind; ///< 流体への加速・減速の計算に使う外力の種類。
    math::Vector3 center = { 0.0f, 0.0f, 0.0f }; ///< 移動キーを加える前の、局所座標での外力の中心。
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f }; ///< Wind: 向き / Vortex: 回転軸 (2D では常に画面の奥行き軸)。長さは問わない。
    float strength = 2.0f; ///< 加速度 [領域単位/秒²]。Drag は減衰係数 [1/秒]。
    float radius = 0.0f; ///< 影響半径。0 以下は領域全体に一様。
    float falloffPower = 2.0f; ///< influence = (1 - 距離/radius)^falloffPower。
    float noiseFrequency = 3.0f; ///< Noise: 細かさ (領域幅あたりの山の数) と、流れる速さ。
    float noiseSpeed = 1.0f; ///< 外力のノイズを時間方向に進める速度。
    float startTime = 0.0f; ///< シミュレーション開始から外力が有効になるまでの時間 [秒]。
    float duration = 0.0f; ///< 0 以下はずっと効く。
    FluidMotion motion; ///< 外力の中心を時間に沿って動かす設定。
    FluidAmount amount; ///< strength に掛かる倍率。
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
    bool enabled = true; ///< 障害物を刻みの対象に含めるかどうか。
    std::string name; ///< レシピ内で障害物を見分ける表示名。
    FluidColliderShape shape = FluidColliderShape::Sphere; ///< 流体を通さない境界を決める幾何形状。
    math::Vector3 center = { 0.0f, 0.0f, 0.0f }; ///< 移動キーを加える前の、局所座標での障害物の中心。
    math::Vector3 size = { 0.2f, 0.2f, 0.2f }; ///< 障害物の形状ごとの半径または局所半幅。
    math::Vector3 direction = { 0.0f, 1.0f, 0.0f }; ///< Plane の法線 (こちら側が流体) / Capsule・Cylinder の軸。長さは問わない (0 なら上向き)。
    float friction = 0.4f; ///< 液体: 表面に沿った速度を落とす強さ (liquid.floorFriction と同じ意味)。
    float startTime = 0.0f; ///< シミュレーション開始から障害物が有効になるまでの時間 [秒]。
    float duration = 0.0f; ///< 0 以下はずっと居る。
    FluidMotion motion; ///< inheritVelocity なら、動く障害物が流体を押しのける (気体は中の流速 = 動きの速度)。
};

struct FluidGasSettings {
    /// @brief 2D ベイクの格子の 1 辺。0 は Auto (コマの解像度に合わせる。上限 256)。
    /// @note 既定を Auto にする理由: 格子がコマより粗いと、どれだけ大きく焼いても輪郭は
    /// @note 格子の粗さのままぼやける。コマを大きくしたら格子も付いてくるのが自然な期待。
    int   resolution = 0; ///< 気体格子の解像度。0 は出力設定から選ぶ。
    float buoyancy = 1.5f; ///< 上向きの加速度 = buoyancy × 温度 − weight × 密度 [領域単位/秒²]。
    float weight   = 0.1f; ///< 煙の密度に応じて下向きに掛ける加速の係数。
    float vorticity = 0.3f; ///< 渦度保存 (vorticity confinement)。数値拡散で消える細かい渦を戻す。
    float turbulence      = 0.2f; ///< カールノイズで与える乱流の強さと細かさ。
    float turbulenceScale = 3.0f; ///< 乱流を加えるノイズの空間周波数。
    float densityDissipation     = 0.2f; ///< 散逸 [1/秒]。exp(-rate × dt) で減る。
    float temperatureDissipation = 1.0f; ///< 単位時間当たりに温度を減衰させる係数。
    float velocityDamping        = 0.1f; ///< 単位時間当たりに気体の速度を減衰させる係数。
    /// @name 燃焼
    /// @{
    /// @brief 燃料が燃え始める温度。
    float ignitionTemperature = 0.3f; ///< 燃料が燃え始める温度の下限。
    float burnRate = 4.0f; ///< 1 秒あたりに燃える燃料の割合。
    float burnHeat      = 3.0f; ///< 燃えた燃料 1 あたりの発熱・煤・膨張。膨張は圧力解法へ「湧き出し」として入り、爆風を作る。
    float burnSmoke     = 0.8f; ///< 燃えた燃料から生じる煙の量の倍率。
    float burnExpansion = 2.0f; ///< 燃えた燃料による目標発散の倍率。
    /// @}
    /// @brief 一定の風 (加速度) [領域単位/秒²]。場所で変わる風は FluidForce (Wind + radius) で置く。
    math::Vector3 wind = { 0.0f, 0.0f, 0.0f }; ///< 気体に加える一定の風の加速度 [領域単位/秒²]。
    bool  floor = false; ///< 下端を床として閉じる (砂煙・煙だまり)。false なら全周が開いている。
    int   pressureIterations = 40; ///< CPU の圧力投影で使う反復回数。
    bool  sharpAdvection = true; ///< MacCormack 移流。半ラグランジュだけだと煙の輪郭が 2〜3 コマでぼやける。
    float detailPeriod = 0.8f; ///< 細部ノイズを流れに乗せて運ぶ周期 [秒]。この周期で座標を初期位置へ戻す。 長いほどノイズが流れに引き伸ばされて筋っぽくなり、短いほど入れ替わりの «揺らぎ» が見える。
};

struct FluidLiquidSettings {
    int   maxParticles   = 4000; ///< 保持できる液体粒子数の上限。
    float particleRadius = 0.012f; ///< 粒子の半径 [領域単位]。液面の細かさがこれで決まる。
    float gravity        = 6.0f; ///< 液体の局所 Y 軸に沿う下向き加速度の大きさ。
    float viscosity      = 0.05f; ///< XSPH 粘性 [0,1]。上げると «とろみ» が出る (血・溶岩)。
    float cohesion       = 0.3f; ///< まとまり [0,1]。0 で飛沫が霧状に散り、1 で雫になって固まる。
    int   solverIterations = 4; ///< 刻みごとに液体粒子の密度を補正する反復回数。
    bool  floor         = true; ///< 液体粒子に底面との衝突を適用するかどうか。
    float floorHeight   = -0.9f; ///< 液体粒子が衝突する底面の局所 Y 座標。
    float floorFriction = 0.4f; ///< 液体粒子が床に接触したときの接線速度を抑える係数。
    float particleLifetime = 0.0f; ///< 粒子の寿命 [秒]。0 は無限。正なら飛沫が細りながら消える。
};

inline constexpr int kFluidRampStops = 4;

struct FluidColorStop {
    math::Vector3 color = { 0.0f, 0.0f, 0.0f }; ///< リニア (HDR 可)。
    float position = 0.0f; ///< 色ランプ上でこの色を配置する位置。左端が 0、右端が 1。
};

/// @brief 4 点の折れ線グラデーション。position は昇順であること。
struct FluidColorRamp {
    std::array<FluidColorStop, kFluidRampStops> stops{}; ///< ランプを構成する補間色と位置。
};

struct FluidRenderSettings {
    FluidShading shading = FluidShading::Smoke; ///< 気体または液体の出力で使う着色方式。
    math::Vector4 smokeColor  = { 0.62f, 0.62f, 0.64f, 1.0f }; ///< 煙の地の色 (光が当たった側) と影の色。Glow では発光色。sRGB。
    math::Vector4 shadowColor = { 0.10f, 0.09f, 0.09f, 1.0f }; ///< 煙の遮蔽部分に使う RGBA。
    float opacity    = 4.0f; ///< 密度 → 光学的厚さの倍率。上げると濃く不透明になる。
    float selfShadow = 3.0f; ///< 自己影の吸収係数。0 で影なし。
    math::Vector3 lightDirection = { -0.4f, 0.9f, 0.0f }; ///< 絵の中での光の向き (x: 右, y: 上)。
    /// @brief 格子より細かい起伏 (流れに乗せたノイズ) の強さと細かさ。0 で格子の解像度のまま。
    /// @brief 倍率は `1 ± detailStrength` (2D / 3D で同じ意味。1.0 = 密度 ±100 %)。
    /// @brief 0.375 で 0.62〜1.38 倍。1 に近づけるほど「消える所」と「倍になる所」が同時に出る。
    /// @note 格子を細かくするだけで済ませない理由: 解像度を倍にすると焼き時間は 8 倍になる。
    /// @note 細部は「流れに沿って動くこと」さえ守れば物理で解かなくても見分けが付かない。
    /// @note 2D と 3D は同じ 4 オクターブ、層オフセット、振幅正規化を使う。
    /// @note CPU 側の式は FluidRenderMath.hpp、GPU 側の対応式は VolumeRaymarch.hlsl。
    float detailStrength = 0.375f; ///< 描画時に密度へ加える細部の強さ。
    /// @note 7 で止める理由: 最小の特徴は `1 / (detailScale × 8.37)` (4 オクターブ目は基本の 8.37 倍)。
    /// @note 10 だと 0.0119 で、128³ のボクセル幅 0.0156 を下回り標本化できずちらつくだけになる。
    float detailScale    = 7.0f; ///< 描画時に加える細部の空間周波数。
    /// @name 炎
    /// @{
    /// @brief 温度 1.0 を何ケルビンとみなすか。輝度は T^4 で増える。
    float fireKelvin    = 1500.0f; ///< 温度を黒体色へ変換するときの基準温度 [K]。
    float fireIntensity = 1.0f; ///< 火炎の放射色に掛ける強度倍率。
    /// @brief 温度 (0〜1) → 発光の色を自分で決める。false なら shading から導く (Fire = 黒体 / Glow = 発光色)。
    /// @brief 2D の Fire / Glow と、3D (黒体放射を切っているとき) の両方が使う。
    bool useEmissionRamp = false; ///< 火炎の色に放射用ランプを使うかどうか。
    FluidColorRamp emissionRamp = { { FluidColorStop{ { 0.0f, 0.0f, 0.0f }, 0.0f },
                                      FluidColorStop{ { 0.5f, 0.05f, 0.0f }, 0.33f },
                                      FluidColorStop{ { 2.5f, 0.9f, 0.2f }, 0.66f },
                                      FluidColorStop{ { 6.0f, 5.0f, 4.0f }, 1.0f } } }; ///< 正規化した温度で引く放射色のランプ。
    /// @brief 色の鍵 (FluidSource::colorKey) → 散乱の色 (リニア)。2D の煙 / 液の地の色と、3D の Albedo Ramp の両方が使う。
    /// @brief false なら smokeColor / liquidColor の 1 色。
    bool useAlbedoRamp = false; ///< 煙の色にアルベド用ランプを使うかどうか。
    FluidColorRamp albedoRamp = { { FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.0f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.33f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 0.66f },
                                    FluidColorStop{ { 0.35f, 0.35f, 0.36f }, 1.0f } } }; ///< 発生源の色キーで引くリニアな散乱色のランプ。
    /// @}
    /// @name 液体
    /// @{
    math::Vector4 liquidColor = { 0.30f, 0.55f, 0.85f, 0.85f }; ///< 液体表面の色と不透明度を表す RGBA。
    float liquidRadiusScale = 2.5f; ///< 描画の半径 (粒子半径に対する倍率) と、液面とみなす閾値。
    float liquidThreshold   = 0.5f; ///< 液体の描画で表面として拾う密度の境界値。
    float specular          = 0.8f; ///< 描画する反射ハイライトの強度。
    float liquidSoftness   = 0.08f; ///< 3D (Volume Flipbook Baker) の液面: 縁の柔らかさ・濃さ (大きいほど不透明)・艶・正面の反射率。
    float liquidExtinction = 60.0f; ///< 液体内部の光の減衰に掛ける係数。
    float liquidGloss      = 96.0f; ///< 液体表面のハイライトの集中度。
    float liquidFresnel    = 0.02f; ///< 液体表面の垂直入射時の反射量。
    /// @}
};

struct FluidOutputSettings {
    int   frameSize = 256; ///< フリップブックの 1 フレームの一辺 [ピクセル]。
    int   columns   = 8; ///< フリップブックの横方向のタイル数。
    int   rows      = 8; ///< フリップブックの縦方向のタイル数。
    int   supersampling = 1; ///< 1 コマを何倍の解像度で描いてから縮めるか (1〜4)。液面の縁のギザギザと細かい煙の瞬きを消す。
    float duration  = 2.0f; ///< 焼く時間 [秒] と、焼き始める前に回しておく時間。
    float warmup    = 0.0f; ///< 最初のフレームの収録前にシミュレーションを進める時間 [秒]。
    int   substeps  = 2; ///< 1 コマあたりの最低ソルバー刻み数 (速い流れでは CFL で自動的に増える)。
    bool  loop      = false; ///< 最後のコマから最初のコマへつながるよう、末尾を先頭へクロスフェードする (FPS 再生用)。
    float loopBlendFraction = 0.25f; ///< 先頭へ重ねるコマの割合 [0,0.5]。0 は重ねずにそのまま周回する。
    bool  motionVectors = true; ///< フレーム補間用の動きベクトルも出力するかどうか。
    bool  vectorField = false; ///< 気体のみ: 3D で解き直して 速度場 PNG を焼く (粒子を同じ流れに乗せる)。
    int   vectorFieldResolution = 32; ///< 出力するベクトル場の一辺の格子点数。
    math::Vector3 vectorFieldExtents = { 2.0f, 2.0f, 2.0f }; ///< 速度場 PNG の半分の大きさ [m]。
};

struct FluidRecipe {
    FluidKind kind = FluidKind::Gas; ///< 実行時に選ぶ気体または液体のシミュレーション種別。
    uint32_t  seed = 1; ///< シミュレーションで使用する再現可能な乱数の種。
    FluidGasSettings gas; ///< 気体の格子・力・燃焼を決める設定。
    FluidLiquidSettings liquid; ///< 液体粒子の上限・力・拘束を決める設定。
    std::vector<FluidSource> sources; ///< 発生源 (kMaxFluidSources まで)。気体と液体で共通の部品で、kind が読み替える。
    std::vector<FluidForce> forces; ///< 力 (kMaxFluidForces まで)。気体・液体の両方に効く。
    std::vector<FluidCollider> colliders; ///< 障害物 (kMaxFluidColliders まで)。気体・液体の両方に効く。
    FluidRenderSettings render; ///< シミュレーション結果の着色と描画の設定。
    FluidOutputSettings output; ///< フリップブックやベクトル場の出力設定。
    FluidBakeSettings bake; ///< ボリュームを画像へベイクするときの設定。
};

/// @brief 2D ベイクで実際に使う格子の 1 辺 (Auto を解決した値)。
[[nodiscard]] FBZZ_FLUID_API int ResolveGasResolution(const FluidRecipe& recipe);

} /// @note namespace fbzz::fluid
