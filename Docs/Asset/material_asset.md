# Material Asset 設計書

## 概要

現在の `MaterialComponent` はシェーダーパス・パラメータ・テクスチャパスを GameObject に直接埋め込んでいる。  
複数 GameObject が同じマテリアルを使う場合でもデータが重複し、エディタ上での一括編集ができない。

本設計は Unity の `.mat` ファイルに相当する **スタンドアロンの `.mat` アセット** を導入し、  
`MaterialComponent` がそのアセットへの参照を持つ形に移行する。

---

## 目標

- `.mat` ファイルを Editor から作成・保存・ロードできる
- `MaterialComponent` は `MaterialAsset` の参照 (パス) のみを保持する
- 複数の GameObject が同一 `.mat` を共有し、片方の変更が全体に反映される
- 既存の `FzAssetFormat.hpp` / `AssetManager` の拡張として収まる

---

## 非目標

- キーワードベースのシェーダーマルチバリアント管理  
  (スタティック / スキンの 2 分岐は RenderSystem が自動処理するため対応済み)
- ランタイム (ゲームプレイ中) での `.mat` ホットリロード  
  (Editor 上での保存 → 即反映は対応済み)

---

## ファイルフォーマット `.mat`

テキスト (TOML) 形式を採用する。既存の `FzMaterialExporter` が toml++ を使用しているため統一する。  
バイナリ (`FzAssetFormat.hpp` の他フォーマット) と異なりテキストにする理由:

- マテリアルパラメータは数十バイト〜数百バイト程度で、バイナリ化によるサイズ優位が小さい
- Git の diff・レビューが読める
- シェーダー変数名をキーにするため、バイナリの固定オフセットより名前ベースの方が保守しやすい

### スキーマ

```toml
version = 1
shader = ""           # 空 = RenderSystem がメッシュ種別から自動選択
blend_mode = "Opaque" # "Opaque" / "AlphaBlend" / "Additive"
double_sided = false
render_queue = 2000

[textures]
albedo   = "textures/rock_albedo.png"
normal   = "textures/rock_normal.png"
metallic = ""
roughness = ""
emissive  = ""
ao        = ""

[params]
base_color       = [1.0, 1.0, 1.0, 1.0]
metallic_factor  = 0.0
roughness_factor = 0.65
normal_strength  = 1.0
emissive_color   = [1.0, 1.0, 1.0]
emissive_scale   = 0.0
```

| フィールド | 型 | 説明 |
|---|---|---|
| `version` | int | フォーマットバージョン (破壊的変更時にインクリメント) |
| `shader` | string | `assets/` 相対パス。**空文字 = RenderSystem が自動選択**。FBX インポート時は常に空 |
| `blend_mode` | string | `"Opaque"` / `"AlphaBlend"` / `"Additive"` |
| `double_sided` | bool | 両面描画フラグ |
| `render_queue` | int | 描画優先度 (Unity 互換値) |
| `[textures]` | table | スロット名 → `assets/` 相対パス。`.png` / `.dds` / `.tga` を直接指定。未使用は `""` |
| `[params]` | table | シェーダー変数名 → 値。scalar は float、vector は float array |

#### テクスチャスロット

`FzMaterialExporter` の `kTexSlots` と対応する 6 スロット固定:

| スロット名 | Assimp 型 | 対応する PBR テクスチャ |
|---|---|---|
| `albedo` | `aiTextureType_DIFFUSE` | ベースカラー |
| `normal` | `aiTextureType_NORMALS` | 法線マップ |
| `metallic` | `aiTextureType_METALNESS` | メタリック |
| `roughness` | `aiTextureType_DIFFUSE_ROUGHNESS` | ラフネス |
| `ao` | `aiTextureType_AMBIENT_OCCLUSION` | アンビエントオクルージョン |
| `emissive` | `aiTextureType_EMISSIVE` | エミッシブ |

---

## インメモリ表現

```cpp
// Engine/Asset/MaterialAsset.hpp
namespace fbzz::asset {

struct MaterialAsset {
    std::string shaderPath;
    renderer::BlendMode blendMode   = renderer::BlendMode::OPAQUE_BLEND;
    bool                doubleSided = false;
    int32_t             renderQueue = renderer::RenderQueue::GEOMETRY;

    // slot 名 → assets/ 相対パス ("" = 未使用)
    std::unordered_map<std::string, std::string> textures;

    // 変数名 → 生バイト列 (float, float2, float3, float4 を共用)
    // WHY: ShaderDescriptor のオフセットに依存せず保存し、ロード時にバインドする。
    //      シェーダーが変わっても同名変数があれば値を引き継げる。
    std::unordered_map<std::string, std::vector<float>> params;
};

} // namespace fbzz::asset
```

---

## AssetManager 拡張

`AssetManager::Load<MaterialAsset>` の特殊化を追加する。  
既存の `Load<Model>` と同じキャッシュ方式 (`s_materials` マップ) を使う。

```cpp
// 追加するキャッシュ
static std::unordered_map<std::string, std::shared_ptr<MaterialAsset>> s_materials;

// 特殊化
template<> std::shared_ptr<MaterialAsset> AssetManager::Load<MaterialAsset>(const std::string&);
template<> void AssetManager::Unload<MaterialAsset>(const std::string&);
```

ロード手順:
1. `s_materials` にキャッシュがあれば返す
2. `basePath + relativePath` を開いて TOML をパース (toml++)
3. `MaterialAsset` に詰めてキャッシュ登録・返却

---

## MaterialComponent の変更

現在の `MaterialComponent` が持つ `paramData` / `texturePaths` / `blendMode` / `doubleSided` / `renderQueue` を  
`MaterialAsset` に移動し、Component 自体は **参照パスのみ** を保持する。

```
変更前:
  MaterialComponent
    shaderPath, blendMode, doubleSided, renderQueue
    paramData (vector<uint8_t>)
    texturePaths (vector<string>)
    material (shared_ptr<Material>)

変更後:
  MaterialComponent
    materialPath  (string)          ← .mat の assets/ 相対パス
    materialAsset (shared_ptr<MaterialAsset>)  ← AssetManager 経由でロード済み
    material      (shared_ptr<Material>)       ← GPU リソース (変わらず)
```

`MaterialComponent::Reflect()` は `materialPath` だけをシリアライズする。  
ロード時に `AssetManager::Load<MaterialAsset>(materialPath)` でアセットを復元する。

### オーバーライドパラメータ (将来拡張・今回は非目標)

GameObject 固有の値を持ちたい場合は `overrides` フィールドを後から追加する。  
今回は共有アセットの値をそのまま使う設計とし、複雑性を避ける。

---

## シェーダー未設定時のフォールバック

### フォールバックが必要なシナリオ

| ケース | 発生条件 |
|---|---|
| **A: materialPath 未設定** | `MaterialComponent` に `.mat` が割り当てられていない |
| **B: .mat ファイルが見つからない** | パスが間違っている、またはファイルが削除された |
| **C: shader が空** | `shader = ""` → **正常ケース**。RenderSystem がメッシュ種別から自動選択 |
| **D: シェーダーロード失敗** | `ResourceManager::LoadShader()` が `Null()` を返した (ファイル不在・コンパイルエラー) |

### フォールバックマテリアル (BuiltIn)

エンジン起動時に **エラーマテリアル** を組み込みリソースとして登録する。  
Unity の "Missing" マテリアルと同様にマゼンタ単色で描画し、問題箇所をすぐに視認できるようにする。

```
assets/shaders/BuiltIn/ErrorUnlit.hlsl
  — CB なし、テクスチャなし
  — PixelShader が float4(1, 0, 1, 1) を返す固定色シェーダー
```

`ResourceManager::Init()` 内でこのシェーダーをロードし、エラーマテリアルとして保持する:

```cpp
// ResourceManager が持つ
ResourceHandle<ShaderTag>   m_errorShader;   // ErrorUnlit.hlsl
std::shared_ptr<Material>   m_errorMaterial; // Init() 時に構築
```

### ケース別の挙動

```
ケース A (materialPath 未設定)
  → RenderSystem がスキップ (Draw Call を発行しない)
  → Inspector に "No Material assigned" 警告表示

ケース B (.mat 読み込み失敗)
  → AssetManager::Load<MaterialAsset>() が nullptr を返す
  → FBZZ_LOG_WARN("MaterialAsset not found: %s", path)
  → RenderSystem がエラーマテリアルで描画

ケース C (shader = "" → 自動選択)
  → 正常フロー。ResourceManager::LoadShader() は呼ばない
  → RenderSystem がコンポーネント種別でシェーダーを決定:
      MeshRendererComponent   → StandardPBR.hlsl
      SkinnedMeshComponent    → SkinnedPBR.hlsl

ケース D (シェーダーロード失敗)
  → ResourceManager::LoadShader() が Null() を返す (既存動作)
  → FBZZ_LOG_ERROR 済み (ResourceManager.cpp:53)
  → RenderSystem が Null ハンドルを検出してエラーマテリアルで代替描画
```

### RenderSystem での判定フロー

```
RenderSystem が MaterialComponent を処理するとき:

  if materialAsset == nullptr          → スキップ (ケース A) or エラーマテリアル (ケース B)
  else if shaderPath.empty()           → コンポーネント種別でシェーダーを自動選択 (ケース C)
  else if shaderHandle.IsNull()        → エラーマテリアルで描画 (ケース D)
  else                                 → 通常描画 (明示シェーダー)
```

ケース A はスキップ (何も描かない) とし、ケース B/D はマゼンタで描画する。  
`materialPath` が空なのは「意図的に非表示」、ファイルが壊れているのは「バグ」として区別する。

### Inspector での表示

| 状態 | Inspector 表示 |
|---|---|
| 正常 (明示シェーダー) | 通常の MaterialAsset プロパティ |
| 正常 (shader = "") | `(auto)` グレーテキストでシェーダーフィールドに自動選択シェーダー名を表示 |
| ケース A | `[No Material]` グレーテキスト |
| ケース B | `⚠ Missing: materials/foo.mat` 赤テキスト |
| ケース D | `⚠ Shader compile error` 赤テキスト、エラーログへのリンク |

### `ShaderDescriptor` フォールバック

ケース C/D では `ShaderDescriptor` が取得できないため `s_fallback`  
(空の `ShaderDescriptor`、`cbufferSize = 0`) を使う。  
これは既存の `InspectorPanel` の動作 (L.207) と同じ方式であり、変更不要。

---

## FBX インポートとの連携

FBX インポート時は `FzMaterialExporter::Export()` が `.mat` を自動生成する。  
マテリアルは「見た目の性質」であり、スキン有無はメッシュ側の性質のため `.mat` には含めない。

### FzMaterialExporter が出力するフィールド

| `.mat` フィールド | Assimp ソース | 備考 |
|---|---|---|
| `shader = ""` | (なし) | 常に空。RenderSystem が描画時に自動選択 |
| `[params].base_color` | `AI_MATKEY_BASE_COLOR` / `AI_MATKEY_COLOR_DIFFUSE` | フォールバックあり |
| `[params].metallic_factor` | `AI_MATKEY_METALLIC_FACTOR` | デフォルト 0.0 |
| `[params].roughness_factor` | `AI_MATKEY_ROUGHNESS_FACTOR` | デフォルト 0.5 |
| `[textures].albedo` | `aiTextureType_DIFFUSE` | |
| `[textures].normal` | `aiTextureType_NORMALS` | |
| `[textures].metallic` | `aiTextureType_METALNESS` | |
| `[textures].roughness` | `aiTextureType_DIFFUSE_ROUGHNESS` | |
| `[textures].ao` | `aiTextureType_AMBIENT_OCCLUSION` | |
| `[textures].emissive` | `aiTextureType_EMISSIVE` | |

`blend_mode` / `double_sided` / `render_queue` はインポート時には書き出さず、  
ローダーがデフォルト値 (`"Opaque"` / `false` / `2000`) で補完する。

### シェーダーの自動選択 (RenderSystem)

```
描画時:
  MaterialAsset::shaderPath が空 かつ SkinnedMeshComponent がある
    → SkinnedPBR.hlsl

  MaterialAsset::shaderPath が空 かつ MeshRendererComponent のみ
    → StandardPBR.hlsl

  MaterialAsset::shaderPath が非空
    → そのパスのシェーダーを使用 (エディタ上書き)
```

FBX 由来の `.mat` は `shader = ""` なので、同じアセットをスタティックメッシュとスキンメッシュの  
両方で共有しても RenderSystem が適切なシェーダーを選択する。

---

## Editor ワークフロー

### 新規作成

1. Asset Browser のコンテキストメニュー **[Create → Material]** を選択
2. `assets/materials/New Material.mat` を TOML テンプレートで生成
3. Inspector に `MaterialAsset` の編集 UI を表示

### 編集・保存

- Inspector でパラメータ / テクスチャを変更 → `MaterialAsset` のインメモリデータを更新
- **[Ctrl+S] / Save ボタン** で TOML にシリアライズして上書き保存 (toml++)
- 保存後、`AssetManager` のキャッシュを無効化してシーン内の参照を再ロード

### GameObject への割り当て

- Inspector の `MaterialComponent::materialPath` フィールドに `.mat` をドロップ
- または Asset Browser から GameObject へドラッグ＆ドロップ

---

## シーンシリアライズへの影響

`SceneSerializer` が `MaterialComponent` を保存する際、現在は以下をシリアライズしている:

```
shaderPath, blendMode, doubleSided, renderQueue, paramData, texturePaths
```

変更後は **`materialPath` のみ** を保存する。  
ロード時は `materialPath` を読んで `AssetManager::Load<MaterialAsset>()` を呼ぶだけで復元できる。

---

## 実装ステップ

1. **`MaterialAsset` 構造体** と TOML シリアライザ/デシリアライザを実装 (toml++)  
   (`Engine/Asset/MaterialAsset.hpp` / `.cpp`)

2. **`AssetManager` 特殊化** を追加  
   (`Load<MaterialAsset>` / `Unload<MaterialAsset>`)

3. **`MaterialComponent` をリファクタリング**  
   インライン保持フィールドを削除し `materialPath` / `materialAsset` に置き換え

4. **`RenderSystem` / `SyncMaterial`** を `MaterialAsset` から読むよう変更

5. **`SceneSerializer`** の保存・ロードを `materialPath` のみに変更  
   (既存シーンファイルのマイグレーション対応)

6. **Editor UI**  
   - Asset Browser への [Create → Material] メニュー追加  
   - `MaterialAsset` 用 Inspector パネルの実装  
   - ドラッグ＆ドロップ割り当て

---

## 既存アセットとの関係

| アセット種別 | ファイル | 担当 |
|---|---|---|
| Mesh | `.mesh` | `FzAssetLoader` + `AssetManager::Load<Model>` |
| Texture | `.png` / `.dds` / `.tga` | `AssetManager::LoadTexture` (`DX11Texture` が拡張子で分岐) |
| Skeleton / Anim | `.skel` / `.anim` | `FzAssetLoader` |
| **Material** | **`.mat`** | **本設計** |

`.mat` は他のバイナリアセットと異なり TOML テキストとする (toml++ で読み書き)。  
ただし `AssetManager` の `basePath` ベースのキャッシュ方式は統一する。
