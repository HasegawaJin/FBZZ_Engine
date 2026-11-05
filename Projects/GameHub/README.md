# FBZZ GameHub

新規プロジェクトのゲームロジックは C# です。Empty / Standard の両テンプレートへ、選択 SDK の共通 ManagedStarter から csproj・サンプル・ビルド設定・global.json・AGENTS を追加します。選択 SDK は `Fbzz.Sdk.props` に記録し、IDE と Editor が同じ公開 API / Analyzer を参照します。SDK 同梱の `share/fbzz/Docs/csharp-start.md` が導入の入口です。

Editor の起動には `Assets` と `.fbzz_proj` が指す設定ファイル・起動シーンを使います。`Include`、`Lib`、`Src/Scripts` は不要です。`Src/AppMain.cpp` と CMake は Standalone 実行ファイルの起動入口だけを持ち、C++ Script は生成しません。Standalone のビルド先は `Build/VS/`、出力先は `Binaries/<構成>/` です。

`npm run test:authoring` は Node 24 で両テンプレートの生成・起動ファイル検査、共通スターターの導入、版不一致・欠損の拒否、既存ファイル保持を検証します。

FBZZ Engineのプロジェクト管理ツールです。Electron・React・TypeScriptで構築し、`Projects/GameHub`を正式な実装として使用します。

## 学習するときの読み順

1. `src/shared/contracts.ts` — 全プロセスが共有する型
2. `src/ui/App.tsx` — Reactの状態と画面
3. `src/preload.ts` — 安全なAPIの橋渡し
4. `src/main/ipc.ts` — UI要求をmainプロセスで受け取る場所
5. `src/main/projectService.ts` — ファイル検証とEditor起動
6. `src/main/templateService.ts` — 新規プロジェクト生成
7. `src/main/configStore.ts` — TOML設定の永続化

rendererではNode.js APIを直接使用しません。`contextIsolation`とsandboxを有効にし、preloadが用途別のAPIだけを公開します。これにより、UIとOS権限の境界をコード上で確認できます。

## 実行

```powershell
cd Projects/GameHub
npm start
```

型チェックは`npm run typecheck`で行います。構成別パッケージは次のコマンドで`out/<構成>/`へ生成します。

```powershell
npm run build:debug
npm run build:development
npm run build:release
```

VS Codeでは`GameHub (Debug)`、`GameHub (Development)`、`GameHub (Release)`を選ぶと、対応構成をビルドしてから起動します。

## C++版から引き継ぐもの

- `%APPDATA%/FBZZHub/hub_config.toml`
- `Projects/GameHub/Templates`
- `.fbzz_proj`フォーマット
- `FBZZEditor.exe --project <path>`起動契約

SDK設定が空、または旧Engineルートを指している場合は、起動時に祖先ディレクトリの`SDK/` storeを探索します。`fbzz-sdk.toml`、`cmake/FBZZ/FBZZConfig.cmake`、`include/Engine`を確認できるimmutable SDKだけを選択し、manifestのSDK IDを`hub_config.toml`へ保存します。

GameHubの構成別ビルドは、開始時にGit revisionと未コミット差分からCMakeと同じimmutable SDK IDを計算し、同じ構成のDLL・LIB・Editorまで公開済みか確認します。SDKが古い、または不完全な場合はビルドを中止するため、案内された`CMake: Build SDK (<構成>)`を実行してから再ビルドしてください。GameHub・SDKをまとめた配布物が必要な場合は`Distribution: Assemble (<構成>)`を使用します。新規プロジェクトは`.fbzz_proj`へSDK IDのみを保存し、SDKの絶対パスはGameHubが起動時に環境変数で渡します。
