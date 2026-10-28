# Editor メニューと Create の表示導線

## メニューの役割

操作は引き続き `Editor/src/Op/` の登録簿を正本とし、メニューは既存の operator ID を呼ぶ。
`EditorMenuCatalog.hpp` はパネル一覧と表示補助の投影だけを共有する。パネルの内部名、
`GetWindowName()`、既定表示、`ShowInViewMenu()`、保存済みホットキーは変更しない。

| 変更前 | 変更後 |
|---|---|
| View > Panels に登録順で全表示パネルを平置き | View 直下に Scene、Game、UI、Scene Hierarchy、Inspector、Asset Browser、Console、Undo History |
| View > Panels のアニメーション関連 | View > Animation に Animation Graph、Animation Mask Preview、Sequence |
| View > Panels の制作ツール | View > Effects & Audio に VFX Timeline、Fluid Editor、SFX Editor |
| View > Panels のアセット関連 | View > Assets に Preview、Sprite Editor、Dependency View、Asset Browser 2〜4 |
| View > Panels / Tools > Navigation | View > AI & Navigation に Behavior Tree、Navigation |
| View > Panels / Debug の診断パネル | View > Diagnostics に Performance Profiler、Script Profiler、Memory Debug、Render Pass Viewer、Build Output、必須設定チェック |
| View > Panels / AI > AI Settings | View > Settings に Project Settings、Hotkey Editor、AI Settings |
| Ctrl+K のみで発見しにくいコマンドパレット | View > Command Palette に実際の割り当てを表示 |
| Debug の多数の表示トグル | View > Scene Overlays。Grid、Stats、Scene Icons、Script Gizmosは直下。Physics、Animation、Effects、Navigation、Renderingは用途別に1段分類 |
| Debug > View Mode | View > View Mode。開発者用のRayモードは既存のDeveloper Mode条件を維持 |
| View のUI Scaleスライダー | View > UI Scale に倍率とリセットをまとめる |
| Tools > Terrain & Map > Terrain Tool | Tools > Terrain Tool |
| Tools > Open PIX / AI > Editor Command Bus | Debug > Open PIX / Editor Command Bus |

ToolsにはMap Editing Mode、Terrain Tool、Build Settings、IBL Baker、Volume Flipbook Baker、
Asset Maintenanceを置く。設定・ベイク・編集モードに従属するパネルの既存ライフサイクルは維持する。
DebugにはScene View Occlusion Culling、Hot Reload、Hot Reload Sound、Shadow、PIX、
Editor Command Bus、Developer Mode、開発者用Crash操作を置く。
File、Edit、GameObject、Help、およびPlayツールバーの役割は維持する。

`InstallNativeMenuBar()`にも同じ分類と表示補助一覧を適用する。現在のEditor起動経路はImGuiメニューを
使い、ネイティブメニューをインストールしない。ネイティブ側の既存Window APIはchecked/disabled状態や
スライダーを扱わないため、その制約は変わらない。OSメニュー内の数値IDは保存対象ではない。

## 表示・検索・保存

- Viewのパネル項目は従来どおり `panel.set_visible` を呼び、checkedを実際の可視状態から取得する。
- 閉じたパネルも一覧から消さない。追加のAsset Browserは別のIDで列挙し、各配置と開閉を保存する。
- パレットは非表示パネルに `Show:`、表示済みパネルに `Focus:` と `Hide:` を提示する。前面化は
  `panel.focus`、非表示は `panel.set_visible(visible=false)` を使う。背面タブへ一度で到達できる。
- 値が無いまま失敗する汎用 `Focus Panel` / `Show/Hide Panel` 行は、具体的な各パネル行へ置き換える。
- 登録済みパネルは1回ずつ分類する。将来のパネルはその `GetMenuCategory()`、未分類ならOther Panelsへ入れる。
- 保存は既存の `panelVisibility` と `imgui_layout.ini` のウィンドウIDを使う。起動時復元、Map/Playの
  一時レイアウトと通常レイアウトの保存分離、UI倍率の永続化は変更しない。
- `SearchEverythingPanel` の実装は存在するが現在のEditorへ登録されておらず、記載された
  Ctrl+Shift+Fも未接続。今回その名だけをメニューへ足さない。既存のCtrl+Kによる横断検索を使う。

## Hierarchy Create

Hierarchyの右クリック・上端CreateとGameObjectメニューは同じ `CreateObjectMenu` を使う。
Empty GameObject、Cube、Sphere、Plane、Camera、Target Pointを直下へ置き、
3D Object、Rendering、Light、Camera Rig、Environment、Terrain、Navigation、Effects、VFX、
Decal、Physics、Audio、Animation、Spline、UIを各1段で維持する。
GameObjectメニューだけにあったCreate EmptyとEmptyの重複を取り除く。

既存プリセットIDはすべて保持する。`3d.quad` は固定向きの板とフィットしたBox Colliderを作り、
旧 `QuadBillboardComponent` の登録状況に依存させない。カメラを向く板は既存の
`rendering.billboard`（BillboardComponent）を明示的に選ぶ。

Audioへ以下の実装済みAudioSource設定を使ったプリセットを追加する。音声アセットは推測せず、
作成後にInspectorでclipPathを割り当てる。

| 安定ID | 初期設定 |
|---|---|
| audio.spatialSource | spatialBlend=1、Loop/Play On Awakeはfalse |
| audio.ambient | spatialBlend=1、Loop/Play On Awakeはtrue、Streamingはfalse |
| audio.music | spatialBlend=0、Loop/Play On Awake/Streamingはtrue、既定BGMバス |

生成は従来の `node.create_preset` → `ObjectCreation` を通る。選択した親のローカル原点へ置き、
同じ親の下で名前を一意化し、生成物を選択する。ルート生成はScene Viewの注視点を使う。
UI要素は既存Canvasか自動Canvasへ入り、自動生成した子とCanvasも同じUndoで戻る。
Redoで同じGUIDを復元する契約も維持する。
Script Objectの説明はC#と必須コンポーネントへ修正する。

## 検証

`EditorMenuCatalogTest` は非連続カテゴリ、非表示パネル、追加Asset Browser、将来のパネルを含め、
列挙漏れ・重複・空サブメニュー・ID変更がないことを検証する。
`ObjectPresetMenuTest` は常用項目の浅い配置、既存カタログのID一意性とカテゴリ連続性、
Quad/Billboardの区別、音声初期値、親のローカル原点、命名、選択、Undo/Redoを検証する。

実UIではパネル項目のチェック、閉じたパネルの再表示、パレットでの背面タブ前面化、
追加Asset Browser、保存済みレイアウトでの起動復元、Map/Play往復、倍率変更を確認する。
