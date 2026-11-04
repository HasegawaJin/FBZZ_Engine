/// @file    ScriptRenderingProxies.cpp
/// @brief   Rendering 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Core/Logger.hpp>
namespace fbzz::scene {
namespace {


ProceduralMeshComponent* EnsureProceduralMesh(GameObject& go)
{
    if (auto* existing = go.GetComponent<ProceduralMeshComponent>()) return existing;
    return &go.AddComponent<ProceduralMeshComponent>();
}


void ApplyProceduralMesh(GameObject& go, const MeshBuilder& builder)
{
    ProceduralMeshComponent* procedural = EnsureProceduralMesh(go);
    if (!procedural) return;
    /// @note 三角形の数が変われば当然、同数でも繋がり方は変わりうる。値で受けた以上、
    /// @note 何が変わったかは判別できないので常に全更新として扱う。
    procedural->builder = builder;
    procedural->dirty   = MeshDirty::All;
    procedural->enabled = true;
}




renderer::RenderSettings* ActiveRenderSettings()
{
    return core::Application::Get().GetActiveRenderSettings();
}

/// @brief getter 用。未登録でも既定値を返せるよう、読み取りは静的な既定インスタンスへ落とす。

const renderer::RenderSettings& RenderSettingsForRead()
{
    static const renderer::RenderSettings s_defaults{};
    const renderer::RenderSettings* active = ActiveRenderSettings();
    return active ? *active : s_defaults;
}




[[nodiscard]] bool SameEntity(const renderer::RenderSelectionID& lhs, const EntityID& rhs)
{
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}


void SubmitObjectMaskRequest(GameObject* target, const math::Vector4& color,
                             float value, bool includeChildren, bool visibleOnly)
{
    if (!target || !target->IsValid()) return;
    auto* settings = ActiveRenderSettings();
    if (!settings) {
        /// @note 申告先が無い。Play 中は EditorApp が、配布ビルドは StandaloneProjectModule が
        /// @note 差す。ここが null だと «Set は呼べているのに何も起きない» になり、
        /// @note 呼んだ側からは成功と区別が付かない。
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            FBZZ_LOG_WARN("ObjectMask: 申告先の RenderSettings がありません "
                          "(Application::SetActiveRenderSettings が呼ばれていない)。"
                          "この間のマスク申告はすべて捨てられます");
        }
        return;
    }

    const EntityID id = target->GetID();

    /// @note 同じ対象の行があれば上書きし、無ければ «前のフレームで途絶えた行» を再利用する。
    /// @note 拾わずに追加し続けると、一度でも印された対象の数だけ配列が伸びたままになる。
    renderer::RenderObjectMaskRequest* slot = nullptr;
    for (renderer::RenderObjectMaskRequest& request : settings->objectMaskRequests) {
        if (SameEntity(request.id, id)) {
            slot = &request;
            break;
        }
        if (!slot && !renderer::IsObjectMaskRequestLive(request, Time::frameCount)) slot = &request;
    }
    if (!slot) slot = &settings->objectMaskRequests.emplace_back();

    slot->id              = { id.index, id.generation };
    slot->color[0]        = color.x;
    slot->color[1]        = color.y;
    slot->color[2]        = color.z;
    slot->color[3]        = color.w;
    slot->value           = std::clamp(value, 0.0f, 1.0f);
    slot->includeChildren = includeChildren;
    slot->visibleOnly     = visibleOnly;
    slot->frame           = Time::frameCount;
}


void WithdrawObjectMaskRequest(GameObject* target)
{
    if (!target) return;
    auto* settings = ActiveRenderSettings();
    if (!settings) return;

    const EntityID id = target->GetID();
    std::erase_if(settings->objectMaskRequests,
                  [&id](const renderer::RenderObjectMaskRequest& request) {
                      return SameEntity(request.id, id);
                  });
}


} /// @note namespace
renderer::PostProcessSettings& ScriptPostProcessProxy::Get() const
{
    assert(script && "Script context is not set");
    return script->GetRuntimePostProcessSettings();
}


const renderer::PostProcessSettings* ScriptPostProcessProxy::TryGet() const
{
    return script ? script->TryGetRuntimePostProcessSettings() : nullptr;
}


void ScriptPostProcessProxy::Set(const renderer::PostProcessSettings& settings) const
{
    if (script) script->SetRuntimePostProcessSettings(settings);
}


void ScriptPostProcessProxy::Clear() const
{
    if (script) script->ClearRuntimePostProcessSettings();
}


renderer::CustomPostProcessSettings& ScriptPostProcessProxy::AddCustom(
    std::string_view name,
    std::string_view shaderPath,
    bool enabled) const
{
    auto& settings = Get();
    auto& custom = settings.customEffects.emplace_back();
    custom.name = std::string(name);
    custom.shaderPath = std::string(shaderPath);
    custom.enabled = enabled;
    return custom;
}


renderer::CustomPostProcessSettings& ScriptPostProcessProxy::EnsureCustom(
    std::string_view name,
    std::string_view shaderPath,
    bool enabled) const
{
    if (auto* custom = FindCustom(name)) {
        if (!shaderPath.empty())
            custom->shaderPath = std::string(shaderPath);
        custom->enabled = enabled;
        return *custom;
    }

    return AddCustom(name, shaderPath, enabled);
}


renderer::CustomPostProcessSettings* ScriptPostProcessProxy::FindCustom(std::string_view name) const
{
    if (!script) return nullptr;

    auto& effects = script->GetRuntimePostProcessSettings().customEffects;
    auto it = std::find_if(effects.begin(), effects.end(), [name](const renderer::CustomPostProcessSettings& custom) {
        return custom.name == name;
    });
    return it != effects.end() ? &(*it) : nullptr;
}


bool ScriptPostProcessProxy::RemoveCustom(std::string_view name) const
{
    if (!script) return false;

    auto& effects = script->GetRuntimePostProcessSettings().customEffects;
    const auto oldSize = effects.size();
    effects.erase(
        std::remove_if(effects.begin(), effects.end(), [name](const renderer::CustomPostProcessSettings& custom) {
            return custom.name == name;
        }),
        effects.end());
    return effects.size() != oldSize;
}


bool ScriptPostProcessProxy::SetCustomEnabled(std::string_view name, bool enabled) const
{
    auto* custom = FindCustom(name);
    if (!custom) return false;
    custom->enabled = enabled;
    return true;
}


bool ScriptPostProcessProxy::SetCustomParameter(std::string_view name, uint32_t index, float value) const
{
    if (index >= 8) return false;

    auto* custom = FindCustom(name);
    if (!custom) return false;
    custom->parameters[index] = value;
    return true;
}


bool ScriptPostProcessProxy::SetCustomParameters(std::string_view name, float x, float y, float z, float w) const
{
    auto* custom = FindCustom(name);
    if (!custom) return false;

    custom->parameters[0] = x;
    custom->parameters[1] = y;
    custom->parameters[2] = z;
    custom->parameters[3] = w;
    return true;
}


bool ScriptPostProcessProxy::LoadProfile(std::string_view profilePath) const
{
    /// @note DataAssetRegistry 経由: 同じプロファイルを複数箇所から読んでも TOML の再パースが起きず、
    /// @note エディタでの編集が即座に反映される。旧 .fzpp の直接パース経路より安く共有実体とも一致する。
    const asset::DataAsset* resolvedAsset = asset::DataAssetRegistry::Resolve(std::string(profilePath));
    if (!resolvedAsset) return false;
    const char* typeName = resolvedAsset->GetTypeName();
    if (!typeName || std::string_view(typeName) != asset::PostProcessProfile::TYPE_NAME) return false;
    const auto* profile = static_cast<const asset::PostProcessProfile*>(resolvedAsset);

    /// @note プロファイルは «効果のリスト» なので、まず既定値へ重み 1 で解決してから渡す。渡すのは
    /// @note ポストプロセス部分のみ: このプロキシが書くランタイム上書きの器は Scene の
    /// @note PostProcessSettings で SSR/TAA 等の高度グラフィクスの置き場が無い。恒久的に効かせたい
    /// @note 場合は PostProcessVolume からこのプロファイルを参照させる。
    renderer::VolumeSettings resolved;
    profile->ApplyTo(resolved, 1.0f);
    Set(resolved.post);
    return true;
}

/// @note OnDrawGizmos は DebugDraw の記録区間 (CaptureScriptGizmos) で呼ばれるので直接積める。

void ScriptMeshProxy::SetEnabled(bool enabled) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->enabled = enabled;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->enabled = enabled;
}


bool ScriptMeshProxy::IsEnabled() const
{
    if (!script || !script->m_gameObject) return false;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        if (mr->enabled) return true;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        if (smr->enabled) return true;
    return false;
}


void ScriptMeshProxy::SetCastShadows(bool castShadows) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->castShadows = castShadows;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->castShadows = castShadows;
}


bool ScriptMeshProxy::GetCastShadows() const
{
    if (!script || !script->m_gameObject) return false;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        if (mr->castShadows) return true;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        if (smr->castShadows) return true;
    return false;
}


void ScriptMeshProxy::SetMeshPath(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    auto* mr = script->m_gameObject->GetComponent<MeshRenderer>();
    if (!mr || mr->meshPath == path) return;
    mr->meshPath      = std::string(path);
    /// @note 引き直しには ResourceManager が要る。実際に差し替えるのは RuntimeMeshSystem。
    mr->meshPathDirty = true;
}


std::string ScriptMeshProxy::GetMeshPath() const
{
    if (!script || !script->m_gameObject) return {};
    const auto* mr = script->m_gameObject->GetComponent<MeshRenderer>();
    return mr ? mr->meshPath : std::string{};
}


void ScriptMeshProxy::SetModelPath(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>())
        smr->modelPath = std::string(path);
}


std::string ScriptMeshProxy::GetModelPath() const
{
    if (!script || !script->m_gameObject) return {};
    const auto* smr = script->m_gameObject->GetComponent<SkinnedMeshRenderer>();
    return smr ? smr->modelPath : std::string{};
}


void ScriptMeshProxy::Apply(const MeshBuilder& builder) const
{
    if (!script || !script->m_gameObject) return;
    ApplyProceduralMesh(*script->m_gameObject, builder);
}


void ScriptMeshProxy::Apply(GameObject& target, const MeshBuilder& builder) const
{
    ApplyProceduralMesh(target, builder);
}


std::span<MeshVertex> ScriptMeshProxy::Vertices() const
{
    if (!script || !script->m_gameObject) return {};
    auto* procedural = script->m_gameObject->GetComponent<ProceduralMeshComponent>();
    if (!procedural) return {};
    return { procedural->builder.Vertices().data(), procedural->builder.Vertices().size() };
}


void ScriptMeshProxy::Touch(MeshDirty dirty) const
{
    if (!script || !script->m_gameObject) return;
    auto* procedural = script->m_gameObject->GetComponent<ProceduralMeshComponent>();
    if (!procedural || dirty == MeshDirty::None) return;
    /// @note 既に全更新が積まれているフレームで «頂点だけ» を要求されても、弱い方へは下げない。
    if (procedural->dirty != MeshDirty::All) procedural->dirty = dirty;
}


void ScriptMeshProxy::Clear() const
{
    if (!script || !script->m_gameObject) return;
    auto* procedural = script->m_gameObject->GetComponent<ProceduralMeshComponent>();
    if (!procedural) return;
    procedural->builder.Clear();
    procedural->enabled = false;
    procedural->dirty   = MeshDirty::All;
    if (auto* mr = script->m_gameObject->GetComponent<MeshRenderer>())
        mr->enabled = false;
}


void ScriptMeshProxy::SetProceduralMaterial(std::string_view path) const
{
    if (!script || !script->m_gameObject) return;
    ProceduralMeshComponent* procedural = EnsureProceduralMesh(*script->m_gameObject);
    if (!procedural) return;
    procedural->materialPath = std::string(path);
}

/// @brief ScriptIKProxy

void ScriptGraphicsProxy::SetBrightness(float value) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->userBrightness = std::clamp(value, 0.1f, 4.0f);
}


float ScriptGraphicsProxy::GetBrightness() const
{
    return RenderSettingsForRead().userBrightness;
}


void ScriptGraphicsProxy::SetBloomScale(float value) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->userBloomScale = std::clamp(value, 0.0f, 2.0f);
}


float ScriptGraphicsProxy::GetBloomScale() const
{
    return RenderSettingsForRead().userBloomScale;
}


void ScriptGraphicsProxy::SetRenderScale(float scale) const
{
    if (auto* settings = ActiveRenderSettings()) {
        settings->renderScale =
            std::clamp(scale, renderer::kMinRenderScale, renderer::kMaxRenderScale);
    }
}


float ScriptGraphicsProxy::GetRenderScale() const
{
    return RenderSettingsForRead().renderScale;
}


uint32_t ScriptGraphicsProxy::GetRenderWidth() const
{
    uint32_t width = 0, height = 0;
    auto& app = core::Application::Get();
    renderer::ResolveRenderResolution(app.GetWindowWidth(), app.GetWindowHeight(),
                                      RenderSettingsForRead().renderScale, width, height);
    return width;
}


uint32_t ScriptGraphicsProxy::GetRenderHeight() const
{
    uint32_t width = 0, height = 0;
    auto& app = core::Application::Get();
    renderer::ResolveRenderResolution(app.GetWindowWidth(), app.GetWindowHeight(),
                                      RenderSettingsForRead().renderScale, width, height);
    return height;
}


void ScriptGraphicsProxy::SetQualityPreset(renderer::QualityPreset preset) const
{
    if (auto* settings = ActiveRenderSettings())
        renderer::ApplyQualityPreset(*settings, preset);
}


renderer::QualityPreset ScriptGraphicsProxy::GetQualityPreset() const
{
    return renderer::DetectQualityPreset(RenderSettingsForRead());
}


void ScriptGraphicsProxy::SetShadowsEnabled(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->shadowEnabled = enabled;
}


bool ScriptGraphicsProxy::GetShadowsEnabled() const
{
    return RenderSettingsForRead().shadowEnabled;
}


void ScriptGraphicsProxy::SetShadowResolution(uint32_t resolution) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->shadow.mapResolution = std::clamp(resolution, 512u, 8192u);
}


uint32_t ScriptGraphicsProxy::GetShadowResolution() const
{
    return RenderSettingsForRead().shadow.mapResolution;
}


void ScriptGraphicsProxy::SetShadowCascades(int count) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->shadow.cascadeCount = std::clamp(count, 1, 4);
}


int ScriptGraphicsProxy::GetShadowCascades() const
{
    return RenderSettingsForRead().shadow.cascadeCount;
}


void ScriptGraphicsProxy::SetSSR(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->ssr.enabled = enabled;
}


bool ScriptGraphicsProxy::GetSSR() const { return RenderSettingsForRead().ssr.enabled; }


void ScriptGraphicsProxy::SetGTAO(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->gtao.enabled = enabled;
    /// @note GTAO と SSAO は同じスロット。立てた側を残すため、先に相手を落としてから正規化する。
    if (enabled) settings->postProcess.ambientOcclusion.enabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}


bool ScriptGraphicsProxy::GetGTAO() const { return RenderSettingsForRead().gtao.enabled; }


void ScriptGraphicsProxy::SetSSAO(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->postProcess.ambientOcclusion.enabled = enabled;
    if (enabled) settings->gtao.enabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}


bool ScriptGraphicsProxy::GetSSAO() const
{
    return RenderSettingsForRead().postProcess.ambientOcclusion.enabled;
}


void ScriptGraphicsProxy::SetTAA(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->taa.enabled = enabled;
    if (enabled) settings->postProcess.fxaaEnabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}


bool ScriptGraphicsProxy::GetTAA() const { return RenderSettingsForRead().taa.enabled; }


void ScriptGraphicsProxy::SetFXAA(bool enabled) const
{
    auto* settings = ActiveRenderSettings();
    if (!settings) return;
    settings->postProcess.fxaaEnabled = enabled;
    if (enabled) settings->taa.enabled = false;
    (void)settings->NormalizeExclusivePipelineSlots();
}


bool ScriptGraphicsProxy::GetFXAA() const
{
    return RenderSettingsForRead().postProcess.fxaaEnabled;
}


void ScriptGraphicsProxy::SetBloom(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings())
        settings->postProcess.bloom.enabled = enabled;
}


bool ScriptGraphicsProxy::GetBloom() const
{
    return RenderSettingsForRead().postProcess.bloom.enabled;
}


void ScriptGraphicsProxy::SetMotionBlur(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->motionBlur.enabled = enabled;
}


bool ScriptGraphicsProxy::GetMotionBlur() const
{
    return RenderSettingsForRead().motionBlur.enabled;
}


void ScriptGraphicsProxy::SetVolumetricLight(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->volumetricLight.enabled = enabled;
}


bool ScriptGraphicsProxy::GetVolumetricLight() const
{
    return RenderSettingsForRead().volumetricLight.enabled;
}


void ScriptGraphicsProxy::SetContactShadow(bool enabled) const
{
    if (auto* settings = ActiveRenderSettings()) settings->contactShadow.enabled = enabled;
}


bool ScriptGraphicsProxy::GetContactShadow() const
{
    return RenderSettingsForRead().contactShadow.enabled;
}

/// @name ScriptEventProxy
/// @brief Subscribe / Publish はテンプレートなのでヘッダ側。ここは非テンプレート分だけ。


void ScriptObjectMaskProxy::Set(const math::Vector4& color, float value) const
{
    if (script) SubmitObjectMaskRequest(script->m_gameObject, color, value, true, true);
}


void ScriptObjectMaskProxy::Set(GameObject& target, const math::Vector4& color, float value,
                                bool includeChildren, bool visibleOnly) const
{
    SubmitObjectMaskRequest(&target, color, value, includeChildren, visibleOnly);
}


void ScriptObjectMaskProxy::Clear() const
{
    if (script) WithdrawObjectMaskRequest(script->m_gameObject);
}


void ScriptObjectMaskProxy::Clear(GameObject& target) const
{
    WithdrawObjectMaskRequest(&target);
}

/// @brief ScriptSpringBoneProxy

} /// @note namespace fbzz::scene
