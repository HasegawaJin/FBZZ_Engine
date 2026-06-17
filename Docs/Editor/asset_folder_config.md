# Asset Browser: ユーザー定義フォルダ構成 & 拡張子レジストリ

## 背景・課題

現状の Asset Browser は以下が全てハードコードされている。

| 箇所 | ハードコードされている内容 |
|------|--------------------------|
| `AssetBrowserCore.cpp UpdateMounts` | "Scripts" / "Shaders" フォルダのみマウント |
| `AssetBrowserPanel.cpp passesTypeFilter` | 拡張子リスト (`.mat`, `.hlsl` 等) が switch 文に直書き |
| `AssetBrowserPanel.hpp TypeFilter` | アセット種別 enum が固定 |

ユーザーが独自フォルダ (例: `Assets/VFX`, `Assets/Levels`) を作ったり、
独自拡張子 (プロジェクト固有スクリプト型等) を追加したりできない。

---

## 設計方針

### A. `AssetTypeRegistry`（Editor/Util 新規）

**目的:** 拡張子 → アセット型情報のエンジン側一元管理。  
`passesTypeFilter` 内の switch 文をレジストリ参照に置き換え、
後から新しい拡張子を登録できるようにする。

```cpp
// Editor/Util/AssetTypeRegistry.hpp
struct AssetTypeInfo {
    std::string  label;        // "Material", "Script", ...
    std::string  typeKey;      // 内部キー。フォルダ設定の typeKey と対応
    ImVec4       iconColor;    // アイコン色 (0,0,0,0 = デフォルト)
};

class AssetTypeRegistry {
public:
    // エンジン既定型の一括登録 (EditorApp::OnInit で 1 回呼ぶ)
    static void RegisterDefaults();
    // 追加登録 (プロジェクト設定ロード後に呼ぶ)
    static void Register(std::string_view ext, AssetTypeInfo info);

    // 拡張子 (小文字 ".xxx") → 型情報。未登録なら nullptr
    static const AssetTypeInfo* Find(std::string_view ext);
    // typeKey に属する全拡張子を返す ("material" → [".mat"])
    static std::vector<std::string> ExtensionsForKey(std::string_view typeKey);
    // 登録済み型一覧 (UI Combo 用)
    static const std::vector<std::pair<std::string, AssetTypeInfo>>& GetAll();

private:
    static std::unordered_map<std::string, AssetTypeInfo> s_byExt;   // ".mat" → info
    static std::vector<std::pair<std::string, AssetTypeInfo>> s_all; // 表示順リスト
};
```

既定登録内容 (RegisterDefaults):

| typeKey | 拡張子 |
|---------|--------|
| `scene`      | `.fbzz` |
| `material`   | `.mat` |
| `script`     | `.hpp .cpp .h .c .cc .cxx .py .lua .cs` |
| `texture`    | `.png .jpg .jpeg .dds .bmp .tga .ico .fnt .ttf .otf` |
| `audio`      | `.wav .mp3 .ogg .flac` |
| `mesh`       | `.fbx .obj .gltf .glb` |
| `shader`     | `.hlsl .hlsli` |
| `prefab`     | `.prefab` |
| `model`      | `.asset` |
| `animation`  | `.animcontroller` |
| `graph_layout` | `.fbzz.animgraph` |
| `terrain`    | `.terrain` |
| `water`      | `.water` |
| `data`       | `.toml .json .yaml .yml` |
| `text`       | `.txt .md .rst` |

`.mesh` / `.skel` / `.anim` は `.fbx` から生成される内部バイナリであり、
ユーザーが直接選択する入口は `.asset` とする。そのため型レジストリの公開対象には含めず、
Asset Browser でも非表示にする。

---

### B. `AssetFolderConfig`（EditorSettings 拡張）

**目的:** マウント先フォルダをユーザーが自由に追加・編集・削除できるようにする。  
現在ハードコードの Scripts / Shaders マウントをここに移行する。

```cpp
// EditorSettings.hpp に追加
struct AssetFolderEntry {
    std::string displayName;   // Asset Browser 左ペインに表示する名前
    std::string path;          // 絶対パス OR projectRoot からの相対パス
    std::string typeKey;       // 任意。設定時はこのフォルダ内のデフォルトフィルタになる
                               // 例: "script" にするとこのフォルダを開いたら Script フィルタが ON
};

// EditorSettings に追加
std::vector<AssetFolderEntry> customFolders;  // TOML: [[custom_folders]]
```

TOML 例 (`editor_settings.toml`):
```toml
[[custom_folders]]
display_name = "Scripts"
path = "Assets/Scripts"
type_key = "script"

[[custom_folders]]
display_name = "Shaders"
path = "Assets/Shaders"
type_key = "shader"

[[custom_folders]]
display_name = "VFX"
path = "Assets/VFX"
type_key = ""
```

---

### C. Asset Browser 統合

#### C-0. 内部ファイルの非表示

未登録拡張子を一律に隠すと、プロジェクト固有拡張子を追加できる本設計の目的と衝突する。
そのため表示判定は許可リストではなく、生成物・内部ファイルだけを明示的に除外する。

| 非表示対象 | 理由 |
|-----------|------|
| `.*` | OS / VCS の管理ファイルであり、アセット操作対象ではない |
| `*.generated.hpp` | FBZZ Header Tool が `.hpp` から再生成する |
| `*.tex` | 廃止済みのテクスチャメタデータ。画像原本を直接使用する |
| `*.mesh` / `*.skel` / `*.anim` | FBX importer が生成し、`.asset` から参照する内部バイナリ |
| `*.cso` / `Shaders/compiled/` | HLSL から再生成するシェーダーバイナリ |
| `*.dll` / `*.lib` / `*.pdb` / `*.exp` / `*.ilk` | コンパイラ・リンカーが生成するビルド成果物 |
| `*.tmp` / `*.bak` | 一時・バックアップファイル |
| `compile_*.bat` / `compile*_log.txt` | エンジン内蔵シェーダーの保守用スクリプト・ログ |

この判定はアセット一覧とフォルダツリーの両方で共通利用し、
検索や Type Filter より前に適用する。

#### C-1. `UpdateMounts` の置き換え
`AssetBrowserCore.cpp` の `UpdateMounts`:
- `scriptsSourceDir` / `hlslSourceDir` のハードコードロジックを廃止
- `ctx.editorSettings.customFolders` を読んでマウントリストを構築
- 起動時に `customFolders` が空なら Scripts / Shaders のデフォルトエントリを自動挿入

#### C-2. `passesTypeFilter` のレジストリ化
`AssetBrowserPanel.cpp`:
```cpp
// Before
case TypeFilter::Material: return e.ext == ".mat";

// After
case TypeFilter::Material:
    return AssetTypeRegistry::Find(e.ext) &&
           AssetTypeRegistry::Find(e.ext)->typeKey == "material";
```

#### C-3. フォルダ移動時の自動フィルタ切替 (オプション)
`HandleEntryDoubleClick` や `DrawFolderTree` でフォルダに入る際、
そのフォルダが `customFolders` に登録されていて `typeKey` が設定されていれば
`m_typeFilter` を対応する値に自動セットする。

---

### D. フォルダ設定 UI

Asset Browser のツールバーに `[Folders ⚙]` ボタンを追加。
クリックで ImGui モーダルを開く。

```
┌─────────────────────────────────────────────────────────┐
│  Folder Configuration                              [X]  │
├──────────────────┬───────────────────────┬─────────────┤
│  Display Name    │  Path                 │  Type Key   │
├──────────────────┼───────────────────────┼─────────────┤
│  Scripts         │  Assets/Scripts       │  script  [▼]│
│  Shaders         │  Assets/Shaders       │  shader  [▼]│
│  VFX             │  Assets/VFX           │       ── [▼]│
│  [+ Add]         │                       │  [×] Delete │
└──────────────────┴───────────────────────┴─────────────┘
│                                         [Save] [Cancel] │
└─────────────────────────────────────────────────────────┘
```

実装クラス: `AssetBrowserPanel::DrawFolderConfigDialog()` (AssetBrowserItems.cpp)

保存時: `m_settings.customFolders` を更新 → `m_settings.Save()` → `UpdateMounts` 再実行

---

### E. EditorContext / EditorApp の整理

- `EditorContext::scriptsSourceDir` / `hlslSourceDir` は hot reload (ToolchainLocator) のためだけに残す
- `UpdateMounts` はこれらを参照しなくなるが、スクリプトコンパイルパスとして引き続き使用
- `EditorApp::OpenProject` でプロジェクトロード後に `RegisterDefaults()` を呼ぶ

---

## ファイル変更リスト

| ファイル | 変更内容 |
|---------|---------|
| `Editor/Util/AssetTypeRegistry.hpp` (新規) | 拡張子レジストリ定義 |
| `Editor/Util/AssetTypeRegistry.cpp` (新規) | RegisterDefaults + 実装 |
| `Editor/Util/EditorSettings.hpp` | `AssetFolderEntry` 構造体 + `customFolders` フィールド追加 |
| `Editor/Util/EditorSettings.cpp` | TOML load/save の `[[custom_folders]]` 配列対応 |
| `Editor/EditorContext.hpp` | `const EditorSettings*` ポインタを追加、または customFolders を直接持つ |
| `Editor/EditorApp.cpp` | `AssetTypeRegistry::RegisterDefaults()` 呼び出し |
| `AssetBrowser/AssetBrowserCore.cpp` | `UpdateMounts` をレジストリ/設定ベースに置き換え |
| `AssetBrowser/AssetBrowserCore.cpp` | 生成物・内部ファイルの共通非表示判定を追加 |
| `AssetBrowserPanel.cpp` | `passesTypeFilter` をレジストリ参照に変更、ツールバーに `[Folders ⚙]` ボタン追加 |
| `AssetBrowserPanel.hpp` | `TypeFilter` を動的対応に変更、`DrawFolderConfigDialog` 宣言追加 |
| `AssetBrowser/AssetBrowserItems.cpp` | `DrawFolderConfigDialog` 実装、フォルダツリーにも非表示判定を適用 |

---

## 実装順序

1. `AssetTypeRegistry` を実装（基盤、他に依存なし）
2. `EditorSettings` に `AssetFolderEntry` / `customFolders` を追加（TOML 対応）
3. `EditorApp::OpenProject` で `RegisterDefaults()` を呼ぶ
4. `UpdateMounts` を customFolders ベースに移行（デフォルト自動挿入付き）
5. `passesTypeFilter` をレジストリ参照に変更
6. `DrawFolderConfigDialog` UI 実装
7. フォルダ移動時の自動フィルタ切替 (オプション)

---

## 検証方法

1. `customFolders` が空の初回起動 → Scripts / Shaders が自動挿入されてマウント表示される
2. `[Folders ⚙]` から "VFX" フォルダを追加 → 左ペインに表示、再起動後も保持
3. Type フィルタ "Material" → `.mat` のみ (AssetTypeRegistry 経由で一致)
4. カスタム拡張子 `.myasset` を Registry に追加 → "Custom" フィルタに表示
5. typeKey 付きフォルダに入ったとき自動フィルタが切り替わる
6. `.generated.hpp` / importer・shader 生成物が一覧とツリーに表示されない
7. `.fbzz.animgraph` が Graph Layout として表示され、選択・移動・名前変更・削除できる
