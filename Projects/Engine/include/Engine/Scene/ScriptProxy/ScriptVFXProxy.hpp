/// @file    ScriptVFXProxy.hpp
/// @brief   Script から同じ GameObject の VFXComponent 再生状態を制御する
/// @author  Hasegawa Jin
/// @date    2026-08-26

/// @note パラメーター操作 (SetFloat/SetColor/SetAsset 等) は持たない。.vfx はプレハブになり、
/// @note 公開パラメーターはルートスクリプトの公開フィールドへ移った (名前渡しは型が効かず
/// @note 綴り違いが黙って通るため)。
/// @see Docs/design/vfx-prefab.md §6
#pragma once
#include <Engine/Api.hpp>

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptVFXProxy {
    explicit ScriptVFXProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    FBZZ_ENGINE_API void Play(bool restart = true) const;
    FBZZ_ENGINE_API void Pause() const;
    FBZZ_ENGINE_API void Stop() const;
    FBZZ_ENGINE_API void SetSpeed(float speed) const;
    /// @note trigger 名を持つ VFXElement を開始する。空名は明示的に許可しない。
    FBZZ_ENGINE_API bool Trigger(std::string_view name) const;

    [[nodiscard]] FBZZ_ENGINE_API bool IsPlaying() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetTime() const;
    /// @note 全体の尺 [秒]。duration が 0 (自動) なら配下から算出した実効尺を返す。
    [[nodiscard]] FBZZ_ENGINE_API float GetDuration() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetSpeed() const;
};

} /// @note namespace fbzz::scene
