/// @file    ScriptCursorProxy.hpp
/// @brief   Script からマウスカーソル表示・拘束・見た目を制御するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once

#include <Engine/Core/Cursor.hpp>

#include <utility>

namespace fbzz::scene {

class Script;

using CursorLockMode = core::CursorLockMode;
using CursorShape    = core::CursorShape;
namespace CursorPriority = core::CursorPriority;

/// カーソルの要求 1 件。この変数が生きている間だけ効く。
///
/// WHY 値を «持つ» 形にするか: cursor.SetLockMode で直接書くと、状態を元へ戻す責任が
///     呼んだ側に残る。ポーズを閉じたときに «開く前が何だったか» を覚えておく必要が
///     あり、画面が増えるほど戻し漏れが起きる。スクリプトのメンバーとして持てば、
///     破棄・シーン遷移・DLL リロードのどれで消えても要求は自動的に畳まれ、
///     残っている中で最も強い要求へ戻る。
///
/// 使い方:
/// @code
/// class PauseMenu : public Script {
///     CursorRequest m_cursor;
///     void OnStart() override {
///         m_cursor = cursor.Push(CursorLockMode::Confined, true, CursorPriority::Modal);
///     }
/// };
/// @endcode
class CursorRequest {
public:
    CursorRequest() = default;
    explicit CursorRequest(core::CursorRequestId id) : m_id(id) {}

    CursorRequest(const CursorRequest&)            = delete;
    CursorRequest& operator=(const CursorRequest&) = delete;

    CursorRequest(CursorRequest&& other) noexcept
        : m_id(std::exchange(other.m_id, core::kInvalidCursorRequest)) {}

    CursorRequest& operator=(CursorRequest&& other) noexcept
    {
        if (this != &other) {
            Release();
            m_id = std::exchange(other.m_id, core::kInvalidCursorRequest);
        }
        return *this;
    }

    ~CursorRequest() { Release(); }

    /// 要求を取り下げる。デストラクタからも呼ばれるので、明示的に呼ぶのは
    /// 「まだ生きているが今は要らない」ときだけでよい。
    void Release()
    {
        if (m_id == core::kInvalidCursorRequest) return;
        core::Cursor::Release(m_id);
        m_id = core::kInvalidCursorRequest;
    }

    /// 積んだままの要求の中身を差し替える。マウスとパッドで表示を切り替えるように、
    /// 「主張し続けたまま内容だけ変える」場面で使う。
    void Set(CursorLockMode mode, bool visible)
    {
        core::Cursor::UpdateRequest(m_id, { mode, visible });
    }

    [[nodiscard]] bool IsActive() const { return m_id != core::kInvalidCursorRequest; }

private:
    core::CursorRequestId m_id = core::kInvalidCursorRequest;
};

struct ScriptCursorProxy {
    Script* script = nullptr;

    /// この Script が生きている間だけ効くカーソル要求を積む。戻り値をメンバーへ保持すること。
    /// 同じ優先度の要求が並んだときは、後から積んだ方が勝つ。
    [[nodiscard]] CursorRequest Push(CursorLockMode mode, bool visible,
                                     int priority = CursorPriority::Gameplay) const;

    /// 誰も Push していないときの状態 (Unity の Cursor.visible / lockState 相当)。
    /// 主張者が 1 つしか居ない画面ならこれで足りる。
    void SetVisible(bool visible) const;
    bool IsVisible() const;
    void SetLockMode(CursorLockMode mode) const;
    CursorLockMode GetLockMode() const;

    /// カーソルの絵の種類。実際の画像は ProjectSettings の [cursor] が持つ。
    void        SetShape(CursorShape shape) const;
    CursorShape GetShape() const;
    /// その種類に画像が割り当てられているか。OS カーソルを見せるか自前で描くかの判断に使う。
    bool        HasShapeImage(CursorShape shape) const;

    void ResetForEditor() const;
};

} // namespace fbzz::scene
