<!-- @file    README.md -->
<!-- @brief   Read only asset audit usage, scope and limitations. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->
# AssetAudit

FBZZの制作アセットを読み取り専用で検査する独立CLI。Node.js 22以上、外部依存なし。Engine / SDK / CMake / Editor / Importerを呼ばず、アセット・`.meta`・キャッシュ・シーンへの書き込みは行いません。ユーザーによるCLI呼び出しが入口です。

実装の所有範囲は `Tools/AssetAudit/` です。共通 `Tools/README.md` には、ユーザーが許可した最小接続として役割とCLI呼び出し元の索引行だけを追加しています。

| ファイル | 用途 |
|---|---|
| `Audit.mjs` | CLI・入力の不変検証・GUID/パス参照・閾値・重複・JSON/HTML出力 |
| `Parsers.mjs` | 上限付き純粋解析器（ファイルI/Oを行わない） |
| `Tests.mjs` | 一時フォルダだけで実行する少量fixtureと契約テスト |
| `Config.example.json` | 設定例。CLI指定は設定ファイルより優先 |
| `.gitignore` | 専用ReportsフォルダをGit管理の対象から除外 |
| `Integration.md` | 共通索引への登録内容と検証契約 |

```powershell
node C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\AssetAudit\Audit.mjs `
  --project C:\Users\jinhs\Downloads\FBZZCTestProject `
  --shared-assets C:\Users\jinhs\Downloads\FBZZ_Engine\Assets `
  --output C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\AssetAudit\Reports\Run001 `
  --profile desktop --workers 1

node C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\AssetAudit\Audit.mjs `
  --project C:\Users\jinhs\Downloads\FBZZCTestProject `
  --output C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\AssetAudit\Reports\Run002 `
  --config C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\AssetAudit\Config.example.json

node --test C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\AssetAudit\Tests.mjs
```

`--output`は必須。プロジェクト全体とすべての入力rootの外側を指定してください。既存の`Audit.json` / `Audit.html`は上書きせず失敗します。新しいrunフォルダを指定してください。symlink/junctionを含む出力パスを拒否します。レポートHTMLはスクリプト・外部通信・外部リソースを使用せず、すべての入力文字列をescapeします。入力からレポート先を生成することはありません。

既定の入力は `Assets/` と `Library/Baked/`。その他のLibrary・Editor・GPU・共通ビルドには触れません。`--scope assets`で派生物を外せますが、Library参照の不在は未確認となります。Engine共通アセットを解決する場合は `--shared-assets <Assetsフォルダ>` を明示します（複数指定可）。入力rootの重なりは拒否します。入力のsymlink/junctionは追跡せず、対応率と終了コードに未確認を反映します。監視対象外の生成レポートフォルダを使用してください。

## 出口の契約

| Exit | 意味 |
|---|---|
| `0` | 実装された検査が確定し、`--fail-on`以上の問題なし。partial/unknownはゼロ |
| `1` | `--fail-on`以上の**確定した**指摘あり。未完了を併発する場合はJSONの`incomplete=true`も確認 |
| `2` | 確定指摘がfail-on未満で、partial/unknown/生成中/未観測/入力変化あり。成功ではない |
| `3` | 使用法・設定・読み取りroot・出力などの致命エラー |

優先度は致命エラー3 → 確定指摘1 → 未完了2 → 完了0です。既定`--fail-on error`、`warning`と`info`も指定可。危険閾値超過は用途によって判断が変わるためwarningです。閾値を超えるモデルをCIで失敗にする場合は`--fail-on warning`を指定します。自動削除・修復・再インポートはありません。

**確定errorが0件でも、全アセットが健全とは判断しません。** partial/unknown/pendingには未検査や判定保留が残るため、`coverage`・`incomplete`・各アセットの`limitations`を併せて確認してください。終了コード0も、実装された検査と観測した入力範囲に限った結果です。

JSONは `schemaVersion=1`。`scan`に入力不変検証結果、`coverage`にカテゴリ別supported/partial/unknown/pendingと読取率・完全対応率、`assets`に前後のstat/SHA-256・統計・制限、`findings`にpath/severity/certainty/code/evidence/suggestion、`duplicates`にバイト一致と類似候補を記録します。certaintyはconfirmed/pending/unknown/candidateです。バイト一致はサイズとSHA-256が一致した安定入力だけが対象です。SHA-256の衝突可能性を数学的に排除する比較ではありません。

## 検査範囲

| 形式 | 実装した検査 | 限界・対応表示 |
|---|---|---|
| `.meta` | `[meta].guid`またはinline `meta={guid=...}`とquoted参照、孤児、GUID衝突 | partial。TOML全体/schemaの完全検証はしない |
| `.mat/.asset/.fzdata/.scene/.prefab/.terrain/.json`等 | quoted GUID / Assets / Library/Baked参照、TOML NaN/Inf | partial。JSONは構文検証、TOMLは保守的な字句抽出（複数行・Unicode escapeも対応）。未対応escapeはunknown |
| `.mesh` v1/v2 | vertices/triangles、index範囲、NaN/Inf、4ウェイト値/和 | 静的はsupported。スキン単体は骨indexの上限を確認できずpartial |
| `.fzasset` v4/v5 | LOD0のvertices/triangles、各LOD/submesh/material/morph、骨数、骨index、ウェイト、階層循環/範囲、NaN/Inf | supported。v1等はunknown。新規flag/trailingチャンクはunknown |
| `.skel` v1 | 骨/ノード数・parent/child/root/index・循環・NaN/Inf | supported |
| `.anim` v3 | clip長/tick/Hz/track/property/event/key数、キー順/範囲、NaN/Inf、Quaternion長 | supported。旧v1/v2等はunknown。名前による対象scene bindingとイベント動作は未検査 |
| binary `.fbx` 7000–7700 | control points、polygon数とfan triangle推定、material数、skin cluster数/ウェイト/接続、curve/key/clip長、テクスチャパス | partial。ASCII FBXはunknown。元FBXの統計はエンジン変換後の頂点/骨パレットと異なり得る。FBX skin modeによるウェイト和は候補warning |
| `.obj` | control points、face/fan triangle推定、material数、mtllib | partial。engine変換・index全体の検証なし |
| PNG/JPEG/DDS/GIF/BMP/TGA | header寸法・ファイル容量・RGBA8参考容量 | partial。画素/見た目/全codec整合性/DDS surface本体は未確認。PNGはIHDR CRCとchunk境界/IENDも確認 |
| `.ibl` v1 | 固定layoutのDDS参照 | supported |
| `.blend/.glb/.gltf/.hdr/.exr/.webp/.ktx`等 | ファイルの前後hash/size/mtimeのみ | unknown。読めたかのように表示しない |
| script/shader/audio/video等 | 入力不変検証だけ | other / not_applicable。アセット解析の対応率とは分離 |

GUIDの権威と`|`以降のパスヒントを分けて処理します。権威が無くても実在ヒントで読める場合は`GUID_FALLBACK_ONLY`であり、参照切れの断定を避けます。Engine共通rootと同じ相対パスのコピーは、内容が異なっていてもEngineの規約に合わせて`GUID_SHARED_COPY`に分類します。sprite/submesh接尾辞は本体だけ解決し接尾辞自体をunknownにします。`Library/Baked/<sourceGuid>/<subKey>`の `.anim/.mat/画像`にはエンジンの二系統FNV-1aによる派生GUIDを計算します。入力範囲外の外部パスは開かずunknown。Engine共通rootを指定しなければ、そのrootのGUIDは観測されません。missingの根拠は観測scope内の索引に限られ、現在のEngineのランタイム索引を問い合わせる検査ではありません。

FBXテクスチャの相対パス不在はcandidate warningです。元ファイルの絶対パスやImporterの検索規約で読める可能性があり、破損の確定に使いません。JSONのwildcard/placeholder・destination/説明フィールドは未確認として区別します。`Library/Baked`内の`[texture]`設定だけのsidecarは、GUIDが原本から導出されるためGUIDなしのpartialとして読み取ります。それ以外のmetaに権威GUIDがない場合はunknownを保ちます。

類似候補は、材質の字句値一致、または画像の同形式・同寸法・容量bucket一致です。画像の知覚的な類似性、shaderの意味的な等価性、外部埋め込み画像の同一性は保証しません。バイト一致と別の指摘・JSON配列で出力します。候補からの自動統合は行いません。

## 同時制作と負荷

1. 入力一覧・size/mtime/ctime/inodeを採取。
2. 各入力を`r`で開き、上限付きchunk読取でSHA-256を計算。読取前後のstatも比較。
3. 解析に使ったバイト列のhashを上記hashと比較。
4. 走査全体の最後に再hash・stat・一覧を採取し、追加/削除/変更を記録。

変更・読み取り失敗・上限省略・既定30秒以内の更新はpending。生成途中の破損指摘もpendingに落とします。`.meta`が不明/変化中、入力一覧が変化、または未観測rootがあるときはmissingの確定を保留します。重複には安定入力だけを使います。`--settle-seconds 0`は静止fixture用途で、制作中は既定を推奨。

これは非ロックの観測検証です。最後の検証後の変更や、観測間で変更して完全に元に戻った短い状態を排除する原子的snapshotは保証しません。入力をロックして制作を妨げることはありません。

既定workers=1（上限2）。同時I/Oジョブ数を制限し、JS解析は同一スレッドで逐次実行します。独自GPU・Editor・外部Import/Export・子ビルド・ネットワーク処理はありません。既定の単体解析上限128MiB（テキスト/OBJは16MiB）、hash上限512MiB、全体hash予算2GiB/各pass、展開上限256MiB/FBX、要素処理上限2000万、一覧上限25000。省略はunknown/pending。上限を設定で変更できる項目にも保守的なhard capがあります。最大二重hashに伴うディスク読取量を考慮し、頻繁な全走査を避けてください。

閾値はmobile/desktop/cinematicと`thresholds`個別上書きで調整できます。これらはツールのレビュー用初期値であり、EngineやGPUの仕様上限を意味しません。

実装の形式根拠は `FzAssetFormat.hpp` / `FzModelFormat.hpp` / `AnimationImporter.cpp` / `Graphics/Renderer/Mesh.hpp` / `GuidRefCodec.hpp` / `AssetDatabase.cpp`。外部仕様は実装直近の`@see`に記載しています。BlenderのFBX仕様説明と型幅確認、Nodeのzlib、W3C PNG、Microsoft DDS、TOML仕様を参照し、他製品のImporterは実行していません。
