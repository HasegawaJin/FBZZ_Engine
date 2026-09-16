/// @file    EngineRebuildBootstrap.hpp
/// @brief   起動時 Engine 鮮度チェック + 自動リビルド・リランチ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// WHAT:
/// 開発ビルド (Debug / Development) で exe の起動直後に呼ぶ。
/// Engine / Physics / Math のソースが FBZZEngine.dll より新しい場合、
/// 「Engine が古い」ことをユーザーへ確認ダイアログで伝え、
/// 同意されたら cmake でエンジンを含む全ターゲットを再ビルドし、
/// 同じ引数・作業ディレクトリで exe を再起動する。
///
/// WHY (なぜ実行中に直接ビルドしないか):
/// fbzz_engine は SHARED ライブラリ (FBZZEngine.dll) で、各 exe が暗黙リンクする。
/// プロセス起動と同時に DLL がロードされロックされるため、実行中は再リンクできない。
/// スクリプト DLL のホットリロード (Editor 側) が skipDeps=true で Engine を意図的に
/// スキップしているのはこのため。よって「Engine が古いとき」は
/// 「一旦終了 → DLL 解放後にビルド → 再起動」というブートストラップ経路が唯一の解になる。
///
/// WHY (共通ユーティリティとして Engine に置く理由):
/// ParticleGame / Sandbox / EditorLauncher / GameHub 生成プロジェクトなど、
/// すべての exe が fbzz_engine をリンクする。Editor の ToolchainLocator / Compiler は
/// スタンドアロン構成にはリンクされないため、依存を持てない。
/// 起動時の一発同期ビルドは非同期である必要がなく、cmake を子プロセスとして直接叩けば足りる。
///
/// WHY (Release では無効):
/// Release は配布構成。開発機のビルドパスは埋め込まれず、この関数は常に false を返す。
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
