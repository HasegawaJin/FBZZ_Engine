---
name: add-script
description: C#ゲームスクリプトを追加・削除する。公開SDK・protected override・managed buildを使い、関連する.metaとシーン参照も確認する。
---

# ゲームスクリプトの追加・削除

SDK 0.10.0以降のゲーム Script はC#で追加する。現行 authoring の入口は `Docs/csharp-start.md`、`Docs/design/csharp-authoring.md`、`Docs/design/ai-script-workflow.md`。SDK 0.9.1の旧ゲーム資料を現行 API として使わない。

## 追加

1. csprojが無ければ選択SDKの共通 `share/fbzz/managed/ManagedStarter` を導入する。API / AnalyzerとSDKのglobal.jsonを揃え、プロジェクト直下の `.Scripts.csproj` は一つにする。
2. `script_api_catalog` で現行の公開API・単位・座標・lifecycleCallbacksを確認する。エンジン内部ソース無しでも同じ手順を使う。
3. `Assets/Scripts/<領域>/XxxComponent.cs` に `using Fbzz;` とScript派生classを書く。対応設定はpublic field、内部状態はprivate field、必要なcallbackだけprotected override。登録マクロ・手書き型GUID・登録リスト編集は不要。
4. 未設定/失効したGameObjectをguardする。物理変更はFixedUpdate。Jobs / Eventsにasync delegateを渡さず、TaskからEngine APIを呼ばない。
5. `build_run` に `target: "managed"` を指定する。診断と同じ要求IDの `applied` を確認してから、最新の `script_catalog` で追加・設定・参照接続する。ビルド成功だけを適用成功と扱わない。
6. 保存・Playtestを確認する。削除時は `.cs` / `.meta` とScene/Prefabの参照を確認し、型sidecarや生成登録を手書きしない。

## 削除

- `.cs` と対応 `.meta`、参照するScene/Prefabを確認し、既存ファイルへ差分で修正する。
- [旧 GreenWare](https://github.com/HasegawaJin/GreenWare) のC++ Script資料はSDK 0.9.1の履歴記録であり、現行 Script の削除・移行手順として扱わない。

## 検証

- `build_run` に `target: "managed"` を指定し、診断と同じ要求IDの `applied` を確認する。
- `playtest` スキルでシナリオを実行し、保存・Play の振る舞いを確認する。
