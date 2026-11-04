/// @file    ScriptCursorProxy.hpp
/// @brief   Script からマウスカーソル表示・拘束・見た目を制御するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/Cursor.hpp>

#include <utility>

namespace fbzz::scene {

class Script;

using CursorLockMode = core::CursorLockMode;
using CursorShape    = core::CursorShape;
namespace CursorPriority = core::CursorPriority;

/// @brief カーソルの要求 1 件。この変数が生きている間だけ効く。
/// @note RAII 管理。破棄・シーン遷移・DLL リロードのどれで消えても要求は自動的に畳まれ、
/// @note 残る中で最も強い要求へ戻る (直接 SetLockMode を書くと戻す責任が呼び出し側に残る)。
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

    /// @note 要求を取り下げる。デストラクタからも呼ばれるので、明示的に呼ぶのは
    /// @note 「まだ生きているが今は要らない」ときだけでよい。
    void Release()
    {
        if (m_id == core::kInvalidCursorRequest) return;
        core::Cursor::Release(m_id);
        m_id = core::kInvalidCursorRequest;
    }

    /// @note 積んだままの要求の中身を差し替える。マウスとパッドで表示を切り替えるように、
    /// @note 「主張し続けたまま内容だけ変える」場面で使う。
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

    /// @note この Script が生きている間だけ効くカーソル要求を積む。戻り値をメンバーへ保持すること。
    /// @note 同じ優先度の要求が並んだときは、後から積んだ方が勝つ。
    [[nodiscard]] FBZZ_ENGINE_API CursorRequest Push(CursorLockMode mode, bool visible,
                                     int priority = CursorPriority::Gameplay) const;

    /// @note 誰も Push していないときの状態 (Unity の Cursor.visible / lockState 相当)。
    /// @note 主張者が 1 つしか居ない画面ならこれで足りる。
    FBZZ_ENGINE_API void SetVisible(bool visible) const;
    FBZZ_ENGINE_API bool IsVisible() const;
    FBZZ_ENGINE_API void SetLockMode(CursorLockMode mode) const;
    FBZZ_ENGINE_API CursorLockMode GetLockMode() const;

    /// @note カーソルの絵の種類。実際の画像は ProjectSettings の [cursor] が持つ。
    FBZZ_ENGINE_API void        SetShape(CursorShape shape) const;
    FBZZ_ENGINE_API CursorShape GetShape() const;
    /// @note その種類に画像が割り当てられているか。OS カーソルを見せるか自前で描くかの判断に使う。
    FBZZ_ENGINE_API bool        HasShapeImage(CursorShape shape) const;

    FBZZ_ENGINE_API void ResetForEditor() const;
};

} /// @note namespace fbzz::scene
