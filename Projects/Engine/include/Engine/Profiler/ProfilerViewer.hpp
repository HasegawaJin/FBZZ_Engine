/// @file    ProfilerViewer.hpp
/// @brief   収集済みプロファイル結果を ImGui で確認するデバッグビュー。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

namespace fbzz::profiler {

/// Profiler が保持する直近フレームの CPU サンプルを描画するビュー。
/// @note Editor/Sandbox どちらからでも確認できるよう、計測 API と表示を同じ機能単位に置く。
class ProfilerViewer {
public:
    /// ImGui フレーム内で呼び、直近フレームのサンプル一覧と合計時間を描画する。
    static void Draw(bool* open = nullptr);
};

} // namespace fbzz::profiler
