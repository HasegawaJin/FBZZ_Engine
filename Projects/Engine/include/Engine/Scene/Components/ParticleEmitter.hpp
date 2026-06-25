// FBZZ Engine
// ParticleEmitter.hpp | fbzz::scene
// パーティクルシミュレーション設定コンポーネント
// 発生率・寿命・速度などを保持し、System が毎フレーム粒子状態を進める。
// 描画リソースの所有は Renderer 側に分ける。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
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
    float         pad1;            // 4B
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
    float         pad0;            // 4B
    float         pad1;            // 4B
};

struct Particle {
    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector4 color;
    float         size;
    float         age;
    float         rotation = 0.0f;
    float         angularVelocity = 0.0f;
    math::Vector4 uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
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

    ParticleBlendMode blendMode = ParticleBlendMode::Additive;
    ParticleSortMode  sortMode  = ParticleSortMode::None;
    ParticleSimulationMode simulationMode = ParticleSimulationMode::Cpu;

    // .mat アセットへの参照。albedo テクスチャ・blendMode を .mat から解決する。
    // WHY: シェーダー・テクスチャ・ブレンドを .mat に集約し複数エミッター間で共有できるようにする。
    //      空文字のとき texturePath へフォールバックするため既存シーンデータは無変更で動く。
    std::string materialPath;
    // texturePath — deprecated。materialPath が空のときのフォールバック。
    std::string texturePath;
    int spriteColumns = 1;
    int spriteRows    = 1;
    int spriteStartFrame = 0;
    int spriteEndFrame   = 0;
    float sizeCurvePower = 1.0f;
    float colorCurvePower = 1.0f;
    float velocityDamping = 0.0f;
    float angularVelocityMin = 0.0f;
    float angularVelocityMax = 0.0f;

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
    std::string           loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。

    // GPU パーティクル実行時状態 (シーン保存不要、デバイスリセット時に再生成)
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuParticleBuffer; // RWStructuredBuffer: CS が更新
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuSpawnBuffer;    // DYNAMIC SRV: CPU がスポーンデータを書く
    renderer::ResourceHandle<renderer::ConstantBufferTag>   gpuEmitterCB;      // CS 用エミッター定数バッファ
    uint32_t gpuWriteHead    = 0;   // gpuSpawnBuffer の次書き込み位置 (リングバッファインデックス)
    uint32_t gpuSpawnCount   = 0;   // 今フレームのスポーン数
    bool     gpuInitialized  = false;
    uint64_t gpuResetVersion = 0;   // 最後に確認した ResourceManager::GetResetVersion()

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
        r.Field("texturePath", texturePath);
        int shapeValue = static_cast<int>(shape);
        r.Field("shape", shapeValue);
        shapeValue = shapeValue < 0 ? 0 : (shapeValue > 3 ? 3 : shapeValue);
        shape = static_cast<ParticleEmitterShape>(shapeValue);
        r.Field("sphereRadius", sphereRadius);
        r.Field("coneAngleDegrees", coneAngleDegrees);
        r.Field("coneRadius", coneRadius);
        r.Field("boxExtents", boxExtents);

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

        r.Field("spriteColumns", spriteColumns);
        r.Field("spriteRows", spriteRows);
        r.Field("spriteStartFrame", spriteStartFrame);
        r.Field("spriteEndFrame", spriteEndFrame);
        r.Field("sizeCurvePower", sizeCurvePower);
        r.Field("colorCurvePower", colorCurvePower);
        r.Field("velocityDamping", velocityDamping);
        r.Field("angularVelocityMin", angularVelocityMin);
        r.Field("angularVelocityMax", angularVelocityMax);
    }
};

} // namespace fbzz::scene
