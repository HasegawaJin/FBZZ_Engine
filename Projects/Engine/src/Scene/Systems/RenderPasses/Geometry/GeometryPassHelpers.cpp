/// @file    RenderPasses/GeometryPassHelpers.cpp
/// @brief   ジオメトリパス共有ヘルパー関数。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "GeometryPasses.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
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

// 束縛の規則そのものは Engine/Asset/MaterialParamBinding.hpp が持つ。
// WHY ここから出したか: UI のように「同じ .mat 形式を、違う頂点入力とパスで描く」側が
//     増えると、無名名前空間に置いた実装は写経するしかなくなる。写した先だけが
//     スロット名や既定値の変更に追従しなくなるので、規則は 1 箇所に置く。
using asset::ApplyMaterialAssetParams;
using asset::ApplyMaterialParamOverrides;
using asset::FindMaterialParam;
using asset::InitDefaultMaterialParams;
constexpr auto& kTextureSlotNames = asset::kMaterialTextureSlotNames;

// albedo に Sprite サブアセットが指定された場合、Sprite矩形を標準UV変換へ合成する。
// WHY: GPU Texture 自体はatlas全体を共有するため、3D Materialで個別Spriteを使うには
//      頂点UVを矩形のscale/offsetへ写像する必要がある。
//
// WHY 共有 .mat ではなく「実効 albedo 参照」を受け取るか:
//   textureOverrides で GameObject 単位に albedo を差し替えた場合、共有 .mat 側を見ていると
//   矩形が元のスプライトのまま残る。atlas は 1 枚のテクスチャなので、テクスチャだけ
//   差し替わって矩形が変わらないと「別のコマを指定したのに絵が変わらない」という、
//   スクリプトからは原因の見えない壊れ方をする。表情・目パチのようにコマを
//   ランタイムで切り替える用途はこの経路しか通らないため、実効値で解決する。
void ApplyAlbedoSpriteUv(std::string_view albedoReference,
                         const renderer::ShaderDescriptor& desc,
                         renderer::ResourceManager& resources,
                         std::vector<uint8_t>& paramData)
{
    if (albedoReference.empty()) return;

    std::string texturePath;
    std::string spriteToken;
    if (!asset::ParseSpriteReference(albedoReference, texturePath, spriteToken)) return;

    // 矩形の取り出しは ResolveSpriteReference が持つ (ID / 名前 / 暗黙 Single と
    // .meta のキャッシュまで含めて 1 箇所)。ここは UV への合成だけを受け持つ。
    const std::string absoluteTexturePath = asset::AssetManager::ResolveAssetPath(texturePath);
    const renderer::ITexture* texture = resources.Get(resources.LoadTexture(absoluteTexturePath));
    if (texture == nullptr) return;

    const asset::ResolvedSprite resolved = asset::ResolveSpriteReference(
        albedoReference,
        static_cast<float>(std::max<uint32_t>(1, texture->GetWidth())),
        static_cast<float>(std::max<uint32_t>(1, texture->GetHeight())));
    if (!resolved.resolved) return;

    // 矩形が画像からはみ出していても UV は画像内へ収める。
    // はみ出した分を素通しすると Clamp サンプリングで端の 1 列が伸びる。
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

} // namespace

AnimatorComponent* FindAnimator(GameObject& go)
{
    // Skinned submesh はモデル構造により複数階層下へ配置されるため、直親だけで打ち切らない。
    // WHY: Animatorを見失うとbind pose用CBへフォールバックし、Trailの初期位置もずれる。
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

// 実体。MaterialComponent とスロットを分けて受け取り、
// 「コンポーネント全体の有効/無効」と「スロット単体の有効/無効」を両方尊重する。
static renderer::Material* SyncMaterialSlotImpl(MaterialSlot& mc,
                                                bool componentEnabled,
                                                renderer::ResourceManager& resources,
                                                bool preferSkinnedFallback)
{
    if (!componentEnabled || !mc.visible) return nullptr;

    if (!mc.materialPath.empty() && !mc.materialAsset.IsValid())
        mc.materialAsset = asset::AssetManager::LoadMaterial(mc.materialPath);

    auto activeAsset = mc.materialAsset;
    if (!activeAsset.IsValid()) {
        // .mat が読めない / 未割当でも、メッシュを画面から絶対に消さない。
        // 原色紫のフォールバック材質で描画を続け、問題を可視化する (Unity のマゼンタ相当)。
        // WHY: mc.materialAsset には書き戻さず毎フレーム再解決させる。壊れた .mat を
        //      修復・再インポートした瞬間 (FlushFailed 後) に正規材質へ自動復帰できる。
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        static std::unordered_set<std::string> s_warnedMissingMaterials;
        const std::string warnKey =
            mc.materialPath.empty() ? std::string("<unassigned>") : mc.materialPath;
        if (s_warnedMissingMaterials.insert(warnKey).second) {
            FBZZ_LOG_WARN("Material load failed '%s' -> using fallback %s.",
                          warnKey.c_str(), fallbackPath);
        }
        activeAsset = asset::AssetManager::LoadMaterial(fallbackPath);
        // フォールバック .mat 自体が存在しない場合だけは描画を諦める。
        if (!activeAsset.IsValid()) return nullptr;
    }

    if (!mc.material)
        mc.material = std::make_unique<renderer::Material>();

    auto& material = *mc.material;
    const std::string& shaderPath = mc.GetShaderPath();
    const auto* matAsset = asset::AssetManager::GetMaterial(activeAsset);
    if (matAsset && shaderPath.empty()) {
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        // WHY: shader 未設定の .mat を PBR 推定で描くと、未設定と意図した PBR の区別が付かない。
        //      原色紫の Unlit フォールバック材質へ明示的に差し替え、問題箇所を見つけやすくする。
        static std::unordered_set<std::string> s_warnedEmptyShaderMaterials;
        const std::string warnKey = mc.materialPath.empty() ? std::string("<unnamed>") : mc.materialPath;
        if (s_warnedEmptyShaderMaterials.insert(warnKey + "|" + fallbackPath).second) {
            FBZZ_LOG_WARN("Material '%s' has an empty shader path -> using %s.",
                          warnKey.c_str(), fallbackPath);
        }
        activeAsset = asset::AssetManager::LoadMaterial(fallbackPath);
        matAsset = asset::AssetManager::GetMaterial(activeAsset);
        if (!matAsset) return nullptr;
    }
    const std::string effectiveShaderPath = matAsset ? matAsset->shaderPath : std::string{};
    material.shaderPath = effectiveShaderPath;
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
    // 共有アセット適用後にこの GO 専用の上書きを重ねる (per-instance パラメータ)。
    if (desc && !mc.paramOverrides.empty())
        ApplyMaterialParamOverrides(mc.paramOverrides, *desc, material.paramData);

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

    // Sprite 矩形は「最終的に t0 へ束縛される参照」から決める。共有 .mat の値ではなく
    // 上書き適用後の texturePaths[0] を渡すため、テクスチャ解決より後に置く。
    if (desc)
        ApplyAlbedoSpriteUv(texturePaths[0], *desc, resources, material.paramData);

    const size_t slotCount = texturePaths.size();
    material.textures.resize(slotCount);
    for (size_t i = 0; i < slotCount; ++i)
    {
        material.textures[i] = texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resources.LoadTexture(texturePaths[i]);
    }

    static renderer::ShaderDescriptor s_fallback;
    material.Upload(resources, desc ? *desc : s_fallback);
    return &material;
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback)
{
    return SyncMaterialSlotImpl(mc, mc.enabled, resources, preferSkinnedFallback);
}

renderer::Material* SyncMaterialSlot(MaterialComponent& mc, size_t slotIndex,
                                     renderer::ResourceManager& resources, bool preferSkinnedFallback)
{
    // mc.enabled は基底 (スロット 0) の enabled であり、コンポーネント全体の有効判定を兼ねる。
    return SyncMaterialSlotImpl(mc.SlotAt(slotIndex), mc.enabled, resources, preferSkinnedFallback);
}

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided)
{
    // 両面描画はバックフェースカリングを無効化する。
    const renderer::RasterizerMode raster = doubleSided
        ? renderer::RasterizerMode::SOLID_NOCULL
        : renderer::RasterizerMode::SOLID;
    // 半透明・加算は深度書き込みをオフにし、背後のオブジェクトが透けて見えるようにする。
    const renderer::DepthMode depth = (blend == renderer::BlendMode::OPAQUE_BLEND)
        ? renderer::DepthMode::DEPTH_ON
        : renderer::DepthMode::DEPTH_READ;

    // WHY: ビットパッキング (旧実装) は enum 値追加時にサイレントなキー衝突が起きるため、
    //      構造体を直接比較する std::map に変更した。
    struct DescLess {
        bool operator()(const renderer::PipelineStateDesc& a,
                        const renderer::PipelineStateDesc& b) const noexcept {
            if (a.rasterizer != b.rasterizer) return a.rasterizer < b.rasterizer;
            if (a.blend      != b.blend)      return a.blend      < b.blend;
            return a.depth < b.depth;
        }
    };
    static std::map<renderer::PipelineStateDesc,
                    renderer::ResourceHandle<renderer::PipelineStateTag>,
                    DescLess> s_cache;
    const renderer::PipelineStateDesc desc{ raster, blend, depth };
    auto it = s_cache.find(desc);
    if (it != s_cache.end()) return it->second;
    auto handle = resources.CreatePipelineState(desc);
    s_cache[desc] = handle;
    return handle;
}

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask)
{
    return go.activeInHierarchy() && fbzz::Layer::Contains(mask, go.layer);
}

bool IsForwardOnly(const MaterialSlot& mc)
{
    const auto* a = asset::AssetManager::GetMaterial(mc.materialAsset);
    if (a) {
        // WHY: 2枚のGBufferにはclearcoat/sheen/anisotropyと接線基底を保持できない。
        //      拡張ローブをDeferredへ落とすと情報が欠落し、物理的なエネルギー配分も
        //      変わるため、拡張値が有効な場合だけ Forward の完全評価へフォールバックする。
        //      これは Material の render_path 指定ではなく、現在の GBuffer 仕様からの自動判定。
        const auto hasFeature = [&](std::string_view name) {
            const auto overrideIt = mc.paramOverrides.find(std::string(name));
            if (overrideIt != mc.paramOverrides.end())
                return !overrideIt->second.empty() && std::abs(overrideIt->second.front()) > 1.0e-4f;
            const auto* values = FindMaterialParam(*a, name);
            return values && !values->empty() && std::abs(values->front()) > 1.0e-4f;
        };
        const bool advancedPbr = a->meshType != asset::MeshType::Skinned &&
            (hasFeature("clearcoat") || hasFeature("sheen") || hasFeature("anisotropy"));
        if (advancedPbr) return true;
        if (a->shaderPath.empty()) return true;
    }
    return a == nullptr;
}

bool IsSurfaceMaterial(const MaterialSlot& mc)
{
    const auto* a = asset::AssetManager::GetMaterial(mc.materialAsset);
    if (a) {
        if (a->meshType == asset::MeshType::Surface) return true;
        if (a->meshType == asset::MeshType::Skinned) return false;
        if (a->shaderPath.empty()) return false;
    }
    return false;
}

// ── カリング ヘルパー ────────────────────────────────────────────────────────

void UpdateShadowConstants(RenderPassContext& ctx)
{
    const auto& rs = ctx.settings;
    ShadowConstantsCB data{};

    // 全カスケードが共有する 1 枚のアトラスなので、テクセルサイズはアトラス全体基準。
    // カスケード内 UV → アトラス UV への写像は HLSL 側 (cascadeAtlasRect) が行う。
    const float texel = 1.0f / static_cast<float>((std::max)(rs.shadow.mapResolution, 1u));
    data.shadowMapTexelSize[0] = texel;
    data.shadowMapTexelSize[1] = texel;

    const int count = std::clamp(ctx.shadowCascadeCount, 1, renderer::kMaxShadowCascades);
    data.cascadeCount     = count;
    data.cascadeBlend     = std::clamp(rs.shadow.cascadeBlend, 0.0f, 0.5f);
    // 可視化は分割している時だけ意味がある。1 分割で有効なままだと画面全体が
    // カスケード 0 の色に染まるだけなので、ここで落とす。
    data.cascadeDebugView = (rs.shadow.debugVisualizeCascades && count > 1) ? 1 : 0;

    float bias[renderer::kMaxShadowCascades] = {};
    for (int i = 0; i < renderer::kMaxShadowCascades; ++i) {
        // 未使用スロットは最遠カスケードで埋める。HLSL 側は cascadeCount までしか
        // 見ないが、未初期化の行列が残ると RenderDoc 等で追うときに紛らわしい。
        const ShadowCascade& cascade = ctx.shadowCascades[(i < count) ? i : count - 1];
        data.cascadeViewProjection[i] = cascade.viewProjection;
        data.cascadeAtlasRect[i]      = cascade.atlasRect;
        bias[i]                       = cascade.biasNDC;
    }
    data.cascadeBias = { bias[0], bias[1], bias[2], bias[3] };

    // 単一のライト行列で足りるパス向け (= 最遠カスケード)。
    // cascadeCount == 1 のときはカスケード 0 と同一なので、従来の単一シャドウマップ経路と一致する。
    data.lightViewProjection = ctx.lightVP;
    data.shadowBias          = ctx.shadowBiasNDC;
    data.shadowStrength      = ctx.shadowStrength;
    data.shadowPcfRadius     = rs.shadow.pcfRadius;

    data.cloudShadowStrength = ctx.cloudShadowStrength;
    data.cloudShadowCoverage = ctx.cloudShadowCoverage;
    data.cloudShadowScale    = ctx.cloudShadowScale;
    data.cloudShadowSpeed    = ctx.cloudShadowSpeed;
    data.cloudShadowTime     = ctx.cloudShadowTime;
    data.cloudShadowWindX    = ctx.cloudShadowWindX;
    data.cloudShadowWindZ    = ctx.cloudShadowWindZ;

    ctx.resources.Update(ctx.handles.shadowCB, &data, sizeof(ShadowConstantsCB));
}

void UpdatePunctualShadowConstants(RenderPassContext& ctx)
{
    if (!ctx.handles.punctualShadowCB.IsValid()) return;

    const auto& rs = ctx.settings;
    PunctualShadowConstantsCB data{};

    // 全スロットが 1 枚のアトラスを共有するので、テクセルサイズはアトラス全体基準。
    // タイル内 UV → アトラス UV への写像は HLSL 側 (punctualShadowRect) が行う。
    const float texel =
        1.0f / static_cast<float>((std::max)(ctx.punctualShadowResolution, 1u));
    data.punctualShadowTexel[0] = texel;
    data.punctualShadowTexel[1] = texel;
    data.punctualShadowPcf      = std::clamp(rs.shadow.punctualPcfRadius, 0, 3);

    // 未使用スロットは 0 のまま残す。HLSL 側は punctualShadowCount までしか見ない。
    const int count = std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    data.punctualShadowCount = count;
    for (int i = 0; i < count; ++i) {
        const PunctualShadowView& view = ctx.punctualShadowViews[i];
        data.punctualShadowVP[i]     = view.viewProjection;
        data.punctualShadowRect[i]   = view.atlasRect;
        data.punctualShadowParams[i] =
            { view.biasNDC, view.shadowStrength, view.penumbraTexels, 0.0f };
    }

    data.lightCookieTexel[0] = 1.0f / static_cast<float>(kLightCookieAtlasWidth);
    data.lightCookieTexel[1] = 1.0f / static_cast<float>(kLightCookieAtlasHeight);
    const int cookieCount = std::clamp(ctx.lightCookieViewCount, 0, kMaxLightCookies);
    data.lightCookieCount = cookieCount;
    for (int i = 0; i < cookieCount; ++i) {
        data.lightCookieVP[i]   = ctx.lightCookieViews[i].viewProjection;
        data.lightCookieRect[i] = ctx.lightCookieViews[i].atlasRect;
    }

    // レガシー経路の「大きさを持つ光源」。b3 に型が無いので実体ごと載せる。
    // レイアウトは PunctualShadowConstants.hlsli のコメントと FBZZ_PunctualAt が正本。
    const int shapedCount = std::clamp(ctx.legacyShapedLightCount, 0, kMaxLegacyShapedLights);
    data.legacyShapedLightCount = shapedCount;
    for (int i = 0; i < shapedCount; ++i) {
        const PunctualLightGPU& a = ctx.legacyShapedLights[i];
        math::Vector4* dst = &data.legacyShapedLight[i * kLegacyShapedLightStride];
        dst[0] = { a.position,  a.range };
        dst[1] = { a.color,     a.intensity };
        dst[2] = { a.direction, 0.0f };
        dst[3] = { a.tangent,   a.halfWidth };
        dst[4] = { a.bitangent, a.halfHeight };
        // y は両面フラグ。PunctualLightGPU では outerCos の枠に載せてある。
        dst[5] = { static_cast<float>(a.type), a.outerCos, 0.0f, 0.0f };
    }

    // レガシー経路のスロット番号と光源半径。「無し」は -1 (ctx 側の既定値がそう)。
    // WHY 0 埋めで済ませられないか: 0 は「スロット 0」という有効な番号なので、
    //     影を持たないライトが他のライトのシャドウマップを引いてしまう。
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i) {
        data.legacyPunctualSlots[i] = {
            static_cast<float>(ctx.legacyShadowSlots[i]),
            static_cast<float>(ctx.legacyCookieSlots[i]),
            ctx.legacySourceRadius[i],
            0.0f
        };
    }

    ctx.resources.Update(ctx.handles.punctualShadowCB, &data,
                         sizeof(PunctualShadowConstantsCB));
}

bool IsReliableOccluder(const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius <= 0.0f) return false;

    // 立方体で 0.577、球で 1.0。板・壁・棒はこれを大きく下回る。
    // 0.40 は「立方体は通し、厚み比 1:4 を超える扁平は落とす」あたりの線。
    constexpr float kMinFillRatio = 0.40f;
    const float minExtent = std::min({ mesh.boundsExtents.x,
                                       mesh.boundsExtents.y,
                                       mesh.boundsExtents.z });
    return minExtent / mesh.boundsRadius >= kMinFillRatio;
}

WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh, float padding)
{
    const math::Matrix4 world = tf.GetWorldMatrix();

    // ローカル空間バウンディング球中心をワールド空間に変換する。
    // 行列は列ベクトル規則 (M * v) なので:
    //   wx = m[0][0]*bx + m[0][1]*by + m[0][2]*bz + m[0][3]
    const float bx = mesh.boundsCenter.x;
    const float by = mesh.boundsCenter.y;
    const float bz = mesh.boundsCenter.z;
    const math::Vector3 worldCenter = {
        world.m[0][0]*bx + world.m[0][1]*by + world.m[0][2]*bz + world.m[0][3],
        world.m[1][0]*bx + world.m[1][1]*by + world.m[1][2]*bz + world.m[1][3],
        world.m[2][0]*bx + world.m[2][1]*by + world.m[2][2]*bz + world.m[2][3],
    };

    // ワールド行列の各軸ベクトルのノルムからスケールを取得し、最大値を掛ける。
    // WHY: 非一様スケールの場合は最大成分で保守的な球にする。
    //      球半径を過大評価しても偽カリング (見えているのに消える) は発生しない。
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

    // 骨から作った球があればそちらを使う。
    //
    // WHY バインドポーズ球では足りないか: 下のループはバインドポーズの submesh バウンズを
    //     Renderer の Transform で運んでいるだけで、骨がどこへ行ったかを見ていない。
    //     Script が骨を数十 m 動かす構成では、球がバインドポーズの場所に取り残されて
    //     «実体は見えているのに視錐台の外／遮蔽物の中» と判定され、部位ごとに明滅する。
    //
    // WHY 肉の厚みを足すか: Animator が持っているのは骨の広がりだけ。その周りの
    //     ジオメトリは submesh のバインド半径ぶん外へ出る。Renderer ごとに違う値なので、
    //     この Renderer が描く submesh の最大値をここで足す。
    // GetComponent に const 版が無いので剥がす。ここは読むだけ。
    for (GameObject* current = const_cast<GameObject*>(&go); current;
         current = current->GetParent()) {
        const auto* animator = current->GetComponent<AnimatorComponent>();
        if (!animator) continue;
        if (animator->skinnedBoundsRadius <= 0.0f) break;

        float flesh = 0.0f;
        for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot)
            if (const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot))
                flesh = (std::max)(flesh, meshPtr->boundsRadius);

        // 球は Animator の owner のローカル空間にある。運ぶのもその Transform。
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

    // WHAT: 各 submesh のワールド球を半径重みで平均し、最後に全 submesh を包む半径へ拡張する。
    // WHY: 毎フレーム CPU スキニングして厳密 bounds を取ると頂点数に比例して重い。
    //      バインドポーズ球は保守的だが、視錐台外の遠いキャラクターを安く除外できる。
    // この Renderer が描く submesh だけを包む。
    // WHY モデル全体で取らないか: ノードごとに子 GameObject へ分けた構成では、
    //     モデル全体の球はどの子にとっても過大になり、画面外の部位のぶんまで
    //     視錐台に残ってしまう。カリングの単位は「実際に描くもの」に揃える。
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
    // この Renderer が描く submesh だけを包む。
    // WHY モデル全体で取らないか: ノードごとに子 GameObject へ分けた構成では、
    //     モデル全体の球はどの子にとっても過大になり、画面外の部位のぶんまで
    //     視錐台に残ってしまう。カリングの単位は「実際に描くもの」に揃える。
    for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot) {
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot);
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        const WorldBounds bounds = ComputeWorldBounds(tf, *meshPtr);
        const math::Vector3 delta = bounds.center - outBounds.center;
        outBounds.radius = (std::max)(outBounds.radius, delta.Length() + bounds.radius);
    }

    // 余白は submesh ごとではなく合成後の球へ 1 回だけ足す。
    // WHY: ComputeWorldBounds へ渡して submesh 単位で足すと、合成時の
    //      「中心距離 + 半径」に padding が二重・三重で積み上がる。
    if (padding > 0.0f) outBounds.radius += padding;

    return true;
}

namespace {

// このレイヤーに適用する描画距離 [m] を返す。0 は「距離カリングしない」。
float ResolveCullDistance(const RenderPassContext& ctx, int layer)
{
    if (ctx.hasLayerCullDistances) {
        const float perLayer = ctx.cullLayerDistances[layer & (kCullLayerCount - 1)];
        if (perLayer > 0.0f) return perLayer;
    }
    return ctx.cullMaxDistance;
}

// 距離 → 極小 → 錐台 の順で判定し、落ちた場合は理由の統計を加算して false を返す。
// WHY この順序か: 前段ほど計算が安く、かつ後段より多くを落とす。
//      距離は減算と内積だけ、極小は除算 1 回、錐台は 6 平面。
//      逆順にすると、遠くて画面に 1 ピクセルも占めないオブジェクトにまで
//      毎フレーム 6 平面テストを通すことになる。
bool TestBoundsVisible(RenderPassContext& ctx, const GameObject& go, const WorldBounds& bounds)
{
    // 半径 0 = ComputeBounds 未実行。安全に判定できないので必ず描く。
    if (bounds.radius <= 0.0f) return true;

    const math::Vector3 toObject = bounds.center - ctx.camera.m_position;

    // 距離カリングと極小判定は同じ距離を使うので 1 回だけ求める。
    // 球距離モードでも深度距離モードでも「カメラからの前方距離」として扱う。
    const float distance = ctx.cullDistanceSpherical
        ? toObject.Length()
        : math::Vector3::Dot(toObject, ctx.cullCameraForward);

    // ── 距離カリング ──
    // 球の最近点で測る。中心で測ると、大きな地形メッシュが境界をまたいだ瞬間に丸ごと消える。
    const float cullDistance = ResolveCullDistance(ctx, go.layer);
    if (cullDistance > 0.0f && distance - bounds.radius > cullDistance) {
        ++ctx.statsDistanceCulled;
        return false;
    }

    // ── 極小オブジェクトカリング ──
    // 画面高さ比 = radius * (1/tan(fovY/2)) / 距離。
    // 単位は LODLevel::screenRelativeHeight と同じで、LODSystem の projectedHeight と一致する。
    if (ctx.smallObjectScreenHeight > 0.0f && ctx.cullProjScaleY > 0.0f && distance > 0.0f) {
        // 平行投影は距離で縮まない。割ってしまうと、引きの大きい正投影カメラでは
        // ほぼ全オブジェクトが「極小」と判定されて消える。
        const float screenHeight = ctx.cullOrthographic
            ? bounds.radius * ctx.cullProjScaleY
            : bounds.radius * ctx.cullProjScaleY / distance;
        if (screenHeight < ctx.smallObjectScreenHeight) {
            ++ctx.statsSmallObjectCulled;
            return false;
        }
    }

    // ── フラスタムカリング ──
    // 錐台未設定 (プローブキャプチャ等の派生コンテキスト) は「カリングしない」に倒す。
    if (ctx.frustumCullingEnabled && ctx.cameraFrustum &&
        !ctx.cameraFrustum->IntersectsSphere(bounds.center, bounds.radius)) {
        ++ctx.statsFrustumCulled;
        return false;
    }

    return true;
}

} // namespace

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
    // bounds を作れない (CPU 頂点未生成など) 場合は安全側に倒して描く。
    if (!ComputeSkinnedWorldBounds(go, smr, bounds, ctx.cullingBoundsPadding))
        return true;
    return TestBoundsVisible(ctx, go, bounds);
}

bool IsWithinCullDistance(const RenderPassContext& ctx,
                          const GameObject& go,
                          const WorldBounds& bounds)
{
    const float cullDistance = ResolveCullDistance(ctx, go.layer);
    if (cullDistance <= 0.0f) return true;

    const math::Vector3 toObject = bounds.center - ctx.camera.m_position;
    const float distance = ctx.cullDistanceSpherical
        ? toObject.Length()
        : math::Vector3::Dot(toObject, ctx.cullCameraForward);
    return distance - bounds.radius <= cullDistance;
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
        break; // シーンに 1 つ想定。環境風と同じ扱い
    }
    return result;
}

math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point)
{
    // 帯の面をカメラへ向けるには、幅方向を「進行方向 × 視線方向」に取る。
    math::Vector3 up = cameraPos - point;
    if (up.LengthSq() > math::EPSILON * math::EPSILON) up = up.Normalized();
    else up = math::Vector3::UP;
    // 進行方向と視線がほぼ平行だと外積が退化して帯が消える。安定な軸へ逃がす。
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::UP;
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::RIGHT;

    const math::Vector3 normal = math::Vector3::Cross(direction, up);
    return normal.LengthSq() > math::EPSILON * math::EPSILON
        ? normal.Normalized() : math::Vector3::RIGHT;
}

bool IsEffectTextureSrgb(const std::string& texturePath)
{
    // 1x1 白フォールバックなど参照が無い場合。1.0 はどちらの空間でも 1.0 なので変換しない。
    if (texturePath.empty()) return false;

    // 毎フレーム呼ばれる経路 (Trail) があるため結果を持つ。.meta の更新時刻で無効化して、
    // エディターで再インポートしたときに古い判定が残らないようにする。
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
    // サイドカーが無い素材はインポーターと同じ推定に従う (色テクスチャ = sRGB)。
    const auto slash = sourcePath.find_last_of("/\\");
    const std::string filename =
        slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);
    cached.srgb = asset::DefaultSettingsForType(asset::GuessTextureType(filename)).srgb;
    return cached.srgb;
}

} // namespace fbzz::scene
