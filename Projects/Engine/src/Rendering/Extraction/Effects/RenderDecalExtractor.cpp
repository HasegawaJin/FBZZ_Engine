/// @file    RenderDecalExtractor.cpp
/// @brief   デカールの寿命・投影・表示順に対応する描画入力。
/// @author  Hasegawa Jin
/// @date    2026-08-23

#include "RenderDecalMaterials.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderDecalExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Rendering/DecalComponent.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

using namespace decal_extraction_detail;

bool DecalHasFlipbook(const DecalComponent& decal)
{
    return decal.frameCount > 1;
}

/// @brief age を進める必要があるか。
/// @note 永続デカール (lifetime < 0) は age を進めない。無条件に足すと、編集中にシーンを開いているだけで age が増え、保存するたびに «誰も触っていない差分» が出る。
bool DecalNeedsAge(const DecalComponent& decal)
{
    return decal.lifetime >= 0.0f || decal.fadeInTime > 0.0f || DecalHasFlipbook(decal);
}

/// @note アトラスの 1 コマぶんの UV スケールと、今のコマ番号を求める。
/// @return フリップブックが有効なら true (無効時は恒等な (1,1) / 0 を書く)
bool ResolveDecalFlipbook(const DecalComponent& decal, float outScale[2], float& outIndex)
{
    outScale[0] = 1.0f;
    outScale[1] = 1.0f;
    outIndex    = 0.0f;
    if (!DecalHasFlipbook(decal)) return false;

    const int frames = decal.frameCount;
    const int perRow = std::clamp(decal.framesPerRow, 1, frames);
    const int rows   = (frames + perRow - 1) / perRow;

    /// @note frameRate 0 は「寿命いっぱいで 1 周」。血の乾きや焦げの定着は消えるまでに
    /// @note 終わるのが正しく、lifetime を触るたびに fps を計算し直させたくない。
    float progress = 0.0f;
    if (decal.frameRate > 0.0f)
        progress = decal.age * decal.frameRate / static_cast<float>(frames);
    else if (decal.lifetime > 0.0f)
        progress = decal.age / decal.lifetime;

    if (decal.frameLoop) progress -= std::floor(progress);
    /// @note 止める側は 1.0 を含めない。含めると frames 番目 (存在しないコマ) を指す。
    else                 progress = std::clamp(progress, 0.0f, 0.9999f);

    const int index = std::clamp(static_cast<int>(progress * static_cast<float>(frames)),
                                 0, frames - 1);
    outScale[0] = 1.0f / static_cast<float>(perRow);
    outScale[1] = 1.0f / static_cast<float>(rows);
    outIndex    = static_cast<float>(index);
    return true;
}

float ComputeDecalFade(const DecalComponent& decal)
{
    float fade = 1.0f;
    if (decal.lifetime >= 0.0f && decal.fadeTime > 0.0f)
        fade = std::min(1.0f, (decal.lifetime - decal.age) / decal.fadeTime);
    /// @note 出現側のフェード。永続デカールでも効く。寿命が fadeInTime + fadeTime より短いと両窓が重なるため掛けずに小さい方を採る (掛けると «出きる前に消え始める» 山が二重に低くなり短命な痕がはっきり見えないまま終わる)。
    if (decal.fadeInTime > 0.0f)
        fade = std::min(fade, decal.age / decal.fadeInTime);
    fade = std::clamp(fade, 0.0f, 1.0f);
    return fade;
}

void SetDecalProjection(const GameObject& owner, const DecalComponent& decal,
    float fade, bool hasCustomMaterial, renderer::RenderDecalInput& input)
{
    input.layer = static_cast<uint32_t>(owner.layer);
    input.sortOrder = decal.sortOrder;
    auto& projection = input.projection;
    projection.invDecalWorld  = math::Matrix4::Inverse(owner.transform.GetWorldMatrix());
    projection.decalTangent   = owner.transform.right.Normalized();
    projection.decalBitangent = owner.transform.forward.Normalized();
    projection.decalNormal    = owner.transform.up.Normalized();
    /// @note 組み込み経路のアルベドアルファはコンポーネント側の値。.mat 経路では
    /// @note 材質が tint を持つため、ここはライフタイムフェードと opacity だけにする。
    projection.alpha = fade * std::clamp(decal.opacity, 0.0f, 1.0f)
                    * (hasCustomMaterial ? 1.0f : decal.albedoColor[3]);
    projection.angleFadeStrength = std::clamp(decal.angleFadeStrength, 0.0f, 1.0f);
    /// @note 角度は度で持ち、シェーダーへは cos で渡す。ピクセルごとに acos を取るのは無駄。
    /// @note 90 度で cos=0 になり全ての面が残るため、89 度で上限を切って「必ず何かは消える」形にする。
    projection.angleFadeCos = std::cos(
        std::clamp(decal.angleFadeDegrees, 0.0f, 89.0f) * (3.14159265358979323846f / 180.0f));
    projection.receiverLayerMask = decal.receiverLayerMask;
    ResolveDecalFlipbook(decal, projection.frameScale, projection.frameIndex);
}

} /// @note namespace

void ExtractRenderDecals(RenderPassContext& context, renderer::RenderScene& output)
{
    auto& resources = context.resources;
    auto& handles = context.handles;
    BeginDecalMaterialPass();
    if (context.scene.GetEntities<DecalComponent>().empty()) return;
    const float deltaTime = Time::deltaTime;
    std::vector<EntityID> expiredDecals;
    for (auto& owner : context.scene.GameObjects()) {
        if (!owner.activeInHierarchy()) continue;

        auto* decal = owner.GetComponent<DecalComponent>();
        if (!decal || !decal->enabled) continue;

        if (decal->lastExtractionFrame != resources.FrameStamp()) {
            decal->lastExtractionFrame = resources.FrameStamp();
            if (DecalNeedsAge(*decal)) decal->age += deltaTime;
        }
        if (decal->lifetime >= 0.0f && decal->age >= decal->lifetime) {
            decal->enabled = false;
            expiredDecals.push_back(owner.GetID());
            continue;
        }

        const float fade = ComputeDecalFade(*decal);
        if (fade < 0.001f) continue;
        auto* binding = ResolveDecalMaterial(resources, *decal);
        renderer::RenderDecalInput input;
        SetDecalProjection(owner, *decal, fade, binding != nullptr, input);
        if (binding) BindCustomDecalMaterial(resources, *binding, *decal, input);
        else BindBuiltInDecalMaterial(resources, handles, *decal, input);
        output.decals.push_back(std::move(input));
    }
    for (EntityID id : expiredDecals) {
        context.scene.DestroyGameObject(id);
    }
}

} /// @note namespace fbzz::scene
