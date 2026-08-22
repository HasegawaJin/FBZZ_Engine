// FBZZ Engine
// ParticleEmitter.hpp | fbzz::scene
// パーティクルシミュレーション設定コンポーネント
// 発生率・寿命・速度などを保持し、System が毎フレーム粒子状態を進める。
// 描画リソースの所有は Renderer 側に分ける。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

// CS/VS 共通の GPU パーティクル 1 粒子レイアウト (96 bytes, 16-byte aligned)
// StructuredBuffer<GpuParticle> に格納し、CS が lifetime/age を更新、VS が位置を読む。
struct GpuParticle {
    math::Vector3 position;        // 12B
    float         size;            // 4B
    math::Vector3 velocity;        // 12B
    float         age;             // 4B
    math::Vector4 color;           // 16B
    float         lifetime;        // 4B
    float         rotation;        // 4B
    float         angularVelocity; // 4B
    float         spriteSeed;      // 4B
    math::Vector4 uvRect;          // 16B
    // 粒子ごとの色倍率 (colorVariation の結果)。
    // WHY: CS は毎フレーム色を CB の colorStart/End (または gradient) から作り直すため、
    //      スポーン時に配ったゆらぎがそのままでは翌フレームに消える。倍率として保持し、
    //      再計算した色へ毎フレーム掛け直すことで CPU 経路と同じ見た目になる。
    math::Vector3 colorScale;      // 12B
    float         colorScalePad;   // 4B
};

// CPU → CS へのスポーンリクエスト 1 件 (96 bytes, 16-byte aligned)
// DYNAMIC StructuredBuffer に毎フレーム書き込み、CS がリングバッファで配置する。
struct GpuSpawnEntry {
    math::Vector3 position;        // 12B
    float         lifetime;        // 4B
    math::Vector3 velocity;        // 12B
    float         size;            // 4B
    math::Vector4 colorStart;      // 16B
    // 粒子ごとの色ゆらぎ倍率 (xyz)。w は未使用。
    // WHY: 旧実装は CS 側で colorStart / CB の基準色の「比」から倍率を復元していた。
    //      基準色が黒に近いチャンネルでは比が数値的に暴れ、グラデーション使用時は
    //      そもそも基準色と無関係な色になるため復元が成立しなかった。
    //      CPU が求めた倍率をそのまま渡せば CPU 経路と必ず一致する。
    math::Vector4 colorScale;      // 16B
    math::Vector4 uvRect;          // 16B
    float         rotation;        // 4B
    float         angularVelocity; // 4B
    float         spriteSeed;      // 4B
    float         pad1;            // 4B
};

static_assert(sizeof(GpuParticle) == 96, "GpuParticle must match ParticleGpuSim.cs.hlsl (96 bytes)");
static_assert(sizeof(GpuSpawnEntry) == 96, "GpuSpawnEntry must match ParticleGpuSim.cs.hlsl (96 bytes)");

// 1 粒子が保持するトレイル履歴の最大点数。
// WHY: 固定長にして Particle を POD のまま保つ。粒子ごとに vector を持たせると
//      スポーン/消滅のたびにヒープ確保が走り、数千粒子では確保コストが支配的になる。
inline constexpr int kMaxParticleTrailPoints = 8;

struct Particle {
    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector4 color;
    float         size;
    float         age;
    float         rotation = 0.0f;
    float         angularVelocity = 0.0f;
    float         lifetime = 1.0f;
    float         startSize = 1.0f;
    float         endSize = 0.0f;
    float         spriteSeed = 0.0f;
    math::Vector4 startColor = { 1, 1, 1, 1 };
    math::Vector4 endColor = { 1, 1, 1, 0 };
    // 発生時に配る色ゆらぎ倍率。毎フレーム作り直す色へ掛け直すため保持する。
    // WHY: グラデーション使用時は color が毎フレーム上書きされるので、
    //      スポーン時に色そのものへ焼き込むとゆらぎが翌フレームに消える。
    math::Vector3 colorScale = { 1.0f, 1.0f, 1.0f };
    math::Vector4 uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    math::Vector4 nextUvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    float spriteBlend = 0.0f;
    // トレイル履歴。[0] が最新で、後ろほど古い (＝尾の先端側)。
    // 位置はシミュレーション空間で持ち、描画時に粒子本体と同じ変換を通す。
    std::array<math::Vector3, kMaxParticleTrailPoints> trailPoints{};
    uint8_t trailCount = 0;
    float   trailSampleTimer = 0.0f;
};

// ParticleCurveKey — 正規化時間に対する値 1 点。
// 固定長にしてGPU定数バッファへそのまま転送できるようにする。
struct ParticleCurveKey {
    float time = 0.0f;
    float value = 0.0f;
    bool operator==(const ParticleCurveKey&) const = default;
};

// カーブ / グラデーションのキー上限。
// WHY: 4 キーでは「立ち上がり → 保持 → 減衰 → 余韻」のような 4 区間すら表せず、
//      爆発の閃光やループする炎の呼吸を作るのに足りなかった。8 キーあれば
//      実用上の作り込みは足りる。GPU 定数バッファは float4 が 1 キー 2 点なので
//      curve 1 本あたり 4 レジスタで収まる (上限を上げる場合は HLSL 側も対で直すこと)。
inline constexpr uint32_t kMaxParticleCurveKeys = 8;

// キー間の繋ぎ方。キー単位ではなくカーブ単位に持つ。
// WHY: キー単位にすると GPU へ 1 キーあたり追加の float が要り、パッキングが崩れる。
//      実用上「このカーブ全体をなめらかにしたい / 階段にしたい」が大半で、
//      混在が要る場面はキーを増やして近似できる。
enum class ParticleCurveInterpolation : uint8_t {
    Linear = 0, // 直線
    Step,       // 次のキーまで前の値を保持 (フリップブックの段階切替・点滅)
    Smooth,     // smoothstep。始点と終点で速度 0 になり、機械的な折れ線に見えない
};

// 補間係数へ曲線モードを適用する。CPU/GPU で必ず同じ式にすること
// (GPU 側は ParticleGpuSim.cs.hlsl の ApplyCurveInterpolation)。
inline float ApplyCurveInterpolation(float alpha, ParticleCurveInterpolation mode)
{
    if (mode == ParticleCurveInterpolation::Step) return 0.0f;
    if (mode == ParticleCurveInterpolation::Smooth) return alpha * alpha * (3.0f - 2.0f * alpha);
    return alpha;
}

// ParticleCurve — 軽量カーブ。Editorで最大 kMaxParticleCurveKeys キーを編集する。
struct ParticleCurve {
    std::array<ParticleCurveKey, kMaxParticleCurveKeys> keys{{
        {0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f},
        {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}
    }};
    uint32_t keyCount = 2;
    ParticleCurveInterpolation interpolation = ParticleCurveInterpolation::Linear;

    float Evaluate(float time) const
    {
        const uint32_t count = keyCount < 1 ? 1 : (keyCount > keys.size() ? static_cast<uint32_t>(keys.size()) : keyCount);
        if (time <= keys[0].time) return keys[0].value;
        for (uint32_t index = 1; index < count; ++index) {
            if (time <= keys[index].time) {
                const float span = (std::max)(keys[index].time - keys[index - 1].time, 0.0001f);
                float alpha = (std::max)(0.0f, (std::min)(1.0f, (time - keys[index - 1].time) / span));
                alpha = ApplyCurveInterpolation(alpha, interpolation);
                return keys[index - 1].value + (keys[index].value - keys[index - 1].value) * alpha;
            }
        }
        return keys[count - 1].value;
    }
};

struct ParticleGradientKey {
    float time = 0.0f;
    math::Vector4 color = { 1, 1, 1, 1 };
    bool operator==(const ParticleGradientKey& other) const
    {
        return time == other.time
            && color.x == other.color.x && color.y == other.color.y
            && color.z == other.color.z && color.w == other.color.w;
    }
};

// ParticleGradient — GPU転送可能な色Gradient。キー上限はカーブと共通。
// Step 補間は「炎から煙へ切り替わる瞬間」のような硬い変化を作るのに使う。
//
// 色空間の規約 (エンジン全体で 1 つ):
//   キーの RGB は「カラーピッカーに表示される値」= sRGB でオーサリングする。
//   RGB は 1 を超えてよい (HDR)。シェーダーへ渡る直前に一度だけリニアへ変換する。
//   CPU 経路は EvaluateLinear、GPU 経路は ParticleGpuSim.cs.hlsl の EvaluateGradient8 が
//   同じ順序 (補間 → リニア化) で処理する。片方だけ変えると CPU/GPU で色が食い違う。
struct ParticleGradient {
    std::array<ParticleGradientKey, kMaxParticleCurveKeys> keys{{
        {0.0f, {1, 1, 1, 1}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}}
    }};
    uint32_t keyCount = 2;
    ParticleCurveInterpolation interpolation = ParticleCurveInterpolation::Linear;
    ParticleColorSpace colorSpace = ParticleColorSpace::Gamma;

    // オーサリング空間 (sRGB) で評価する。Editor のプレビュー帯やカラーピッカーは
    // こちらを使う (ImGui は sRGB 値を受け取る前提のため)。
    math::Vector4 Evaluate(float time) const
    {
        const uint32_t count = keyCount < 1 ? 1 : (keyCount > keys.size() ? static_cast<uint32_t>(keys.size()) : keyCount);
        if (time <= keys[0].time) return keys[0].color;
        for (uint32_t index = 1; index < count; ++index) {
            if (time <= keys[index].time) {
                const float span = (std::max)(keys[index].time - keys[index - 1].time, 0.0001f);
                float alpha = (std::max)(0.0f, (std::min)(1.0f, (time - keys[index - 1].time) / span));
                alpha = ApplyCurveInterpolation(alpha, interpolation);
                return MixKeys(keys[index - 1].color, keys[index].color, alpha);
            }
        }
        return keys[count - 1].color;
    }

    // 描画へ渡すリニア色。シミュレーションが粒子へ書くのは常にこちら。
    math::Vector4 EvaluateLinear(float time) const
    {
        return ParticleSrgbToLinear(Evaluate(time));
    }

    // 2 キーを colorSpace に従って混ぜる。アルファは常に線形 (不透明度は光量ではないため)。
    math::Vector4 MixKeys(const math::Vector4& a, const math::Vector4& b, float alpha) const
    {
        const float w = a.w + (b.w - a.w) * alpha;
        switch (colorSpace) {
        case ParticleColorSpace::Linear: {
            const math::Vector4 la = ParticleSrgbToLinear(a);
            const math::Vector4 lb = ParticleSrgbToLinear(b);
            const math::Vector4 mixed = { la.x + (lb.x - la.x) * alpha,
                                          la.y + (lb.y - la.y) * alpha,
                                          la.z + (lb.z - la.z) * alpha, w };
            return ParticleLinearToSrgb(mixed);
        }
        case ParticleColorSpace::Oklab: {
            const math::Vector4 la = ParticleSrgbToLinear(a);
            const math::Vector4 lb = ParticleSrgbToLinear(b);
            const math::Vector3 oa = ParticleLinearToOklab({ la.x, la.y, la.z });
            const math::Vector3 ob = ParticleLinearToOklab({ lb.x, lb.y, lb.z });
            const math::Vector3 om = { oa.x + (ob.x - oa.x) * alpha,
                                       oa.y + (ob.y - oa.y) * alpha,
                                       oa.z + (ob.z - oa.z) * alpha };
            const math::Vector3 back = ParticleOklabToLinear(om);
            return ParticleLinearToSrgb({ back.x, back.y, back.z, w });
        }
        default:
            return { a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha,
                     a.z + (b.z - a.z) * alpha, w };
        }
    }
};

// ParticleBurst — 再生時間上の繰り返しBurst設定。
struct ParticleBurst {
    float time = 0.0f;
    int count = 10;
    int cycles = 1;
    float interval = 0.1f;
    float probability = 1.0f;
};

// MeshShapeVertex — MeshSurface が静的頂点とスキンウェイトを共通形式で保持する。
// WHY: AssetManager 所有の Model を参照し続けず、スポーン時は必要な頂点情報だけを高速に抽選する。
struct MeshShapeVertex {
    math::Vector3 position;
    uint32_t boneIndices[4] = {};
    float boneWeights[4] = {};
    bool skinned = false;
};

// materialPath 未設定の Emitter が使う既定 .mat。
// WHY: 未設定は 1x1 白テクスチャへ落ちるが、白は alpha=1 なので粒子が「色付きの
//      不透明な四角」として描かれる。素材の付け忘れが最も分かりにくい形で現れるうえ、
//      柔らかい既定さえあれば置いた瞬間から煙にも光にも見える。加算の丸い光を既定にする。
// NOTE: これが無いプロジェクトでは従来どおり白へ落ちる (存在しなくても壊れない)。
inline constexpr const char* PARTICLE_FALLBACK_MATERIAL =
    "Assets/Materials/Particles/ParticleFallback.mat";

struct ParticleEmitter {
    math::Vector3 emitPosition   = {};
    math::Vector3 emitVelocity   = { 0.0f, 4.0f, 0.0f };
    // 初速のばらつき [m/s]。emitVelocity の 3 軸へ等方に ±velocitySpread を加える。
    // Sphere / Cone 形状が持つ方向成分 (shapeVelocity) とは別枠で、こちらは純粋な揺らぎ。
    float         velocitySpread = 1.5f;
    math::Vector4 colorStart     = { 1.0f, 0.7f, 0.2f, 1.0f };
    math::Vector4 colorEnd       = { 1.0f, 0.1f, 0.0f, 0.0f };
    float         sizeStart      = 0.4f;
    float         sizeEnd        = 0.05f;
    // ビルボードの縦横比。size に対する軸ごとの倍率で、xy のみ使う (z は Mesh Particle 用)。
    // WHY: 粒子サイズが等方の正方形しか作れないと、縦に伸びる炎・平たい衝撃波・
    //      横に流れる煙といった AAA で常用する形が組めない。粒子ごとではなく
    //      エミッター単位の値なので、per-particle データではなく定数バッファへ載せる
    //      (頂点フォーマットを太らせずに済む)。
    math::Vector3 sizeAxisScale  = { 1.0f, 1.0f, 1.0f };
    float         lifetime       = 2.0f;
    float         lifetimeRandom = 0.0f;
    float         emitRate       = 30.0f;
    int           maxParticles   = 300;
    // WHY: 固定重力では炎・煙・火花の挙動を作り分けられないため、エミッター単位で加速度を持つ。
    math::Vector3 gravity        = { 0.0f, -5.0f, 0.0f };
    // WHY: std::rand() のグローバル状態を避け、エミッター単位で再現可能な分布にする。
    uint32_t      randomSeed     = 1;
    bool          enabled        = true;

    // 再生状態。enabled は Component の有効/無効、playing はエフェクト再生を表す。
    bool  playing     = true;
    bool  loop        = true;
    float duration    = 5.0f;
    float startDelay  = 0.0f;
    bool  clearOnStop = false;

    ParticleEmitterShape shape = ParticleEmitterShape::Point;
    float         sphereRadius = 1.0f;
    float         coneAngleDegrees = 25.0f;
    float         coneRadius = 1.0f;
    math::Vector3 boxExtents = { 1.0f, 1.0f, 1.0f };
    // FBX / Model の頂点群を発生位置として使う。meshIndex < 0 なら全サブメッシュを結合する。
    // WHY: CPU/GPUの両モードで同じスポーンバッファを使い、モデル形状Particleを同じ見た目にする。
    std::string meshShapePath;
    int         meshShapeIndex = -1;
    float       meshShapeScale = 1.0f;
    // 同じ GameObject の AnimatorComponent が持つ現在のボーン行列で発生点を変形する。
    bool        meshShapeFollowSkinnedAnimation = false;

    ParticleBlendMode blendMode = ParticleBlendMode::Additive;
    ParticleSortMode  sortMode  = ParticleSortMode::None;
    // エミッター間の描画順。小さいほど先に描かれる (＝奥に見える)。
    // WHY: sortMode は 1 エミッター内の粒子しか並べ替えない。炎と煙のように
    //      別エミッターが重なる構成では、GameObject の並び順で前後が決まってしまい、
    //      シーンを編集しただけで見た目が変わる。優先度を明示して安定させる。
    //      同値のときはカメラから遠い順に描く (半透明の一般的な描画順)。
    int renderPriority = 0;
    ParticleSimulationMode simulationMode = ParticleSimulationMode::Cpu;
    ParticleSimulationSpace simulationSpace = ParticleSimulationSpace::World;
    ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
    float stretchedVelocityScale = 0.1f;
    float stretchedLengthScale = 1.0f;

    // CollisionはCPUで自作Physics WorldをQueryする。GPU指定時は正確性優先でCPUへ縮退する。
    ParticleCollisionMode collisionMode = ParticleCollisionMode::None;
    ParticleCollisionResponse collisionResponse = ParticleCollisionResponse::Bounce;
    float collisionRadius = 0.05f;
    float collisionBounciness = 0.5f;
    float collisionDamping = 0.0f;
    float collisionPlaneY = 0.0f;
    int collisionCountThisFrame = 0;
    int deathCountThisFrame = 0; // VFX GraphのOnDeath eventがフレーム単位で消費する。

    // .mat アセットへの参照。albedo テクスチャ・blendMode を .mat から解決する。
    // WHY: シェーダー・テクスチャ・ブレンドを .mat に集約し複数エミッター間で共有できるようにする。
    // NOTE: 空のときは PARTICLE_FALLBACK_MATERIAL が使われる (白い矩形にはならない)。
    std::string materialPath;
    // 空でない場合はbillboardの代わりに静的Meshを各CPU粒子のTRSで描画する。
    std::string meshParticlePath;
    // テクスチャからアルファをどう取り出すか。素材の作りの違いを吸収する。
    // 既定は TextureAlpha なので、既存アセットの見た目は変わらない。
    ParticleAlphaSource alphaSource = ParticleAlphaSource::TextureAlpha;
    int spriteColumns = 1;
    int spriteRows    = 1;
    int spriteStartFrame = 0;
    int spriteEndFrame   = 0;
    ParticleFlipbookMode flipbookMode = ParticleFlipbookMode::Lifetime;
    float flipbookFramesPerSecond = 24.0f;
    bool flipbookFrameBlending = false;
    // 粒子ごとに再生位相をずらす。同時に湧いた煙が全部同じコマで回るのを防ぐ。
    bool spriteRandomStartFrame = false;
    // アトラスの各行を「見た目の異なるバリエーション」として扱い、粒子ごとに1行を選ぶ。
    // 選ばれた行の中だけでアニメーションする (spriteStartFrame/EndFrame より優先)。
    bool spriteRandomRow = false;
    // Motion Vector atlasは各frameのRGを[-1,1]速度として読み、隣接frameを双方向warpする。
    bool motionVectorFlipbook = false;
    std::string motionVectorTexturePath;
    float motionVectorStrength = 1.0f;
    // 粒子ごとの色ゆらぎ [0,1]。発生時に RGB を各チャンネル独立で ±colorVariation 倍する。
    // WHY: 同じエミッターから出た粒子が完全に同色だと、群れが一枚のベタ塗りに見える。
    //      チャンネル独立にすることで明度差と軽い色相差が同時に出て、炎・火花に厚みが出る。
    //      per-particle の startColor/endColor は CPU/GPU 双方のスポーン経路に既にあるため、
    //      頂点フォーマットも定数バッファも増やさずに効かせられる。
    float colorVariation = 0.0f;
    float sizeCurvePower = 1.0f;
    float colorCurvePower = 1.0f;
    float velocityDamping = 0.0f;
    float angularVelocityMin = 0.0f;
    float angularVelocityMax = 0.0f;
    bool useSizeCurve = false;
    ParticleCurve sizeCurve;
    bool useVelocityCurve = false;
    ParticleCurve velocityCurve;
    bool useColorGradient = false;
    ParticleGradient colorGradient;

    // ── 黒体放射 (色温度オーサリング) ──
    // 有効にすると colorGradient の RGB を温度カーブから毎フレーム作り直す
    // (アルファはグラデーション側の値をそのまま使う)。
    // WHY: 炎・爆発の色は「すす粒子の温度による黒体放射」で決まり、任意の RGB を
    //      並べても炎に見えない。さらに輻射輝度は T^4 に比例するため、根元と先端の差は
    //      色差ではなく数倍〜十数倍の輝度差として出る。RGB を手で置く限り
    //      「白熱した芯 + 彩度の高い橙の縁」は作れない (芯を明るくすると縁まで白む)。
    bool  blackbodyEnabled = false;
    // 寿命 [0,1] → 色温度 [K]。既定は焚き火の実測域 (根元 1900K → 先端 1100K)。
    // WHY: ParticleCurve の既定キーは 0→1 で、そのまま温度として使うと 1K = 真っ黒になる。
    //      有効化した瞬間に炎らしい値が出ないと、機能があること自体に気付けない。
    ParticleCurve temperatureCurve{
        {{ {0.0f, 1900.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f},
           {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f}, {1.0f, 1100.0f} }},
        2, ParticleCurveInterpolation::Linear };
    float blackbodyReferenceTemperature = 1800.0f; // ここで intensity 倍の明るさになる
    float blackbodyIntensity = 1.0f;
    // 角速度に掛ける時間倍率。定数の angularVelocity だけでは
    // 「勢いよく回り始めて減速する」火の粉・破片の動きが作れない。
    bool useRotationCurve = false;
    ParticleCurve rotationCurve;
    // velocityDamping に掛ける時間倍率。噴き出し直後は素直に飛び、
    // 後半で急に空気抵抗が効く、といった減衰の作り分けに使う。
    bool useDragCurve = false;
    ParticleCurve dragCurve;

    // ── 速度モジュール (エミッター原点まわりの周回・放射) ──
    // WHY: 重力とノイズだけでは「渦を巻きながら広がる」魔法陣・竜巻・吸い込みが作れない。
    //      ForceField はシーン全体の場だが、こちらはエミッターに追従する固有の運動として効く。
    math::Vector3 orbitalAxis = { 0.0f, 1.0f, 0.0f }; // 周回の回転軸 (正規化して使う)
    float orbitalVelocity = 0.0f;  // 軸まわりの接線加速度 [m/s^2]
    float radialVelocity  = 0.0f;  // 原点から外向きの加速度 [m/s^2]。負で吸い込み
    // 発生時にエミッター自身の移動速度を初速へ加算する割合 [0,1]。
    // 移動する剣・ロケットから出る火花が置き去りにならず、引きずられて見えるようになる。
    float inheritVelocity = 0.0f;

    // Emission拡張: 移動距離、Prewarm、時刻指定Burst。
    float rateOverDistance = 0.0f;
    bool prewarm = false;
    std::vector<ParticleBurst> bursts;

    // ── per-particle Trail ──
    // 粒子 1 つ 1 つに尾を付ける。火の粉・魔法の軌跡のように「粒が線を引く」表現用。
    // 実装は履歴点へビルボードを連ねる方式で、専用の ribbon シェーダーは持たない。
    // WHY: 既存のパーティクル描画 (シェーダー・PSO・テクスチャ・ブレンド) をそのまま
    //      使えるため、素材やブレンド設定が本体と自動的に揃う。サンプル間隔を十分
    //      短くすれば連続した尾として見える。真の連続リボンが要るケース (太い帯) は
    //      従来どおり Trail ノードを使う。
    // GPU シミュレーションでは履歴を保持できないため、有効時は CPU へ縮退する
    // (CanUseGpuSimulation を参照)。
    bool  trailEnabled = false;
    int   trailPointCount = 6;          // 使用する履歴点数 [1, kMaxParticleTrailPoints]
    float trailSampleInterval = 0.03f;  // 履歴を刻む間隔 [秒]。短いほど滑らか
    float trailWidthScale = 0.6f;       // 尾の先端 (最古) 側のサイズ倍率
    float trailAlphaScale = 0.5f;       // 尾の先端側の不透明度倍率
    math::Vector4 trailColorTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    // 履歴点へビルボードを並べるのではなく、連続した 1 枚の帯として描く。
    // WHY: ビルボード方式は「点を細かく打てば線に見える」だけで、太くすると必ず粒の連なりが露見する。
    //      剣閃・魔法の軌跡・リボン状の炎のように「幅のある帯」が主役の表現はこれでは作れない。
    //      有効時は履歴点をポリラインとみなし、隣り合う点をマイター接合した帯を張る
    //      (Trail ノードと同じリボン生成・同じシェーダーを使う)。
    // NOTE: 帯は 1 エミッターぶんをまとめて 1 DrawCall で描くため、色は粒子ごとではなく
    //       エミッターの colorStart / colorEnd を帯の長さ方向へ配る。
    //       粒子ごとの色ゆらぎを尾へ乗せたい場合はビルボード方式のままにすること。
    // 自己影。粒子群が自分自身へ落とす影の濃さ。0 で無効。
    // WHY: 受け影 (receiveShadows) は他の物体が落とす影しか扱えない。厚みのある煙・雲は
    //      自分の内部で光が減衰することで初めて立体に見え、これが無いと
    //      どれだけ粒子を重ねても平坦な塊のままになる。
    // NOTE: 有効にすると光源から見た密度を 1 枚 RT へ積む追加パスが走る
    //       (エミッター単位ではなくシーン全体で 1 パス)。
    float selfShadowStrength = 0.0f;
    bool  trailRibbon = false;
    // 帯の幅 [m]。0 以下なら粒子サイズをそのまま使う。
    // WHY: 帯は粒子サイズと独立に太さを決めたいことが多い (小さな火の粉が太い軌跡を引く等)。
    float trailRibbonWidth = 0.0f;

    // SubEmitterはGameObject名で参照し、各イベントで対象EmitterへBurstを積む。
    std::string birthSubEmitter;
    std::string deathSubEmitter;
    std::string collisionSubEmitter;
    int subEmitterBurstCount = 1;
    // 名前引きの探索範囲を限定するルート GameObject。INVALID でシーン全体 (従来どおり)。
    // WHY: VFX Graph が生成するノード実体は、同じ .vfx を複数配置すれば同名の GO が並ぶ。
    //      シーン全体を名前で引くと、隣に置いた別インスタンスの粒子を誤って吹かせてしまう。
    //      VFXGraphSystem がここへ owner を入れ、参照をそのエフェクト内へ閉じる。
    // NOTE: ランタイム専用。シーン保存対象ではない (シーン上の手置き Emitter は INVALID のまま)。
    EntityID subEmitterScopeRoot = EntityID::INVALID;

    bool softParticles = false;
    float softParticleFadeDistance = 0.5f;
    // Heat hazeは不透明シーンcopyを背景として屈折し、lit smokeはbillboard疑似法線で照明応答する。
    bool distortion = false;
    float distortionStrength = 0.015f;
    // 歪みベクトル専用のノーマルマップ。空なら従来どおり albedo の RG を流用する。
    // WHY: albedo の RG を歪みベクトルとして使うと、素材を差し替えただけで屈折の向きが
    //      意味不明に変わる。衝撃波・陽炎は「どちらへ曲げるか」が絵の要なので、
    //      色とは独立した専用マップを持てないと調整が成立しない。
    std::string distortionTexturePath;
    // 色収差量 [画面 UV]。RGB を歪み方向へずらして屈折の分散を出す。0 で無効。
    float distortionChromatic = 0.0f;
    bool sixWayLighting = false;
    float lightingStrength = 1.0f;
    // ── 煙の散乱 (sixWayLighting 有効時) ──
    // WHY: 素の N·L は「不透明な球」の陰影で、光を透かす媒質には合わない。
    //      煙・雲が背後の光で縁から光るのは前方散乱 (Mie 散乱の位相関数が前方に尖る) が
    //      原因で、この項が無いと炎が煙の向こうにあっても煙は暗いままになる。
    // 巻き込み拡散 [0,1]。大きいほど陰側へ光が回り込み、明暗の境界が消える。
    float smokeWrap = 0.5f;
    // 逆光透過の強さ。0 で無効 (従来の見た目)。
    float smokeTransmission = 0.0f;
    // 前方散乱の鋭さ。大きいほど光源の真後ろだけが強く光る。
    float smokeBackScatterPower = 4.0f;
    float emissiveScale = 1.0f;
    // 影を受けるか。既定は無効 (発光エフェクトは影の中でも光るのが自然なため)。
    // WHY: 煙・埃のような非発光の粒子は、影の中で暗くならないと背景から浮いて見える。
    //      AAA で「パーティクルが浮く」最大の原因がこれ。
    bool receiveShadows = false;
    float shadowStrength = 1.0f;
    // ボリュメトリック煙: ビルボード内で球状密度場をレイマーチして厚みを出す。
    // WHY: 板にテクスチャを貼るだけでは、カメラが回り込むと紙が回ったように見える。
    //      視線方向へ積分すると立体感と逆光での前方散乱が出る。
    // 役割が重複するため sixWayLighting とは排他 (Inspector 側で相互に落とす)。
    bool volumetric = false;
    int volumetricSteps = 8;
    float volumetricDensity = 1.0f;
    float volumetricAnisotropy = 0.3f;
    float volumetricNoiseScale = 2.0f;

    // Culling/LOD — 粒子の現在Boundsを使い、遠距離では発生数と描画数を段階的に削減する。
    bool cullingEnabled = true;
    float cullingBoundsPadding = 0.25f;
    bool lodEnabled = true;
    float lodNearDistance = 12.0f;
    float lodFarDistance = 40.0f;
    float lodNearRateScale = 1.0f;
    float lodFarRateScale = 0.25f;
    float screenCoverageThreshold = 0.0f;
    bool pauseWhenCulled = false;

    // ── ノイズモジュール (乱流ベクトルフィールド) ──
    // カールノイズ (発散ゼロのベクトル場) を粒子速度へ加算する。炎の揺らぎ・煙の乱れ用。
    // WHY: ParticleForceField(Turbulence) はシーン全体の場だが、こちらはエミッター固有の
    //      揺らぎとして粒子ごとに常時作用させたいケース (Unity の Noise モジュール相当) に使う。
    float noiseStrength  = 0.0f;  // 加速度の大きさ [m/s^2]。0 で無効
    float noiseFrequency = 0.5f;  // ノイズ格子の空間周波数 [1/m]
    float noiseSpeed     = 1.0f;  // 時間スクロール速度

    // シーン内の ParticleForceField から力を受けるか。
    // WHY: UI 演出用パーティクルなど、環境の風に反応させたくないエミッターを除外できるようにする。
    bool receiveForceFields = true;

    std::vector<Particle> particles;
    // emitRate * dt の累積値。1.0 を超えるたびに 1 粒子を発生させる。
    // こうすることで低フレームレートでも発生数が dt に比例して安定する。
    float                 emitAccum = 0.0f;
    // randomSeed から初期化されるランタイム状態。シーン保存対象ではない。
    uint32_t              randomState = 1;
    float                 playTime = 0.0f;
    float                 delayTime = 0.0f;
    int                   burstPending = 0;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::string           loadedTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> motionVectorTexture;
    std::string           loadedMotionVectorTexturePath;
    renderer::ResourceHandle<renderer::TextureTag> distortionTexture;
    std::string           loadedDistortionTexturePath;
    std::string           loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。
    // albedo テクスチャが sRGB でエンコードされているか (.meta の srgb)。
    // WHY: 手描き素材は sRGB、ProceduralVFXTextures が焼くものはリニア。
    //      一律にリニア化すると後者が暗く沈む。素材ごとの実際の値に従う。
    bool                  textureIsSrgb = true;
    // .mat の [params] albedo。リニアへ変換済み。
    math::Vector4         materialTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    // 黒体放射を焼き込んだ後の実効グラデーション。blackbodyEnabled が false なら
    // colorGradient のコピー。シミュレーションと GPU 定数バッファはこちらだけを見る。
    ParticleGradient      runtimeGradient;
    // RefreshRuntimeGradient の再計算判定に使う入力の写し。シーン保存対象外。
    bool                                                       runtimeGradientValid = false;
    bool                                                       runtimeGradientBlackbody = false;
    float                                                      runtimeGradientReference = 0.0f;
    float                                                      runtimeGradientIntensity = 0.0f;
    std::array<ParticleGradientKey, kMaxParticleCurveKeys>     runtimeGradientSourceKeys{};
    uint32_t                                                   runtimeGradientSourceCount = 0;
    std::array<ParticleCurveKey, kMaxParticleCurveKeys>        runtimeGradientTemperatureKeys{};
    uint32_t                                                   runtimeGradientTemperatureCount = 0;

    // GPU パーティクル実行時状態 (シーン保存不要、デバイスリセット時に再生成)
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuParticleBuffer; // RWStructuredBuffer: CS が更新
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSpawnBuffer;    // DYNAMIC SRV: CPU がスポーンデータを書く
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuEmitterCB;      // CS 用エミッター定数バッファ
    renderer::ResourceHandle<renderer::ConstantBufferTag>   renderCB;          // VS/PS 描画モード・Soft Particle
    // GPU ソート。sortMode != None のときだけ確保する。
    // WHY: 粒子プールそのものは並べ替えられない (リングバッファ位置が動くとスポーンが壊れる)。
    //      並べ替えるのは (キー, 粒子 index) の対だけで、描画 VS がその順に粒子を引く。
    // 連続リボン (trailRibbon)。帯の頂点は毎フレーム CPU で作り直す。
    // WHY: 履歴点はビルボード用にしか持っていないため、帯の形は粒子の運動から
    //      その場で組み立てるしかない (GPU シミュレーションでは履歴を持てないので CPU 限定)。
    // 頂点バッファはここに持たず、描画側の DynamicVertexBufferPool から借りる。
    // WHY: 帯はカメラへ正対させるので形がビューごとに変わる。エディタは 1 フレームで
    //      Scene View と Game View を続けて描くため、エミッターに 1 本持たせると
    //      DX12 では後のビューの形が先のビューの Draw まで書き替えてしまう。
    renderer::ResourceHandle<renderer::ConstantBufferTag>   trailRibbonCB;
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSortBuffer;     // RWStructuredBuffer<uint2>
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuSortCB;         // bitonic の (k, j) を段ごとに更新
    uint32_t gpuSortCapacity = 0;   // gpuSortBuffer の要素数 (2 のべき乗、maxParticles 以上)
    uint32_t gpuWriteHead    = 0;   // gpuSpawnBuffer の次書き込み位置 (リングバッファインデックス)
    uint32_t gpuSpawnCount   = 0;   // 今フレームのスポーン数
    bool     gpuInitialized  = false;
    uint32_t gpuCapacity = 0;
    bool     gpuClearPending = false;
    uint64_t gpuResetVersion = 0;   // 最後に確認した ResourceManager::GetResetVersion()
    uint64_t lastGpuSimulationFrame = UINT64_MAX;
    uint64_t lastCpuSimulationFrame = UINT64_MAX;
    uint64_t lastPlaybackFrame = UINT64_MAX;
    bool emitThisFrame = false;
    bool prewarmed = false;
    math::Vector3 boundsCenter = {};
    float boundsRadius = 0.0f;
    float lodRateScale = 1.0f;
    int visibleParticleCount = 0;
    bool isCulledThisFrame = false;
    int prewarmSpawnPending = 0;
    bool hasLastEmitterPosition = false;
    math::Vector3 lastEmitterPosition = {};
    // エミッター自身のワールド速度 [m/s]。inheritVelocity がスポーン時に参照する。
    // ParticleSimulationSystem が lastEmitterPosition を更新するのと同じ場所で毎フレーム求める。
    math::Vector3 emitterVelocity = {};
    float distanceEmitAccum = 0.0f;
    std::vector<int> burstCyclesFired;

    // MeshSurface Shapeのランタイムキャッシュ。位置と4ボーンウェイトだけを複製する。
    std::vector<MeshShapeVertex> meshShapeVertices;
    std::string loadedMeshShapePath;
    int         loadedMeshShapeIndex = -2;

    // ── エディタープレビュー制御 (VFX Editor 用ランタイム状態。シーン保存対象外) ──
    // WHY: VFX Editor の再生速度・一時停止をシミュレーション dt へ注入するための口。
    //      パネルが毎フレーム editorTimeScale と editorTimeScaleFrame を書き込み、
    //      書き込みが途絶えたら自動で通常速度へ戻る (パネルを閉じても凍結が残らないフェイルセーフ)。
    float    editorTimeScale      = 1.0f;
    uint64_t editorTimeScaleFrame = 0;     // editorTimeScale を最後に書き込んだ Time::frameCount
    // タイムラインスクラブ要求 [秒]。>=0 のとき ParticleSimulationSystem が消費し、
    // randomSeed から決定論的にその時刻まで再シミュレートする。負値 = 要求なし。
    float    editorScrubTime      = -1.0f;

    /// runtimeGradient を作り直す。入力が変わっていなければ何もしないので、どこから呼んでもよい。
    /// @note 黒体モードでは各キーの時刻で温度カーブを引き、色温度 → リニア RGB → オーサリング空間
    ///       (sRGB) へ戻して格納する。オーサリング空間で持つのは、CPU 経路も GPU 経路も
    ///       「補間 → リニア化」という同じ順序を通すため。ここだけ別空間にすると片方が破綻する。
    void RefreshRuntimeGradient()
    {
        // 黒体の焼き込みはキー 1 点あたり可視域 81 サンプルの積分になる。粒子ごとの
        // 更新から間接的に呼ばれても潰れないよう、入力が変わったときだけ作り直す。
        if (runtimeGradientValid
            && !blackbodyEnabled == !runtimeGradientBlackbody
            && runtimeGradientReference == blackbodyReferenceTemperature
            && runtimeGradientIntensity == blackbodyIntensity
            && runtimeGradientSourceKeys == colorGradient.keys
            && runtimeGradientSourceCount == colorGradient.keyCount
            && runtimeGradientTemperatureKeys == temperatureCurve.keys
            && runtimeGradientTemperatureCount == temperatureCurve.keyCount) {
            return;
        }
        runtimeGradientValid = true;
        runtimeGradientBlackbody = blackbodyEnabled;
        runtimeGradientReference = blackbodyReferenceTemperature;
        runtimeGradientIntensity = blackbodyIntensity;
        runtimeGradientSourceKeys = colorGradient.keys;
        runtimeGradientSourceCount = colorGradient.keyCount;
        runtimeGradientTemperatureKeys = temperatureCurve.keys;
        runtimeGradientTemperatureCount = temperatureCurve.keyCount;

        runtimeGradient = colorGradient;
        if (!blackbodyEnabled) return;
        const uint32_t count = (std::min)(runtimeGradient.keyCount,
                                          static_cast<uint32_t>(runtimeGradient.keys.size()));
        for (uint32_t index = 0; index < count; ++index) {
            const float kelvin = temperatureCurve.Evaluate(runtimeGradient.keys[index].time);
            const math::Vector3 linear = ParticleBlackbodyLinear(
                kelvin, blackbodyReferenceTemperature, blackbodyIntensity);
            runtimeGradient.keys[index].color = ParticleLinearToSrgb(
                { linear.x, linear.y, linear.z, runtimeGradient.keys[index].color.w });
        }
    }

    // 実効プレビュー速度。エディターからの書き込みが 2 フレーム以上途絶えていたら 1.0 に戻す。
    // WHY: ゲーム実行時 (エディターなし) は書き込みが存在しないため常に 1.0 になり、影響しない。
    float GetEditorTimeScale(uint64_t currentFrame) const
    {
        return (editorTimeScaleFrame + 2 >= currentFrame) ? editorTimeScale : 1.0f;
    }

    // 再生状態と粒子を先頭へ巻き戻す。Inspector / VFX Editor の Restart とスクラブ前処理が共用する。
    // WHY: リセットすべきランタイム状態が多く、呼び出し側ごとに列挙すると漏れが出るため一箇所に集約する。
    void ResetPlayback()
    {
        playing                = true;
        playTime               = 0.0f;
        delayTime              = 0.0f;
        emitAccum              = 0.0f;
        burstPending           = 0;
        burstCyclesFired.clear();
        prewarmed              = false;
        prewarmSpawnPending    = 0;
        hasLastEmitterPosition = false;
        emitterVelocity        = {};
        distanceEmitAccum      = 0.0f;
        randomState            = randomSeed != 0 ? randomSeed : 1;
        particles.clear();
        gpuClearPending        = true;
        // ロード直後やスクラブ開始時に、再生が 1 フレームも進まないまま粒子色を
        // 引かれることがある。実効グラデーションを先に用意しておく。
        RefreshRuntimeGradient();
    }

    // Inspector / Script / Operator が共有する再生制御。
    // WHY: UI と ScriptProxy がそれぞれランタイム状態を列挙すると、Clear や Restart の
    //      対象漏れが経路ごとに発生する。エミッター自身に責務を集め、AI も同じ挙動を使う。
    void Play(bool restart = false)
    {
        if (restart) ResetPlayback();
        else         playing = true;
    }

    void Pause() { playing = false; }

    void Stop(bool clear = false)
    {
        playing      = false;
        emitAccum     = 0.0f;
        burstPending  = 0;
        if (clear) ClearParticles();
    }

    void Burst(int count)
    {
        if (count > 0) burstPending += count;
    }

    void ClearParticles()
    {
        particles.clear();
        emitAccum = 0.0f;
        burstPending = 0;
        playTime = 0.0f;
        delayTime = 0.0f;
        gpuClearPending = true;
        gpuWriteHead = 0;
        gpuSpawnCount = 0;
        collisionCountThisFrame = 0;
        prewarmSpawnPending = 0;
    }

    const char* GetTypeName() const { return "Particle Emitter"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("emitPosition", emitPosition);
        r.Field("emitVelocity", emitVelocity);
        r.Field("velocitySpread", velocitySpread);
        r.ColorField("colorStart", colorStart);
        r.ColorField("colorEnd", colorEnd);
        r.Field("sizeStart", sizeStart);
        r.Field("sizeEnd", sizeEnd);
        r.Field("lifetime", lifetime);
        r.Field("lifetimeRandom", lifetimeRandom);
        r.Field("emitRate", emitRate);
        r.Field("maxParticles", maxParticles);
        r.Field("gravity", gravity);
        r.Field("playing", playing);
        r.Field("loop", loop);
        r.Field("duration", duration);
        r.Field("startDelay", startDelay);
        r.Field("clearOnStop", clearOnStop);
        // IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int seed = static_cast<int>(randomSeed);
        r.Field("randomSeed", seed);
        const uint32_t clampedSeed = static_cast<uint32_t>(seed < 1 ? 1 : seed);
        if (randomSeed != clampedSeed) {
            randomSeed = clampedSeed;
            randomState = randomSeed;
        }
        r.Field("materialPath", materialPath);
        r.Field("meshParticlePath", meshParticlePath);
        int shapeValue = static_cast<int>(shape);
        r.Field("shape", shapeValue);
        shapeValue = shapeValue < 0 ? 0 : (shapeValue > 4 ? 4 : shapeValue);
        shape = static_cast<ParticleEmitterShape>(shapeValue);
        r.Field("sphereRadius", sphereRadius);
        r.Field("coneAngleDegrees", coneAngleDegrees);
        r.Field("coneRadius", coneRadius);
        r.Field("boxExtents", boxExtents);
        r.Field("meshShapePath", meshShapePath);
        r.Field("meshShapeIndex", meshShapeIndex);
        r.Field("meshShapeScale", meshShapeScale);
        r.Field("meshShapeFollowSkinnedAnimation", meshShapeFollowSkinnedAnimation);

        int blendValue = static_cast<int>(blendMode);
        r.Field("blendMode", blendValue);
        // Premultiplied(2)を保存後も維持する。上限1のままだとExplosion生成時の設定がAlphaへ戻る。
        blendValue = blendValue < 0 ? 0 : (blendValue > 2 ? 2 : blendValue);
        blendMode = static_cast<ParticleBlendMode>(blendValue);

        int sortValue = static_cast<int>(sortMode);
        r.Field("sortMode", sortValue);
        sortValue = sortValue < 0 ? 0 : (sortValue > 1 ? 1 : sortValue);
        sortMode = static_cast<ParticleSortMode>(sortValue);

        int simValue = static_cast<int>(simulationMode);
        r.Field("simulationMode", simValue);
        simValue = simValue < 0 ? 0 : (simValue > 1 ? 1 : simValue);
        simulationMode = static_cast<ParticleSimulationMode>(simValue);

        int simulationSpaceValue = static_cast<int>(simulationSpace);
        r.Field("simulationSpace", simulationSpaceValue);
        simulationSpace = static_cast<ParticleSimulationSpace>(simulationSpaceValue < 0 ? 0 : (simulationSpaceValue > 1 ? 1 : simulationSpaceValue));
        int renderModeValue = static_cast<int>(renderMode);
        r.Field("renderMode", renderModeValue);
        renderMode = static_cast<ParticleRenderMode>(renderModeValue < 0 ? 0 : (renderModeValue > 3 ? 3 : renderModeValue));
        r.Field("stretchedVelocityScale", stretchedVelocityScale);
        r.Field("stretchedLengthScale", stretchedLengthScale);
        int collisionModeValue = static_cast<int>(collisionMode);
        r.Field("collisionMode", collisionModeValue);
        collisionMode = static_cast<ParticleCollisionMode>(collisionModeValue < 0 ? 0 : (collisionModeValue > 3 ? 3 : collisionModeValue));
        int collisionResponseValue = static_cast<int>(collisionResponse);
        r.Field("collisionResponse", collisionResponseValue);
        collisionResponse = static_cast<ParticleCollisionResponse>(collisionResponseValue < 0 ? 0 : (collisionResponseValue > 2 ? 2 : collisionResponseValue));
        r.Field("collisionRadius", collisionRadius);
        r.Field("collisionBounciness", collisionBounciness);
        r.Field("collisionDamping", collisionDamping);
        r.Field("collisionPlaneY", collisionPlaneY);

        r.Field("spriteColumns", spriteColumns);
        r.Field("spriteRows", spriteRows);
        r.Field("spriteStartFrame", spriteStartFrame);
        r.Field("spriteEndFrame", spriteEndFrame);
        int flipbookModeValue = static_cast<int>(flipbookMode);
        r.Field("flipbookMode", flipbookModeValue);
        flipbookMode = static_cast<ParticleFlipbookMode>(flipbookModeValue < 0 ? 0 : (flipbookModeValue > 3 ? 3 : flipbookModeValue));
        r.Field("flipbookFramesPerSecond", flipbookFramesPerSecond);
        r.Field("flipbookFrameBlending", flipbookFrameBlending);
        r.Field("motionVectorFlipbook", motionVectorFlipbook);
        r.Field("motionVectorTexturePath", motionVectorTexturePath);
        r.Field("motionVectorStrength", motionVectorStrength);
        r.Field("sizeCurvePower", sizeCurvePower);
        r.Field("colorCurvePower", colorCurvePower);
        r.Field("velocityDamping", velocityDamping);
        r.Field("angularVelocityMin", angularVelocityMin);
        r.Field("angularVelocityMax", angularVelocityMax);
        r.Field("useSizeCurve", useSizeCurve);
        r.Field("useVelocityCurve", useVelocityCurve);
        r.Field("useColorGradient", useColorGradient);
        r.Field("rateOverDistance", rateOverDistance);
        r.Field("prewarm", prewarm);
        r.Field("birthSubEmitter", birthSubEmitter);
        r.Field("deathSubEmitter", deathSubEmitter);
        r.Field("collisionSubEmitter", collisionSubEmitter);
        r.Field("subEmitterBurstCount", subEmitterBurstCount);
        r.Field("softParticles", softParticles);
        r.Field("softParticleFadeDistance", softParticleFadeDistance);
        r.Field("distortion", distortion);
        r.Field("distortionStrength", distortionStrength);
        r.Field("distortionTexturePath", distortionTexturePath);
        r.Field("distortionChromatic", distortionChromatic);
        r.Field("sixWayLighting", sixWayLighting);
        r.Field("lightingStrength", lightingStrength);
        r.Field("smokeWrap", smokeWrap);
        r.Field("smokeTransmission", smokeTransmission);
        r.Field("smokeBackScatterPower", smokeBackScatterPower);
        r.Field("emissiveScale", emissiveScale);
        r.Field("blackbodyEnabled", blackbodyEnabled);
        r.Field("blackbodyReferenceTemperature", blackbodyReferenceTemperature);
        r.Field("blackbodyIntensity", blackbodyIntensity);
        int gradientColorSpaceValue = static_cast<int>(colorGradient.colorSpace);
        r.Field("gradientColorSpace", gradientColorSpaceValue);
        colorGradient.colorSpace = static_cast<ParticleColorSpace>(
            gradientColorSpaceValue < 0 ? 0 : (gradientColorSpaceValue > 2 ? 2 : gradientColorSpaceValue));
        r.Field("cullingEnabled", cullingEnabled);
        r.Field("cullingBoundsPadding", cullingBoundsPadding);
        r.Field("lodEnabled", lodEnabled);
        r.Field("lodNearDistance", lodNearDistance);
        r.Field("lodFarDistance", lodFarDistance);
        r.Field("lodNearRateScale", lodNearRateScale);
        r.Field("lodFarRateScale", lodFarRateScale);
        r.Field("screenCoverageThreshold", screenCoverageThreshold);
        r.Field("pauseWhenCulled", pauseWhenCulled);
        r.Field("noiseStrength", noiseStrength);
        r.Field("noiseFrequency", noiseFrequency);
        r.Field("noiseSpeed", noiseSpeed);
        r.Field("receiveForceFields", receiveForceFields);
    }
};

} // namespace fbzz::scene
