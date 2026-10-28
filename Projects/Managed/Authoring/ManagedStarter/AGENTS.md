# C#ゲームコードの作成

このプロジェクトは公開FBZZ SDKだけでC#をビルドできます。`Fbzz.Sdk.props` または `FBZZ_SDK_ROOT` が指すSDKの `share/fbzz/Docs/csharp-start.md` を入口に、同じDocsの `design/csharp-authoring.md` と `design/ai-script-workflow.md` を確認してください。エンジン内部ソースは必須ではありません。

- `Assets/Scripts/**/*.cs` に `using Fbzz;` と `Script` 派生classを書きます。namespace、登録マクロ、手書き型GUIDや登録リストは不要です。
- callbackは基底の `protected virtual` を `protected override` で実装します。`OnUpdate`等のnative名を模倣せず、IDEのoverride補完または `script_api_catalog` の `lifecycleCallbacks` を使います。
- 設定は対応型のpublic field、内部状態はprivate fieldです。GameObject参照の未設定・失効を確認し、APIの単位・座標・実行権限は現在のSDK catalogから読みます。
- Engine APIは同期callbackの現在スレッドから呼びます。物理変更はFixedUpdateです。Jobs / Eventsへasync delegateを渡さず、TaskにEngine実行権限を持ち越しません。
- Editor MCPの `build_run` は `target: "managed"` を指定します。診断と要求IDの `applied` を確認してから、現在の `script_catalog` で設定・参照を接続し、保存とPlaytestを行います。
- C++ Scriptは提供しません。重い一括処理が必要なら、測定結果に基づいてEngineの公開C# APIを追加します。
