/// @file    JobBreakaway.hpp
/// @brief   起動元の Job Object から抜けて、親の終了に巻き込まれないようにする。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note GameHub は Editor を detached + `cmd /c start` 越しに起動する (projectService.ts) が、開発起動 (`npm start`) の
///       GameHub を閉じると Editor も道連れになる。プロセスツリー全体が Job Object に入り
///       JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE で孫まで殺されるためで、Job の所属は親子関係と別軸なので detached では
///       抜けられず、CREATE_BREAKAWAY_FROM_JOB で自分を作り直すしかない。
#pragma once

namespace fbzz::editor_launcher {

/// 「閉じられたら道連れ」の Job に入っていれば、Job の外へ自分を起動し直す。
///
/// @return true なら呼び出し側は直ちに 0 を返して終了すること (後続の起動は新プロセスが行う)。
///         false ならそのまま起動を続けてよい (Job に居ない / 既に抜けた / 抜けられない)。
///
/// 抜けられない Job (BREAKAWAY_OK が無い) のときは警告を 1 本残して false を返す。
/// 配布物やエクスプローラーからの起動では Job に入らないので、何も起きない。
[[nodiscard]] bool RelaunchOutsideKillOnCloseJob();

} // namespace fbzz::editor_launcher
