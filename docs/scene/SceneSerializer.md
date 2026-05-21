# SceneSerializer 設計書

`fbzz::scene::SceneSerializer` — TOML ベースのシーン保存・復元。  
`FileSystem::ReadText` / `WriteText` で I/O し、toml++ でパースする。

---

## 目的

`main.cpp` のシーン生成ラムダ (~400 行) を `.fbzz` ファイルに移し、  
ゲームループだけが残るようにする (目標 ~80 行)。

---

## 依存関係

```
fbzz::scene::SceneSerializer
├── fbzz::util::FileSystem   (ReadText / WriteText)
├── toml++                   (TOML_EXCEPTIONS=0)
├── fbzz::renderer::IRenderer  (Mesh / Material の GPU リソース生成)
├── fbzz::renderer::ShaderManager  (singleton)
└── fbzz::asset::AssetManager      (singleton)
```

### CMake 追加

```cmake
# engine/CMakeLists.txt — target_link_libraries に追加
PUBLIC  tomlplusplus
```

---

## API

```cpp
// engine/include/engine/Scene/SceneSerializer.hpp
namespace fbzz::scene {

class SceneSerializer {
public:
    // Scene を .fbzz ファイルに保存する
    // path: "assets/scenes/game.fbzz"
    static bool Save(const Scene& scene, const std::string& path);

    // .fbzz ファイルから Scene を復元する
    // renderer: Mesh / Material の GPU リソース生成に使う
    static std::unique_ptr<Scene> Load(const std::string& path,
                                       renderer::IRenderer& renderer);
};

} // namespace fbzz::scene
```

---

## ファイルフォーマット (.fbzz)

拡張子 `.fbzz`。中身は UTF-8 TOML。

```toml
# ---- scene ヘッダー ----
[scene]
name           = "PostProcessShowcase"
format_version = 1

# ---- GameObject 配列 ----
# [[gameobjects]] を 1 オブジェクト 1 エントリで並べる
# 各エントリの直後にコンポーネント sub-table を続ける

[[gameobjects]]
name   = "DirectionalLight"
tag    = "Untagged"
active = true
parent = ""         # 空文字 = ルートオブジェクト

[gameobjects.transform]
localPosition = [0.0, 0.0, 0.0]
localRotation = [0.0, 0.0, 0.0, 1.0]   # Quaternion xyzw
localScale    = [1.0, 1.0, 1.0]

[gameobjects.LightComponent]
type      = "Directional"   # "Directional" | "Point" | "Spot"
color     = [1.0, 0.92, 0.76]
intensity = 3.5
# range / innerCone / outerCone は Point / Spot のみ記述

# ---- MeshRenderer の例 ----
[[gameobjects]]
name   = "Sphere_Gold"
active = true
parent = ""

[gameobjects.transform]
localPosition = [-3.5, 1.0, 1.5]
localRotation = [0.0, 0.0, 0.0, 1.0]
localScale    = [1.3, 1.3, 1.3]

[gameobjects.MeshRenderer]
mesh          = "primitive:sphere"                  # or "models/test.fbx:0"
shader        = "assets/shaders/Material/PBR.hlsl"
albedo        = [1.0, 0.78, 0.34, 1.0]
metallic      = 1.0
roughness     = 0.04
emissiveScale = 0.0
albedoTex     = ""    # 空文字 = テクスチャなし
normalTex     = ""

# ---- AudioSourceComponent の例 ----
[[gameobjects]]
name   = "BGM"
active = true
parent = ""

[gameobjects.AudioSourceComponent]
clipPath    = "assets/bgm/title.wav"
playOnAwake = true
loop        = true
volume      = 0.8
```

---

## mesh フィールドの解決規則

| 値 | 解決方法 |
|----|---------|
| `"primitive:cube"` | `PrimitiveMesh::Cube(renderer)` |
| `"primitive:sphere"` | `PrimitiveMesh::Sphere(renderer, 32)` |
| `"primitive:plane"` | `PrimitiveMesh::Plane(renderer)` |
| `"models/foo.fbx"` | `AssetManager::Load<Model>("models/foo.fbx")`、mesh index 0 |
| `"models/foo.fbx:2"` | 同上、mesh index 2 |

---

## v1 対応コンポーネント

| コンポーネント | 保存 | 復元 | 備考 |
|--------------|------|------|------|
| Transform | ✓ | ✓ | localPosition / localRotation / localScale のみ |
| MeshRenderer | ✓ | ✓ | mesh / shader / material params / tex paths |
| LightComponent | ✓ | ✓ | type / color / intensity / range / cone |
| CameraComponent | ✓ | ✓ | fovY / nearZ / farZ / isMain |
| AudioSourceComponent | ✓ | ✓ | m_played は保存しない (runtime state) |
| ParticleEmitter | ✓ | ✓ | emitter params のみ。particles は保存しない |
| SkyRenderer | ✓ | ✓ | 大気散乱パラメーター |
| RigidBodyComponent | ✗ | ✗ | v1 対象外 (physics::World との連携が複雑) |

親子関係は `parent = "name"` で保存する。

---

## ロードパイプライン (2 パス)

```
Pass 1: GameObject 生成
  for each [[gameobjects]]:
    scene.CreateGameObject(name)
    go.tag    = tag
    go.SetActive(active)
    apply transform
    add components

Pass 2: 親子関係の解決
  for each [[gameobjects]] where parent != "":
    child  = scene.Find(name)
    parent = scene.Find(parent_name)
    if child && parent: child.SetParent(*parent)
```

2 パスにする理由: 子の定義が親より先に現れる場合でも正しく解決できる。

---

## セーブパイプライン

```
for go in scene.GameObjects():
    entry["name"]   = go.name
    entry["parent"] = go.GetParent() ? go.GetParent()->name : ""
    entry["active"] = go.activeSelf()
    serialize transform (localPosition / localRotation / localScale)
    if has MeshRenderer: serialize meshPath / shaderPath / params
    if has LightComponent: serialize type / color / ...
    ... (他コンポーネント同様)

FileSystem::WriteText(path, toml::format(doc))
```

---

## ファイル構成 (実装時)

```
engine/include/engine/Scene/
└── SceneSerializer.hpp

engine/src/Scene/
└── SceneSerializer.cpp
```

---

## main.cpp 目標形 (SceneSerializer 導入後)

```cpp
sm.Register("PostProcessShowcase", []{ 
    return SceneSerializer::Load("assets/scenes/postprocess.fbzz", renderer); 
});
sm.Register("MultiLight", []{
    return SceneSerializer::Load("assets/scenes/multilight.fbzz", renderer);
});

sm.LoadScene("MultiLight");

while (app.IsRunning()) {
    // ゲームループ (~30 行)
}
```
