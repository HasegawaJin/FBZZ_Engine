/// @file    ManagedScriptAdapter.hpp
/// @brief   既存 Script の lifecycle と反射を managed 個体へ転送する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

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
    ManagedScriptAdapter(ManagedScriptRuntime& runtime, std::string typeName, uint64_t generation,
                         uint64_t instanceId, uint64_t ownerId, bool executeAlways, std::string savedScriptId);
    ~ManagedScriptAdapter() override;
    const char* GetTypeName() const override { return m_typeName.c_str(); }
    bool ExecuteInEditMode() const override { return m_executeAlways; }
    int ExecutionOrder() const override { return m_executionOrder; }
    std::span<const std::string> RequiredComponents() const override { return m_requiredComponents; }
    void OnAwake() override;
    void OnStart() override;
    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    void OnLateUpdate() override;
    void OnDestroy() override;
    void Reset() override;
    void OnValidate() override;
    void OnBeforeSerialize() override;
    void OnAfterDeserialize() override;
    void OnDrawGizmos() override;
    void OnDrawGizmosSelected() override;
    void OnCollisionEnter(const CollisionInfo& collision) override;
    void OnCollisionStay(const CollisionInfo& collision) override;
    void OnCollisionExit(const CollisionInfo& collision) override;
    void OnTriggerEnter(const CollisionInfo& collision) override;
    void OnTriggerStay(const CollisionInfo& collision) override;
    void OnTriggerExit(const CollisionInfo& collision) override;
    void OnAnimationEvent(const AnimationEventInfo& info) override;
    void OnAnimatorMove(const RootMotionInfo& info) override;
    void OnSequenceEvent(const SequenceEventInfo& info) override;
    void OnSequenceFinished(const char* sequenceName) override;
    void OnNavMeshDestinationReached() override;
    void OnNavMeshPathFailed() override;
    void OnNavMeshTargetSpotted() override;
    void OnNavMeshTargetLost() override;
    void Reflect(IReflector& reflector) override;
    /// @note FieldInfo の値だけを投影し、Inspector の変更を managed 個体へ書き戻さない。
    void ReflectReadOnly(IReflector& reflector);
    /// @note 保存 callback・設定の検証・取得に失敗した場合は false と診断を返す。既に停止した個体の callback は再実行しない。
    bool ReflectForSerialization(IReflector& reflector, std::string& diagnostic, bool invokeCallbacks = true);

    [[nodiscard]] uint64_t Generation() const { return m_generation; }
    [[nodiscard]] uint64_t InstanceId() const { return m_instanceId; }
    [[nodiscard]] uint64_t OwnerId() const { return m_ownerId; }
    [[nodiscard]] const std::string& SavedScriptId() const { return m_savedScriptId; }
    [[nodiscard]] const std::string& ReferenceBirth() const { return m_referenceBirth; }
    [[nodiscard]] const std::vector<ManagedFieldDescriptor>& Fields() const { return m_fields; }
    /// @note 個体を停止させた最初の失敗。別個体の dispatch や観測では上書きしない。
    [[nodiscard]] const std::string& FaultDiagnostic() const { return m_faultDiagnostic; }
    [[nodiscard]] const std::string& ResumeDiagnostic() const { return m_resumeDiagnostic; }
    [[nodiscard]] bool HasLiveContext() const;
    bool RefreshOwnerBinding();
    enum class SettingsValidationPurpose : uint32_t { RUNTIME = 0, SNAPSHOT = 1, AUTHORING = 2 };
    /// @note Authoring は未完成の必須参照を許容する。Runtime は開始・候補公開前の必須参照生存を要求する。
    bool ValidateSettingsJson(std::string_view json, SettingsValidationPurpose purpose = SettingsValidationPurpose::RUNTIME,
        const std::vector<std::string>& rebindReferences = {});
    bool ApplySettingsJson(std::string_view json, SettingsValidationPurpose purpose = SettingsValidationPurpose::RUNTIME,
        const std::vector<std::string>& rebindReferences = {});
    /// @note Editor undo の内部復元。値型の検証は保ち、未接続の必須設定も元の状態へ戻す。
    bool RestoreSettingsJson(std::string_view json);
    bool ValidateReferences();
    /// @note Scene の全 entry 構築後、ディスクから復元した未解決 ComponentRef だけを明示的に接続する。
    bool ResolveSavedComponentReferences();
    bool InitializeFields();
    bool CaptureFields(std::vector<ManagedFieldValue>& fields, bool includeReloadState = false);
    /// @note 設定・観測・path 別の exact 参照 identity を callback や保存 codec 投影なしで取得する。
    bool ReadSettingsSnapshot(std::string& json);
    /// @note Visible page reads never hash or copy the full parent collection.
    bool ReadSettingsPage(std::string_view path, uint64_t start, uint32_t count, std::string& json);
    bool OpenSettingsEdit(uint64_t& sessionId, std::string& revision);
    void CloseSettingsEdit(uint64_t sessionId, bool retainPrepared = false);
    /// @note Host 経由の設定変更のみを数える。ユーザー callback 内の直接 field 変更は検出しない。
    bool ReadSettingsRevision(uint64_t& revision);
    void CommitInstance(uint64_t generation, uint64_t instanceId, std::vector<ManagedFieldDescriptor> fields,
                        bool preserveLifecycle = false);

private:
    bool ReflectFields(IReflector& reflector, bool write, std::string* diagnostic = nullptr);
    void Invoke(ManagedCallback callback);
    void InvokeCollision(ManagedCallback callback, const CollisionInfo& collision);
    void RecordFault();
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
