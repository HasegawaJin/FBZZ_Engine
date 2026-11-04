/// @file    ManagedScriptAdapter.hpp
/// @brief   既存 Script の lifecycle と反射を managed 個体へ転送する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

class ManagedScriptRuntime;

struct ManagedFieldDescriptor {
    std::string name;
    ManagedValueKind kind = ManagedValueKind::NONE;
    uint32_t flags = 0;
    std::string label;
    std::string schemaJson;
    std::vector<std::string> previousKeys;
    float minimum = 0;
    float maximum = 0;
};

struct ManagedFieldValue {
    std::string name;
    ManagedBridgeReply value;
    std::string text;
};

class ManagedScriptAdapter final : public Script {
public:
    FBZZ_ENGINE_API ManagedScriptAdapter(ManagedScriptRuntime& runtime, std::string typeName, uint64_t generation,
                         uint64_t instanceId, uint64_t ownerId, bool executeAlways, std::string savedScriptId);
    FBZZ_ENGINE_API ~ManagedScriptAdapter() override;
    const char* GetTypeName() const override { return m_typeName.c_str(); }
    bool ExecuteInEditMode() const override { return m_executeAlways; }
    int ExecutionOrder() const override { return m_executionOrder; }
    std::span<const std::string> RequiredComponents() const override { return m_requiredComponents; }
    FBZZ_ENGINE_API void OnAwake() override;
    FBZZ_ENGINE_API void OnStart() override;
    FBZZ_ENGINE_API void OnEnable() override;
    FBZZ_ENGINE_API void OnDisable() override;
    FBZZ_ENGINE_API void OnUpdate() override;
    FBZZ_ENGINE_API void OnFixedUpdate() override;
    FBZZ_ENGINE_API void OnLateUpdate() override;
    FBZZ_ENGINE_API void OnDestroy() override;
    FBZZ_ENGINE_API void Reset() override;
    FBZZ_ENGINE_API void OnValidate() override;
    FBZZ_ENGINE_API void OnBeforeSerialize() override;
    FBZZ_ENGINE_API void OnAfterDeserialize() override;
    FBZZ_ENGINE_API void OnDrawGizmos() override;
    FBZZ_ENGINE_API void OnDrawGizmosSelected() override;
    FBZZ_ENGINE_API void OnCollisionEnter(const CollisionInfo& collision) override;
    FBZZ_ENGINE_API void OnCollisionStay(const CollisionInfo& collision) override;
    FBZZ_ENGINE_API void OnCollisionExit(const CollisionInfo& collision) override;
    FBZZ_ENGINE_API void OnTriggerEnter(const CollisionInfo& collision) override;
    FBZZ_ENGINE_API void OnTriggerStay(const CollisionInfo& collision) override;
    FBZZ_ENGINE_API void OnTriggerExit(const CollisionInfo& collision) override;
    FBZZ_ENGINE_API void OnAnimationEvent(const AnimationEventInfo& info) override;
    FBZZ_ENGINE_API void OnAnimatorMove(const RootMotionInfo& info) override;
    FBZZ_ENGINE_API void OnSequenceEvent(const SequenceEventInfo& info) override;
    FBZZ_ENGINE_API void OnSequenceFinished(const char* sequenceName) override;
    FBZZ_ENGINE_API void OnNavMeshDestinationReached() override;
    FBZZ_ENGINE_API void OnNavMeshPathFailed() override;
    FBZZ_ENGINE_API void OnNavMeshTargetSpotted() override;
    FBZZ_ENGINE_API void OnNavMeshTargetLost() override;
    FBZZ_ENGINE_API void Reflect(IReflector& reflector) override;
    /// @note FieldInfo の値だけを投影し、Inspector の変更を managed 個体へ書き戻さない。
    FBZZ_ENGINE_API void ReflectReadOnly(IReflector& reflector);
    /// @note 保存 callback・設定の検証・取得に失敗した場合は false と診断を返す。既に停止した個体の callback は再実行しない。
    FBZZ_ENGINE_API bool ReflectForSerialization(IReflector& reflector, std::string& diagnostic, bool invokeCallbacks = true);

    [[nodiscard]] uint64_t Generation() const { return m_generation; }
    [[nodiscard]] uint64_t InstanceId() const { return m_instanceId; }
    [[nodiscard]] uint64_t OwnerId() const { return m_ownerId; }
    [[nodiscard]] const std::string& SavedScriptId() const { return m_savedScriptId; }
    [[nodiscard]] const std::string& ReferenceBirth() const { return m_referenceBirth; }
    [[nodiscard]] const std::vector<ManagedFieldDescriptor>& Fields() const { return m_fields; }
    /// @note 個体を停止させた最初の失敗。別個体の dispatch や観測では上書きしない。
    [[nodiscard]] const std::string& FaultDiagnostic() const { return m_faultDiagnostic; }
    [[nodiscard]] const std::string& ResumeDiagnostic() const { return m_resumeDiagnostic; }
    [[nodiscard]] FBZZ_ENGINE_API bool HasLiveContext() const;
    FBZZ_ENGINE_API bool RefreshOwnerBinding();
    enum class SettingsValidationPurpose : uint32_t { RUNTIME = 0, SNAPSHOT = 1, AUTHORING = 2 };
    /// @note Authoring は未完成の必須参照を許容する。Runtime は開始・候補公開前の必須参照生存を要求する。
    FBZZ_ENGINE_API bool ValidateSettingsJson(std::string_view json, SettingsValidationPurpose purpose = SettingsValidationPurpose::RUNTIME,
        const std::vector<std::string>& rebindReferences = {});
    FBZZ_ENGINE_API bool ApplySettingsJson(std::string_view json, SettingsValidationPurpose purpose = SettingsValidationPurpose::RUNTIME,
        const std::vector<std::string>& rebindReferences = {});
    /// @note Editor undo の内部復元。値型の検証は保ち、未接続の必須設定も元の状態へ戻す。
    FBZZ_ENGINE_API bool RestoreSettingsJson(std::string_view json);
    FBZZ_ENGINE_API bool ValidateReferences();
    /// @note Scene の全 entry 構築後、ディスクから復元した未解決 ComponentRef だけを明示的に接続する。
    FBZZ_ENGINE_API bool ResolveSavedComponentReferences();
    FBZZ_ENGINE_API bool InitializeFields();
    FBZZ_ENGINE_API bool CaptureFields(std::vector<ManagedFieldValue>& fields, bool includeReloadState = false);
    /// @note 設定・観測・path 別の exact 参照 identity を callback や保存 codec 投影なしで取得する。
    FBZZ_ENGINE_API bool ReadSettingsSnapshot(std::string& json);
    /// @note Visible page reads never hash or copy the full parent collection.
    FBZZ_ENGINE_API bool ReadSettingsPage(std::string_view path, uint64_t start, uint32_t count, std::string& json);
    FBZZ_ENGINE_API bool OpenSettingsEdit(uint64_t& sessionId, std::string& revision);
    FBZZ_ENGINE_API void CloseSettingsEdit(uint64_t sessionId, bool retainPrepared = false);
    /// @note Host 経由の設定変更のみを数える。ユーザー callback 内の直接 field 変更は検出しない。
    FBZZ_ENGINE_API bool ReadSettingsRevision(uint64_t& revision);
    FBZZ_ENGINE_API void CommitInstance(uint64_t generation, uint64_t instanceId, std::vector<ManagedFieldDescriptor> fields,
                        bool preserveLifecycle = false);

private:
    FBZZ_ENGINE_API bool ReflectFields(IReflector& reflector, bool write, std::string* diagnostic = nullptr);
    FBZZ_ENGINE_API void Invoke(ManagedCallback callback);
    FBZZ_ENGINE_API void InvokeCollision(ManagedCallback callback, const CollisionInfo& collision);
    FBZZ_ENGINE_API void RecordFault();
    ManagedScriptRuntime& m_runtime;
    std::string m_typeName;
    std::string m_savedScriptId;
    /// @note Reload は元 adapter の寿命を保ち、実際の add/redo は新しい寿命を持つ。
    std::string m_referenceBirth;
    uint64_t m_generation;
    uint64_t m_instanceId;
    uint64_t m_ownerId;
    bool m_executeAlways;
    int m_executionOrder = 0;
    std::vector<std::string> m_requiredComponents;
    bool m_managedAwoken = false;
    bool m_managedStarted = false;
    bool m_connectionsActive = false;
    std::vector<ManagedFieldDescriptor> m_fields;
    std::string m_faultDiagnostic;
    std::string m_resumeDiagnostic;
    Scene* m_boundScene = nullptr;
    GameObject* m_boundObject = nullptr;
    uint64_t m_contextSceneGeneration = 0;
    EntityID m_contextEntity;
    uint64_t m_objectId = 0;
    friend class ManagedScriptRuntime;
};

} /// @note namespace fbzz::scene
