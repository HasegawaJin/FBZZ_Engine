# Engine / Mesh

Assimp を使ったメッシュ読み込みと頂点フォーマット定義。

---

## 頂点フォーマット

DX11 の入力レイアウトと 1:1 に対応する。

```cpp
namespace fbzz::mesh {

struct Vertex {
    math::Vector3 position;   // POSITION : offset  0, 12 byte
    math::Vector3 normal;     // NORMAL   : offset 12, 12 byte
    math::Vector2 uv;         // TEXCOORD : offset 24,  8 byte
                           // 合計 32 byte / vertex
};

} // namespace fbzz::mesh
```

---

## Mesh 構造体

```cpp
namespace fbzz::mesh {

struct SubMesh {
    uint32_t m_indexOffset = 0;
    uint32_t m_indexCount  = 0;
    uint32_t m_materialIndex = 0;
};

struct Mesh {
    std::vector<Vertex>   m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<SubMesh>  m_subMeshes;    // マテリアル分割がある場合
    std::string           m_name;
};

} // namespace fbzz::mesh
```

---

## MeshLoader

```cpp
namespace fbzz::mesh {

class MeshLoader {
public:
    // OBJ / FBX / glTF 等を読み込む (Assimp が判別)
    static Mesh Load(const std::string& path);

private:
    static void ProcessNode(const aiNode* node, const aiScene* scene,
                            Mesh& outMesh);
    static void ProcessMesh(const aiMesh* mesh, Mesh& outMesh,
                            uint32_t indexOffset);
};

} // namespace fbzz::mesh
```

### Assimp フラグ

```cpp
unsigned int flags =
    aiProcess_Triangulate            // ポリゴンを三角形に分解
    | aiProcess_GenNormals           // 法線がなければ生成
    | aiProcess_FlipUVs              // DX の UV 座標系に合わせる
    | aiProcess_JoinIdenticalVertices;
```

---

## MeshRenderer Component

```cpp
namespace fbzz::scene {

class MeshRenderer : public Component {
public:
    void SetMesh(const mesh::Mesh& mesh, renderer::IRenderer& renderer);
    void SetMaterial(std::shared_ptr<renderer::Material> material);

    void OnUpdate(float dt) override;   // 描画コマンドを発行

private:
    std::shared_ptr<renderer::IBuffer>   m_vertexBuffer;
    std::shared_ptr<renderer::IBuffer>   m_indexBuffer;
    uint32_t                             m_indexCount = 0;
    std::shared_ptr<renderer::Material>  m_material;
};

} // namespace fbzz::scene
```

---

## 使用例

```cpp
// シーンセットアップ時
mesh::Mesh cubeMesh = mesh::MeshLoader::Load("assets/meshes/Cube.obj");

auto& obj = m_scene->CreateObject("Cube");
auto& mr  = obj.AddComponent<MeshRenderer>();
mr.SetMesh(cubeMesh, *m_renderer);
mr.SetMaterial(ShaderManager::Load("assets/shaders/Phong.hlsl"));
```

---

## 対応フォーマット (Assimp 依存)

| フォーマット | 拡張子 |
|------------|--------|
| OBJ | `.obj` |
| FBX | `.fbx` |
| glTF 2.0 | `.gltf`, `.glb` |
| Collada | `.dae` |

---

## ファイル構成

```
engine/
├── include/engine/
│   ├── Mesh/
│   │   ├── Mesh.hpp
│   │   └── MeshLoader.hpp
│   └── Scene/
│       └── MeshRenderer.hpp
└── src/
    ├── Mesh/
    │   └── MeshLoader.cpp
    └── Scene/
        └── MeshRenderer.cpp
```
