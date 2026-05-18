# Asset System 設計書

`fbzz::asset` — アセットのロード・キャッシュ管理。RefCount は `shared_ptr` に委ねる。

---

## 依存関係

```
fbzz::asset
├── fbzz::renderer  (IRenderer, Mesh, Material, ITexture)
└── third_party/Assimp   ← DirectXTex は DX11Renderer が内部処理するため asset 層では触らない
```

> **シェーダーは管理しない。** シェーダーは `fbzz::renderer::ShaderManager` が担当する。
> AssetManager が扱うのは **Model** (FBX 等) と **Texture** (PNG/JPG/DDS) のみ。

---

## 設計方針

| 決定事項 | 理由 |
|---------|------|
| static クラス | `ShaderManager` と同パターン。`Init(renderer)` で一度だけ注入 |
| `Load<Model>` に統一。`Load<Mesh>` は作らない | FBX = Model (メッシュ群 + マテリアル群) が正確な概念。単一メッシュも `model->meshes[0]` で取得できる |
| 型別キャッシュ | `s_models` / `s_textures` を分離。`shared_ptr<void>` の unsafe cast を避ける |
| キャッシュは keep-alive | `shared_ptr` で保持。外部参照が切れても `UnloadAll()` まで解放しない |
| TextureImporter を作らない | `renderer.CreateTexture(path)` が DirectXTex を完全実装済み。asset 層が DirectXTex に触れると DX12 移行時に asset 層まで書き直しが必要になる |
| 失敗時は nullptr を返す | キャッシュには格納しない。呼び出し元で assert する |
| ASCII パスのみ | DX11Texture が `std::wstring(path.begin(), path.end())` で変換しているため |

---

## DirectX 制約と Assimp フラグの根拠

ModelImporter が正しく動くには以下の変換が必要。実装時に迷わないよう根拠を明記する。

### 座標系: 左手系

`math/Matrix4.cpp` の `LookAt` / `Perspective` に「DirectX 左手系」と明記されている。  
+Z がカメラの奥方向。

Assimp のデフォルト出力は**右手系** (OpenGL 準拠、+Z がカメラ手前方向)。  
→ **`aiProcess_MakeLeftHanded`** で Z 軸を反転して左手系に変換する。

### ワインディング順: 時計回り (CW)

DX11 デフォルトのラスタライザは **CW = 表面** として背面カリングする。  
右手系で CCW だった面は、左手系変換 (Z 反転) により **CW に変わる**。  
→ **`aiProcess_FlipWindingOrder`** で変換する。

> `aiProcess_MakeLeftHanded` と `aiProcess_FlipWindingOrder` は必ずセットで使う。  
> どちらか片方だけだと表面・裏面が逆になる。

### UV: 左上原点

DX11 は UV の左上が (0,0)。OpenGL / Assimp デフォルトは左下が (0,0)。  
→ **`aiProcess_FlipUVs`** で V 座標を反転する。

### ノード階層をベイク

Assimp はシーングラフ (aiNode ツリー) にトランスフォームを持つ。  
例: Blender の FBX エクスポートでは "Armature" ノードが 100x スケールを持つことがある。  
Asset System はノード階層を使わないため、頂点座標にトランスフォームをベイクする。  
→ **`aiProcess_PreTransformVertices`** でノード変換を頂点に適用してから GPU にアップロード。

> `aiProcess_PreTransformVertices` 使用後は `aiScene::mRootNode` 以外のノードが無効になる。  
> ノード階層が必要になる場合 (アニメーション等) は Step 6 以降で設計し直す。

### インデックス: uint32_t (32bit)

`DX11Renderer::CreateIndexBuffer` のコメントより:  
`DXGI_FORMAT_R32_UINT` 固定。MeshImporter は `uint32_t` でインデックス配列を作ること。

### まとめ: Assimp フラグ

```cpp
const unsigned int ASSIMP_FLAGS =
    aiProcess_Triangulate            // ポリゴンを三角形に分割
  | aiProcess_JoinIdenticalVertices  // 重複頂点をマージ (省略すると頂点数が爆発する)
  | aiProcess_GenNormals             // 法線がなければ面法線を生成
  | aiProcess_MakeLeftHanded         // 右手系 → 左手系 (Z 反転)
  | aiProcess_FlipWindingOrder       // CCW → CW (MakeLeftHanded とセット)
  | aiProcess_FlipUVs                // V 反転 (OpenGL → DX11)
  | aiProcess_PreTransformVertices;  // ノード変換を頂点にベイク
```

---

## Model 構造体

`fbzz::asset` 名前空間に定義する。ファイルロードの単位 = Model。

```cpp
// engine/include/engine/Asset/Model.hpp
#pragma once
#include <vector>
#include <memory>

namespace fbzz::renderer { struct Mesh; class Material; }

namespace fbzz::asset {

struct Model {
    std::vector<std::shared_ptr<renderer::Mesh>>     meshes;
    std::vector<std::shared_ptr<renderer::Material>> materials;  // meshes[i] と 1:1
};

} // namespace fbzz::asset
```

---

## AssetManager

```cpp
// engine/include/engine/Asset/AssetManager.hpp
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::renderer { class IRenderer; class ITexture; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class AssetManager {
public:
    // Application 初期化直後に一度だけ呼ぶ。二重呼び出しは assert で検出する
    static void Init(renderer::IRenderer& renderer, const std::string& basePath = "assets/");

    // UnloadAll → ShaderManager::Shutdown → app.Shutdown の順で呼ぶこと
    static void UnloadAll();

    // Load<T>: 対応型は Model / ITexture のみ
    // 未対応型はヘッダー内の static_assert によりコンパイルエラーになる
    template<typename T>
    static std::shared_ptr<T> Load(const std::string& relativePath) {
        static_assert(sizeof(T) == 0,
            "AssetManager::Load<T>: unsupported type. Use Model or ITexture.");
        return nullptr;
    }

    template<typename T>
    static void Unload(const std::string& relativePath) {
        static_assert(sizeof(T) == 0,
            "AssetManager::Unload<T>: unsupported type.");
    }

private:
    static renderer::IRenderer* s_renderer;  // 非所有。Init()〜UnloadAll() の間のみ有効
    static std::string          s_basePath;
    static bool                 s_initialized;

    static std::unordered_map<std::string, std::shared_ptr<Model>>            s_models;
    static std::unordered_map<std::string, std::shared_ptr<renderer::ITexture>> s_textures;

    static std::string Normalize(const std::string& path);  // '\\' → '/'
};

// 明示的特殊化の宣言
template<> std::shared_ptr<Model>              AssetManager::Load<Model>(const std::string&);
template<> std::shared_ptr<renderer::ITexture> AssetManager::Load<renderer::ITexture>(const std::string&);
template<> void AssetManager::Unload<Model>(const std::string&);
template<> void AssetManager::Unload<renderer::ITexture>(const std::string&);

} // namespace fbzz::asset
```

### Init の二重呼び出し検出

```cpp
void AssetManager::Init(renderer::IRenderer& renderer, const std::string& basePath) {
    assert(!s_initialized && "AssetManager::Init() は一度だけ呼ぶこと");
    s_renderer    = &renderer;
    s_basePath    = basePath;
    s_initialized = true;
}
```

### Load の内部フロー

```
Load<Model>("models/player.fbx")
├── assert(s_initialized)
├── key = Normalize("models/player.fbx")
├── s_models.count(key) > 0  →  shared_ptr を返す (終了)
└── 存在しない
    ├── fullPath = s_basePath + key
    ├── model = ModelImporter::Import(fullPath, *s_renderer)
    ├── model == nullptr  →  FBZZ_LOG_ERROR / nullptr を返す (キャッシュに入れない)
    ├── s_models[key] = model
    └── shared_ptr を返す

Load<ITexture>("textures/player.png")
├── assert(s_initialized)
├── key = Normalize(...)
├── s_textures.count(key) > 0  →  返す
└── 存在しない
    ├── tex = s_renderer->CreateTexture(s_basePath + key)
    ├── tex == nullptr  →  FBZZ_LOG_ERROR / nullptr を返す
    ├── s_textures[key] = tex
    └── shared_ptr を返す
```

---

## ModelImporter

```cpp
// engine/include/engine/Asset/ModelImporter.hpp
#pragma once
#include <memory>
#include <string>

namespace fbzz::renderer { class IRenderer; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class ModelImporter {
public:
    // 失敗時は nullptr を返す
    static std::shared_ptr<Model> Import(
        const std::string& path,
        renderer::IRenderer& renderer);
};

} // namespace fbzz::asset
```

### 変換フロー

```cpp
// ModelImporter.cpp の擬似コード
std::shared_ptr<Model> ModelImporter::Import(const std::string& path, IRenderer& renderer)
{
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, ASSIMP_FLAGS);

    if (!scene || scene->mNumMeshes == 0) {
        FBZZ_LOG_ERROR("ModelImporter: %s の読み込み失敗", path.c_str());
        return nullptr;
    }

    auto model = std::make_shared<Model>();

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* aim = scene->mMeshes[mi];

        // --- 頂点配列 ---
        std::vector<renderer::Vertex> vertices(aim->mNumVertices);
        for (uint32_t i = 0; i < aim->mNumVertices; ++i) {
            vertices[i].position = { aim->mVertices[i].x,
                                     aim->mVertices[i].y,
                                     aim->mVertices[i].z };
            vertices[i].normal   = { aim->mNormals[i].x,
                                     aim->mNormals[i].y,
                                     aim->mNormals[i].z };
            if (aim->mTextureCoords[0])
                vertices[i].uv = { aim->mTextureCoords[0][i].x,
                                   aim->mTextureCoords[0][i].y };
        }

        // --- インデックス配列 (uint32_t 固定: DXGI_FORMAT_R32_UINT) ---
        std::vector<uint32_t> indices;
        indices.reserve(aim->mNumFaces * 3);
        for (uint32_t fi = 0; fi < aim->mNumFaces; ++fi) {
            const aiFace& f = aim->mFaces[fi];
            indices.push_back(f.mIndices[0]);
            indices.push_back(f.mIndices[1]);
            indices.push_back(f.mIndices[2]);
        }

        // --- GPU バッファ生成 ---
        auto mesh = std::make_shared<renderer::Mesh>();
        mesh->vertexBuffer = renderer.CreateVertexBuffer(
            vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        mesh->indexBuffer  = renderer.CreateIndexBuffer(indices.data(), (uint32_t)indices.size());
        mesh->vertexCount  = (uint32_t)vertices.size();
        mesh->indexCount   = (uint32_t)indices.size();

        // --- マテリアル (albedo color のみ。テクスチャは呼び出し元が別途 Load<ITexture>) ---
        auto mat = std::make_shared<renderer::Material>();
        if (aim->mMaterialIndex < scene->mNumMaterials) {
            aiColor4D color(1, 1, 1, 1);
            scene->mMaterials[aim->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, color);
            mat->params.albedo = { color.r, color.g, color.b, color.a };
        }
        mat->Init(renderer);  // paramsBuffer を GPU に確保

        model->meshes.push_back(mesh);
        model->materials.push_back(mat);
    }

    return model;
}
```

---

## 使い方

```cpp
// main.cpp
asset::AssetManager::Init(renderer, "assets/");

// 単一メッシュ FBX
auto model = asset::AssetManager::Load<asset::Model>("models/cube.fbx");
go.AddComponent<scene::MeshRenderer>({ model->meshes[0], model->materials[0] });

// 多メッシュ FBX (キャラクター等)
auto chara = asset::AssetManager::Load<asset::Model>("models/character.fbx");
for (size_t i = 0; i < chara->meshes.size(); ++i) {
    auto& part = scene->CreateGameObject("Part_" + std::to_string(i));
    part.AddComponent<scene::MeshRenderer>({ chara->meshes[i], chara->materials[i] });
}

// テクスチャを後から差し込む
auto tex = asset::AssetManager::Load<renderer::ITexture>("textures/player.png");
model->materials[0]->albedoTexture = tex;

// シャットダウン順序 (必ず守る)
asset::AssetManager::UnloadAll();
renderer::ShaderManager::Shutdown();
app.Shutdown();
```

---

## engine/CMakeLists.txt の追加事項

```cmake
set(ASSIMP_INCLUDE   "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Assimp/include")
set(ASSIMP_LIB_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Assimp/lib/Debug/assimp-vc145-mtd.lib")
set(ASSIMP_LIB_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Assimp/lib/Release/assimp-vc145-mt.lib")

target_include_directories(fbzz_engine PRIVATE ${ASSIMP_INCLUDE})
target_link_libraries(fbzz_engine
    PRIVATE
        "$<$<CONFIG:Debug>:${ASSIMP_LIB_DEBUG}>"
        "$<$<CONFIG:Release>:${ASSIMP_LIB_RELEASE}>"
)
```

DLL を実行ファイルと同じディレクトリにコピーする (sandbox/CMakeLists.txt に追加):

```cmake
add_custom_command(TARGET sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<$<CONFIG:Debug>:${CMAKE_SOURCE_DIR}/third_party/Assimp/dll/Debug/assimp-vc145-mtd.dll>"
        "$<$<CONFIG:Release>:${CMAKE_SOURCE_DIR}/third_party/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:sandbox>"
)
```

---

## 変更ファイル一覧

| 種別 | ファイル |
|------|---------|
| 新規 | `engine/include/engine/Asset/Model.hpp` |
| 新規 | `engine/include/engine/Asset/AssetManager.hpp` |
| 新規 | `engine/src/Asset/AssetManager.cpp` |
| 新規 | `engine/include/engine/Asset/ModelImporter.hpp` |
| 新規 | `engine/src/Asset/ModelImporter.cpp` |
| 修正 | `engine/CMakeLists.txt` (Assimp include / lib 追加) |
| 修正 | `sandbox/CMakeLists.txt` (Assimp DLL コピー追加) |
| 修正 | `sandbox/src/main.cpp` (AssetManager::Init / FBX ロード / UnloadAll 追加) |

---

## 既知の制約

| 制約 | 内容 |
|------|------|
| ASCII パスのみ | DX11Texture の `wstring(path.begin(), path.end())` による制約 |
| テクスチャは自動ロードしない | FBX 埋め込みテクスチャパスは無視。別途 `Load<ITexture>` で取得する |
| バッファは DYNAMIC | 現行 DX11Buffer が DYNAMIC 固定のため静的メッシュも DYNAMIC になる。Step 6 で IMMUTABLE に移行 |
| ノード階層は無視 | `PreTransformVertices` でベイク済み。アニメーションは Step 6 以降 |
| 法線: 非一様スケール非対応 | HLSL が `mul(normal, (float3x3)world)` を使用。Step 6 で inverse-transpose に変更予定 |
| `aiProcess_FlipUVs` 無条件 | DCC ツール側で既に DX 向けフリップ済みのモデルは二重フリップになる可能性あり |
