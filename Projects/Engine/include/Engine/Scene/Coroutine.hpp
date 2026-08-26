// FBZZ Engine
// Coroutine.hpp | fbzz::scene
// Unity 風コルーチン (C++20 coroutine ベース)。
//
// 設計意図 (WHY):
//   「少し待ってから戻す」「数フレーム後に実行」「条件成立まで待つ」といった時間軸の処理を、
//   OnUpdate にステートマシンとフラグを展開せず、直線的なコードで書けるようにする。
//   ヒットストップ (一瞬 timeScale を 0 にして戻す) のような演出が典型例。
//
// 使い方:
//   Coroutine HitStop() {
//       time.SetTimeScale(0.0f);
//       co_await WaitForSecondsRealtime(0.08f); // timeScale=0 中でも進む実時間待ち
//       time.SetTimeScale(1.0f);
//   }
//   // どこかで:
//   StartCoroutine(HitStop());
//
//   待機命令: WaitForSeconds / WaitForSecondsRealtime / WaitForFrames / WaitUntil / WaitWhile
//   どの待機命令も必ず 1 フレーム以上中断する (0 秒・0 フレーム・成立済み述語であっても)。
//   その場で再開できてしまうと、ループ内の co_await が回り続けてフレームが返らなくなるため。
//
// 進行は ScriptSystem が毎フレーム Script::UpdateCoroutines() を呼んで行う。
// Script が破棄されると保持中のコルーチンも安全に破棄される (再開されない)。
#pragma once

#include <Engine/Core/Time.hpp>
#include <coroutine>
#include <functional>
#include <utility>

namespace fbzz::scene {

struct Coroutine {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    // 現在の待機命令。yield ごとの heap 割り当てを避けるためプロミスに直接保持する。
    struct WaitState {
        enum class Kind { Resume, Seconds, SecondsRealtime, Frames, Predicate };
        Kind  kind  = Kind::Resume; // 既定: 次フレームに再開
        float seconds = 0.0f;
        int   frames  = 0;
        std::function<bool()> predicate;
        bool  predicateTarget = true; // WaitUntil:true で成立 / WaitWhile:false で成立

        // 待機命令を差し替えるたびに作り直す。
        // WHY: kind だけ上書きすると、一度 WaitUntil を通ったコルーチンが
        //      predicate のキャプチャを終了まで抱えたままになる。
        void Reset() { *this = WaitState{}; }
    };

    struct promise_type {
        WaitState wait;
        // WHY: 生成フレームでは Step させない。ScriptSystem は OnStart や Invoke の
        //      コールバックより後に UpdateCoroutines を呼ぶため、そこで開始した
        //      コルーチンだけ同じフレームで最初の待機を消費してしまう。
        uint64_t startFrame = fbzz::Time::frameCount;

        Coroutine get_return_object() { return Coroutine{ handle_type::from_promise(*this) }; }
        // suspend_never: 起動時に最初の co_await まで即実行する (Unity と同じ挙動)。
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend()   noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {} // throw 禁止方針: 万一の例外は握り潰す
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

    // フレームを破棄せずに手放す。
    // WHY: アクセス違反を SEH で巻き戻したフレームは中断点に居らず、done() も
    //      destroy() も呼べない。畳もうとすると二次クラッシュになるため、
    //      その一本道だけ解放を諦めて切り離す。
    void Release() noexcept { m_handle = {}; }

    // 1 フレーム分進める。待機条件を満たしていれば再開する。完了済みなら false。
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
            // 空の述語は判定できない。待ち続けると復帰不能になるため次フレームで再開する。
            ready = !w.predicate || (w.predicate() == w.predicateTarget); break;
        }
        if (ready)
            m_handle.resume(); // 再開すると body が次の co_await まで進み wait を再設定する
        return !m_handle.done();
    }

private:
    void Destroy() { if (m_handle) { m_handle.destroy(); m_handle = {}; } }
    handle_type m_handle;
};

// ── co_await できる待機命令 ───────────────────────────────────────────────────

// 指定秒数 (timeScale 適用) だけ待つ。
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

// 指定秒数 (timeScale 非適用の実時間) だけ待つ。ヒットストップ等、停止中でも進めたい待機に使う。
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

// 指定フレーム数だけ待つ。1 を渡すと「次フレームまで」。0 以下でも 1 フレームは待つ。
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

// 述語が true になるまで待つ。
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

// 述語が false になるまで待つ。
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

} // namespace fbzz::scene
