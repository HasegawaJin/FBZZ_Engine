/// @file    Coroutine.hpp
/// @brief   Unity 風コルーチン (C++20 coroutine ベース)。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// @note 待機命令 (WaitForSeconds / WaitForSecondsRealtime / WaitForFrames / WaitUntil / WaitWhile) は必ず 1 フレーム以上中断する (0 秒・0 フレーム・成立済み述語でも)。即座に再開できるとループ内の co_await が回り続けフレームが返らなくなるため。
/// @note 進行は ScriptSystem が毎フレーム Script::UpdateCoroutines() を呼んで行う。Script が破棄されると保持中のコルーチンも安全に破棄される (再開されない)。
#pragma once

#include <Engine/Core/Time.hpp>
#include <coroutine>
#include <functional>
#include <utility>

namespace fbzz::scene {

struct Coroutine {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    /// @brief 現在の待機命令。yield ごとの heap 割り当てを避けるためプロミスに直接保持する。
    struct WaitState {
        enum class Kind { Resume, Seconds, SecondsRealtime, Frames, Predicate };
        Kind  kind  = Kind::Resume; ///< 既定: 次フレームに再開
        float seconds = 0.0f;
        int   frames  = 0;
        std::function<bool()> predicate;
        bool  predicateTarget = true; ///< WaitUntil:true で成立 / WaitWhile:false で成立

        /// @brief 待機命令を差し替えるたびに作り直す。
        /// @note kind だけ上書きすると、一度 WaitUntil を通ったコルーチンが predicate のキャプチャを終了まで抱えたままになる。
        void Reset() { *this = WaitState{}; }
    };

    struct promise_type {
        WaitState wait;
        /// @note 生成フレームでは Step させない。ScriptSystem は OnStart や Invoke のコールバックより後に UpdateCoroutines を呼ぶため、そこで開始したコルーチンだけ同じフレームで最初の待機を消費してしまう。
        uint64_t startFrame = fbzz::Time::frameCount;

        Coroutine get_return_object() { return Coroutine{ handle_type::from_promise(*this) }; }
        /// @note 起動時に最初の co_await まで即実行する (Unity と同じ挙動)。
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend()   noexcept { return {}; }
        void return_void() noexcept {}
        /// @note throw 禁止方針: 万一の例外は握り潰す。
        void unhandled_exception() noexcept {}
    };

    Coroutine() = default;
    explicit Coroutine(handle_type h) : m_handle(h) {}
    Coroutine(const Coroutine&)            = delete;
    Coroutine& operator=(const Coroutine&) = delete;
    Coroutine(Coroutine&& o) noexcept : m_handle(std::exchange(o.m_handle, {})) {}
    Coroutine& operator=(Coroutine&& o) noexcept {
        if (this != &o) { Destroy(); m_handle = std::exchange(o.m_handle, {}); }
        return *this;
    }
    ~Coroutine() { Destroy(); }

    [[nodiscard]] bool Done() const { return !m_handle || m_handle.done(); }

    /// @brief フレームを破棄せずに手放す。
    /// @note アクセス違反を SEH で巻き戻したフレームは中断点に居らず、done() も destroy() も呼べない。畳もうとすると二次クラッシュになるため、その一本道だけ解放を諦めて切り離す。
    void Release() noexcept { m_handle = {}; }

    /// @brief 1 フレーム分進める。待機条件を満たしていれば再開する。完了済みなら false。
    bool Step() {
        if (!m_handle || m_handle.done()) return false;
        promise_type& p = m_handle.promise();
        if (p.startFrame == fbzz::Time::frameCount) return true;
        WaitState& w = p.wait;
        bool ready = false;
        switch (w.kind) {
        case WaitState::Kind::Resume:          ready = true; break;
        case WaitState::Kind::Seconds:
            w.seconds -= fbzz::Time::deltaTime;          ready = w.seconds <= 0.0f; break;
        case WaitState::Kind::SecondsRealtime:
            w.seconds -= fbzz::Time::unscaledDeltaTime;  ready = w.seconds <= 0.0f; break;
        case WaitState::Kind::Frames:          ready = (--w.frames <= 0); break;
        case WaitState::Kind::Predicate:
            /// @note 空の述語は判定できない。待ち続けると復帰不能になるため次フレームで再開する。
            ready = !w.predicate || (w.predicate() == w.predicateTarget); break;
        }
        /// @note 再開すると body が次の co_await まで進み wait を再設定する。
        if (ready)
            m_handle.resume();
        return !m_handle.done();
    }

private:
    void Destroy() { if (m_handle) { m_handle.destroy(); m_handle = {}; } }
    handle_type m_handle;
};

/// @name co_await できる待機命令
/// @{

/// @brief 指定秒数 (timeScale 適用) だけ待つ。
struct WaitForSeconds {
    float seconds;
    explicit WaitForSeconds(float s) : seconds(s) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(Coroutine::handle_type h) const noexcept {
        auto& w = h.promise().wait;
        w.Reset();
        w.kind = Coroutine::WaitState::Kind::Seconds;
        w.seconds = seconds;
    }
    void await_resume() const noexcept {}
};

/// @brief 指定秒数 (timeScale 非適用の実時間) だけ待つ。ヒットストップ等、停止中でも進めたい待機に使う。
struct WaitForSecondsRealtime {
    float seconds;
    explicit WaitForSecondsRealtime(float s) : seconds(s) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(Coroutine::handle_type h) const noexcept {
        auto& w = h.promise().wait;
        w.Reset();
        w.kind = Coroutine::WaitState::Kind::SecondsRealtime;
        w.seconds = seconds;
    }
    void await_resume() const noexcept {}
};

/// @brief 指定フレーム数だけ待つ。1 を渡すと「次フレームまで」。0 以下でも 1 フレームは待つ。
struct WaitForFrames {
    int frames;
    explicit WaitForFrames(int n) : frames(n) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(Coroutine::handle_type h) const noexcept {
        auto& w = h.promise().wait;
        w.Reset();
        w.kind = Coroutine::WaitState::Kind::Frames;
        w.frames = frames;
    }
    void await_resume() const noexcept {}
};

/// @brief 述語が true になるまで待つ。
struct WaitUntil {
    std::function<bool()> predicate;
    explicit WaitUntil(std::function<bool()> p) : predicate(std::move(p)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(Coroutine::handle_type h) const {
        auto& w = h.promise().wait;
        w.Reset();
        w.kind = Coroutine::WaitState::Kind::Predicate;
        w.predicate = predicate;
        w.predicateTarget = true;
    }
    void await_resume() const noexcept {}
};

/// @brief 述語が false になるまで待つ。
struct WaitWhile {
    std::function<bool()> predicate;
    explicit WaitWhile(std::function<bool()> p) : predicate(std::move(p)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(Coroutine::handle_type h) const {
        auto& w = h.promise().wait;
        w.Reset();
        w.kind = Coroutine::WaitState::Kind::Predicate;
        w.predicate = predicate;
        w.predicateTarget = false;
    }
    void await_resume() const noexcept {}
};

/// @}

} // namespace fbzz::scene
