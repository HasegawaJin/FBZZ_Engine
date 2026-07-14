# 共有 Engine SDK 参照モデル

## 目的

GameHub 生成プロジェクトが Engine ソースを `add_subdirectory` して再ビルドする構成を廃止し、版別の共有 SDK を CMake `IMPORTED` target として参照する。Editor は SDK ごとに一つだけ配置し、ゲーム側は `Scripts.dll` と Standalone 実行ファイルだけを所有する。

## SDK レイアウト

`fbzz_sdk` ターゲットは `SDK/<engine-version>/` に次を生成する。

- `include/`: Math、Physics、Engine、公開依存の toml++ ヘッダー
- `lib/<Config>/`: `FBZZMath.lib`、`FBZZPhysics.lib`、`FBZZEngine.lib`
- `bin/<Config>/`: Engine runtime DLL と Assimp、ImGui、利用可能な DXC runtime
- `tools/<Config>/Editor/`: 共有 `FBZZEditor.exe`、Editor assets、runtime DLL
- `cmake/FBZZ/`: `FBZZConfig.cmake`、version 判定、`FBZZ::Math` / `FBZZ::Physics` / `FBZZ::Engine` IMPORTED target、runtime staging 関数
- `Assets/` と `fbzz-sdk.toml`: 新規プロジェクト用共通 assets と ABI manifest

外部プロジェクトは `FBZZ_SDK_ROOT` を版別 SDK に設定し、`find_package(FBZZ <version> EXACT CONFIG REQUIRED)` を呼ぶ。Standalone の実行時 DLL は `fbzz_stage_runtime(<target>)` が出力先へ配置する。

## ABI 保証

初期ポリシーは完全一致のみとする。Engine version、MSVC toolset、x64、Debug/Development/Release、`/MDd` または `/MD` が一致しない構成は CMake configure、MSVC link、Script DLL load の各段階で拒否する。Script DLL 署名には型サイズ、component 数、reflection ABI、MSVC 完全版、iterator debug level、版数、構成、ポインタ幅、CRT 種別を含める。

同一 minor 間のバイナリ互換は保証しない。公開 C++ 型または仮想関数表を変更した SDK は Engine version を更新し、全構成の SDK とゲーム Script DLL を再ビルドする。

## Migration

GameHub は Migration 前に `.fbzz_proj`、CMake/preset、ProjectSettings、公開 API、GameMain を `Backups/` へ保存する。旧 `FBZZ_ENGINE_ROOT` と Engine `add_subdirectory` を持つ既知テンプレートだけを canonical SDK CMake へ置換し、独自 CMake は自動変更せずエラーにする。`.fbzz_proj` は `[engine].sdk_root`、preset は `FBZZ_SDK_ROOT` へ移行し、古い `Build/VS` は削除して再 configure する。

## 導入順序

1. Visual Studio で Debug、Development、Release の `fbzz_sdk` を順にビルドする。
2. GameHub Settings で `SDK/<engine-version>` を選択する。
3. 既存プロジェクトを Migration し、新規プロジェクトと同じ package 参照へ揃える。
4. GameHub から共有 Editor を開き、Editor のホットリロードで同じ構成の `Scripts.dll` を生成・ロードできることを確認する。
