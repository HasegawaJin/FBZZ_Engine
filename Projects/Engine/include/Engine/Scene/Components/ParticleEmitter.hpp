// FBZZ Engine
// ParticleEmitter.hpp | fbzz::scene
// パーティクルシミュレーション設定コンポーネント
// 発生率・寿命・速度などを保持し、System が毎フレーム粒子状態を進める。
// 描画リソースの所有は Renderer 側に分ける。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
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

// CS/VS 共通の GPU パーティクル 1 粒子レイアウト (80 bytes, 16-byte aligned)
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
};

// CPU → CS へのスポーンリクエスト 1 件 (96 bytes, 16-byte aligned)
// DYNAMIC StructuredBuffer に毎フレーム書き込み、CS がリングバッファで配置する。
struct GpuSpawnEntry {
    math::Vector3 position;        // 12B
    float         lifetime;        // 4B
    math::Vector3 velocity;        // 12B
    float         size;            // 4B
    math::Vector4 colorStart;      // 16B
    math::Vector4 colorEnd;        // 16B
    math::Vector4 uvRect;          // 16B
    float         rotation;        // 4B
    float         angularVelocity; // 4B
    float         spriteSeed;      // 4B
    float         pad1;            // 4B
};

static_assert(sizeof(GpuParticle) == 80, "GpuParticle must match ParticleGpuSim.cs.hlsl (80 bytes)");
static_assert(sizeof(GpuSpawnEntry) == 96, "GpuSpawnEntry must match ParticleGpuSim.cs.hlsl (96 bytes)");

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
    math::Vector4 uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    math::Vector4 nextUvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    float spriteBlend = 0.0f;
};

// ParticleCurveKey — 正規化時間に対する値 1 点。
// 最大4点の固定長にしてGPU定数バッファへそのまま転送できるようにする。
struct ParticleCurveKey {
    float time = 0.0f;
    float value = 0.0f;
};

// ParticleCurve — 線形補間の軽量カーブ。Editorで最大4キーを編集する。
struct ParticleCurve {
    std::array<ParticleCurveKey, 4> keys{{ {0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }};
    uint32_t keyCount = 2;

    float Evaluate(float time) const
    {
        const uint32_t count = keyCount < 1 ? 1 : (keyCount > keys.size() ? static_cast<uint32_t>(keys.size()) : keyCount);
        if (time <= keys[0].time) return keys[0].value;
        for (uint32_t index = 1; index < count; ++index) {
            if (time <= keys[index].time) {
                const float span = (std::max)(keys[index].time - keys[index - 1].time, 0.0001f);
                const float alpha = (std::max)(0.0f, (std::min)(1.0f, (time - keys[index - 1].time) / span));
                return keys[index - 1].value + (keys[index].value - keys[index - 1].value) * alpha;
            }
        }
        return keys[count - 1].value;
    }
};

struct ParticleGradientKey {
    float time = 0.0f;
    math::Vector4 color = { 1, 1, 1, 1 };
};

// ParticleGradient — GPU転送可能な最大4色の線形Gradient。
struct ParticleGradient {
    std::array<ParticleGradientKey, 4> keys{{
        {0.0f, {1, 1, 1, 1}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}}
    }};
    uint32_t keyCount = 2;

    math::Vector4 Evaluate(float time) const
    {
        const uint32_t count = keyCount < 1 ? 1 : (keyCount > keys.size() ? static_cast<uint32_t>(keys.size()) : keyCount);
        if (time <= keys[0].time) return keys[0].color;
        for (uint32_t index = 1; index < count; ++index) {
            if (time <= keys[index].time) {
                const float span = (std::max)(keys[index].time - keys[index - 1].time, 0.0001f);
                const float alpha = (std::max)(0.0f, (std::min)(1.0f, (time - keys[index - 1].time) / span));
                const math::Vector4& a = keys[index - 1].color;
                const math::Vector4& b = keys[index].color;
                return { a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha,
                         a.z + (b.z - a.z) * alpha, a.w + (b.w - a.w) * alpha };
            }
        }
        return keys[count - 1].color;
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

struct ParticleEmitter {
    math::Vector3 emitPosition   = {};
    math::Vector3 emitVelocity   = { 0.0f, 4.0f, 0.0f };
    float         velocitySpread = 1.5f;
    math::Vector4 colorStart     = { 1.0f, 0.7f, 0.2f, 1.0f };
    math::Vector4 colorEnd       = { 1.0f, 0.1f, 0.0f, 0.0f };
    float         sizeStart      = 0.4f;
    float         sizeEnd        = 0.05f;
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
    //      空文字のとき texturePath へフォールバックするため既存シーンデータは無変更で動く。
    std::string materialPath;
    // 空でない場合はbillboardの代わりに静的Meshを各CPU粒子のTRSで描画する。
    std::string meshParticlePath;
    // texturePath — deprecated。materialPath が空のときのフォールバック。
    std::string texturePath;
    int spriteColumns = 1;
    int spriteRows    = 1;
    int spriteStartFrame = 0;
    int spriteEndFrame   = 0;
    ParticleFlipbookMode flipbookMode = ParticleFlipbookMode::Lifetime;
    float flipbookFramesPerSecond = 24.0f;
    bool flipbookFrameBlending = false;
    // Motion Vector atlasは各frameのRGを[-1,1]速度として読み、隣接frameを双方向warpする。
    bool motionVectorFlipbook = false;
    std::string motionVectorTexturePath;
    float motionVectorStrength = 1.0f;
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

    // Emission拡張: 移動距離、Prewarm、時刻指定Burst。
    float rateOverDistance = 0.0f;
    bool prewarm = false;
    std::vector<ParticleBurst> bursts;

    // SubEmitterはGameObject名で参照し、各イベントで対象EmitterへBurstを積む。
    std::string birthSubEmitter;
    std::string deathSubEmitter;
    std::string collisionSubEmitter;
    int subEmitterBurstCount = 1;

    bool softParticles = false;
    float softParticleFadeDistance = 0.5f;
    // Heat hazeは不透明シーンcopyを背景として屈折し、lit smokeはbillboard疑似法線で照明応答する。
    bool distortion = false;
    float distortionStrength = 0.015f;
    bool sixWayLighting = false;
    float lightingStrength = 1.0f;
    float emissiveScale = 1.0f;

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
    std::string           loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。

    // GPU パーティクル実行時状態 (シーン保存不要、デバイスリセット時に再生成)
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuParticleBuffer; // RWStructuredBuffer: CS が更新
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSpawnBuffer;    // DYNAMIC SRV: CPU がスポーンデータを書く
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuEmitterCB;      // CS 用エミッター定数バッファ
    renderer::ResourceHandle<renderer::ConstantBufferTag>   renderCB;          // VS/PS 描画モード・Soft Particle
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
        distanceEmitAccum      = 0.0f;
        randomState            = randomSeed != 0 ? randomSeed : 1;
        particles.clear();
        gpuClearPending        = true;
    }

    const char* GetTypeName() const { return "Particle Emitter"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("emitPosition", emitPosition);
        r.Field("emitVelocity", emitVelocity);
        r.Field("velocitySpread", velocitySpread);
        r.Field("colorStart", colorStart);
        r.Field("colorEnd", colorEnd);
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
        r.Field("texturePath", texturePath);
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
        blendValue = blendValue < 0 ? 0 : (blendValue > 1 ? 1 : blendValue);
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
        r.Field("sixWayLighting", sixWayLighting);
        r.Field("lightingStrength", lightingStrength);
        r.Field("emissiveScale", emissiveScale);
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
