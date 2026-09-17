/// @file    EngineRebuildBootstrap.hpp
/// @brief   起動時 Engine 鮮度チェック + 自動リビルド・リランチ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// @note Debug/Development で起動直後に呼ぶ。Engine/Physics/Math のソースが FBZZEngine.dll より新しければ確認ダイアログの同意後に cmake で再ビルドし、同じ引数・作業ディレクトリで exe を再起動する。
/// @note fbzz_engine は SHARED ライブラリで起動時に暗黙リンクされロックされるため実行中は再リンクできず、「終了→DLL 解放後にビルド→再起動」が唯一の経路 (スクリプト DLL のホットリロードが skipDeps=true で Engine を意図的にスキップしているのも同じ理由)。
/// @note 全 exe が fbzz_engine をリンクする一方 Editor の ToolchainLocator/Compiler はスタンドアロン構成にリンクされないため、共通ユーティリティとして Engine に置き cmake を子プロセスとして直接叩く。
/// @note Release は配布構成で開発機のビルドパスを埋め込まないため、この関数は常に false を返す。
#pragma once

namespace fbzz::core {

/// Engine ソースが FBZZEngine.dll より新しければ確認ダイアログを出し、
/// ユーザーが同意した場合に「cmake 再ビルド → 再起動」を予約する。
///
/// @return true のとき再ビルド・再起動を予約済み。呼び出し側は速やかにプロセスを
///         正常終了 (return 0) させること。DLL を解放しないと予約したビルドが進めない。
///         false のときは通常どおり起動を続行してよい (最新 / ユーザー拒否 / 配布ビルド等)。
[[nodiscard]] bool CheckEngineFreshnessAndRelaunch();

} // namespace fbzz::core
