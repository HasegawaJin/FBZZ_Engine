/// @file    ScriptVFXProxy.hpp
/// @brief   Script から同じ GameObject の VFXComponent 再生状態を制御する
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY パラメーター操作 (SetFloat / SetColor / SetAsset …) を持たないか:
///   .vfx がプレハブになり、公開パラメーターは «ルートに載せたスクリプトの公開
///   フィールド» へ移った (Docs/design/vfx-prefab.md §6)。名前で値を差すのは
///   型が効かず、綴り違いが黙って通る。値の受け口はスクリプト自身が持つ。
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptVFXProxy {
    explicit ScriptVFXProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    void Play(bool restart = true) const;
    void Pause() const;
    void Stop() const;
    void SetSpeed(float speed) const;
    /// trigger 名を持つ VFXElement を開始する。空名は明示的に許可しない。
    bool Trigger(std::string_view name) const;

    [[nodiscard]] bool IsPlaying() const;
    [[nodiscard]] float GetTime() const;
    /// 全体の尺 [秒]。duration が 0 (自動) なら配下から算出した実効尺を返す。
    [[nodiscard]] float GetDuration() const;
    [[nodiscard]] float GetSpeed() const;
};

} // namespace fbzz::scene
