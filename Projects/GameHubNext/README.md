# FBZZ GameHub Next

FBZZ Engineのプロジェクト管理ツールを、Electron・React・TypeScriptで再構築したものです。既存C++版は比較とロールバックのため`Projects/GameHub`に残しています。

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
cd Projects/GameHubNext
npm start
```

型チェックは`npm run typecheck`、配布用パッケージの検証は`npm run package`で行います。

## C++版から引き継ぐもの

- `%APPDATA%/FBZZHub/hub_config.toml`
- `Projects/GameHub/Templates`
- `.fbzz_proj`フォーマット
- `FBZZEditor.exe --project <path>`起動契約

SDK設定が空、または旧Engineルートを指している場合は、起動時に`SDK/<engine-version>`を探索します。`fbzz-sdk.toml`、`cmake/FBZZ/FBZZConfig.cmake`、`include/Engine`を確認できたパスだけを`hub_config.toml`へ保存します。

C++版の`fbzz_hub`はルートCMakeから除外済みです。共有SDKが必要な場合は、Visual Studioから`fbzz_sdk`ターゲットを明示的にビルドしてください。
