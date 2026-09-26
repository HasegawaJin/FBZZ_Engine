<!-- @file asset-streaming.md -->
<!-- @brief アセットの非同期読み込みと常駐量制御の設計案。 -->
<!-- @author Hasegawa Jin -->
<!-- @date 2026-09-19 -->

# アセットの非同期読み込み・ストリーミング

- 状態: 段階 A〜D と配布・描画呼び出し元への接続を実装 (2026-09-21)。

## 実装状況

| 設計の要素 | 実装 | 備考 |
|---|---|---|
| 要求台帳・`AssetLease<T>`・重複集約・完了キュー | `Engine/Asset/AssetStreaming.hpp` の `AssetStreamer` | ジョブ実行関数を注入できる。本番は `AssetStreamer::Engine()` (TaskSystem) |
| 型別の経路 | `IAssetStreamChannel` / `AssetStreamChannel<T>` | `IAssetImporter` は変えず、Decode / BeginUpload / Publish を経路側に分けた |
| 世代照合 | projectEpoch・attempt・contentRevision・deviceEpoch | slotGeneration はハンドルの gen。`AssetStore::Clear` は世代を床へ畳み旧ハンドルを蘇らせない |
| 依存 | 必須 / 任意、循環は経路付きで失敗 | Material がテクスチャを任意依存として宣言する |
| 経路 | Texture / Model / ModelAsset / MaterialAsset | Model は公開中の差し替えをしない (MeshRenderer が `Mesh*` を持つため)。.mesh と DCC 直読みは同期 importer へ倒す |
| Texture の CPU 展開 | `renderer::DecodeTextureFileRGBA8` | 同期経路 (DX12Texture::Init) も同じ関数を通し、画素を一致させる |
| GPU 転送 | `ResourceManager::BeginTextureUpload` / `IsUploadComplete` / `PublishTexture` / `ReplaceTextureContents` | 品質変更はハンドルを保ったまま中身を差し替え、旧実体と bindless 枠はフェンス付きで回収 |
| 専用コピーキュー | `DX12Context` の COPY キューと専用フェンス | 転送先は COMMON で作り、コピー後の COMMON → PIXEL_SHADER_RESOURCE はフレーム冒頭で描画キューに記録。作れない機械・`FBZZ_COPY_QUEUE=0` では描画キューへ倒す。転送先は PendingUpload が持ち、コピー完了前に破棄されても消えない |
| チャンク索引・部分 I/O | `Engine/Format/FzChunkFormat.hpp` (.fztc: 索引 = offset・格納サイズ・展開サイズ・CRC32) と `ChunkFileReader` | テクスチャは品質段ごとのチャンクを `<Project>/Library/StreamCache` に持ち、要る段だけ読む。元画像の大きさ・更新時刻・展開規則の版が変われば捨てる。書き込みは一時ファイル → 置き換え |
| モデルの部分 I/O | `ModelAssetImporter::ImportPartial` | .fzasset は submesh ヘッダーから本体の大きさが決まるので、索引無しで高品質 LOD の本体をシークで飛ばす (形式の版は上げない) |
| 品質の自動選択 | `StreamedTextureResolver::SetAutoQuality`・`EstimateScreenPixels` | マテリアルを描く物体の投影直径から品質段を決める (テクスチャ長辺 / 直径 の log2 − 1)。既定は無効 |
| フレーム境界 | `Application::Run(IModule&)` が OnUpdate の前に `Pump()` | レンダラーのフレーム外 |
| 同期 `Load<T>` との共存 | 同期 Load が配ったスロットは固定 (`AssetStore::Pin`) し自動解放しない | 予約中スロットを同期 Load が埋めたら非同期結果を棄却して計数・警告 |
| 常駐予算・LRU 解放 | `Config::residentBudgetBytes` / `evictionGracePumps` | 既定は予算も猶予も 0 (解放しない)。解放した物の再利用が同期ロードの引っかかりになるため、プロジェクトが `Configure` で明示的に有効にする |
| 先読みの延期・品質の上乗せ | 予算超過中は Prefetch / Background を投入しない。超過が続けば全体の品質段を 1 段ずつ落とす | 固定物だけで超えたら新しいキーを `BudgetExceeded` で断る |
| 品質段 (部分常駐) | `AssetLease::SetDesiredQuality`・ヒステリシス `qualityHoldPumps` | Texture は上位 mip を落とす (1 段の画像は縮小)。ModelAsset は高品質 LOD のメッシュを持たない (`FirstResidentLod`)。スプライト UV は `TextureAsset::sourceWidth` で換算する |
| 描画呼び出し元の移行 | マテリアル・デカール・カスタムポスト・ライトクッキー・スプライト・UI・地形・粒子・トレイル・Fiber マスク・コースティクス・IBL 設定を `StreamedTextureResolver` 経由に | 毎フレーム利用権を更新し、未使用時は猶予後に手放す。UI の UV は元画像寸法を使用。`.ibl` は descriptor が 4 枚の DDS の利用権を保持する |
| シーン単位の利用権 | `SceneManager::PrefetchScene` / `GetScenePrefetchProgress`、遷移成功で旧シーンの束だけ手放す | シーン本文から参照を拾う (`CollectSceneAssetReferences`)。Play 停止で束を捨てる |
| スクリプト | `assets` プロキシ (`ScriptAssetProxy`) | 不透明な要求 ID。スクリプトの破棄でまとめて手放す。ID はシリアライズしない |
| 配布キャッシュ | BuildPipeline のコピー先で `<画像>.fztc` を事前生成し、同期・非同期どちらも優先利用する | コピー先のサイズ・更新時刻で世代を記録。元画像と隣接するので配布フォルダーの移動でキーが変わらない。古い・壊れたキャッシュは元画像へ戻る |
| スキンドメッシュの品質 | Animator の姿勢を含む `RenderObject` の境界から投影直径を推定 | 静的メッシュは submesh 境界。不明なら最高品質。正射影・カメラが球内にある場合にも対応 |

### 配布・保持型呼び出し元の接続 (2026-09-21)

BuildPipeline はコピーした 2D 画像から全品質段を生成し、生成失敗なら配布を失敗させる。既存の `.fztc` はコピーせず、コピー先の元画像から生成し直す。開発環境の絶対パスや `Library/StreamCache` に依存しない。元画像は残し、世代不一致・破損・対応外形式は既存の読み込み経路を使う。アーカイブ展開等が元画像の更新時刻を変えた場合も、誤ったキャッシュを採用せず再生成へ戻る。

保持していた GPU ハンドルは使用前に resolver から引き直す。別ビューでの使用は同じ利用権を延長し、非表示・破棄後は resolver の猶予と AssetStreamer の常駐予算に従う。UI の材質テクスチャもキャッシュヒット時に利用権を更新する。生成した白テクスチャ・スプラット等は ResourceManager の生成資源として維持する。

キューブ・配列・浮動小数点 DDS は 2D RGBA8 に変換しない。利用権は同じ台帳で管理し、ネイティブ形式の同期 GPU ロードを固定せずに使用する。この形式の部分 mip 常駐・非同期ネイティブ転送は本段の対象ではない。2D RGBA8 の非同期転送と品質制御は既存経路を使う。

## 目的と採用案

ゲーム中のファイル読み込み・デコードをフレーム処理から外し、必要なアセットを先読みしながら CPU / GPU の常駐量を制御する。既存の `AssetHandle<T>` と `ResourceManager` の単独所有を維持し、ロード要求、利用権、GPU 転送の寿命を分ける。

最初はアセット全体を単位とする非同期読み込みと解放までを実装する。次にテクスチャ mip、モデル LOD の部分常駐へ進む。シーンの分割ロード、音声の連続再生バッファ、仮想テクスチャ、DirectStorage、専用 copy queue は初期範囲に含めない。それぞれ必要性を計測して別途設計する。

## 現状と変更理由

| 現在の入口 | 確認した性質 | 必要な変更 |
|---|---|---|
| `AssetManager::Load<T>` / `AssetStore<T>` | 同期 Import、型別キャッシュ、世代付きハンドル。ストアの並行更新を保護する契約はない | ストアはメインスレッド所有のまま、完成した結果だけを反映する |
| `IAssetImporter<T>::Import` | `ResourceManager*` を受け取り CPU 処理と GPU 生成を同じ呼び出しで行える | CPU デコードと GPU 生成を分離する |
| `TextureAsset` / `ModelAsset` | GPU ハンドルを含む。モデル破棄は GPU バッファの返却を伴う | ワーカーの成果物は GPU 非依存の専用データにする |
| `ResourceManager` | GPU 実体とパスキャッシュを所有、Reload / Reset を持つ | 非同期転送、完成判定、差し替え、遅延解放の窓口を追加する |
| `TaskSystem::Submit` | FIFO の共有スレッドプールと future | 既存基盤を再利用し、投入前の優先順位と同時実行数を Asset 側で制御する |
| `ReloadPath` | 一部 CPU 型はハンドルを維持して差し替える | 読み込み途中の旧リビジョンを棄却する |

参照実装は `Projects/Engine/include/Engine/Asset/AssetManager.hpp`、`IAssetImporter.hpp`、`TextureAsset.hpp`、`ModelAsset.hpp`、`Projects/Engine/include/Engine/Renderer/ResourceManager.hpp`、`Projects/Engine/src/Core/Concurrency/TaskSystem.cpp`。

現行バックエンドは [DX11 撤去設計](dx11-removal.md) に従い DX12 を対象とする。上位層は引き続き `IRenderer` 越しに接続し、DX 型を公開しない。[bindless 設計](bindless.md) のディスクリプタ遅延解放と整合させる。

## 責務とスレッド境界

```text
Scene / Editor / ScriptProxy
    → AssetManager: 要求、利用権、依存関係、常駐方針
        → TaskSystem: ファイル読み込み → CPU デコード
        ← 有界の完了キュー: CPU データとリビジョン
        → ResourceManager → IRenderer: GPU 転送投入
        ← 転送完了トークン
    ← 次のフレーム境界で READY を公開
```

| 実行場所 | 許可する処理 | 触らないもの |
|---|---|---|
| メインスレッド | GUID 解決、台帳、依存グラフ、要求集約、成果物の公開 | ブロッキング I/O、ゲーム中の future 待機 |
| TaskSystem ワーカー | 確定済みパスの読み込み、展開、パース、CPU データ生成 | Scene、AssetStore、ResourceManager、GPU 所有物 |
| レンダラーを所有するスレッド | GPU 生成・転送記録・差し替え・退役 | ファイル読み込み、画像デコード |

初期実装は既存のレンダラー呼び出しスレッドで GPU 作業を行う。描画専用スレッドの新設は前提にしない。インポーターの再入可能性を型ごとに監査し、共有状態を持つデコーダーはジョブ単位のインスタンスにするか同型処理を直列化する。

Application のフレーム冒頭、Scene 更新と描画コマンド構築より前に `PumpCompletions` と公開処理を置く。フレーム末尾に次回の投入を決める。これはアプリケーションのサービスであり、新しい Scene Phase は増やさない。ワーカーから依存ロードの完了を待たせず、依存グラフが次の実行を決める。

## 識別子・利用権・API 契約

永続キーは `GUID + subasset ID + asset type + import variant`。GUID を持たないエンジン組み込み物のみ正規化パスを代用する。同じキーへの要求は一つの処理へ集約する。mip / LOD の希望値はキーを増やさず、同じアセットへの品質要求として集約する。

次の名前は提案であり、既存 API ではない。

| API / 型 | 契約 |
|---|---|
| `Request<T>(reference, options)` | メインスレッドから呼ぶ。予約された `AssetHandle<T>` と move-only の `AssetLease<T>` を返す。I/O を待たない |
| `AssetLease<T>` | 呼び出し元一つ分の利用権と希望品質。CPU / GPU 実体は所有しない。破棄・解放もメインスレッドで行う |
| `GetStatus(handle)` | ロード段階、公開リビジョン、常駐品質、失敗理由を返す |
| `TryGet(handle)` | READY の公開済み実体のみ返す。未完成・失敗・解放済みなら nullptr。raw pointer はフレームを跨いで保持しない |
| `lease.SetDesiredQuality(...)` | 同じアセットへの希望品質を更新する。実際の常駐品質とは区別する |
| `lease.Release()` | 自分の要求だけを解除する。他の利用者の処理はキャンセルしない |
| `Retry(handle)` | 明示的な再試行。毎フレームの自動再要求による失敗ループを防ぐ |

`AssetHandle` は識別子であり所有権ではない。`IsValid()` はロード完了を表さない。利用権が一つでもあれば最低限必要な表現を保持するが、希望する最高品質の常駐までは保証しない。要求受付時のメタデータ領域にも上限を設け、枠不足は無効な結果と理由を返す。

AssetStore は空の予約スロットを扱えるよう拡張する。evict 時はスロットを解放して gen を進め、古いハンドルを無効化する。品質変更・ホットリロードではスロットを維持して contentRevision を進める。プロジェクト切り替え時は epoch も照合し、ストア Clear 後に同じ id / gen が再登場しても旧ハンドルが復活しない構造へ変更する。

既存の `Load<T>` は同期互換 API として残す。そこで得たアセットは明示的な Unload まで互換用の利用権で固定し、自動 eviction の対象にしない。非同期処理中の同一キーを同期 Load が要求した場合は、ロード専用画面など待機を許可した箇所のみ同じ処理の完了を進める。通常フレームでの同期ミスは計測・警告対象とし、既存呼び出し元の移行を段階的に進める。

Script DLL へは Engine の lease 実装を公開しない。後続段階で ScriptProxy に不透明な要求 ID と状態取得を追加し、OnDestroy / Play 停止で所有要求を回収する。要求 ID やハンドルはシリアライズせず、GUID とロード方針を保存する。

## 状態と世代

```text
QUEUED → READING → DECODING → WAITING_DEPENDENCIES
       → UPLOAD_QUEUED → UPLOADING → READY
```

CPU だけの型は GPU 段階を飛ばす。どの段階でも FAILED または CANCELED に終了できる。GPU 転送投入後のキャンセルは公開を止めるだけであり、転送資源の即時破棄はしない。

各ジョブは `(projectEpoch, slotGeneration, contentRevision, deviceEpoch)` を持つ。メインスレッドは完了を受けた時点と公開直前の両方で照合する。deviceEpoch は GPU 成果物に適用し、古い GPU 成果物は退役させる。利用権がゼロになり再要求された場合も旧キャンセル結果を採用しないよう、要求実行ごとの attempt ID を追加する。

再読み込み・品質向上は「公開中の実体」と「読み込み中の候補」を別に持つ。候補が失敗しても旧実体を READY のまま使い、更新失敗を別の状態欄へ記録する。初回失敗は理由を保持し、ファイル変更通知または Retry で再試行する。

## CPU 読み込み・依存関係

`IAssetImporter` に GPU 非依存の Decode 段階を追加する。Texture はサイズ・形式・色空間・各 mip のバイト列と配置情報、Model は頂点・インデックス・LOD・スケルトン・ノード情報を CPU 成果物として返す。GPU 解放を伴う `ModelAsset` をワーカーの一時物に使わない。

GUID の解決と import 設定をメインスレッドでスナップショット化してからジョブへ渡す。ファイル監視で更新された世代と結果を比較し、読み込み中に変更されたデータは再要求する。Editor の焼き直しは一時出力を完成してから公開し、読者が半端な Library キャッシュを掴まない形にする。

依存は必須と任意を区別する。必須依存の失敗は親の初回ロードを FAILED にし、任意テクスチャ等は型別の代替表示を許す。親が保持する依存 lease によって eviction を防ぐ。循環する必須依存はパス付きで失敗させ、ワーカー同士の待機にはしない。

配布ビルドはベイク済みデータを使い、実行中の FBX 変換やテクスチャ圧縮を通常ロードへ混ぜない。ビルド時に依存一覧、サイズ、設定ハッシュ、形式バージョンを生成する方向とし、既存 BuildPipeline への具体的な接続は実装段階で監査する。Editor では不足キャッシュをインポート工程へ渡して待機状態を表示する。

## GPU 転送と公開

ResourceManager に「CPU 成果物の転送投入」「転送トークンの完了確認」「候補の公開 / 退役」を追加する。DX12 型と fence 値の操作はバックエンド内部へ閉じる。初期は graphics queue 上へ転送をまとめ、専用 copy queue とその間の同期を増やさない。

毎フレームの転送バイト数・CPU 投入時間・生成件数を制限する。大きな mip / バッファは分割転送し、1 個が上限を超えるため永遠に投入されない状態を防ぐ。単一の GPU メモリ確保やドライバー呼び出しは途中で止められないため、時間予算は絶対的なフレーム時間保証ではない。

初期の READY 条件は必須依存が揃い、GPU 転送が完了し、使用時の resource state が確定していること。フレーム処理で CPU の fence 待機は行わず、完了していなければ旧実体・代替物を描く。転送用 upload memory は転送完了まで保持する。旧 GPU 実体は最後に利用した描画・計算が完了するまで保持する。

bindless の公開済みディスクリプタを実行中に上書きしない。新しい候補に新しい枠を割り当て、フレーム境界で新しい添字を公開する。旧実体・旧枠は最終利用 fence の完了後に回収する。マテリアル等の添字キャッシュは contentRevision の更新で再構築する。

GPU 完了を使う寿命管理の根拠は [Microsoft: Fence-Based Resource Management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management)。実装時は転送用メモリの再利用と遅延解放のコード直近にも `@see` を置く。

## 常駐予算とストリーミング

台帳は常駐 CPU、デコード中 CPU、完了待ち CPU、GPU 常駐、転送用メモリ、GPU 退役待ちを別々に計上する。候補の追加予約は旧実体との同時保持も含める。解放要求を出しただけでは空き容量に数えず、実際の退役完了時に返す。

要求は必須、現在可視、近傍先読み、背景の順を基本にし、同順位では待機フレーム数を考慮する。TaskSystem へ大量投入せず、予算を予約できた小数のジョブだけを投入する。完了キューはジョブ枠とバイト数で制限する。サイズ不明のデータはヘッダー検査後に上限付き予約を取り、過大・破損入力は割り当て前に失敗させる。

利用権なしのアセットは一定の猶予後に LRU で解放する。予算不足なら先読みを延期し、任意の高品質表現を落とし、未使用物を退役させる。固定物だけで予算を超える場合は常駐予算超過を報告し、新規要求を BUDGET_EXCEEDED で失敗させる。無制限に割り当てて解決しない。

部分常駐の段階では次を採用する。

| 対象 | 初期の部分常駐方式 | 制約 |
|---|---|---|
| Texture | 低解像度 mip 群を持つ別リソースから開始し、必要な mip 群の候補へ差し替える | 全 mip を確保して SRV だけ狭めても VRAM 節約にはならない。旧新のピーク容量を予約する |
| Model | 最低品質 LOD と共有スケルトンを保持し、各 LOD の GPU バッファを必要に応じて追加・解放する | 描画要求 LOD が未常駐なら常駐 LOD へ倒す。現在の LodCount / lods 利用箇所を監査する |
| CPU データ | 再生成不要な一時データを公開後に解放する | CPU スキニング・衝突・モーフ等の利用者が必要なデータは個別に固定する |

希望品質は画面上の投影サイズを基本とし、しきい値にヒステリシスと最低保持フレーム数を設ける。Physics の衝突形状を描画 LOD の都合で外さない。UI、フォント、起動必須物は初期状態では固定する。

部分 I/O のため、ベイク形式には mip / LOD ごとの offset、圧縮サイズ、展開サイズ、チェック値を持つ索引が必要になる。`Engine/Format/` でバージョンを管理し、独立に展開できるチャンクにする。既存形式を読めることは維持し、索引のない旧形式は全体ロードへ倒す。形式の具体的なレイアウトは部分常駐段階で別途確定する。

## シーン切り替え・停止・復旧

シーンは自分の lease 群を解放するだけにし、別シーンや Editor が使用中の共有物を UnloadAll しない。先読み中の次シーンも別の利用権集合を持ち、切り替えに失敗した場合は旧シーンの利用権を保持する。

Play 停止では Play 所有の要求を解除し、Editor の要求は維持する。プロジェクト切り替えでは受付停止、epoch 更新、旧要求キャンセル、旧完了結果の回収、GPU 退役の順で進める。ワーカーは Scene や AssetManager の寿命へ直接依存しない。

終了時は受付停止 → キャンセル通知 → Asset ジョブ終了・完了キュー回収 → GPU 転送 / 最終利用完了 → AssetStore と ResourceManager 破棄の順とする。終了待ちの間に有界キューへワーカーが詰まって join できなくなる構造を避ける。共有 TaskSystem の停止順は他の利用者と合わせて調整する。

デバイスロストでは deviceEpoch を更新し GPU 候補を無効にする。CPU 成果物は内容世代が一致する場合だけ再利用し、他は再ロードする。失われたデバイスの fence が正常に完了することを待ち続けず、バックエンドのロスト処理で退役を完結する。

## 導入順と受け入れ条件

| 段階 | 実装範囲 | 完了の判断 |
|---|---|---|
| A | CPU decode 分離、要求台帳、lease、重複集約、完了キュー | CPU 型で重複要求・キャンセル・古い結果の棄却を決定的に検証できる |
| B | Texture / Model の GPU 転送、フレーム境界公開、bindless 退役 | 描画中のロードで同期 I/O / fence 待機を発生させず、旧資源の早期再利用がない |
| C | シーン / 描画呼び出し元の移行、予算、先読み、eviction | 移動とシーン往復で常駐量が制限内に戻り、共有アセットが消えない |
| D | チャンク索引、mip / LOD 部分常駐、品質選択 | 希望品質へ収束し、縮退により実際の GPU 使用量が減る |

最初の実装単位は A と Texture の B を縦に通す小さな範囲とする。巨大な全型共通フレームワークを先に完成させず、Texture で転送と寿命を確かめてから Model へ広げる。

検証は TEST_F + TestKit で手動進行できるジョブ / 転送完了のテスト用実装を使う。sleep や実時間に依存させない。要求の重複、利用者一人だけのキャンセル、全員キャンセル直後の再要求、必須依存の循環 / 失敗、予算超過、ホットリロード逆順完了、Clear 後の古いハンドル、デバイスリセット、終了中の完了を含める。

実機 Playtest は低い予算でロード・移動・シーン往復・Play 停止を繰り返し、状態と常駐バイト数を表明する。画像比較は任意フレームではなく対象アセットが READY になった後に行う。GPU validation で実行中資源とディスクリプタの再利用を確認する。

計測値はロード段階別時間、優先度別待機数、キャッシュ命中、CPU / GPU / 退役待ちバイト、1 フレームの転送量・投入時間、代替表示数、同期 Load のキャッシュミス数。目標の予算値とフレーム時間は対象 GPU と GreenWare の代表シーンで決める。現時点では性能改善率や一定 FPS を保証しない。

## 接続実装の検証 (2026-09-21)

`AssetPartialIoTests` で配布用 `.fztc` の生成・移動後の全品質読み込み・更新検知を確認し、`AssetStreamingIntegrationTests` で実 ResourceManager / AssetManager / Resolver を通したキャッシュ利用、元画像寸法、利用中の保持、未使用後の解放・再取得を確認した。`RenderSceneExtractorTests` はスキンド境界、カメラ内包、平行投影、境界不明時の推定を含む。音声・アセット・描画関連 272 テストが全件合格した。

最終 Editor バイナリで FiberLifecycle (93 手順)、WaterWireframe (10 手順)、LightProbeGI (14 手順)、Title の Play/Stop と画像取得 (7 手順) が合格。Fiber の Golden 比較 10 枚は差分 0。Title は既存 Golden がないため比較ではなく画像取得と目視確認とした。配布パイプライン全体の成果物を別環境で起動する検証と性能計測は含まない。
