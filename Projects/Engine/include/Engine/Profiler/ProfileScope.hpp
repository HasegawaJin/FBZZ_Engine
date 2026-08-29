/// @file    ProfileScope.hpp
/// @brief   スコープ寿命に合わせて CPU 計測区間を自動記録する RAII ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <Engine/Profiler/Profiler.hpp>

namespace fbzz::profiler {

// 関数やブロックの実行時間を自動計測する RAII オブジェクト。
// WHY: return が複数ある関数でも EndSample の呼び忘れを防ぎ、計測コードを安全に差し込める。
class ProfileScope {
public:
    // コンストラクタで計測を開始する。marker は ProfilerMarker と同じ寿命前提を持つ。
    explicit ProfileScope(const ProfilerMarker& marker)
    {
        Profiler::BeginSample(marker);
    }

    ProfileScope(const ProfileScope&)            = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

    // デストラクタで計測を閉じる。例外は禁止方針のため noexcept を明示する。
    ~ProfileScope() noexcept
    {
        Profiler::EndSample();
    }
};

} // namespace fbzz::profiler

// 同一スコープに複数マクロを置いても変数名が衝突しないよう __LINE__ を連結する。
#define FBZZ_PROFILE_CONCAT_IMPL(a, b) a##b
#define FBZZ_PROFILE_CONCAT(a, b) FBZZ_PROFILE_CONCAT_IMPL(a, b)

// 任意名のスコープ計測を追加する。例: FBZZ_PROFILE_SCOPE("RenderSystem::Update");
#define FBZZ_PROFILE_SCOPE(name) \
    ::fbzz::profiler::ProfileScope FBZZ_PROFILE_CONCAT(fbzzProfileScope, __LINE__)( \
        ::fbzz::profiler::ProfilerMarker((name)))

// 現在の関数名を使ってスコープ計測を追加する。
#define FBZZ_PROFILE_FUNCTION() FBZZ_PROFILE_SCOPE(__FUNCTION__)

// 時間幅を持たない目印を現在フレームへ記録する。
#define FBZZ_PROFILE_MARKER(name) \
    ::fbzz::profiler::Profiler::PushMarker(::fbzz::profiler::ProfilerMarker((name)))
