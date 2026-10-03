/// @file    GeometryPassHelpers.cpp
/// @brief   ジオメトリパス共有ヘルパー関数。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "GeometryPasses.hpp"
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/RenderVisibility.hpp>
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
#include "Engine/Asset/StreamedTextureResolver.hpp"
#include "Engine/Asset/TexDescSerializer.hpp"
#include "Engine/Asset/TextureAsset.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/WeatherComponent.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ITexture.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Util/FileSystem.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {

namespace {

/// @note 束縛の規則そのものは `Engine/Asset/MaterialParamBinding.hpp` が持つ。UI のように「同じ .mat 形式を、違う頂点入力とパスで描く」側が増えると、無名名前空間に置いた実装は写経するしかなくなり写した先だけが追従しなくなるため、規則は 1 箇所に置く。
using asset::ApplyMaterialAssetParams;
using asset::ApplyMaterialParamOverrides;
using asset::FindMaterialParam;
using asset::InitDefaultMaterialParams;
constexpr auto& kTextureSlotNames = asset::kMaterialTextureSlotNames;

/// @brief albedo に Sprite サブアセットが指定された場合、Sprite 矩形を標準 UV 変換へ合成する。
/// @note GPU Texture は atlas 全体を共有するため、3D Material で個別 Sprite を使うには頂点 UV を矩形の scale/offset へ写像する必要がある。
/// @note 共有 .mat でなく実効 albedo 参照を受け取る。textureOverrides で GameObject 単位に albedo を差し替えると、共有 .mat 側を見ていては矩形が元のスプライトのまま残り「別のコマを指定したのに絵が変わらない」という原因不明の壊れ方をする。表情・目パチのようにコマをランタイムで切り替える用途はこの経路しか通らないため実効値で解決する。
void ApplyAlbedoSpriteUv(std::string_view albedoReference,
                         const renderer::ShaderDescriptor& desc,
                         renderer::ResourceManager& resources,
                         std::vector<uint8_t>& paramData)
{
    if (albedoReference.empty()) return;

    std::string texturePath;
    std::string spriteToken;
    if (!asset::ParseSpriteReference(albedoReference, texturePath, spriteToken)) return;

    /// @note 矩形の取り出しは ResolveSpriteReference が持つ (ID / 名前 / 暗黙 Single と
    /// @note .meta のキャッシュまで含めて 1 箇所)。ここは UV への合成だけを受け持つ。
    /// @note 矩形はピクセル単位なので元画像の寸法で換算する。品質段で縮小して常駐していても GPU 実体の寸法は使わない。
    asset::StreamedTextureResolver& resolver = asset::StreamedTextureResolver::Engine();
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    if (const asset::TextureAsset* textureAsset = resolver.ResolveAsset(resources, texturePath)) {
        sourceWidth = textureAsset->sourceWidth;
        sourceHeight = textureAsset->sourceHeight;
        if ((sourceWidth == 0 || sourceHeight == 0) && resources.Get(textureAsset->gpuHandle)) {
            sourceWidth = resources.Get(textureAsset->gpuHandle)->GetWidth();
            sourceHeight = resources.Get(textureAsset->gpuHandle)->GetHeight();
        }
    } else if (resolver.GetMissPolicy() == asset::StreamedTextureResolver::MissPolicy::Synchronous) {
        const std::string absoluteTexturePath = asset::AssetManager::ResolveAssetPath(texturePath);
        if (const renderer::ITexture* texture = resources.Get(resources.LoadTexture(absoluteTexturePath))) {
            sourceWidth = texture->GetWidth();
            sourceHeight = texture->GetHeight();
        }
    }
    if (sourceWidth == 0 || sourceHeight == 0) return;

    const asset::ResolvedSprite resolved = asset::ResolveSpriteReference(
        albedoReference,
        static_cast<float>(sourceWidth),
        static_cast<float>(sourceHeight));
    if (!resolved.resolved) return;

    /// @note 矩形が画像からはみ出していても UV は画像内へ収める。
    /// @note はみ出した分を素通しすると Clamp サンプリングで端の 1 列が伸びる。
    struct { float scaleX, scaleY, offsetX, offsetY; } transform{
        std::clamp(resolved.uvMax.x - resolved.uvMin.x, 0.0f, 1.0f),
        std::clamp(resolved.uvMax.y - resolved.uvMin.y, 0.0f, 1.0f),
        std::clamp(resolved.uvMin.x, 0.0f, 1.0f),
        std::clamp(resolved.uvMin.y, 0.0f, 1.0f),
    };

    const auto* tilingVar = desc.FindVar("uvTiling");
    const auto* offsetVar = desc.FindVar("uvOffset");
    if (tilingVar == nullptr || offsetVar == nullptr
        || tilingVar->varType != renderer::ShaderVarType::Float || tilingVar->columns < 2
        || offsetVar->varType != renderer::ShaderVarType::Float || offsetVar->columns < 2
        || tilingVar->offset + 2u * sizeof(float) > paramData.size()
        || offsetVar->offset + 2u * sizeof(float) > paramData.size())
        return;

    float tiling[2] = { 1.0f, 1.0f };
    float offset[2] = { 0.0f, 0.0f };
    std::memcpy(tiling, paramData.data() + tilingVar->offset, sizeof(tiling));
    std::memcpy(offset, paramData.data() + offsetVar->offset, sizeof(offset));
    tiling[0] *= transform.scaleX;
    tiling[1] *= transform.scaleY;
    offset[0] = offset[0] * transform.scaleX + transform.offsetX;
    offset[1] = offset[1] * transform.scaleY + transform.offsetY;
    std::memcpy(paramData.data() + tilingVar->offset, tiling, sizeof(tiling));
    std::memcpy(paramData.data() + offsetVar->offset, offset, sizeof(offset));
}

}

AnimatorComponent* FindAnimator(GameObject& go)
{
    /// @note Skinned submesh はモデル構造により複数階層下へ配置されるため、直親だけで打ち切らない。Animator を見失うと bind pose 用 CB へフォールバックし、Trail の初期位置もずれる。
    for (GameObject* current = &go; current; current = current->GetParent())
        if (auto* animator = current->GetComponent<AnimatorComponent>())
            return animator;
    return nullptr;
}

const char* GetFallbackMaterialPath(bool skinned)
{
    return skinned ? "Assets/Materials/Fallback/FallbackSkinned.mat"
                   : "Assets/Materials/Fallback/Fallback.mat";
}

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath)
{
    static std::unordered_set<std::string> s_warnedSurfaceOnSkinned;
    std::string path(shaderPath);
    if (path.empty()) path = "<empty>";
    if (!s_warnedSurfaceOnSkinned.insert(path).second) return;
    FBZZ_LOG_WARN("SkinnedMeshRenderer has a Surface shader assigned: %s -> using %s. Use shaders under Skinned/.",
                  path.c_str(),
                  GetFallbackMaterialPath(true));
}

renderer::Material* GetFallbackMaterial(renderer::ResourceManager& resources, bool skinned)
{
    static MaterialComponent s_surfaceFallback;
    static MaterialComponent s_skinnedFallback;
    MaterialComponent& fallback = skinned ? s_skinnedFallback : s_surfaceFallback;
    fallback.materialPath = GetFallbackMaterialPath(skinned);
    if (!fallback.EnsureMaterialAsset()) return nullptr;
    return SyncMaterial(fallback, resources, skinned);
}

/// @note 実体。MaterialComponent とスロットを分けて受け取り、
/// @note 「コンポーネント全体の有効/無効」と「スロット単体の有効/無効」を両方尊重する。
static renderer::Material* SyncMaterialSlotImpl(MaterialSlot& mc,
                                                bool componentEnabled,
                                                renderer::ResourceManager& resources,
                                                bool preferSkinnedFallback,
                                                float screenPixels)
{
    if (!componentEnabled || !mc.visible) return nullptr;

    if (!mc.materialPath.empty() && !mc.materialAsset.IsValid())
        mc.materialAsset = asset::AssetManager::Load<asset::MaterialAsset>(mc.materialPath);

    auto activeAsset = mc.materialAsset;
    if (!activeAsset.IsValid()) {
        /// @note .mat が読めない / 未割当でも、メッシュを画面から絶対に消さない。原色紫のフォールバック材質で描画を続け、問題を可視化する (Unity のマゼンタ相当)。
        /// @note mc.materialAsset には書き戻さず毎フレーム再解決させる。壊れた .mat を修復・再インポートした瞬間 (FlushFailed 後) に正規材質へ自動復帰できる。
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        static std::unordered_set<std::string> s_warnedMissingMaterials;
        const std::string warnKey =
            mc.materialPath.empty() ? std::string("<unassigned>") : mc.materialPath;
        if (s_warnedMissingMaterials.insert(warnKey).second) {
            FBZZ_LOG_WARN("Material load failed '%s' -> using fallback %s.",
                          warnKey.c_str(), fallbackPath);
        }
        activeAsset = asset::AssetManager::Load<asset::MaterialAsset>(fallbackPath);
        /// @note フォールバック .mat 自体が存在しない場合だけは描画を諦める。
        if (!activeAsset.IsValid()) return nullptr;
    }

    if (!mc.material)
        mc.material = std::make_unique<renderer::Material>();

    auto& material = *mc.material;
    const std::string& shaderPath = mc.GetShaderPath();
    const auto* matAsset = asset::AssetManager::Get<asset::MaterialAsset>(activeAsset);
    if (matAsset && shaderPath.empty()) {
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        /// @note shader 未設定の .mat を PBR 推定で描くと、未設定と意図した PBR の区別が付かない。原色紫の Unlit フォールバック材質へ明示的に差し替え、問題箇所を見つけやすくする。
        static std::unordered_set<std::string> s_warnedEmptyShaderMaterials;
        const std::string warnKey = mc.materialPath.empty() ? std::string("<unnamed>") : mc.materialPath;
        if (s_warnedEmptyShaderMaterials.insert(warnKey + "|" + fallbackPath).second) {
            FBZZ_LOG_WARN("Material '%s' has an empty shader path -> using %s.",
                          warnKey.c_str(), fallbackPath);
        }
        activeAsset = asset::AssetManager::Load<asset::MaterialAsset>(fallbackPath);
        matAsset = asset::AssetManager::Get<asset::MaterialAsset>(activeAsset);
        if (!matAsset) return nullptr;
    }
    const std::string effectiveShaderPath = matAsset ? matAsset->shaderPath : std::string{};
    material.shaderPath = effectiveShaderPath;
    material.dielectric = matAsset ? matAsset->dielectric : renderer::SolidDielectricSettings{};
    material.shader = effectiveShaderPath.empty()
        ? renderer::ResourceHandle<renderer::ShaderTag>{}
        : resources.LoadShader(effectiveShaderPath);

    const renderer::ShaderDescriptor* desc = nullptr;
    if (auto* shader = resources.Get(material.shader))
        desc = &shader->GetDescriptor();

    material.paramData.assign(desc ? desc->cbufferSize : 0u, 0u);
    if (desc)
        InitDefaultMaterialParams(*desc, material.paramData);
    if (desc && matAsset)
        ApplyMaterialAssetParams(*matAsset, *desc, material.paramData);
    /// @note 共有アセット適用後にこの GO 専用の上書きを重ねる (per-instance パラメータ)。
    if (desc && !mc.paramOverrides.empty())
        ApplyMaterialParamOverrides(mc.paramOverrides, *desc, material.paramData);
    if (desc && !mc.integerParamOverrides.empty())
        asset::ApplyMaterialIntegerOverrides(mc.integerParamOverrides, *desc, material.paramData);

    std::array<std::string, kTextureSlotNames.size()> texturePaths{};
    if (matAsset) {
        for (size_t i = 0; i < kTextureSlotNames.size(); ++i) {
            const auto it = matAsset->textures.find(kTextureSlotNames[i]);
            texturePaths[i] = it != matAsset->textures.end() ? it->second : std::string{};
        }
    }
    for (size_t i = 0; i < kTextureSlotNames.size(); ++i) {
        const auto overrideIt = mc.textureOverrides.find(kTextureSlotNames[i]);
        if (overrideIt != mc.textureOverrides.end())
            texturePaths[i] = overrideIt->second;
    }

    /// @note Sprite 矩形は「最終的に t0 へ束縛される参照」から決める。共有 .mat の値ではなく
    /// @note 上書き適用後の texturePaths[0] を渡すため、テクスチャ解決より後に置く。
    if (desc)
        ApplyAlbedoSpriteUv(texturePaths[0], *desc, resources, material.paramData);

    /// @note テクスチャは非同期台帳の利用権越しに引く。毎フレーム引かれている間は利用権が続き、引かれなく
    /// @note なると手放されて台帳の猶予と予算で解放される。先読み済みなら同期ロードは起きない。
    const size_t slotCount = texturePaths.size();
    material.textures.resize(slotCount);
    asset::StreamedTextureResolver& resolver = asset::StreamedTextureResolver::Engine();
    for (size_t i = 0; i < slotCount; ++i)
    {
        material.textures[i] = texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resolver.ResolveGpu(resources, texturePaths[i], screenPixels);
    }

    static renderer::ShaderDescriptor s_fallback;
    material.Upload(resources, desc ? *desc : s_fallback);
    return &material;
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources,
                                 bool preferSkinnedFallback, float screenPixels)
{
    return SyncMaterialSlotImpl(mc, mc.enabled, resources, preferSkinnedFallback, screenPixels);
}

renderer::Material* SyncMaterialSlot(MaterialComponent& mc, size_t slotIndex,
                                     renderer::ResourceManager& resources, bool preferSkinnedFallback,
                                     float screenPixels)
{
    /// @note mc.enabled は基底 (スロット 0) の enabled であり、コンポーネント全体の有効判定を兼ねる。
    return SyncMaterialSlotImpl(mc.SlotAt(slotIndex), mc.enabled, resources, preferSkinnedFallback, screenPixels);
}

float EstimateScreenPixels(const RenderPassContext& ctx, const GameObject& go, const renderer::Mesh& mesh)
{
    if (ctx.cullProjScaleY <= 0.0f || ctx.height == 0 || mesh.boundsRadius <= 0.0f) return 0.0f;
    const WorldBounds bounds = ComputeWorldBounds(go.transform, mesh);
    /// @note 投影半径 (NDC) = r * P[1][1] / 距離。NDC の縦 [-1, 1] が height px なので直径は r*P11/d*height。
    /// @note カメラが球の中へ入ったら画面いっぱいとして扱う。
    const float distance = (bounds.center - ctx.camera.m_position).Length();
    if (!ctx.cullOrthographic && distance <= bounds.radius) return static_cast<float>(ctx.height);
    const float ndcRadius = ctx.cullOrthographic
        ? bounds.radius * ctx.cullProjScaleY
        : bounds.radius * ctx.cullProjScaleY / distance;
    return ndcRadius * static_cast<float>(ctx.height);
}

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    const MaterialSlot&        slot,
    bool                      wireframe)
{
    return GetOrCreateMaterialPSO(resources, slot.GetBlendMode(), slot.IsDoubleSided(),
                                  slot.GetDepthBias(), slot.GetDepthBiasSlope(), wireframe);
}

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask)
{
    return go.activeInHierarchy() && fbzz::Layer::Contains(mask, go.layer);
}

/// @note かつてここに IsForwardOnly があった。GBuffer に入れるかどうかの判断は
/// @note ResolveGeometryRoute (Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp) が唯一の正本。
/// @note 判定が 2 つあると片方だけ古くなり、物が二重に描かれるか黙って消える。
/// @see Docs/design/pipeline-boundary.md

bool IsSurfaceMaterial(const MaterialSlot& mc)
{
    const auto* a = asset::AssetManager::Get<asset::MaterialAsset>(mc.materialAsset);
    if (a) {
        if (a->meshType == asset::MeshType::Surface) return true;
        if (a->meshType == asset::MeshType::Skinned) return false;
        if (a->shaderPath.empty()) return false;
    }
    return false;
}

/// @name カリング ヘルパー

bool IsReliableOccluder(const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius <= 0.0f) return false;

    /// @note 立方体で 0.577、球で 1.0。板・壁・棒はこれを大きく下回る。
    /// @note 0.40 は「立方体は通し、厚み比 1:4 を超える扁平は落とす」あたりの線。
    constexpr float kMinFillRatio = 0.40f;
    const float minExtent = std::min({ mesh.boundsExtents.x,
                                       mesh.boundsExtents.y,
                                       mesh.boundsExtents.z });
    return minExtent / mesh.boundsRadius >= kMinFillRatio;
}

WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh, float padding)
{
    const math::Matrix4 world = tf.GetWorldMatrix();

    /// @note ローカル空間バウンディング球中心をワールド空間に変換する。
    /// @note 行列は列ベクトル規則 (M * v) なので:
    /// @note wx = m[0][0]*bx + m[0][1]*by + m[0][2]*bz + m[0][3]
    const float bx = mesh.boundsCenter.x;
    const float by = mesh.boundsCenter.y;
    const float bz = mesh.boundsCenter.z;
    const math::Vector3 worldCenter = {
        world.m[0][0]*bx + world.m[0][1]*by + world.m[0][2]*bz + world.m[0][3],
        world.m[1][0]*bx + world.m[1][1]*by + world.m[1][2]*bz + world.m[1][3],
        world.m[2][0]*bx + world.m[2][1]*by + world.m[2][2]*bz + world.m[2][3],
    };

    /// @note ワールド行列の各軸ベクトルのノルムからスケールを取得し、最大値を掛ける。非一様スケールでは最大成分で保守的な球にする。球半径を過大評価しても偽カリング (見えているのに消える) は発生しない。
    const float sx = std::sqrt(world.m[0][0]*world.m[0][0] + world.m[1][0]*world.m[1][0] + world.m[2][0]*world.m[2][0]);
    const float sy = std::sqrt(world.m[0][1]*world.m[0][1] + world.m[1][1]*world.m[1][1] + world.m[2][1]*world.m[2][1]);
    const float sz = std::sqrt(world.m[0][2]*world.m[0][2] + world.m[1][2]*world.m[1][2] + world.m[2][2]*world.m[2][2]);
    const float maxScale = std::max({ sx, sy, sz });

    return { worldCenter, mesh.boundsRadius * maxScale + (padding > 0.0f ? padding : 0.0f) };
}

bool ComputeSkinnedWorldBounds(const GameObject& go,
                               const SkinnedMeshRenderer& smr,
                               WorldBounds& outBounds,
                               float padding)
{
    if (!smr.model) return false;

    const Transform& tf = go.transform;

    /// @note 骨から作った球があればそちらを使う。下のループはバインドポーズの submesh バウンズを Renderer の Transform で運ぶだけで骨の移動を見ないため、Script が骨を数十 m 動かす構成では球がバインドポーズの場所に取り残され部位ごとに明滅する。
    /// @note Animator が持つのは骨の広がりだけで周囲のジオメトリは submesh のバインド半径ぶん外へ出るため、この Renderer が描く submesh の最大値を肉の厚みとして足す。
    /// @note GetComponent に const 版が無いので剥がす。ここは読むだけ。
    for (GameObject* current = const_cast<GameObject*>(&go); current;
         current = current->GetParent()) {
        const auto* animator = current->GetComponent<AnimatorComponent>();
        if (!animator) continue;
        if (animator->skinnedBoundsRadius <= 0.0f) break;

        float flesh = 0.0f;
        for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot)
            if (const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot))
                flesh = (std::max)(flesh, meshPtr->boundsRadius);

        /// @note 球は Animator の owner のローカル空間にある。運ぶのもその Transform。
        const math::Matrix4 world = current->transform.GetWorldMatrix();
        const math::Vector3& c = animator->skinnedBoundsCenter;
        outBounds.center = {
            world.m[0][0]*c.x + world.m[0][1]*c.y + world.m[0][2]*c.z + world.m[0][3],
            world.m[1][0]*c.x + world.m[1][1]*c.y + world.m[1][2]*c.z + world.m[1][3],
            world.m[2][0]*c.x + world.m[2][1]*c.y + world.m[2][2]*c.z + world.m[2][3],
        };
        const float sx = std::sqrt(world.m[0][0]*world.m[0][0] + world.m[1][0]*world.m[1][0] + world.m[2][0]*world.m[2][0]);
        const float sy = std::sqrt(world.m[0][1]*world.m[0][1] + world.m[1][1]*world.m[1][1] + world.m[2][1]*world.m[2][1]);
        const float sz = std::sqrt(world.m[0][2]*world.m[0][2] + world.m[1][2]*world.m[1][2] + world.m[2][2]*world.m[2][2]);
        const float maxScale = (std::max)({ sx, sy, sz });

        outBounds.radius = (animator->skinnedBoundsRadius + flesh) * maxScale;
        if (padding > 0.0f) outBounds.radius += padding;
        return true;
    }

    bool hasBounds = false;
    math::Vector3 weightedCenter = math::Vector3::ZERO;
    float totalWeight = 0.0f;

    /// @note 各 submesh のワールド球を半径重みで平均し、最後に全 submesh を包む半径へ拡張する。毎フレーム CPU スキニングして厳密 bounds を取ると頂点数に比例して重いため、視錐台外の遠いキャラクターを安く除外できる保守的なバインドポーズ球を使う。
    /// @note この Renderer が描く submesh だけを包む。ノードごとに子 GameObject へ分けた構成ではモデル全体の球はどの子にとっても過大になり画面外の部位のぶんまで視錐台に残るため、カリングの単位は「実際に描くもの」に揃える。
    for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot) {
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot);
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        const WorldBounds bounds = ComputeWorldBounds(tf, *meshPtr);
        const float weight = (std::max)(bounds.radius, 0.001f);
        weightedCenter = weightedCenter + bounds.center * weight;
        totalWeight += weight;
        hasBounds = true;
    }

    if (!hasBounds || totalWeight <= 0.0f) return false;

    outBounds.center = weightedCenter * (1.0f / totalWeight);
    outBounds.radius = 0.0f;
    /// @note この Renderer が描く submesh だけを包む。モデル全体で取ると、ノードごとに子 GameObject へ分けた構成では過大な球になり画面外の部位のぶんまで視錐台に残るため、カリングの単位は「実際に描くもの」に揃える。
    for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot) {
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot);
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        const WorldBounds bounds = ComputeWorldBounds(tf, *meshPtr);
        const math::Vector3 delta = bounds.center - outBounds.center;
        outBounds.radius = (std::max)(outBounds.radius, delta.Length() + bounds.radius);
    }

    /// @note 余白は submesh ごとではなく合成後の球へ 1 回だけ足す。ComputeWorldBounds へ渡して submesh 単位で足すと、合成時の「中心距離 + 半径」に padding が二重・三重で積み上がる。
    if (padding > 0.0f) outBounds.radius += padding;

    return true;
}

namespace {

/// @note このレイヤーに適用する描画距離 [m] を返す。0 は「距離カリングしない」。
float ResolveCullDistance(const RenderPassContext& ctx, int layer)
{
    if (ctx.hasLayerCullDistances) {
        const float perLayer = ctx.cullLayerDistances[layer & (kCullLayerCount - 1)];
        if (perLayer > 0.0f) return perLayer;
    }
    return ctx.cullMaxDistance;
}

renderer::RenderCullingView ExtractCullingView(const RenderPassContext& ctx)
{
    renderer::RenderCullingView view;
    view.position = ctx.camera.m_position;
    view.forward = ctx.cullCameraForward;
    view.frustum = ctx.frustumCullingEnabled ? ctx.cameraFrustum : nullptr;
    view.projectionScaleY = ctx.cullProjScaleY;
    view.smallObjectScreenHeight = ctx.smallObjectScreenHeight;
    view.distanceSpherical = ctx.cullDistanceSpherical;
    view.orthographic = ctx.cullOrthographic;
    return view;
}

bool TestBoundsVisible(RenderPassContext& ctx, const GameObject& go, const WorldBounds& bounds)
{
    const renderer::RenderCullingItem item{
        bounds.center, bounds.radius, ResolveCullDistance(ctx, go.layer)
    };
    switch (renderer::EvaluateVisibility(ExtractCullingView(ctx), item)) {
    case renderer::VisibilityResult::DISTANCE_CULLED:
        ++ctx.statsDistanceCulled;
        return false;
    case renderer::VisibilityResult::SMALL_OBJECT_CULLED:
        ++ctx.statsSmallObjectCulled;
        return false;
    case renderer::VisibilityResult::FRUSTUM_CULLED:
        ++ctx.statsFrustumCulled;
        return false;
    case renderer::VisibilityResult::VISIBLE:
        return true;
    }
    return true;
}

}

bool IsMeshVisible(RenderPassContext& ctx,
                   const GameObject& go,
                   const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius <= 0.0f) return true;
    const WorldBounds bounds =
        ComputeWorldBounds(go.transform, mesh, ctx.cullingBoundsPadding);
    return TestBoundsVisible(ctx, go, bounds);
}

bool IsSkinnedVisible(RenderPassContext& ctx,
                      const GameObject& go,
                      const SkinnedMeshRenderer& smr)
{
    WorldBounds bounds{};
    /// @note bounds を作れない (CPU 頂点未生成など) 場合は安全側に倒して描く。
    if (!ComputeSkinnedWorldBounds(go, smr, bounds, ctx.cullingBoundsPadding))
        return true;
    return TestBoundsVisible(ctx, go, bounds);
}

bool IsWithinCullDistance(const RenderPassContext& ctx,
                          const GameObject& go,
                          const WorldBounds& bounds)
{
    return renderer::IsWithinDrawDistance(ExtractCullingView(ctx), {
        bounds.center, bounds.radius, ResolveCullDistance(ctx, go.layer)
    });
}

ActiveWeather FindActiveWeather(Scene& scene)
{
    ActiveWeather result;
    for (auto& go : scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const auto* weather = go.GetComponent<WeatherComponent>();
        if (!weather || !weather->enabled) continue;
        result.wetness      = std::clamp(weather->wetness, 0.0f, 1.0f);
        result.darkening    = std::clamp(weather->darkening, 0.0f, 1.0f);
        result.puddleAmount = std::clamp(weather->puddleAmount, 0.0f, 1.0f);
        /// @note シーンに 1 つ想定。環境風と同じ扱い
        break;
    }
    return result;
}

bool IsEffectTextureSrgb(const std::string& texturePath)
{
    /// @note 1x1 白フォールバックなど参照が無い場合。1.0 はどちらの空間でも 1.0 なので変換しない。
    if (texturePath.empty()) return false;

    /// @note 毎フレーム呼ばれる経路 (Trail) があるため結果を持つ。.meta の更新時刻で無効化して、
    /// @note エディターで再インポートしたときに古い判定が残らないようにする。
    struct CachedSrgb {
        std::filesystem::file_time_type metaWriteTime{};
        bool srgb = true;
        bool resolved = false;
    };
    static std::unordered_map<std::string, CachedSrgb> s_cache;

    const std::string resolved = asset::AssetManager::ResolveAssetPath(texturePath);
    std::string sourcePath;
    if (!asset::TexDescSerializer::ResolveSourcePath(resolved, sourcePath)) return true;
    const std::string metaPath = sourcePath + ".meta";
    std::error_code ec;
    const auto metaWriteTime = std::filesystem::last_write_time(metaPath, ec);

    CachedSrgb& cached = s_cache[texturePath];
    if (cached.resolved && cached.metaWriteTime == metaWriteTime) return cached.srgb;
    cached.resolved = true;
    cached.metaWriteTime = metaWriteTime;

    asset::TextureAsset described;
    const asset::TexDescSerializer serializer;
    if (!ec && serializer.Load(metaPath, described)) {
        cached.srgb = described.settings.srgb;
        return cached.srgb;
    }
    /// @note サイドカーが無い素材はインポーターと同じ推定に従う (色テクスチャ = sRGB)。
    const auto slash = sourcePath.find_last_of("/\\");
    const std::string filename =
        slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);
    cached.srgb = asset::DefaultSettingsForType(asset::GuessTextureType(filename)).srgb;
    return cached.srgb;
}

}
