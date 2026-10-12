/// @file    ManagedTransformBatch.hpp
/// @brief   一括 Transform 操作の検証と入力順の姿勢計算。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include "Engine/Scene/ManagedBridgeAbi.hpp"
#include "Engine/Scene/Transform.hpp"
#include <cstdint>
#include <vector>
#include <span>

namespace fbzz::scene {
class Scene;
class GameObject;

namespace managed_detail {

/// @note main thread の同期呼び出し専用。ID resolver と GameObject は非所有で、候補計算は Scene を変更しない。
/// @see Docs/design/csharp-authoring.md 大量対象の入力順・欠損・失敗時の契約。
class ManagedTransformBatch {
public:
    using ObjectResolver = ManagedBridgeStatus (*)(void*, uint64_t, GameObject*&);

    /// @return 全体失敗では Scene と output は未変更。容量不足は reply.outputBytes に必要長を返す。
    ManagedBridgeStatus Run(Scene& scene, const ManagedBridgeRequest& request,
        ManagedBridgeReply& reply, ObjectResolver resolver, void* resolverState);
    /// @note caller-sized aggregate の候補用。wire の uint32 byte 数から切り離し、全体検証後に一度だけ変更する。
    ManagedBridgeStatus RunAggregate(Scene& scene, const ManagedBridgeRequest& request,
        std::span<const char> input, std::span<char> output, ManagedBridgeReply& reply,
        ObjectResolver resolver, void* resolverState, bool publish = true);

    /// @note Position setters and local translation validate all computed poses before changing ancestors or descendants.
    ManagedBridgeStatus RunPosition(GameObject& target, const ManagedBridgeRequest& request);

    /// @note 回転・拡縮の単体操作。読み取りは描画 world キャッシュを保ち、書き込み失敗は祖先・子孫も未変更。
    ManagedBridgeStatus RunPose(GameObject& target, const ManagedBridgeRequest& request,
        ManagedBridgeReply& reply);

private:
    struct Entry {
        GameObject* object = nullptr;
        ManagedObjectStatus status = ManagedObjectStatus::UNSET;
        math::Vector3 position = math::Vector3::ZERO;
        bool apply = false;
    };

    Transform* Shadow(GameObject& object);
    bool Synchronize(GameObject& root, bool shadow);
    bool Update(GameObject& object, bool shadow);
    bool PrepareWrite(Entry& entry, bool translate, const math::Vector3& displacement);
    void ApplyWrite(const Entry& entry);
    /// @note 子なしルートが重複しない場合だけ dense 作業配列へ進む。親子・重複は入力順の shadow を使う。
    bool CanStageIndependentRoots(const Scene& scene);
    bool StageIndependentRoots(bool translate, const math::Vector3& displacement);
    void CommitIndependentRoots();
    void BeginEpoch(const Scene& scene);
    void BeginShadow(const Scene& scene);
    void CommitShadow();

    std::vector<Entry> m_entries;
    std::vector<Transform> m_rootShadow;
    std::vector<Transform> m_shadow;
    std::vector<uint32_t> m_cloneEpoch;
    std::vector<GameObject*> m_ancestors;
    std::vector<GameObject*> m_queue;
    uint32_t m_epoch = 0;
};

} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
