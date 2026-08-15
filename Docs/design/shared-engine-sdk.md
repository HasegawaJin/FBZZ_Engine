# 共有 Engine SDK 参照モデル

## 目的

GameHub 生成プロジェクトが Engine ソースを `add_subdirectory` して再ビルドする構成を廃止し、版別の共有 SDK を CMake `IMPORTED` target として参照する。Editor は SDK ごとに一つだけ配置し、ゲーム側は `Scripts.dll` と Standalone 実行ファイルだけを所有する。

## SDK レイアウト

`FBZZSDK`ターゲットはSDK専用build treeから`SDK/<version>/`へartifactをpublishする。SDKはEngine versionごとに1つだけ存在し、publishのたびに同じ場所を上書きする。SDK IDはversionそのもの (`0.1.0`) で、`.fbzz_proj`の`sdk_id`にもこれが入る。

版ごとに1つへ畳むのは、ABI契約が「version完全一致」であり、同じ版のSDKを複数保持しても選択の手間が増えるだけで区別の意味が無いため。旧方式はcommit revisionと未コミット差分のfingerprintをIDに含めていたが、1世代あたり数百MB級のフォルダーが編集のたびに増え続けていた。旧方式が残した`<version>-dev.*`はpublishのたびに回収する。

- 由来: `fbzz-sdk.toml`の`source_revision`がpublish時点のGit revisionを記録する
- 公開時刻: `bin/<Config>/published.stamp`をpublishのたびに書き換える (installは内容が同じファイルを更新しないため、他のファイルの時刻は公開時刻として使えない)
- 最新性: GameHubのチェックは「worktreeの変更ファイル」と「`source_revision`からHEADまでに変わったファイル」の更新時刻をstampと比較する。commitしただけならファイル実体は変わらないので再publishを要求せず、pull/checkoutで中身が書き換わった場合だけstaleと判定する

- `include/`: Math、Physics、Engine、公開依存の toml++ ヘッダー
- `lib/<Config>/`: `FBZZMath.lib`、`FBZZPhysics.lib`、`FBZZEngine.lib`
- `bin/<Config>/`: Engine runtime DLL と Assimp、ImGui、利用可能な DXC runtime
- `tools/<Config>/Editor/`: 共有 `FBZZEditor.exe`、Editor assets、runtime DLL
- `cmake/FBZZ/`: `FBZZConfig.cmake`、version 判定、`FBZZ::Math` / `FBZZ::Physics` / `FBZZ::Engine` IMPORTED target、runtime staging 関数
- `share/fbzz/Assets/` と `fbzz-sdk.toml`: 読み取り専用Engine assetsとSDK ID・ABI manifest

外部プロジェクトはGameHubが選択したSDKを`FBZZ_SDK_ROOT`として受け取り、`find_package(FBZZ <version> EXACT CONFIG REQUIRED)`を呼ぶ。`.fbzz_proj`にはportableな`[engine].sdk_id`だけを保存する。Standaloneの実行時DLLとEngine assetsは`fbzz_stage_runtime(<target>)`が出力先へ配置する。

## ABI 保証

初期ポリシーは完全一致のみとする。Engine version、MSVC toolset、x64、Debug/Development/Release、`/MDd` または `/MD` が一致しない構成は CMake configure、MSVC link、Script DLL load の各段階で拒否する。Script DLL 署名には型サイズ、component 数、reflection ABI、MSVC 完全版、iterator debug level、版数、構成、ポインタ幅、CRT 種別を含める。

同一 minor 間のバイナリ互換は保証しない。公開 C++ 型または仮想関数表を変更した SDK は Engine version を更新し、全構成の SDK とゲーム Script DLL を再ビルドする。

## 既存プロジェクト

旧`FBZZ_ENGINE_ROOT`やEngine `add_subdirectory`を持つプロジェクトは、独自CMakeを安全に判別できないため自動上書きしない。新テンプレートとの差分を取り込んで`.fbzz_proj`を`[engine].sdk_id`へ移行し、古い`Build/VS`を削除して再configureする。移行完了まではGameHubで選択中のSDKを起動環境へ渡す互換動作だけを提供する。

`sdk_id`に旧方式の世代ID (`0.1.0-dev.<rev>`) が残っていても、GameHubはversion部分だけを見て現行の`SDK/0.1.0`へ解決する。ABI契約がversion完全一致である以上、同じ版を指すpinは同じSDKを指すため。

## 導入順序

1. Visual Studioで必要な構成の`CMake: Build SDK`をビルドし、SDK専用build treeからpublishする。
2. GameHub Settingsで`SDK/<version>`とEditor構成を選択する。
3. 既存プロジェクトは新テンプレートとの差分を取り込み、同じpackage参照へ揃える。
4. GameHubからSDK内Editorを開き、EditorのホットリロードがEngineを再ビルドせず同じ構成の`Scripts.dll`だけを生成・ロードすることを確認する。

GameHubの通常ビルドはSDK publishへ依存させない。配布時だけ`Distribution: Assemble`が公開済みGameHubとSDKを`Artifacts/Distribution/<sdk-id>/<config>/`へ集約する。
