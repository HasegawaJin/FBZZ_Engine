<!-- @file    csharp-start.md -->
<!-- @brief   Source-free C# authoring with a published FBZZ SDK. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-05 -->

# SDKからC#を始める

エンジン内部ソースを用意する必要はありません。.NET SDKは同梱された `Projects/Managed/global.json` の版をインストールしてください。ゲーム実行にはx64 .NET 10 runtimeが必要です。製品パッケージはframework-dependentで、runtime本体を同梱しません。

GameHubでSDKを選択して新規作成すると、ゲームテンプレートに `<Project>.Scripts.csproj` と `MoveForwardComponent.cs` を追加します。ゲーム作者向けスクリプトは C# です。C#のサンプルは自動でシーンにアタッチされません。EditorでGameObjectに追加し、Playで動作を確認してください。

既存プロジェクトには、SDKの `share/fbzz/managed/ManagedStarter/` の中身をプロジェクト直下へコピーし、`Starter.Scripts.csproj` を `<Project>.Scripts.csproj` に改名してください。既存ファイルを上書きせず、ルートの `*.Scripts.csproj` は一つにします。`FBZZ_SDK_ROOT` に選択SDKのルートを設定するとIDEにも同じ公開APIとAnalyzerが見えます。GameHubで作成したプロジェクトでは `Fbzz.Sdk.props` が選択SDKを記録します。SDKを移動した場合は、この設定を更新するか環境変数/明示プロパティで上書きしてください。

SDKルートから、ソースなしでコンパイルできます。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 managed-build C:/Games/MyGame/MyGame.Scripts.csproj -Preset development -ManagedApiPath bin/Development/Managed/Fbzz.ScriptApi.dll
```

IDEからのビルドは `dotnet build <Project>.Scripts.csproj -p:FbzzSdkRoot=<SDKルート> -p:FbzzSdkConfiguration=Development` でも可能です。Editorに適用する場合はEditorのmanaged buildを使い、完了に加えて要求IDの `applied` を確認してください。

## C# のデバッグ

SDKスターターは `.vscode/launch.json` にCoreCLR attach設定を含みます。VS Codeでゲームプロジェクトを開き、Microsoft C#拡張を用意してから、Editorを通常どおり起動し、実行とデバッグから `Attach to FBZZ Editor (CoreCLR)` を選んでください。設定は実行中プロセスへ接続するだけで、Editorの起動やスクリプト実行を行いません。Microsoft C#拡張はデバッガに必要です。C# Dev Kitは拡張機能の選択肢で、雛形の導入やEditorへの自動インストールは行いません。[VS CodeのC#デバッグとattach](https://code.visualstudio.com/docs/csharp/debugging) と [launch.json設定](https://code.visualstudio.com/docs/csharp/debugger-settings) を参照してください。

停止位置とローカル変数を使うには、Editor実行中のC# DLLに対応するPDBと同じソースが必要です。VS CodeではDebug構成のEditorを選び、Visual StudioではDebug > Attach to ProcessからEditorを選択して、管理コードの種類を確認してください。[Visual Studioのattachとシンボル条件](https://learn.microsoft.com/en-us/visualstudio/debugger/attach-to-running-processes-with-the-visual-studio-debugger?view=visualstudio) を参照してください。

EditorでC#のビルド要求が `applied` になってから接続します。ソース変更後は新しい要求の `applied` を確認し、その世代のDLL・PDB・ソースに対応する停止位置を使います。2026-10-06に公開Debug SDKとVS CodeのMicrosoft C#拡張で、実接続、ソースのブレークポイント、ローカル変数・Watch、接続中の世代交換、切断後の再接続を実 Editor 上で確認しました。Visual Studioの実接続はこの検証には含みません。

ゲームコードは `Assets/Scripts/**/*.cs` に置きます。`using Fbzz;` と `class XxxComponent : Script` だけで登録され、namespaceや登録マクロは不要です。設定はpublic field、実行中だけの値はprivate fieldを使います。ライフサイクルは同期の `protected override void Update()` / `protected override void FixedUpdate()` などで実装します。物理変更は `FixedUpdate`、Engine APIは現在のcallbackのスレッドから呼びます。

- [公開API・単位・座標・lifecycle・保存・リロードの規約](design/csharp-authoring.md)
- [AIのAPI発見→ソース同期→設定→Playtest手順](design/ai-script-workflow.md)
- [実装と配布条件](design/csharp-scripting.md)

APIの署名は配布SDKの `Fbzz.ScriptApi.dll`、IDE説明は隣のXMLが正本です。Editor MCPの `script_api_catalog` は実APIから型・overload・nullability・単位・座標・許可callbackを取得します。同じカタログの `lifecycleCallbacks` / `lifecycleAuthoring` は共通契約から生成したcallback名・署名・実行権限です。IDEのScript override補完と合わせて利用してください。`script_catalog` はゲーム側の設定schemaです。AIは最初にSDKカタログを確認し、C#の検証には `build_run` の `target: "managed"` を指定してください。エンジン内部ソースへのリンクは実装者向けで、ゲームauthoringの必須入力ではありません。

この雛形とサンプルはSDK/GameHubが共通ソースから配布し、managed-testのビルド時にも同じcsprojをコンパイルして検証します。独立した手書きAPI一覧は持ちません。
