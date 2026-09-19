/// @file    InputRecorder.hpp
/// @brief   Play 中の入力を «変化したフレームだけ» の input.inject 列として記録する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md «入力の記録と再生»
#pragma once
#include <Editor/Ai/Json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fbzz::editor::playtest {

/// @brief 記録は生のキー / マウスと、アクション層 (InputActionMap) の両方を取る。
/// @note パッドは Input へ注入する口が無いので、アクション名の仮想ボタン / 軸として残す。再生時にキーとアクションが二重に入っても OR なので結果は同じ。
class InputRecorder {
public:
    void Start();
    /// @return {"version":1,"frames":N,"events":[{"frame":f,"inject":{...}}]}。記録していなければ空の列。
    [[nodiscard]] ai::JsonValue Stop();
    [[nodiscard]] bool IsRecording() const { return m_recording; }
    [[nodiscard]] uint64_t FrameCount() const { return m_frame; }

    /// @brief 1 フレームぶん読む。アクション層の評価後 (EditorApp::OnUpdate) に呼ぶ。
    /// @pre Play 中。編集中の入力はエディター操作なので記録しない。
    void Capture();

private:
    void Emit(ai::JsonValue inject);

    bool                                   m_recording = false;
    uint64_t                               m_frame = 0;
    std::array<bool, 256>                  m_keys{};
    std::array<bool, 3>                    m_mouseButtons{};
    std::unordered_map<std::string, bool>  m_actions;
    std::unordered_map<std::string, float> m_axes;
    ai::JsonValue                          m_events = ai::JsonValue::MakeArray();
};

} // namespace fbzz::editor::playtest
