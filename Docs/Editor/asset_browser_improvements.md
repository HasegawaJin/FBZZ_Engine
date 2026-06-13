# Asset Browser 改善点リスト

対象ファイル:
- `Projects/Editor/src/Panels/AssetBrowser/AssetBrowserCore.cpp`
- `Projects/Editor/src/Panels/AssetBrowser/AssetBrowserItems.cpp`
- `Projects/Editor/src/Panels/AssetBrowser/AssetBrowserCreate.cpp`
- `Projects/Editor/src/Panels/AssetBrowser/AssetBrowserImport.cpp`
- `Projects/Editor/src/Panels/AssetBrowserPanel.cpp`
- `Projects/Editor/include/Editor/Panels/AssetBrowserPanel.hpp`

凡例: ✅ = 実装済み

---

## 1. UX / 操作性

### ✅ 1-1. 複数選択の未実装
`ctx.selectedAssetPath` が単一パスのため Ctrl+クリック・Shift+クリックによる複数選択ができない。
一括削除・一括移動・一括インポートを行うには先にこれが必要。

→ パネルローカルの `m_selectedPaths: unordered_set<string>` と `m_lastClickedPath` を追加。
`ctx.selectedAssetPath` は Inspector 用の単一パスとして維持。
Ctrl: トグル追加、Shift: 基点〜現在の連続範囲、通常クリック: 単一選択。
選択状態は金枠 (primary) / 青枠 (multi) で描画。

### ✅ 1-2. コピー & ペースト未実装
Ctrl+C / Ctrl+V によるファイル複製機能がない。
コンテキストメニューにも "Duplicate" が存在しない。

→ コンテキストメニューに "Duplicate" を追加。`FileSystem::CopyFile` で `name(2).ext` 形式の連番で保存。

### ✅ 1-3. D&D によるフォルダ間移動が未実装
フォルダアイコンへのドロップは「Hierarchy エンティティ → Prefab 保存」専用であり、
既存ファイルをフォルダへ D&D で移動することができない。

→ フォルダのドロップターゲットで `ASSET_PATH` ペイロードを受け付けるよう拡張。
`FileSystem::Rename` で移動し、プレビューキャッシュとツリーキャッシュを無効化。

### ✅ 1-4. ダブルクリックハンドラが一部アセット型に未対応
| 拡張子 | 現在の挙動 | 期待値 |
|---|---|---|
| `.fbzz` | シーンを開く ✓ | — |
| `.fbzzprefab` | シーン上にインスタンス化 ✓ | — |
| `.fbzzanimcontroller` | 何もしない | AnimationGraphPanel を開く |
| `.fzmat` | 何もしない | Inspector にフォーカス (現状 `ctx.selectedAssetPath` は設定される) |

→ `ctx.requestOpenAnimationGraph` フラグを追加し、ダブルクリック時に set。
`EditorApp::RenderPanels` でフラグを監視して Animation Graph パネルを `visible = true` にする。

### ✅ 1-5. "Reveal in Explorer" がコンテキストメニューにない
OS のエクスプローラーで対象ファイル / フォルダを開く操作が一切ない。

→ コンテキストメニューに "Reveal in Explorer" を追加。`ShellExecuteW` + `explorer /select,path` で対象ファイル選択状態で開く。

### ✅ 1-6. ブレッドクラムがクリック不可
`AssetBrowserPanel.cpp` でパスを `ImGui::TextDisabled` で表示するのみ。
各セグメントをクリックして上位フォルダに一発で移動できない。

→ `DrawBreadcrumb()` を実装し、パスをセグメント分割して `SmallButton` ループで表示。
マウントパスにも対応。

### ✅ 1-7. 検索に拡張子フィルタがない
現在の検索はファイル名の部分一致 (`ContainsCI`) のみ。
「`.fzmat` だけ表示」「`ANIM` タイプだけ表示」などのフィルタができない。

→ ツールバーに `TypeFilter` ドロップダウン (All / Scene / Material / Script / Texture / Audio / Mesh / Shader / Prefab) を追加。
グリッドループで `passesTypeFilter()` によりスキップ。

### ✅ 1-8. ソート方法が固定
「ディレクトリ優先 + 名前昇順」のみ (`AssetBrowserCore.cpp:RefreshDirectory`)。
更新日時順・種類順・サイズ順への変更手段がない。

→ ツールバーに `SortMode` ドロップダウン (Name ^/ Name v / Type / Modified) を追加。
`RefreshDirectory` の `stable_sort` ラムダを `m_sortMode` で切り替え。

### ✅ 1-9. グリッド / リスト表示切替がない
常にアイコングリッドのみ。ファイル名・サイズ・更新日時を列で確認できるリストビューがない。

→ `ViewMode` enum (Grid/List) を追加。ツールバーの "List"/"Grid" トグルボタンで切替。
リストビューは `DrawListView` で `ImGui::BeginTable` 4列 (Name/Type/Size/Modified) + `ImGuiListClipper` 仮想スクロールで実装。

### ✅ 1-10. アイコンサイズが永続化されない
`m_iconSize` (初期値 `84.0f`) はセッション間で保存されない。
`EditorSettings` に保存して次回起動時に復元すべき。

→ `EditorSettings::assetBrowserIconSize` を追加し TOML に保存。
`EditorContext::assetBrowserIconSize` を中継として `OnInit` で読込、スライダー変更時に即時書き戻し。

### ✅ 1-11. フォルダ内アセット件数の表示がない
現在のディレクトリに何件のファイルがあるかの情報が UI にない。
フィルタ後の件数表示も同様。

→ Refresh ボタン横に `(%d files, %d dirs)` を `TextDisabled` で表示。

### 1-12. リネーム時の競合チェックがない
`AssetBrowserItems.cpp:1404` でリネーム先が既存パスと一致する場合の確認ダイアログがない。
既存ファイルを上書き破壊するか、何も起きないかのどちらかになる。

---

## 2. パフォーマンス

### ✅ 2-1. 左ペインのフォルダツリーが毎フレームスキャン
`DrawFolderTree` (`AssetBrowserItems.cpp:977`) は毎フレーム `FileSystem::ListAll` を呼んでいる。
右ペインは `RefreshDirectory` でキャッシュしているのに左ペインはキャッシュなし。
`AssetFileWatcher` のイベントを使って必要時のみ再構築するべき。

→ `m_treeCache: unordered_map<string, vector<Entry>>` を追加。
`DrawFolderTree` はキャッシュミス時のみ `ListAll` を実行。
`RefreshDirectory` とファイルウォッチャーイベントで対象ディレクトリを無効化。

### ✅ 2-2. 仮想スクロールがない
`OnRenderContent` で `m_entries` を全件ループして `DrawEntry` を呼んでいる。
数百件のアセットがあると画面外のアイコンもサムネイル描画コストを発生させる。
ImGui の `ImGuiListClipper` や手動ウィンドウクリップで画面外をスキップすべき。

→ フィルタ済みエントリを `visIndices` ベクターに事前収集し、`ImGuiListClipper` で行単位にクリップ。
リストビュー側も同様に `ImGuiListClipper` で仮想スクロールを実装。

### ✅ 2-3. プレビューキャッシュにサイズ上限がない
`m_texturePreviews`, `m_materialPreviews`, `m_meshPreviews` は
アクセスするたびに無制限に `unordered_map` へ追加される。
フォルダを移動し続けると GPU リソースを含むキャッシュが際限なく膨らむ。
LRU キャッシュや「カレントフォルダ外エントリを破棄」する仕組みが必要。

→ `RefreshDirectory` でカレントフォルダに属さないパスを 3 つのキャッシュから一括削除。
フォルダ移動のたびに不要な GPU リソースが解放される。

### ✅ 2-4. テクスチャ / メッシュプレビューのロードがメインスレッド同期
`DrawAssetPreviewIconAt` 内でテクスチャロードを即時呼び出している。
初めてフォルダに入った際に多数のテクスチャを読むと描画がブロックされる。
インポートスレッドと同様にバックグラウンドでロードキューを処理すべき。

→ `TexturePreview::queued` フラグと `m_texLoadQueue: deque<string>` を追加。
`DrawAssetPreviewIconAt` では即時ロードせずキューに積み、`DrainTexLoadQueue` で 3件/フレームに分散してロード。
`RefreshDirectory` でキューをクリア。

### ✅ 2-5. マウントパス変更時に未変換アセットの再スキャンが行われない
`UpdateMounts` でマウント先が変わっても `ScanAndQueueUnimported` が呼ばれない。
新しいマウントパス内の未変換ファイルが自動検出されない。

→ `UpdateMounts` でマウント変更時に各 `mount.path` に対して `ScanAndQueueUnimported` を呼ぶよう修正 (3-3 と同時実装)。

---

## 3. バグ / エッジケース

### ✅ 3-1. マテリアルサムネイルがシェーダー再コンパイル後に更新されない
`MaterialPreview::thumbnailRendered = true` になった後は `lastWriteTime` の変化でのみリセットされる。
シェーダーを再コンパイルした場合や `.hlsl` を編集した場合にサムネイルが古いまま残る。

→ `MaterialPreview` に `shaderLastWriteTime` フィールドを追加。
`DrawAssetPreviewIconAt` でシェーダーファイルの更新時刻を毎フレーム照合し、変化があれば
`thumbnailRendered = false` にリセット。

### ✅ 3-2. FBX ホバー離脱後も下部プレビューが残り続ける
`DrawEntry` では「ホバー中に `m_selectedFbxPath` を更新」しているが、
ホバーを外した後も最後の FBX のコンテンツが `DrawFbxContents` で表示され続ける。
空白エリアをクリック / カレントフォルダを移動したときにクリアすべき。

→ `RefreshDirectory` で `m_selectedFbxPath` と `m_selectedModel` をクリアするよう修正。
フォルダ移動またはファイルウォッチャーイベントのたびにプレビューがリセットされる。

### ✅ 3-3. `ScanAndQueueUnimported` がマウントパスを対象外
`OnInit` と `SetRootPath` では `m_rootPath` のみをスキャンする。
マウントされた Scripts / Shaders フォルダ内に FBX 等を配置しても自動検出されない。

→ `UpdateMounts` でマウント変更検出時に各 `mount.path` に対して `ScanAndQueueUnimported` を実行 (2-5 と同時実装)。

### ✅ 3-4. フォルダアイコンへの ASSET_PATH D&D を受け付けない
`DrawEntry` のドロップターゲットは `FBZZ_HIERARCHY_ENTITY` ペイロードのみ受け付け、
`ASSET_PATH` ペイロードを無視している。これにより「別フォルダのアセットをこのフォルダへ移動」操作ができない。

→ 1-3 と同時に実装済み。`AcceptDragDropPayload("ASSET_PATH")` をフォルダドロップターゲットに追加。

---

## 4. コード品質

### ✅ 4-1. `AssetBrowserItems.cpp` の肥大
1446 行、うち `DrawEntry` 単独で約 210 行。
「サムネイル描画」「D&D」「コンテキストメニュー」「リネーム」「ダブルクリック」が
一関数に混在している。責務ごとにプライベートメソッドへ分割すべき。

→ `DrawEntry` を以下の 5 つのプライベートメソッドに分割:
- `DrawEntryBadges` — ! バッジ / 橙ドット
- `DrawEntryContextMenu` — 右クリックポップアップ (Import/Duplicate/Reveal/Rename/Delete)
- `DrawEntryRenameLabel` — リネーム InputText または省略ラベル + F2
- `HandleEntryClick` — Ctrl/Shift 対応クリック処理
- `HandleEntryDoubleClick` — シーン開く / Prefab 配置 / AnimGraph 開く

### ✅ 4-2. `RenderMeshThumbnail` 内 function-local static が暗黙共有
`s_shader`, `s_pso`, `s_frameCB` 等が関数スコープの `static` で、
全マテリアル・メッシュサムネイルが同じ GPU リソースを共有している。
意図的な設計だが明示的でない。専用の `ThumbnailRenderer` 構造体に昇格させると意図が明確になる。

→ `ThumbnailRenderer` 構造体を `AssetBrowserItems.cpp` の anonymous namespace に定義し `static ThumbnailRenderer s_tr` を1つ置く。全 GPU リソースをフィールドとして明示。

### ✅ 4-3. サブアイコン描画コードの重複
`DrawFbxContents` (`AssetBrowserCreate.cpp`) と `DrawEntry` 内のアイコン描画ロジック
（5点ポリゴン + ラベルテキスト + D&D ソース）がほぼ同一コードで重複している。
`DrawFileIconAt` と同様の共有ユーティリティに切り出すべき。

→ `DrawFileIconPolygon(dl, origin, sz, cFill, cDark)` を `AssetBrowserCommon.hpp` に追加。両ファイルからの呼び出しに統一。

### ✅ 4-4. `kMaterialTextureSlotNames` とテクスチャ配列サイズの不整合
`kMaterialTextureSlotNames` は 8 要素 (`AssetBrowserItems.cpp:191`) だが、
`preview.textures.assign(16, {})` と `dc.textures` は 16 スロット使用している。
スロット数をひとつの定数に統一すべき。

→ `kMaterialTextureSlotNames` を 16 要素 (`tex8`〜`tex15` を追加) に拡張して統一。

---

## 5. 機能不足

### ✅ 5-1. テクスチャは原本を直接使用
`IsImportableRaw` は FBX / OBJ / GLTF / GLB のみ対応。
PNG / JPG / DDS などは ResourceManager が原本を直接読み込めるため、
未変換バッジ・インポートキュー・サイドカー生成の対象にしない。

→ `.fztex` 生成は追加の管理ファイルだけを増やし、ランタイムでも参照されていなかったため廃止。
マテリアルや UI は `.png` / `.jpg` / `.dds` 等の Assets パスを直接保持する。

### ✅ 5-2. アセット参照（被参照）情報の表示がない
「このテクスチャはどのマテリアルで使われているか」
「このマテリアルはどのシーン・プレハブで使われているか」
のような逆引き（依存関係）を確認する手段がない。

→ コンテキストメニューに "Find References..." を追加。クリック時にプロジェクト内の
`.fbzz` / `.fzmat` / `.fbzzprefab` / `.fbzzanimcontroller` を全スキャンして参照先を収集。
結果を `DrawFindRefsPopup` モーダルで一覧表示し、クリックでそのフォルダへナビゲート。

### ✅ 5-3. 複数ファイルの一括操作がない
複数選択 (#1-1) が未実装なので当然だが、
一括削除・一括リネーム（正規表現置換）・一括インポート（個別指定）等が全てできない。

→ `DrawEntryContextMenu` で `m_selectedPaths.size() > 1` 時に専用の一括操作メニューを表示。
"Duplicate N items"（ファイルのみ、連番コピー）と "Delete N items"（確認ダイアログ付き）を実装。

### ✅ 5-4. アセット型ごとのカスタムサムネイル生成が `.fbzzanimcontroller` / `.fbzzterrain` に未実装
テクスチャ・マテリアル・メッシュはサムネイルレンダリングが実装済みだが、
Animator Controller (`CTRL`) と Terrain アセット (`TERRAIN`) はカラーアイコンのみで
内容を可視化するサムネイルがない。

→ `DrawAssetPreviewIconAt` に `.fbzzanimcontroller`（ステートマシン風: 3ノード+エッジ）と
`.fbzzterrain`（地形シルエット: 波形ヒル）の ImDrawList カスタムサムネイルを追加。`DrawThumbnailLabel` ヘルパーでバッジ描画を統一。

---

---

## 6. 実装済み機能 (追加実装)

以下はリスト作成後に新規実装した機能。

### ✅ 6-1. Unreal スタイル未保存アセット追跡 (AssetDirtyRegistry)

`Projects/Editor/include/Editor/Util/AssetDirtyRegistry.hpp` / `.cpp`

中央レジストリで「パス・表示名・型ラベル・保存コールバック」を管理。
現在対応アセット型: `.fzmat` (マテリアル)、`.fbzzanimcontroller` (アニメーションコントローラー)。

- `Register(path, displayPath, typeLabel, saveFunc)` — upsert
- `MarkClean(path)` / `IsDirty(path)` / `HasAny()`
- `SaveAll()` — 全件保存し保存済みパスをクリア
- `DiscardAll()` — 全件破棄

### ✅ 6-2. アイコン上の dirty インジケータ (橙ドット)

`AssetBrowserItems.cpp: DrawEntry`

`AssetDirtyRegistry::IsDirty(e.path)` が true のとき、
アイコン左上に橙色の小ドット (`IM_COL32(255, 160, 30, 230)`) を描画。

### ✅ 6-3. Save Modified ツールバーボタン + ダイアログ

`AssetBrowserPanel.cpp`

未保存アセットがあるとツールバーに `Save* (N)` ボタンを橙色で表示。
クリックするとアセットをチェックリスト表示するモーダルが開き
「Save Selected / Save All / Discard All / Cancel」で操作できる。

### ✅ 6-4. File > Save All Assets メニュー項目

`EditorApp_MenuBar.cpp`

File メニューに `Save All Assets` を追加。未保存アセットがないときは無効化。

### ✅ 6-5. 終了時の未保存アセット確認

`EditorApp_Scene.cpp: ConfirmDiscardUnsaved`

シーン dirty またはアセット dirty がある場合に確認ダイアログを表示。
ダイアログメッセージに未保存アセット一覧を列挙する。
「Save」でシーンと全アセットを保存、「Discard」で全破棄してから遷移。

### ✅ 6-6. マテリアル deselect 自動保存

`InspectorPanel_Asset.cpp`

Inspector で別のアセットに切り替えた瞬間、
前の `.fzmat` が dirty であれば自動的に `SaveMaterialAssetToFile` を呼ぶ。
mid-drag 中の大量書き込みを避けつつ Save ボタン押し忘れを防ぐ。

---

## 優先度まとめ

| # | 改善項目 | 優先度 | 工数感 | 状態 |
|---|---|---|---|---|
| 1-4 | `.fbzzanimcontroller` ダブルクリック対応 | 高 | 小 | ✅ 完了 |
| 1-6 | ブレッドクラムのクリック可能化 | 中 | 小 | ✅ 完了 |
| 1-12 | リネーム競合チェック | 中 | 小 | ✅ 完了 |
| 3-1 | マテリアルサムネイルのシェーダー再ロード連携 | 中 | 小 | ✅ 完了 |
| 3-2 | FBX 下部プレビューのクリアタイミング修正 | 中 | 小 | ✅ 完了 |
| 2-1 | 左ペインツリーのキャッシュ化 | 中 | 中 | ✅ 完了 |
| 1-10 | アイコンサイズの永続化 | 低 | 小 | ✅ 完了 |
| 1-11 | フォルダ内アセット件数の表示 | 低 | 小 | ✅ 完了 |
| 1-7 | 拡張子フィルタ | 低 | 小 | ✅ 完了 |
| 1-8 | ソート方法の切替 | 低 | 小 | ✅ 完了 |
| 1-9 | グリッド / リスト表示切替 | 低 | 中 | ✅ 完了 |
| 1-5 | Reveal in Explorer | 低 | 小 | ✅ 完了 |
| 4-1 | DrawEntry の分割リファクタ | 低 | 中 | ✅ 完了 |
| 4-2 | ThumbnailRenderer 構造体への昇格 | 低 | 中 | ✅ 完了 |
| 4-3 | サブアイコン描画コードの重複解消 | 低 | 小 | ✅ 完了 |
| 4-4 | テクスチャスロット数の不整合修正 | 低 | 小 | ✅ 完了 |
| 2-2 | 仮想スクロール (ImGuiListClipper) | 低 | 中 | ✅ 完了 |
| 2-3 | プレビューキャッシュ LRU | 低 | 中 | ✅ 完了 |
| 2-4 | プレビューバックグラウンドロード | 低 | 大 | ✅ 完了 |
| 2-5 | マウントパス変更時の未変換スキャン | 低 | 小 | ✅ 完了 |
| 3-3 | ScanAndQueueUnimported マウント対応 | 低 | 小 | ✅ 完了 |
| 1-1 | 複数選択 | 低 | 大 | ✅ 完了 |
| 1-2 | コピー&ペースト (Duplicate) | 低 | 大 | ✅ 完了 |
| 1-3 | D&D によるファイル移動 | 低 | 大 | ✅ 完了 |
| 5-1 | テクスチャ原本の直接使用 (.fztex 廃止) | 低 | 小 | ✅ 完了 |
| 5-2 | アセット依存関係ビュー (Find References) | 低 | 中 | ✅ 完了 |
| 5-3 | 複数選択での一括削除・複製 | 低 | 小 | ✅ 完了 |
| 5-4 | .fbzzanimcontroller / .fbzzterrain サムネイル | 低 | 小 | ✅ 完了 |
| 6-1〜6-6 | Unreal スタイル未保存アセット追跡 | — | — | ✅ 完了 |
