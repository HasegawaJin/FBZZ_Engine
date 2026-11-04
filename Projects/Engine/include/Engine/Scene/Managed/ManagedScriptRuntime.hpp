/// @file    ManagedScriptRuntime.hpp
/// @brief   CoreCLR の常駐 Host とプロジェクト Script 世代の所有。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class Scene;
class Script;
class ManagedScriptAdapter;
struct CollisionInfo;
struct AnimationEventInfo;
struct RootMotionInfo;
struct SequenceEventInfo;
struct EntityID;
struct ScriptDebugDrawCommand;

/// @note Editor のメインスレッドだけが使用する。Unload は Scene 破棄前。destructor は managed コードを呼ばず CoreCLR はプロセス常駐。
class ManagedScriptRuntime {
public:
    FBZZ_ENGINE_API static ManagedScriptRuntime& Instance();
    FBZZ_ENGINE_API ~ManagedScriptRuntime();
    FBZZ_ENGINE_API bool Initialize(const std::filesystem::path& hostDirectory);
    FBZZ_ENGINE_API bool Load(const std::filesystem::path& gameAssemblyPath, bool internalMode = false);
    /// @note Edit 中だけ呼ぶ。全候補の復元後に切り替え、別 Scene/Undo/preview が保持する旧世代は最後の Adapter 解放時に退役する。
    FBZZ_ENGINE_API bool Reload(Scene& scene, const std::filesystem::path& gameAssemblyPath, bool internalMode = false);
    /// @note 指定 Scene の個体と catalog を解除する。別 Scene/Undo/preview が保持する個体は解放まで旧世代を所有する。
    FBZZ_ENGINE_API void Unload(Scene* scene = nullptr);
    /// @note 全 Scene の Unload 後、main が戻る前にだけ呼ぶ。Host の参照を解放し CoreCLR/hostfxr は停止・解放しない。
    /// @return 生存 Adapter、project 世代、実行中 callback があれば false。
    FBZZ_ENGINE_API bool Shutdown();
    [[nodiscard]] FBZZ_ENGINE_API bool IsLoaded() const;
    /// @note 最後の候補準備が GC 完了待ちのときだけ true。現在の世代と Adapter は未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool IsCollectionPending() const;
    [[nodiscard]] FBZZ_ENGINE_API const std::string& LastDiagnostic() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsDispatchIdle() const;
    [[nodiscard]] FBZZ_ENGINE_API uint64_t GetAppliedGeneration() const;
    /// @return 現世代の owned lease を取得し世代 ID を返す。失敗は 0。呼び出しごとに release が必要。
    [[nodiscard]] FBZZ_ENGINE_API uint64_t RetainAppliedGeneration();
    /// @note 保持済みの factory/catalog 選択だけを戻す。既存 Script/CLR 個体を生成・復元・置換しない。
    FBZZ_ENGINE_API bool ActivateRetainedGeneration(uint64_t generation);
    /// @note 保持した assembly を新 ALC 世代へ読み、Play clone の factory だけを選択する。失敗で元の個体・選択・lease は未変更。
    FBZZ_ENGINE_API bool ForkRetainedGenerationForPlay(uint64_t originalGeneration);
    /// @note active または生存 Adapter が所有する世代を解放時に退役しない。
    FBZZ_ENGINE_API void ReleaseRetainedGeneration(uint64_t generation);
    [[nodiscard]] FBZZ_ENGINE_API std::string GetTypeSourceName(std::string_view typeKey) const;
    [[nodiscard]] FBZZ_ENGINE_API std::string GetTypeSourcesJson(std::string_view typeKey) const;
    /// @note Passive schema defaults only; no managed Adapter or user constructor is created for catalog queries.
    [[nodiscard]] FBZZ_ENGINE_API std::string GetTypeSchemaJson(std::string_view typeKey) const;
    [[nodiscard]] FBZZ_ENGINE_API ManagedScriptAdapter* FindAdapter(const Script* script) const;
    FBZZ_ENGINE_API bool DiscoverSourceCatalog(const std::filesystem::path& assemblyPath, std::string& sourceJson);
    FBZZ_ENGINE_API bool GetApiCatalog(std::string& json);

    FBZZ_ENGINE_API bool Dispatch(ManagedBridgeRequest request, ManagedBridgeReply& reply, std::string* output = nullptr);
    /// @note world 操作を許可せず、設定復元中の GUID 解決だけに候補の Scene を提供する。
    FBZZ_ENGINE_API bool DispatchPrepared(ManagedScriptAdapter& adapter, ManagedBridgeRequest request,
                          ManagedBridgeReply& reply, std::string* output = nullptr);
    /// @note Batch の exact targets だけに参照解決を許可する。候補 callback へ Engine scope を渡さない。
    FBZZ_ENGINE_API bool DispatchPreparedSettings(const std::vector<ManagedScriptAdapter*>& adapters, ManagedBridgeRequest request,
                                  ManagedBridgeReply& reply);
    FBZZ_ENGINE_API bool BindOwner(ManagedScriptAdapter& adapter);
    FBZZ_ENGINE_API bool ResolveReference(ManagedScriptAdapter& adapter, ScriptObjectReference& reference);
    FBZZ_ENGINE_API bool ResolveObjectId(ManagedScriptAdapter& adapter, uint64_t objectId, ScriptObjectReference& reference);
    /// @return Scene 内の生存 GameObject の非所有 runtime token。見つからない場合は 0、ユーザー callback は実行しない。
    FBZZ_ENGINE_API uint64_t ReferenceObjectId(Scene& scene, EntityID entity);
    /// @return 非所有 Component の生存 epoch。欠損・遅延削除は 0。Transform は owner の runtime token。
    FBZZ_ENGINE_API uint64_t ReferenceComponentLifetime(Scene& scene, EntityID entity, uint32_t kind);
    /// @note 未公開の個体を復元する。保存値全件の構造を先に検査し、欠損 GUID は設定として保持する。
    FBZZ_ENGINE_API bool RestoreSettingsToml(ManagedScriptAdapter& adapter, std::string_view fieldsToml);
    /// @note Scene の明示移動後だけ保存参照を新しい寿命へ接続する。保持済み/private wrapper は変更しない。
    FBZZ_ENGINE_API bool RebindSceneReferences(Scene& scene);
    FBZZ_ENGINE_API bool Invoke(ManagedScriptAdapter& adapter, ManagedCallback callback);
    /// @note idle 状態で必須設定を再検証し、次の通常 Tick から明示復帰する。lifecycle callback は呼ばない。
    FBZZ_ENGINE_API bool Resume(ManagedScriptAdapter& adapter, bool enabled = true);
    FBZZ_ENGINE_API bool InvokeCollision(ManagedScriptAdapter& adapter, ManagedCallback callback, const CollisionInfo& info);
    /// @note Engine の同期 Play 通知を所有値へ変換し、既存 callback の世代・scope・障害処理へ配送する。
    FBZZ_ENGINE_API bool InvokeAnimationEvent(ManagedScriptAdapter& adapter, const AnimationEventInfo& info);
    FBZZ_ENGINE_API bool InvokeAnimatorMove(ManagedScriptAdapter& adapter, const RootMotionInfo& info);
    /// @note subscription=0 は対象 callback、非零は現在所有する global Sequence 購読。両方とも一回だけ配送する。
    FBZZ_ENGINE_API bool InvokeSequenceEvent(ManagedScriptAdapter& adapter, const SequenceEventInfo& info, uint64_t subscription = 0);
    FBZZ_ENGINE_API bool InvokeSequenceFinished(ManagedScriptAdapter& adapter, std::string_view sequenceName);
    /// @note 独立した設定候補から world 描画 command を一回取得する。失敗で live Script を停止・変更しない。
    FBZZ_ENGINE_API bool CaptureGizmos(ManagedScriptAdapter& adapter, bool selected, std::vector<ScriptDebugDrawCommand>& commands, uint32_t capacity = 1024);
    FBZZ_ENGINE_API bool InvokeOwned(ManagedScriptAdapter& adapter, ManagedCallback callback, uint64_t token,
                     std::string_view payload, float progress, bool completed, bool nativeSequence = false);
    /// @note native scheduler と Host delegate の両方を解除する。user callback を呼ばない。
    FBZZ_ENGINE_API void CancelOwned(ManagedScriptAdapter& adapter);
    FBZZ_ENGINE_API void Release(ManagedScriptAdapter& adapter);
    /// @note pool 個体を既存 Cleanup へ要求する。Play 世代交換と owner 解放で旧貸出を終了する。
    FBZZ_ENGINE_API void ReleaseOwnedObjects(ManagedScriptAdapter& adapter);

private:
    friend class Scene;
    /// @note Scene 寿命の通知は CLR や未初期化 singleton を呼び出さない。
    FBZZ_ENGINE_API static void RetireScene(Scene& scene);
    FBZZ_ENGINE_API static void RetireObject(Scene& scene, EntityID entity);
    static ManagedScriptRuntime* m_liveRuntime;
    FBZZ_ENGINE_API ManagedScriptRuntime();
    ManagedScriptRuntime(const ManagedScriptRuntime&) = delete;
    ManagedScriptRuntime& operator=(const ManagedScriptRuntime&) = delete;
    FBZZ_ENGINE_API std::unique_ptr<Script> CreateAdapter(const std::string& typeName, const std::string& savedScriptId);
    FBZZ_ENGINE_API bool LoadCandidate(const std::filesystem::path& path, uint64_t& generation, std::vector<std::string>& types, bool internalMode);
    FBZZ_ENGINE_API bool Publish(uint64_t generation, const std::vector<std::string>& types);
    FBZZ_ENGINE_API void Retire(uint64_t generation);
    FBZZ_ENGINE_API bool InvokePayload(ManagedScriptAdapter& adapter, ManagedCallback callback, std::string_view payload);
    FBZZ_ENGINE_API static int32_t __cdecl NativeDispatch(const ManagedBridgeRequest* request, ManagedBridgeReply* reply);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} /// @note namespace fbzz::scene
