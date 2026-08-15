# FBZZ Studio UI

更新日: 2026-07-26  
対象: `Projects/Editor/`

## 目的

FBZZ Editor と FBZZ VFX Editor を、深いネイビーとエレクトリックシアンを基調とする
一つの制作環境として見せる。個別パネルは RGB 値ではなく、`EditorTheme` の意味色を使う。

## パレット

| 意味色 | 基準色 | 用途 |
|---|---:|---|
| Canvas | `#0C1018` | 最背面、グラフ、Dock 空領域 |
| Surface | `#111722` | パネル、メニュー、ツールバー |
| Surface Raised | `#18202D` | タブ、カード、ボタン |
| Surface Hover | `#202B3B` | Hover、フォーカス候補 |
| Field | `#0D131D` | Input、Slider、検索欄 |
| Border | `#263345` | 通常境界 |
| Accent | `#39A8FF` | 主選択、フォーカス、リンク |
| Secondary | `#A987FF` | 副選択、特殊編集モード |
| Success | `#4CD59A` | Play、成功 |
| Warning | `#F3B85B` | Pause、未保存、警告 |
| Danger | `#FF667A` | Stop、失敗、破壊操作 |

## 実装規則

- `ImGuiStyle` の基準設定は `EditorTheme::Apply()` だけで行う。
- 独自描画は `EditorTheme::Color()` / `ColorU32()` を使う。
- ImNodes はコンテキスト作成直後に `EditorTheme::ApplyImNodes()` を呼ぶ。
- Graphリンクは横距離を主成分にした制限付きBezierを使い、縦距離だけで大きく膨らませない。
- Play / Pause の背景識別は `ApplyWorkspaceTint()` を使い、パネル側で背景色を上書きしない。
- Inspector の区切りには `widgets::SectionHeader()` を使う。
- 選択表示には `SelectionVisuals.hpp` を使い、Hierarchy と Asset Browser で表現を分けない。
- 機能固有色が必要な場合も、状態色または Accent / Secondary を基準に派生させる。

## 視覚階層

1. Canvas
2. Surface
3. Surface Raised
4. Hover / Active
5. Accent

常時 Accent で面全体を塗らず、選択線、チェック、リンク、重要操作へ限定する。
これにより長時間の制作でも眩しさを抑え、編集対象へ視線を集中させる。
