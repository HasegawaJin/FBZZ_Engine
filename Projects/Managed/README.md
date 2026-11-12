<!-- @file    README.md -->
<!-- @brief   Managed Host・ゲーム SDK・決定的な検証の入口。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-04 -->
# C# スクリプト

.NET SDK 10.0.403 と x64 .NET 10 runtime を使う。SDK は Projects/Managed/global.json で固定し、Roslyn generator と compiler を揃える。ゲームコードは標準 C#、省略したメンバー修飾子は private。`Fbzz.ScriptApi` と常駐 `Fbzz.ManagedHost` は nullable enable、ゲーム csproj は nullable warnings を使う。namespace と nullable 記号は通常の作者コードで必須にしない。

参照・配列・型 GUID・名前による共通設定 Op に加え、[制作・実行経路の完成](../../Docs/design/csharp-completion.md)の追加機能を実装した。GameObject / Component 制御、Collider とゲームサービス、所有者付き Jobs / Events、複合設定と DataAsset、Play 差し替え、Inspector 統合、SDK catalog、製品への managed package を接続する。現在の API と寿命・利用条件は [作者向け契約](../../Docs/design/csharp-authoring.md)、Host・保存・世代の詳細は [ランタイム設計](../../Docs/design/csharp-scripting.md)を参照する。

## Completion の現在の検証状況（2026-10-05）

追加機能の実装と受入検証の完了を区別する。現時点で確定した結果は次の範囲。

| 対象 | 確定した結果 |
|---|---|
| managed-test | Debug / Release とも 43 groups 合格。Generator 8 件・Binding 8 件・Batch 5 件は内側の検証件数であり、43 へ加算しない |
| Editor MCP | 50 件のテストが合格 |
| C++ compile check | エラー 0・警告 0 |
| SDK / native test build | Debug / Release / Development SDK の最終再公開・配置検査がエラー 0・警告 0。全構成の runtime fingerprint が一致し、validated = true。EngineAuto / EditorAuto / PhysicsAuto の Debug ビルドもエラー 0・警告 0 |
| 最終 native 回帰（Debug） | 916 / 916 件合格。Engine 184・Editor 158・Physics 573・SharedSDKTemplates 1。終了時の crash も解消 |
| SDK 単独の managed-build | 公開 Release API と generator を参照する CSharpStage2Verification / ManagedStandaloneSmoke のコンパイルがエラー 0・警告 0 |
| 実 Editor | Release SDK / DX12 / WARP で段階 2 の 7 シナリオ 356 手順、Standalone 準備 15・製品 export 14 を合わせて 9 シナリオ計 385 手順が合格。全プロセスが終了コード 0 |
| 実測 | 100 / 1000 対象の 14 行を記録。ソース保存から Play 中の AfterReload まで 17427.691 ms、Start / AfterReload 各 1 回 |
| Standalone 準備 | PrepareStandaloneSmoke の 15 手順が合格。Editor は終了コード 0 |
| SDK 単独のソース版 Standalone | Release exe のビルドと実起動が合格。managed DLL の実ロードと Engine 操作、native の正常終了を確認し、終了コード 0 |
| 製品 export / 製品 exe | ExportStandaloneSmoke 14 / 14 が合格。書き出した製品を project 指定・SDK 環境変数なしで起動し、製品内の managed DLL の実ロードと正常終了、終了コード 0 を確認 |

SDK の最終再公開は `build/agent/build-20261005-022726-22308.log`（Debug）、`build-20261005-021924-6444.log`（Release）、`build-20261005-022908-61576.log`（Development）で合格した。最後の公開後も全構成の検証記録が有効であることを確認した。916 件の Debug 回帰は `build/agent/test-20261005-011521-66200.log` に記録した。終了時の crash は AssetProxy の要求台帳・Streamer と Runtime の event bus の寿命順を修正して解消した。

SDK 単独のコンパイル結果は `Scratch/CSharpStage2Verification/Library/ManagedBuild/agent/managed-build-20261005-012515-57584.log` と `Scratch/ManagedStandaloneSmoke/Library/ManagedBuild/agent/managed-build-20261005-012520-64584.log` に記録した。この二件はゲーム DLL のコンパイルであり、exe のビルドと起動は下記の別検証で確認した。冒頭の managed-test Debug / Release 各 43 groups、Editor MCP 50 件、Debug native 916 件は現在の最終回帰結果とする。

実 Editor の最終 report と実行ログは `Scratch/CSharpStage2Verification/` にある。Services では名前による共通設定、Undo / Redo、複合設定の保存再読込、typed services・一括生成と位置操作・filtered query、Jobs / Events、Play 中のビルド適用、設定と直接参照 cache の保持、Stop 復元を確認した。初回の `Start → Connect` と差し替えの `AfterReload → Connect` の順序に合わせ、通知は接続後の job から発火する。Transform / Physics も Play 中の適用と Start を繰り返さないことを検証する。

| 最終シナリオ | 合格手順 |
|---|---:|
| CompletionServices | 100 |
| CompletionPerformance | 13 |
| CompletionIteration | 17 |
| LifecyclePhysics | 48 |
| TransformApi | 56 |
| ObjectBatch | 38 |
| Stage2NamesAndReferences | 84 |
| 段階 2 の小計 | 356 |
| PrepareStandaloneSmoke | 15 |
| ExportStandaloneSmoke | 14 |
| 9 シナリオの合計 | 385 |

Standalone の準備は `Scratch/ManagedStandaloneSmoke/PrepareStandalone.report.json` の PrepareStandaloneSmoke が 15 / 15、終了コード 0。公開 SDK だけを参照する同プロジェクトのソース版 Standalone は、ソース側 AgentBuild の `build/agent/build-20261005-015915-39812.log` で Release exe を構築した。

その exe を `--project` と `--warp` で起動し、環境変数 `FBZZ_SDK_ROOT` を外した状態でも実 managed DLL をロードして動作することを確認した。`Scratch/ManagedStandaloneSmoke/Saved/managed-standalone-smoke.json` は proofCode = 4242、created = true、found = true、worldPosition = [7, 8, 9] と、実際にロードした `Library/ManagedScripts/Release/generation-.../ManagedStandaloneSmoke.Scripts.dll` の assemblyPath を記録する。`Saved/native-standalone-smoke.txt` の `PASS normal shutdown` と `Standalone.run.log` の shutdown、終了コード 0 も確認した。これはソース版 Standalone の検証であり、製品パッケージの書き出し・起動とは別に扱う。

製品の書き出しは `Scratch/ManagedStandaloneSmoke/ExportStandalone.report.json` の ExportStandaloneSmoke が 14 / 14、Editor の終了コード 0。共通 Op による preflight の valid / canStart、要求別の package 完了、Release 構成、exe と `Managed/game.managed.toml` の存在、outputReady を確認した。書き出し先は `Scratch/ManagedStandaloneSmoke/Builds/ManagedSmoke`、manifest の assembly は `ManagedStandaloneSmoke.Scripts.dll`、configuration は Release、schema_version は 1。

書き出した `ManagedStandaloneSmoke.exe` は製品ディレクトリを作業場所とし、`--warp` のみで起動した。`--project` と `FBZZ_SDK_ROOT` に依存せず、製品内 `Managed/ManagedStandaloneSmoke.Scripts.dll` を実ロードし、終了コード 0 で正常終了した。製品の `Saved/managed-standalone-smoke.json` は proofCode = 4242、created = true、found = true、worldPosition = [7, 8, 9]、製品内の assemblyPath と processId = 63904 を記録し、`Saved/native-standalone-smoke.txt` は `PASS normal shutdown` を記録する。ソース版の processId = 65592 と Library 内 DLL の記録とは別の実行結果。製品は framework-dependent で、実行環境に x64 .NET 10 runtime が必要。runtime 本体は同梱しない。

計測の正本は `Scratch/CSharpStage2Verification/Library/CompletionPerformance.json`。Windows x64、Release Editor / DX12 / WARP、.NET runtime 10.0.8 で、準備と経路の warmup 後に実 native bridge を呼んだ。100 / 1000 対象で各 7 操作、計 14 行を記録した。以下は 1000 対象の行で、移動は同じ個体群を cached Transform の反復と一括 API で操作する。各対象数につき scalar / batch の生成群を同時に置き、描画処理は計測区間の外とする。

| 1000 対象への操作 | 反復 | 合計 [ms] | 1 回あたり [ms] | 計測区間の managed 確保 [byte] |
|---|---:|---:|---:|---:|
| Create scalar | 1 | 8.8598 | 8.8598 | 568000 |
| CreateMany | 1 | 27.0901 | 27.0901 | 336200 |
| FindAll array | 25 | 733.1055 | 29.3242 | 7023600 |
| ReadObjects caller buffer | 25 | 351.0680 | 14.0427 | 0 |
| TranslateWorld scalar cached Transform | 25 | 7.9058 | 0.3162 | 0 |
| TranslateWorld batch | 25 | 3.2551 | 0.1302 | 0 |
| ReadWorldPositions caller buffer | 25 | 0.9143 | 0.0366 | 0 |

この測定では一括移動の時間が短く、caller buffer の検索・位置読取と両方の移動経路で managed 確保 0 byte を確認した。一方、1000 個の CreateMany は scalar 生成より遅く、managed 確保は小さかった。14 行すべての計測区間で Gen0 / Gen1 / Gen2 の collection count 増分は各 0。確保量は現在の managed thread のみを対象とし、native 確保、計測区間外の確保・GC、ゲーム全体の GC pause は含めない。C++ スクリプトとの比較、汎用の速度倍率、別の Scene・機器・描画条件での性能を示す結果ではない。

`Scratch/CSharpStage2Verification/Library/CompletionIteration.json` は、ソース保存から Play 中の AfterReload 到達まで 17427.691 ms、startCount = 1、reloadCount = 1、validMeasurement = true を記録した。監視・C# コンパイル・ロード・状態復元・commit を含む一回の実測で、コンパイル単体の時間ではない。

製品 export、製品 exe の実ロード・正常終了、全 SDK 構成の配置検証まで受入済み。以下の過去の合格記録は、その時点の成果物と確認範囲を示し、現在の追加機能全体の受入結果には含めない。配布経路と受入条件は [Completion 設計](../../Docs/design/csharp-completion.md#検証)に従う。

## 構成

| ディレクトリ | 役割 |
|---|---|
| Fbzz.ScriptApi | ゲームから参照する値型・参照・Component facade・サービス |
| Fbzz.ScriptGenerator | インストール済み Roslyn を使う compiler library。Script の宣言 metadata と共通 ABI の C# 投影を出力 |
| Fbzz.ScriptGeneratorTests | 実 Roslyn による宣言 metadata・BOM・限定 warning suppression・ABI 配置と生成呼び出しの検証 |
| Fbzz.ManagedHost | collectible ALC、型と設定 schema、同期 lifecycle、C ABI |
| Fbzz.ManagedTests | NuGet 依存のない実 Host の検証 runner |
| Fbzz.ManagedFixtures / V2 | 同じ型名で既定値を変えたロード・reload fixture |
| Fbzz.ManagedInvalidFixtures | constructor の Engine API アクセスを棄却する fixture |

検証は `Tools/AgentBuild.ps1 managed-test`。C++ と同じ排他と診断形式を使う。通常の変更には C++ の再リンクを要求せず、`managed-build <csproj> -ManagedOutputDirectory <世代ディレクトリ> -ManagedApiPath <常駐 API DLL>` で候補を構築する。

Host tests は本物の catalog、instance、callback delegate、collectible ALC を使い、native サービス境界だけを固定した bridge で検証する。protected override lifecycle、継承、保存属性、constructor、fault、候補失敗時の旧世代保持、スレッド制限、実 ALC 解放と外部参照による解放待ち上限を対象にする。SDKとGameHubが共有するAuthoring/ManagedStarterのcsprojも同じAPI/generatorでコンパイルする。native integration は `Projects/Tests/Engine/Auto/ManagedScriptRuntimeTests.cpp` の TestKit fixture で確認する。

Generator tests は managed-test に統合し、空クラス・全 partial 宣言・同じ行の partial 宣言・継承・型の除外・元の BOM を含む checksum・不正な compiler 設定・設定 field の限定警告抑制を検証する。

固定 ABI の正本は `Projects/Engine/include/Engine/Scene/ManagedBridgeContract.inl`。C++ は同じ定義を展開し、Host / API は AdditionalFiles と明示した FbzzBindingTarget により enum・定数・wire 型を生成する。位置・回転・拡縮・方向・raw key・delta time の十二の Host 呼び出しも生成し、可変長 buffer と native の処理は明示実装を使う。通常のゲーム csproj に binding の指定は不要。SDK 配置ではこの共通定義も必須として検査する。

CMake の `FBZZManagedHost` は Host / API / generator を各構成の `Managed/` へ配置し、Editor と Sandbox、SDK に同じ配置を渡す。Roslyn 本体はインストール済み SDK を参照し、NuGet 依存を追加しない。製品は framework-dependent であり、.NET runtime 本体を同梱しない。

## ビルドと実行

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZManagedHost -Preset debug
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 managed-test
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 managed-build GreenWare/GreenWare.Scripts.csproj -Preset debug -ManagedApiPath build/Debug/Binaries/Debug/Managed/Fbzz.ScriptApi.dll
~~~

ゲーム側はプロジェクト直下の `<Project>.Scripts.csproj` と `Assets/Scripts/**/*.cs` を使う。AssemblyName は csproj のファイル名から拡張子を除いた名前へ揃える。[ソースなしSDKの導入手順](../../Docs/csharp-start.md)とAuthoring/ManagedStarterを最小構成の正本とし、GameHubのC#選択も選択SDK内の同じ雛形を使う。API の配置を変える場合は managed-build の ManagedApiPath で Fbzz.ScriptApi.dll を指定し、同じディレクトリの Fbzz.ScriptGenerator.dll を Analyzer として参照する。ChecksumAlgorithm=SHA256 と CompilerVisibleProperty の MSBuildProjectDirectory も必要。

Script を継承しない通常 class / static class / struct / enum / interface も一緒にコンパイルする。追加の登録や属性は不要で、具体的な Script 派生型だけがアタッチ・catalog・自動 lifecycle の対象。helper の Update は自動実行しない。通常 helper は Script から GameObject 等を受け取り、呼び出し元の同期 callback 内で同じ Engine API を使える。[通常 C# の作者契約](../../Docs/design/csharp-authoring.md#script-を継承しない通常の-c)

Vector3.Dot / Cross / Lerp / NormalizedOr と Mathf.Clamp / Clamp01 / Lerp / ToRadians / ToDegrees は純粋な C# 計算で、constructor と通常 helper から利用できる。自作 C++ Math と同じ float32 の式と境界条件を使い、各計算で native 境界を越えない。Lerp は外挿可能で、NormalizedOr は長さが EPSILON 未満なら指定 fallback をそのまま返す。速度や全プラットフォームの bit 単位の一致を保証する機能ではない。

Quaternion の生成・合成・補間も同じ純粋計算として使える。Transform は LocalRotation / WorldRotation / LocalScale / Forward / Right / Up と Rotate / LookAt を持つ。Quaternion の生成角度は radian、Transform.Rotate は既存 C++ API に合わせて degree。Quaternion 設定の XYZW は保存 codec に含める。回転・拡縮の変更は子孫も事前に検証し、失敗時に Scene の一部を更新しない。[回転・拡縮の作者契約](../../Docs/design/csharp-authoring.md)

Objects.TranslateWorld / TryReadWorldPositions / TrySetWorldPositions で普通の GameObject[] を一回の native 呼び出しへまとめられる。読み書きには既存の Vector3[] と private な ObjectStatus[] を再利用する。入力順・重複・即時親子同期を維持し、未設定・失効はスキップ、要求全体のエラーは対象と出力を変更しない。上限は一要求 32768 スロットで、Scene の 4096 Entity 枠とは別。物理 body は FixedUpdate の RigidBody を使う。Scene の ReadObjects、CreateMany / InstantiateMany / DestroyMany も caller buffer / 一括転送を使う。[一括 API の契約と例](../../Docs/design/csharp-authoring.md#大量対象の制御)

~~~csharp
using Fbzz;

class ManagedMoverComponent : Script
{
    public float moveSpeed = 4f;

    void Update()
    {
        if (Input.GetKey(KeyCode.W))
            Transform.WorldPosition += Vector3.FORWARD * (moveSpeed * Time.DeltaSeconds);
    }
}
~~~

Editor は csproj を発見し、C# を独立した世代ディレクトリへ非同期でビルドする。生成した型は通常の Script 一覧へ登録され、Inspector、SceneSerializer、ScriptSystem を通る。Edit / Play とも callback 外の安全点で候補を適用し、ビルド・復元に失敗した候補で稼働中の世代を置き換えない。Play は Awake / Start の実行済み状態、設定、KeepOnReload、直接参照 cache を引き継ぐ。

AI は build.run の target:managed で C# のビルドを要求し、返された managedRequestId を build.status へ渡して、その要求の適用・失敗・保留・新しい要求による置換を追跡できる。script.inspect は fault 後も Host の保存 field を個別に取得し、不正値を fieldDiagnostics へ分離する。getter や lifecycle は再実行せず、最初の停止原因を保持する。[AI の制作経路](../../Docs/design/ai-script-workflow.md)

解放待ちの世代が上限に達した場合は、Edit のロード準備だけで background GC を要求し、Editor が同じ候補を間隔を空けて再確認する。同期の finalizer 待ちは行わず、外部参照が残る場合は有限回数で診断して旧世代を保つ。CLR の通常の GC pause は残る。[GC.Collect の background 指定](https://learn.microsoft.com/en-us/dotnet/api/system.gc.collect?view=net-10.0#system-gc-collect(system-int32-system-gccollectionmode-system-boolean-system-boolean))

設定 codec は bool、int、float、string、int 基底 enum、Vector3、Quaternion、GameObject、配列・List、Serializable class / struct、DataAssetRef を扱う。通常 string と一次元配列は string.Empty / [] で初期化し、GameObject の未設定は CLR null、欠損 GUID は失効参照として区別する。順序・重複・GO null slot を保持する。複合値は版付き JSON で深さ 32・32768 ノード・UTF-8 1 MiB を制限し、循環・未知キー・重複キー・非有限値を拒否する。復元候補の引数なし constructor は Engine スコープを停止して実行する。Dictionary、Task、delegate、多次元配列は保存しない。

Inspector が設定する `public GameObject target;` は初期化記号を要求しない。compiler library は対応 codec の mutable instance 設定 field に限り CS0649 を抑制する。private runtime、static、readonly、NonSerialized、未対応の保存型と nullable flow の警告は保持し、値の検証や C# のアクセス規則は変えない。

GetComponent<T>() / GetComponents<T>() は GameObject と Script の両方で使える。managed Script と登録済みの native facade を取得し、追加・削除も GameObject.AddComponent<T>() / RemoveComponent<T>() で行う。単数の managed Script は ScriptEntry 順の最初の一致、複数は同じ順の通常配列。未検出は null / 空配列、CLR null と失効は別条件。Engine 操作は現在の callback、スレッド、対象とコードの寿命を検証し、constructor・callback 外・別スレッドから使えない。純粋な計算と GameObject 寿命 ID の値比較はスコープを必要としない。

開発時のビルドはソースツリーと公開 SDK の同じ Tools/AgentBuild.ps1 を使う。SDK 単独の入口は明示したゲーム csproj の managed-build / managed-test を受理し、ログはゲーム Library に保存する。内容の fingerprint で変更を検出し、更新日時を保持した import でも再コンパイルする。native C++ Script を managed T として取得せず、両言語の通知は ScriptEventBus の managed channel と版付き JSON で接続する。

## 名前で作成し、GUID で保存する

UI / catalog の displayName は CLR クラス名。AI は script.configure.preview の settings に objects、script:クラス名、fields を渡し、返された plan を script.configure.apply に渡す。参照の入力はオブジェクト名、階層 path、root / tag / scope の selector を使える。通常の制作で型 GUID や対象 GUID の手入力を要求しない。同名型は CLR 完全名、同名対象は root / path で絞る。

内部の型保存キーは `cs:<type GUID>`。.cs.meta の `[meta] guid` はファイルの識別子で、`[[managed.types]]` の guid / clrName は型の識別子。単一宣言の型は自動発行し、同じファイルの複数型にも別 GUID を発行する。partial は全宣言元の既存 mapping が一つだけある場合に採用し、新規の複数ファイルでは明示 primary mapping を要求する。ソート順から主ファイルを推測しない。

型を改名しても同じ GUID を使うには、mapping の clrName を新しい完全名へ変更し、previousClrNames に旧名を残す。改名と新規追加を自動推測しない。削除した型の mapping は tombstone として保持し、コンパイル失敗で GUID を生成し直さない。型が無い間も Missing の保存データを残し、同じ mapping で戻した型へ復元する。

generator は concrete / nongeneric / unnested Script の全宣言位置と、BOM を含む compiler source bytes の SHA256 を assembly 属性へ出す。Editor はゲームコードを実行しない PE metadata 読み取りで discovery し、現在の source bytes と .meta の一意性を照合して `<DLL fullpath>.scripts.json` を生成する。通常ロードは全型 mapping が一致する sidecar を必須とし、不正・重複・古い候補では旧世代を保つ。未知の .meta section と既存のファイル GUID を保持し、個々の書き込みは atomic replace を使う。複数ディスクファイル全体の原子性は保証しない。

共通設定 Op は Edit 専用。preview は候補 constructor を実行する Action だが Scene を変更しない。候補個体は解放して値だけを計画へ保存し、apply の追加時に再構築して全設定を復元する。古い CLR 個体を保留 plan に保持しない。apply は全件を検証し、一件の Undo を積む。準備済み plan は 8 件、receipt は 128 件まで保持し、Scene / コード世代 / 対象を決める階層・名前・タグ・active 状態 / 変更前値が変わったら stale 診断で再計画する。apply と scene.save、任意 callback の副作用は別の結果として扱う。具体例は [AI の制作経路](../../Docs/design/ai-script-workflow.md#scriptconfigure-の共通-op)を参照する。

## 過去の検証記録（2026-10-04）

以下は各追加時点の記録。「後続」「未実施」と Play 中の適用保留は当時の実装・検証範囲を示す。現在の API・Play 差し替え契約は冒頭の設計文書、最新の確認状況は Completion 節を参照する。

Windows x64、.NET SDK 10.0.300 / runtime 10.0.8 で以下を確認した。

| 対象 | 結果 |
|---|---|
| managed-test | Debug / Release とも 12 件合格。private、設定、失敗保持、連続 reload、実 ALC 回収と pin の上限 |
| ManagedScriptRuntimeTest | Debug で実 CoreCLR との接続、更新、保存、Missing 往復、候補失敗、fault、明示 Shutdown と正常終了が合格 |
| 既存スクリプト関連 | Debug で lifecycle、profiler、要求、観測、module、transform の 57 件合格 |
| C++ compile check / EditorLauncher build | compile check と Debug / Release の Editor 全体ビルドでエラー・警告なし |
| GreenWare.Scripts.csproj | 上記 managed-build の入口でエラー・警告なし |
| Release SDK 公開 | FBZZSDK のビルドと配置検査が合格。Host / API / nethost とライセンスを SDK/0.9.1 に配置 |
| 実 Editor: ManagedAutoBuild | Release / DX12 / WARP で 18 手順合格。型と設定の検出、追加、保存再読込、移動、Stop の復元 |
| 実 Editor: ManagedReload | 同構成で 46 手順合格。Edit の差し替え、CS1002 の検出、旧コードの継続、修正後の復帰、Stop の復元 |
| 実 Editor: ManagedDeferred | 同構成で 38 手順合格。Play 中のビルド・適用保留、旧コードの継続、Stop 後の適用と再実行 |
| AgentLint / git diff --check | 変更対象 55 ファイルでエラー・警告なし。空白差分エラーなし |

実 Editor の結果は `Scratch/CSharpVerification/ManagedAutoBuild.report.json` と `ManagedReload.report.json` に記録した。移動結果は初期コードで約 6.3 m、更新後は約 12.6 m、構文エラー後も約 12.6 m、修正後は約 18.9 m。各 Stop で位置 0 へ復元した。ManagedReload のエラーログ 1 件は意図した CS1002 で、シナリオと Editor の終了コードは 0。更新日時を保持した import に対する古い MSBuild 成果物の再利用も修正し、同じシナリオで確認した。

`Scratch/CSharpVerification/ManagedDeferred.report.json` では Play 中の新コード適用を保留し、旧コードが約 18.9 m 移動することを確認した。Stop で位置を復元して新コードを適用し、再実行では約 12.6 m 移動した。3 シナリオ計 102 手順が合格し、各 Editor は終了コード 0 で正常終了した。

この表は当時の段階 1 の受入結果。段階 2 の参照・配列・型 GUID・共通設定 Op の結果は以下。SDK のない PC への製品配布と性能計測は、この時点では未実施だった。

2026-10-04 に段階 2 の受入検証を完了した。確認範囲は以下で、後続 API を含む全段階の完了ではない。

| 段階 2 の検証 | 確認状況 |
|---|---|
| managed-test | Debug / Release とも 23 項目と Generator 内部 6 項目が合格。ビルド警告なし。通常 helper の catalog 除外・呼び出し元と対象の保持・callback 外拒否を含む |
| native / Editor 回帰 | Script / Prefab 参照 / SceneSerializer / EditorBus / 新規保存先等の 256 件が合格 |
| 型 metadata | 上記に含む 12 件。複数型の別 GUID、明示改名と削除後の復帰、partial 主宣言、BOM、古い checksum / 不正 .meta / sidecar 保護 |
| 実 CoreCLR の接続 | 上記に含む 4 件。参照と配列の保存、未ロード保存、RequiredReference、GetComponent、対象・Script・Scene の寿命失効、Prefab 生成時の GUID リマップ |
| 共通設定 Op | AI 入力解析、名前・root / path、1200 件の対象選択、inactive 同名候補、一括設定の失敗保持、Undo / Redo、再送と stale 拒否が合格 |
| GreenWare.Scripts.csproj | Release の managed-build がエラー・警告なし |
| Release SDK / 実 Editor | SDK 公開はエラー・警告なし。Release SDK / DX12 / WARP の 84 手順が、未作成の二つの scene 保存先を含めて合格、終了コード 0 |

回帰結果は `build/agent/test-20261004-164454-12544.log`。managed は `managed-test-20261004-163620-36288.log` / `managed-test-20261004-163744-70380.log`、Release SDK は `build-20261004-163243-10412.log`。型の改名・削除復帰は metadata importer の検証、配列編集と Undo は共通 Op の TestKit 検証であり、実 Editor の 84 手順でその全条件を確認した結果とは扱わない。

実 Editor の `Scratch/CSharpStage2Verification/Stage2NamesAndReferences.report.json` は 84 手順合格、終了コード 0。名前による作成、compiler metadata と型 GUID、配列の既定値、保存済み参照の null / 欠損 / 重複、130 段の名前検索、GetComponent の実行、新規保存・再読込・Stop 復元を確認した。隔離環境では未作成ファイルへの weakly_canonical が Access denied を返したが、同じ SDK / シナリオを通常権限で実行すると新規保存も成功した。パスの保護を緩める回避は追加していない。実行時警告は検証用プロジェクトの既定入力・未使用 native DLL・描画設定の 4 件で、C# の実行エラーはない。

### 数学 API と共通接続定義の追加検証

同日に Vector3 / Mathf の純粋計算と、共通 X macro による固定 ABI・五つの単純 marshaller の生成を追加した。この時点では Quaternion・一括 API・Physics / Audio 等の拡充を後続としていた。

| 追加後の検証 | 結果 |
|---|---|
| managed-test | Debug / Release 各 24 項目、Generator 内部 7 項目、その中の BindingGenerator 8 項目が合格。エラー・警告なし |
| binding 契約 | Request / Reply の全 33 field offset とサイズ、全数値 ID、五つの呼び出しの引数と一回の dispatch を検証。欠損・未知の定義・重複・未閉鎖・型不一致・8進表記を拒否 |
| C++ compile / test build | 追加した数学・生成接続テストと native runtime の compile check、EngineAuto / EditorAuto の Debug ビルドがエラー・警告なし |
| native / Editor 回帰 | 260 件合格。実 CoreCLR の数学と C++ Math を比較する 3 件、生成五経路の位置・即時子孫同期・キー入力・時間を通す 1 件を追加 |
| Release SDK / 実 Editor | SDK ビルドと共通定義を含む配置検査がエラー・警告なし。更新した Release SDK / DX12 / WARP で既存の 84 手順が再び合格、終了コード 0 |
| GreenWare のゲームコード | 公開済み SDK/0.9.1 の API と generator を参照する Release managed-build がエラー・警告なし |

managed は `build/agent/managed-test-20261004-171408-10020.log` / `managed-test-20261004-171603-39608.log`、260 件の回帰は `test-20261004-172004-61672.log` に記録した。純粋計算では外挿、正規化の EPSILON 前後、角度、定数、Vector3 の配置、constructor での通常 helper 呼び出しを照合する。生成した接続は実 native dispatch まで通し、Local / World の区別、書き込み直後の子孫同期、キーの押下・保持・解放と delta time を確認した。

更新した SDK の配置結果は `build/agent/build-20261004-172056-39372.log`。実 Editor の `Scratch/CSharpStage2Verification/Stage2NamesAndReferences.report.json` は再実行後も 84 / 84 で合格し、名前設定・参照・配列・130 段の検索・保存再読込と Stop 復元を維持した。実行時には検証プロジェクトの既定入力・未使用 native DLL と toolchain・描画設定の警告が 4 件あり、C# の実行エラーはない。AgentLint は変更 120 ファイルでエラー・警告なし、git diff --check は空白エラーなし。

GreenWare の配布済み API に対する結果は `build/agent/managed-build-20261004-172446-9540.log` に記録した。

### 一括ワールド位置 API の追加検証

同日に Objects の一括読み取り・位置設定・共通移動を実装した。共通 ABI は version 1 / Request 104 byte / Reply 72 byte を保ち、操作 115–117 と状態 enum、転送 stride の追加に合わせて schema hash を 0x46425A5A4D414E04 に更新した。この時点では一括検索・生成・破棄、物理 body の操作は後続だった。

| 追加後の検証 | 結果 |
|---|---|
| managed-test | Debug / Release 各 26 項目、Generator 内部 7 項目と BindingGenerator 8 項目が合格。エラー・警告なし |
| SDK / Host の一括契約 | 通常配列・caller scope・失効したサービス・空配列・上限・欠損・重複・余剰バッファ・失敗保持・一回の dispatch を検証。native reply の後半スロットが不正でも caller 出力は全て未変更 |
| Host の転送確保 | 固定した native callback に対する容量確保後の反復で、転送経路の managed 確保 0 byte を確認。実ゲーム全体の確保量・native 時間・速度改善の計測ではない |
| C++ compile / test build | kernel・runtime・共通 Transform 数式・追加回帰の compile check と EngineAuto / EditorAuto の Debug ビルドがエラー・警告なし |
| native / Editor 回帰 | Transform 関連も含む 318 件合格。一括操作の実 CoreCLR 7 件と直接 kernel 4 件を含む |
| Release SDK / ゲームコード | SDK 更新と配置検査、GreenWare と実 Editor 用ゲームの Release managed-build がエラー・警告なし |
| 実 Editor | 更新した SDK / Release / DX12 / WARP で一括操作 38 手順と既存の参照・名前 84 手順が合格。両 Editor は終了コード 0 |

managed は `build/agent/managed-test-20261004-175809-74980.log` / `managed-test-20261004-180054-77340.log`、回帰は `test-20261004-181810-49232.log` に記録した。通常位置操作と同じ親子・重複の順序、描画 world キャッシュの読み取り、古いキャッシュの設定時修復、未使用スロットの非有限値、inactive と body の条件、別 Scene と失効 token、32768 / 32769 入力を照合した。後半の数値不正・重複移動の overflow・ゼロスケールで隠れる差分 / 逆回転 overflow・退化 quaternion は、Scene と出力を変更せず拒否する。

SDK は `build/agent/build-20261004-181911-66960.log`、ゲームコードは `managed-build-20261004-182253-63884.log` / `managed-build-20261004-182309-33496.log`。実 Editor の `Scratch/CSharpStage2Verification/ObjectBatch.report.json` は 38 / 38、`Stage2NamesAndReferences.report.json` は 84 / 84 で合格した。一括操作では未設定・重複を含む配列の移動、読み取り・設定、余剰要素の保持、後半の不正位置で全対象・状態が未変更になること、保存再読込と Stop 復元を確認した。既存の 130 段検索・名前接続・欠損 GUID の往復も維持した。Console にある警告 4 件は検証プロジェクトの既定入力・未使用 native DLL と toolchain・描画設定で、C# の実行エラーはない。

### 回転・拡縮と AI 制作経路の追加検証

同日に Quaternion の純計算、Transform の回転・拡縮・方向、要求別の C# ビルド状態、fault 後の field 観測を追加した。固定 ABI のサイズと version は維持し、操作 118–124 の追加に合わせて schema hash を 0x46425A5A4D414E05 に更新した。生成する単純 Host 呼び出しは十二経路となった。この時点では Quaternion の設定保存、native Component、生成・破棄、Physics 等の API は後続だった。

| 追加後の検証 | 結果 |
|---|---|
| managed-test | Debug / Release 各 28 項目、Generator 内部 7 項目、BindingGenerator 8 項目、一括 bridge 5 項目が合格。エラー・警告なし |
| native / Editor 回帰 | 93 件合格。Quaternion の実 CoreCLR / C++ Math 比較、Transform の親子同期と失敗保持、cached 参照失効、field 観測、要求履歴・実 Op 接続、typed capture と型名保持を含む |
| Editor MCP | TypeScript build と 48 件の tests が合格。managed 対象と managedRequestId の入力検証・転送を含む |
| C++ compile / Debug build | 変更 cpp の compile check、EngineAuto / EditorAuto のビルドがエラー・警告なし |
| Release SDK / ゲームコード | SDK 更新と配置検査、公開した API を明示した GreenWare、実 Editor 用ゲームの managed-build がエラー・警告なし |
| 実 Editor | 更新した SDK / Release / DX12 / WARP で回転・制作経路 49 手順、一括操作 38 手順、参照・名前 84 手順が合格 |

managed の結果は `build/agent/managed-test-20261004-190447-63928.log` / `managed-test-20261004-192647-64472.log`、93 件は `test-20261004-193853-37964.log`。Debug のビルドは `build-20261004-191608-2936.log` / `build-20261004-192957-22248.log` に記録した。前者は呼出し側の待機上限を超えた後も処理を継続し、正常に両 target を出力した。後者は型名保持の追加修正後の EditorAuto が RESULT ok。

回転・拡縮では inactive 子孫、非単位 quaternion、負・零スケール、描画用 world cache、零 axis の no-op、子孫 overflow、物理 body の拒否を照合する。fault の観測はユーザー getter と Update を再実行せず、正常 field と最初の停止原因を保持し、不正値だけを個別診断へ分離する。型改名の候補準備では承認済み factory の表示名を保持し、未登録の Missing 型だけ metadata から名前を補完する。

SDK の結果は `build/agent/build-20261004-194126-69864.log`、GreenWare は `managed-build-20261004-194755-50940.log`、検証ゲームは `managed-build-20261004-194736-51068.log`。最初の SDK リンクで LNK1114 / error 5 により既存生成ライブラリを上書きできなかったが、属性と競合プロセスを確認した後の一回の再試行で成功した。ソース変更や生成物の削除は行っていない。

実 Editor の結果は `Scratch/CSharpStage2Verification/TransformApi.report.json` が 49 / 49、`ObjectBatch.report.json` が 38 / 38、`Stage2NamesAndReferences.report.json` が 84 / 84。TransformApi は要求 ID と実適用世代、名前からの GameObject 接続、Undo / Redo、保存再読込、回転・拡縮、fault 後の NaN の個別診断、Play 中の deferred と Stop 後の世代更新・保存設定復元を確認した。typed capture で返された plan / scriptId / 要求 ID を後続要求へ渡し、固定した GUID や ID をシナリオへ埋め込まない。fault の一件は POSE_INSPECTION_FAULT を意図して発生させた。隔離実行は新規 scene 保存のパス確認で拒否されたため、同じ SDK / シナリオを通常権限で実行して保存も確認した。パスの保護を緩める変更は加えていない。

AgentLint は当時の Managed の全 46 source / project / README と変更した native / Editor / MCP / 設計文書でエラー・警告なし。git diff --check は空白エラーなし。この時点では実ゲームでの時間・GC pause・大量対象の性能測定と、段階 3 の残りの API は未実施だった。

### 生成・破棄と Physics の追加

以下も同日の追加時点の記録。現在は一括生成・登録済み native facade の追加等を含み、機能範囲と利用条件は [作者向け契約](../../Docs/design/csharp-authoring.md)に従う。

`Scene.Create(name)` と `Scene.Instantiate(prefab)` が通常の GameObject 参照を返す。ワールド位置と回転を渡す overload、容量の `Scene.CanCreate(count)`、遅延秒を指定する `GameObject.Destroy` / `Scene.Destroy` / `Scene.DestroyMany` を追加した。Prefab は single-root とし、子孫全件の容量・姿勢を確認し、既存の内部 GUID 参照復元を使う。破棄は既存 Cleanup で確定する。欠損・失効・未設定・重複を含む DestroyMany は、有効な固有対象を一度ずつ受理する。

`GetComponent<RigidBody>()` で現在の native body を取得し、速度・角速度・質量・重力倍率・静的状態・sleep・固定軸・力・インパルス・トルク・Teleport を操作する。変更は Play の FixedUpdate のみ。Body を除去・再追加すると同じ GameObject の現在の body を参照し、GameObject の破棄後は古い参照を復活させない。

`Physics.Raycast` / `SphereCast` / `RaycastAll` / `OverlapSphere` とワールド重力・レイヤー行列の読取を、現 Scene の active / enabled Collider に限定して接続した。`OnCollisionEnter` / `Stay` / `Exit(Collision)` と `OnTriggerEnter` / `Stay` / `Exit(GameObject)` は private の通常メソッドとして記述できる。通知は既存 PhysicsSystem の順序と値を使い、失敗を受信 Script に帰属させる。

物理検索は World に登録・同期済みの Collider を読む。生成・Transform 変更・Teleport 直後の検索へ自動同期せず、次の PhysicsSystem の同期・step で反映する。生成・破棄・Physics は Play の有効な Live callback で使い、OnDestroy・検証・シリアライズ・復元の callback では Engine API の読取・書込を拒否する。

この時点の固定 ABI の schema hash は `0x46425A5A4D414E06`。request 104 byte / reply 72 byte と version 1 を維持し、SDK と Host を同時に更新した。生成・変更をバッファ不足で再送せず、検索の読み取りだけ必要容量に合わせて再試行する。生成中は外側の managed scope を停止し、constructor・設定復元へ権限を漏らさない。

当時は native Component の C# 追加と一括 Prefab 生成を後続とし、Kinematic モードを対象外とした。既存 native に Kinematic モードはなく、SphereCast は形状近似を使う。この時点では実行時の大量対象の速度・確保・GC は未計測だった。

#### 生成・破棄と Physics 追加時の検証

| 検証 | 結果 |
|---|---|
| managed-test | Debug / Release 各 36 項目、Generator 内部 7 項目、BindingGenerator 8 項目、一括 bridge 5 項目が合格。エラー・警告なし |
| native / Editor / Physics 回帰 | 872 件合格（Engine 155 / Editor 144 / Physics 573）。Prefab の失敗復元、参照の寿命、一括破棄、有限値と overflow、親子姿勢、剛体の除去・再追加、物理 query、6 通知と例外の分離を含む |
| C++ compile / Debug build | 変更 cpp の compile check と EngineAuto / EditorAuto / PhysicsAuto のビルドがエラー・警告なし |
| Release SDK / ゲームコード | SDK の更新・配置検査、公開した API に対する GreenWare と検証ゲームの managed-build がエラー・警告なし |
| 実 Editor | 更新した SDK / Release / DX12 / WARP で生成・破棄と物理 33 手順、回転・制作経路 49 手順、一括操作 38 手順、参照・名前 84 手順が合格。4 プロセスとも終了コード 0 |

managed の結果は `build/agent/managed-test-20261004-213122-29900.log` / `managed-test-20261004-213230-63152.log`、872 件は `test-20261004-212558-3688.log`。Debug のビルドは `build-20261004-211221-57600.log` / `build-20261004-212437-27852.log`、Release SDK の最終配置は `build-20261004-213303-9944.log`。GreenWare は `managed-build-20261004-212933-70996.log`、検証ゲームは `managed-build-20261004-214731-75412.log` に記録した。DestroyMany は nullable 要素を契約へ反映し、`Scene.DestroyMany([first, first, second, null])` も警告なしでコンパイルできる。

実 Editor の `Scratch/CSharpStage2Verification/LifecyclePhysics.report.json` は 33 / 33、`TransformApi.report.json` は 49 / 49、`ObjectBatch.report.json` は 38 / 38、`Stage2NamesAndReferences.report.json` は 84 / 84。LifecyclePhysics は要求 ID ごとのビルド適用を待ち、生成と遅延破棄、一括破棄の重複・未設定スキップ、剛体の質量・力・インパルス・固定軸、ワールド重力・レイヤー・Raycast、保存再読込と Stop の復元を値の比較で確認した。

静的 Collider の同期更新でも接触 cache を無効化し、剛体のない Trigger の移動で Enter / Stay / Exit が更新されることを native と managed の両経路で確認した。形状を毎フレーム同期する静止シーンも接触を再評価する保守的な修正であり、このコストは未計測。同期更新なしの既存 early-out は維持する。

詳細な契約と段階は [C# ランタイム設計](../../Docs/design/csharp-scripting.md)、[作者向け API](../../Docs/design/csharp-authoring.md)を参照する。
