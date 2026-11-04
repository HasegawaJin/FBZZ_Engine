/// @file    ScriptAssetProxy.cpp
/// @brief   Script が所有するアセット要求と先読みの転送。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include <Engine/Scene/ScriptProxy/ScriptAssetProxy.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/SceneAssetReferences.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
/// @name ScriptAssetProxy

namespace {

struct ScriptOwnedRequest {
    std::uint64_t         leaseId = 0;
    std::string           typeName;
    asset::RawAssetHandle handle;
};

/// @brief プロキシごとの所有要求。キーはプロキシのアドレス (Script を複製しても他人の要求を手放さない)。
/// @note スクリプト DLL の Script には STL の状態を置かない。要求の台帳は Engine 側に閉じる。
std::unordered_map<const ScriptAssetProxy*, std::vector<ScriptOwnedRequest>>& ScriptOwnedRequests()
{
    static std::unordered_map<const ScriptAssetProxy*, std::vector<ScriptOwnedRequest>> owned;
    return owned;
}

const ScriptOwnedRequest* FindOwnedRequest(const ScriptAssetProxy* proxy, std::uint64_t request)
{
    const auto& owned = ScriptOwnedRequests();
    const auto it = owned.find(proxy);
    if (it == owned.end()) return nullptr;
    for (const ScriptOwnedRequest& entry : it->second)
        if (entry.leaseId == request) return &entry;
    return nullptr;
}

asset::AssetLoadState OwnedRequestState(const ScriptAssetProxy* proxy, std::uint64_t request)
{
    if (!FindOwnedRequest(proxy, request)) return asset::AssetLoadState::None;
    return asset::AssetStreamer::Engine().GetLeaseStatus(request).state;
}

} ///< @note namespace

ScriptAssetProxy::ScriptAssetProxy(Script* owner) : script(owner)
{
    /// @note 全 Script の構築中に依存を確定し、static 所有者の破棄が要求台帳や streamer の破棄後へ回ることを防ぐ。
    /// @see https://eel.is/c++draft/basic.start.term static オブジェクトは初期化完了の逆順で破棄される。
    (void)asset::AssetStreamer::Engine();
    (void)ScriptOwnedRequests();
}

ScriptAssetProxy::~ScriptAssetProxy()
{
    /// @note スクリプトの破棄 (GameObject の破棄・Play 停止でのシーン破棄) で自分の要求をまとめて手放す。
    auto& owned = ScriptOwnedRequests();
    const auto it = owned.find(this);
    if (it == owned.end()) return;
    for (const ScriptOwnedRequest& entry : it->second)
        asset::AssetStreamer::Engine().ReleaseLease(entry.leaseId);
    owned.erase(it);
}

std::uint64_t ScriptAssetProxy::Request(std::string_view path, bool prefetch) const
{
    const auto reference = ClassifySceneAssetReference(path, [](std::string_view guidRef) {
        return asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(guidRef));
    });
    if (!reference) {
        FBZZ_LOG_WARN("assets.Request: 種類の分からない参照です (テクスチャ・.fbx・.fzasset・.mat のみ) [%.*s]",
                      static_cast<int>(path.size()), path.data());
        return kInvalid;
    }
    asset::AssetRequestOptions options;
    options.priority = prefetch ? asset::AssetPriority::Prefetch : asset::AssetPriority::Visible;
    const std::string_view typeName = StreamTypeNameOf(reference->kind);
    const auto request = asset::AssetStreamer::Engine().RequestRaw(typeName, reference->reference, options);
    if (request.leaseId == 0) return kInvalid;
    ScriptOwnedRequests()[this].push_back({ request.leaseId, std::string(typeName), request.handle });
    return request.leaseId;
}

bool ScriptAssetProxy::IsReady(std::uint64_t request) const
{
    return OwnedRequestState(this, request) == asset::AssetLoadState::Ready;
}

bool ScriptAssetProxy::IsFailed(std::uint64_t request) const
{
    return OwnedRequestState(this, request) == asset::AssetLoadState::Failed;
}

void ScriptAssetProxy::SetQuality(std::uint64_t request, int quality) const
{
    if (!FindOwnedRequest(this, request)) return;
    const int clamped = std::clamp(quality, 0, 255);
    asset::AssetStreamer::Engine().SetLeaseQuality(request, static_cast<asset::AssetQuality>(clamped));
}

bool ScriptAssetProxy::Retry(std::uint64_t request) const
{
    const ScriptOwnedRequest* entry = FindOwnedRequest(this, request);
    return entry && asset::AssetStreamer::Engine().RetryRaw(entry->typeName, entry->handle);
}

void ScriptAssetProxy::Release(std::uint64_t request) const
{
    auto& owned = ScriptOwnedRequests();
    const auto it = owned.find(this);
    if (it == owned.end()) return;
    auto& list = it->second;
    const auto found = std::find_if(list.begin(), list.end(),
                                    [request](const ScriptOwnedRequest& entry) { return entry.leaseId == request; });
    if (found == list.end()) return;
    asset::AssetStreamer::Engine().ReleaseLease(request);
    list.erase(found);
}

bool ScriptAssetProxy::PrefetchScene(std::string_view sceneName) const
{
    auto* manager = ScriptRuntime::GetCurrent().sceneManager;
    return manager && manager->PrefetchScene(std::string(sceneName));
}

float ScriptAssetProxy::GetScenePrefetchProgress(std::string_view sceneName) const
{
    auto* manager = ScriptRuntime::GetCurrent().sceneManager;
    if (!manager) return 0.0f;
    const auto progress = manager->GetScenePrefetchProgress(std::string(sceneName));
    if (progress.total == 0) return 0.0f;
    return static_cast<float>(progress.ready + progress.failed) / static_cast<float>(progress.total);
}

} /// @note namespace fbzz::scene
