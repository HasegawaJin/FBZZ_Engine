---
name: add-script-proxy
description: C#ゲームからEngine機能を呼べる公開APIを拡張する。C# facadeと固定 managed bridgeを実装し、C#からEngine内部型を隠す。
---

# C# SDK API の追加

ゲーム作者向け API は C# SDK に置き、Engine 内部型は C++ 実装へ閉じる。ゲーム logic は C#、大量処理や共有 Engine 機能は既存 Engine service へ接続する。

## 既存 API の拡張

1. `script_api_catalog` と `Docs/design/csharp-authoring.md` で既存 public signature・単位・座標・許可callbackを確認する。
2. facade を `Projects/Managed/Fbzz.ScriptApi/` に追加し、固定 ABI を `Projects/Engine/include/Engine/Scene/ManagedBridgeContract.inl` と対応する ScriptApi binding / Engine bridge に実装する。
3. 引数・戻り値は値型か不透明 ID に限定し、Engine 内部型を C# に露出しない。失敗・単位・所有者・実行スレッドを API 契約へ記録する。

## 新しい Engine service 接続

- 既存 Component / System / ScriptProxy kernel を再利用し、ゲーム規則を Engine service に移さない。
- 更新した API catalog / SDK XML と必要な source generator binding を同期する。
- C ABI に CLR object や所有ポインターを渡さない。Entity・Script・Component の寿命と callback 権限を検証する。

## ABI と検証

- 固定 ABI の契約を変更した場合は Editor / Engine 実行ファイルを再ビルドする。通常の C# authoring 変更は Engine の再リンクを要しない。
- Engine C++ を変更したら `Tools/AgentBuild.ps1 check <変更したC++ bridge/header>` でコンパイル確認する。
- managed SDK を更新したら managed build と Editor / Playtest 経路で確認する。C# API の追加だけを理由にゲーム側へ Engine 内部ソースを要求しない。
