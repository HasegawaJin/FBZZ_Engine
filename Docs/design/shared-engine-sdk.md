<!-- @file    shared-engine-sdk.md -->
<!-- @brief   SDK を正本にした Editor の起動とエンジン更新の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-03 -->
# 共有 Engine SDK と起動先

- 方針: SDK を正本にして起動先と更新を統一する。2026-10-03 にユーザーが選択した。
- 状態: 実装済み。Debug / Release の実公開と SDK Editor の Play / Stop を検証済み。

## 正本と構成

`SDK/<PROJECT_VERSION>/` を利用側の正本とする。Debug、Development、Release は同じ SDK の構成であり、Editor とゲーム DLL は同じ構成を使う。公開済みとは、schema 2 manifest の構成が `validated=true` で共通 runtime fingerprint と一致し、必要なファイルと SHA256 の検査を通過した状態をいう。

`build/<Preset>/` は CMake cache、オブジェクト、リンクとテストの作業場所として残す。そこにある Editor は公開元であり、通常の Editor 起動先にはしない。二重保存の削除や出力先の移動は今回の範囲に含めない。

## ビルドと公開

Editor のビルド入口は同じ構成の `FBZZSDK` とする。SDK target が Engine、各 DLL、EditorLauncher をビルドし、install、配置、検証を順に実行する。VS Code の Build Editor はこの target を使い、Build All は通常の全体ビルド成功後に同じ preset の SDK を直列公開する。テスト、ベンチ、compile check だけの入口へ SDK 公開は追加しない。

SDK Editor の DLL は `SDK/bin/<Config>/` に install 済みの一式から同期する。source Editor の POST_BUILD が実行されたかどうかに依存しない。Engine、Math、Physics、Fluid、Core、Graphics と runtime DLL は公開 SDK の同構成と一致させ、manifest と validator でも確認する。Editor 内部だけの変更でも Editor EXE を公開する。失敗した公開は有効な構成として扱わない。

公開開始時は共通ファイルの更新中に古い構成を使えないよう、全構成を一時的に無効化する。以前の成功記録は内部候補へ保存し、公開と検証が成功した後、共有ファイルの SHA256 と必須 DLL の一致を再検査できた構成だけを有効へ戻す。publisher・validator も共通契約の fingerprint に含め、同じ版名でも古い配置契約を有効として残さない。

ビルドと公開は一度に一つ。起動中の Editor が DLL を保持する場合は作業を保存して通常のビルドタスクで停止・再起動する。別 preset、強制終了や build の削除で競合を回避しない。

## 起動

VS Code と GameHub は `SDK/<version>/tools/<Config>/Editor/FBZZEditor.exe` を使う。`FBZZ_SDK_ROOT` と EngineAssets は同じ SDK を指す。SDK の構成・版がない場合は公開を案内して停止し、build 出力や別構成へ自動で切り替えない。

VS Code の起動前タスクは同構成の SDK を公開する。現在の SDK 版は CMake `PROJECT_VERSION` と起動設定で一致させ、版更新時は両方を更新する。通常起動では `FBZZ_SKIP_ENGINE_REBUILD=1` を設定し、source tree の部分的な再ビルドによる更新を使わない。

GameHub に保存されている Editor 実行ファイルの手動指定は互換設定として保持し、通常起動は SDK と構成から求めた正規 Editor パスを使う。SDK store とプロジェクトが指定する版の解決は維持する。

初回の GameHub 構成は、そのパッケージをビルドした構成に合わせる。ユーザーが保存した有効な構成選択は維持する。SDK の一覧探索では配置と manifest の一致を調べ、実際の起動境界では Engine DLL 一式の実ファイル SHA256 を同構成の記録と照合する。別フォルダーの Editor や変更された DLL は起動せず、同構成の SDK 公開を案内する。

ゲームスクリプトのホットリロードは consumer の Build と SDK 参照を使い、ゲーム DLL を更新する。Engine の更新は SDK 公開の役割とし、起動中に SDK を部分上書きしない。

## 検証

公開元 Editor に古い Engine DLL を残した fixture でも、SDK Editor が install 済み DLL と一致することを確認した。SDK Editor の DLL だけを欠落・変更した場合は検証で拒否する。VS Code の各 Editor 起動が同構成の SDK と公開タスクを使うこと、Build All の公開順と失敗伝播、GameHub が別 Editor override や別構成へ移らないことも確認した。

| 対象 | 結果 |
|---|---|
| Stage / Validate | `SharedSDKTemplates` 合格。DX12 有効・無効の古い DLL 同期、必須 DLL と Agility / DXC の欠落・改ざんなど 49 件の拒否、他構成の保持と共通契約変更時の無効化を確認 |
| VS Code / VcBuild | JSONC と PowerShell 構文が合格。全 Editor 起動の構成・版・公開タスクが一致。14 件の mock で Build All の順序、失敗の伝播、明示 target と coverage の除外を確認 |
| GameHub | TypeScript 型検査と本番関数を使う Node fixture 126 件が合格。3 構成の初回選択、既存設定と版 pin、手動 Editor 指定、不完全な SDK、8 DLL の実ファイル SHA256 不一致の拒否を確認 |
| 実 SDK 公開 | Debug / Release とも `FBZZSDK` がエラー・警告 0。Debug 79 行、Release 77 行の公開ファイル SHA256 と 8 DLL ペアを本番契約で照合し、Release 公開後も Debug が有効なことを確認 |
| 実 SDK Editor | Debug 366 frames / Release 1285 frames、各 8 / 8 steps と終了コード 0。Play、90 frames 後のシーン、Stop 復帰と、8 DLL・Core・DXC の実ロード元が同構成 SDK 内であることを確認。既存基準画像がないため画像比較は skip |

記録は `build/agent/test-20261003-194245-49756.log`、`build/agent/build-20261003-194327-43736.log`、`build/agent/build-20261003-194455-6216.log`、`Scratch/VcBuildCanonicalAcceptanceReport.json`、`Scratch/GameHubCanonicalAcceptanceReport.json`、`Scratch/CanonicalSDKValidationReport.json`、`Scratch/CanonicalSDKEditor{Debug,Release}.{Report,Modules}.json` と同名 stdout log に残した。mock と fixture は Electron UI の実起動を含めない。Development の実 SDK 再公開と Editor 起動は未確認。

Agility、DXC と構成の配布契約は [dx12-agility-sdk.md](dx12-agility-sdk.md)、ビルドの入口は [ai-verification-loop.md](ai-verification-loop.md) に従う。
