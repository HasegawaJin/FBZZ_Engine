# Engine / ResourceManager

`fbzz::core::ResourceManager`。Mesh・Texture の重複ロードを防ぐキャッシュ付きリソース管理クラス。
`Application` が所有し、シーンセットアップ時に参照する。

---

## クラス定義

```cpp
namespace fbzz::core {

class ResourceManager {
public:
    bool Init(renderer::IRenderer* renderer);
    void Shutdown();

    // 同じパスを渡した場合はキャッシュされた shared_ptr を返す
    std::shared_ptr<mesh::Mesh>         LoadMesh(const std::string& path);
    std::shared_ptr<renderer::ITexture> LoadTexture(const std::string& path);

    // use_count が 1 (自身のみ) のリソースをキャッシュから除去
    void UnloadUnused();

private:
    renderer::IRenderer* m_renderer = nullptr;

    std::unordered_map<std::string, std::shared_ptr<mesh::Mesh>>         m_meshCache;
    std::unordered_map<std::string, std::shared_ptr<renderer::ITexture>> m_textureCache;
};

} // namespace fbzz::core
```

---

## Application への統合

```cpp
// Application.hpp に追加
core::ResourceManager& GetResourceManager() { return *m_resourceManager; }

private:
std::unique_ptr<core::ResourceManager> m_resourceManager;
```

初期化順序:

```
Window::Init()
DX11Renderer::Init(hwnd, w, h)     ← IRenderer が先に必要
ResourceManager::Init(renderer)
Scene::Init()
```

---

## シェーダーとの棲み分け

| リソース | 管理クラス | 理由 |
|---------|-----------|------|
| Mesh | `ResourceManager` | Assimp ロード・CPU メモリキャッシュ |
| Texture | `ResourceManager` | GPU リソース、重複ロードが高コスト |
| Shader | `ShaderManager` | 既に独立実装済み |

---

## 使用例

```cpp
auto& rm = Application::Get().GetResourceManager();

// 同じパスなら同じ shared_ptr が返る (Assimp ロードは 1 回だけ)
auto mesh    = rm.LoadMesh("assets/meshes/Cube.obj");
auto texture = rm.LoadTexture("assets/textures/stone.dds");

// MeshRenderer に渡す
auto& mr = obj.AddComponent<MeshRenderer>();
mr.SetMesh(mesh, *m_renderer);
mr.SetMaterial(material);

// 不要になったリソースを解放 (シーン切り替え時等)
rm.UnloadUnused();
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Core/
│       └── ResourceManager.hpp
└── src/
    └── Core/
        └── ResourceManager.cpp
```

---

## 参考ドキュメント

- [std::unordered_map (cppreference)](https://en.cppreference.com/w/cpp/container/unordered_map) — パスをキーにしたハッシュマップキャッシュ
- [std::shared_ptr::use_count (cppreference)](https://en.cppreference.com/w/cpp/memory/shared_ptr/use_count) — UnloadUnused の参照カウント確認に使う
