/// @file    ManagedServiceAudio.hpp
/// @brief   マネージド音声操作と所有者に紐づく合成音ハンドル。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include "../../Interop/ManagedServiceArguments.hpp"
#include "../ManagedServicesBridge.hpp"

#include <Engine/Scene/ScriptProxy/Audio/ScriptAudioProxy.hpp>
#include <unordered_map>

namespace fbzz::scene::managed_detail {

/// @note クリップとループのハンドルは世代、所有者、シーンに紐づく。解放は AudioManager の生存中に行う。
class ManagedServiceAudio {
public:
    ManagedBridgeStatus Run(Scene& scene, Script& caller, Script& view, GameObject& target,
        const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments, ManagedServiceResult& output);
    void CancelOwner(uint64_t ownerId, Script* caller, Script& view);
    void RetireGeneration(uint64_t generation, Script& view);

private:
    struct ClipRecord {
        uint64_t generation;
        uint64_t owner;
        uint64_t scene;
        SynthClip clip;
    };

    struct LoopRecord {
        uint64_t generation;
        uint64_t owner;
        uint64_t scene;
        AudioLoop loop;
    };

    /// @note 両方の型で同じ採番を使い、解放後も過去の値を再利用しない。
    std::unordered_map<int64_t, ClipRecord> m_clips;
    std::unordered_map<int64_t, LoopRecord> m_loops;
    int64_t m_nextHandle = 1;

    ManagedBridgeStatus RunAdvanced(Scene& scene, Script& caller, Script& view, GameObject& target,
        const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments, ManagedServiceResult& output);
};

}
