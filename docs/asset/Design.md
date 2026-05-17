# Asset System 設計書

`fbzz::asset` — アセットのロード・キャッシュ管理。RefCount は `shared_ptr` に委ねる。

---

## 依存関係

```
fbzz::asset
├── fbzz::renderer  (IRenderer, Mesh, ITexture)
└── third_party     (Assimp, DirectXTex)
```

---

## AssetManager

パスをキーにアセットをキャッシュする。同じパスへの 2 回目以降の `Load` はキャッシュを返す。  
`shared_ptr` の参照カウントが 0 になると自動解放される。

```cpp
class AssetManager {
public:
    static void SetBasePath(const std::string& basePath);  // 例: "assets/"

    template<typename T>
    static std::shared_ptr<T> Load(const std::string& relativePath);

    template<typename T>
    static void Unload(const std::string& relativePath);

    static void UnloadAll();

private:
    static std::string                                        s_basePath;
    static std::unordered_map<std::string, std::shared_ptr<void>> s_cache;
};
```

### 使い方

```cpp
AssetManager::SetBasePath("assets/");

auto mesh = AssetManager::Load<renderer::Mesh>("models/player.fbx");
auto tex  = AssetManager::Load<renderer::ITexture>("textures/player.png");

// shared_ptr が切れれば自動解放。明示的に解放する場合:
AssetManager::Unload<renderer::Mesh>("models/player.fbx");
```

---

## MeshImporter

Assimp ラッパー。`AssetManager::Load<Mesh>()` から内部的に呼ばれる。  
ゲームコードから直接呼ぶ必要はない。

```cpp
class MeshImporter {
public:
    static std::shared_ptr<renderer::Mesh> Import(
        const std::string& path,
        renderer::IRenderer& renderer);
};
```

読み込む頂点属性: Position / Normal / UV (テクスチャ座標 0 のみ)。  
複数メッシュが含まれる場合は最初のメッシュを使用する。

---

## TextureImporter

DirectXTex ラッパー。`AssetManager::Load<ITexture>()` から内部的に呼ばれる。

```cpp
class TextureImporter {
public:
    static std::shared_ptr<renderer::ITexture> Import(
        const std::string& path,
        renderer::IRenderer& renderer);
};
```

対応フォーマット: `.png` / `.jpg` / `.dds` / `.tga`。  
`.dds` は DirectXTex がそのまま読む。それ以外は WIC 経由でデコードする。
