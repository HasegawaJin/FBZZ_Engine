# 所有権・ライフタイム規約

FBZZ Engine 全モジュール共通の所有権モデル。

---

## 3 種類のポインタと使い分け

### `std::unique_ptr` — デフォルト

1 箇所だけが所有するリソースに使う。

```cpp
// Scene が GameObject を所有する
std::vector<std::unique_ptr<GameObject>> m_objects;

// GameObject が Component を所有する
std::vector<std::unique_ptr<Component>> m_components;

// Application が各サブシステムを所有する
std::unique_ptr<Window>              m_window;
std::unique_ptr<renderer::IRenderer> m_renderer;
```

### `std::shared_ptr` — 複数システムをまたぐ場合のみ

複数のオーナーが存在するリソースに限定して使う。

```cpp
// IBuffer は MeshRenderer と DrawCall の両方から参照される
std::shared_ptr<IBuffer> vertexBuffer;

// RigidBody は World と RigidBodyComponent の両方から参照される
std::shared_ptr<RigidBody> body;
```

`shared_ptr` を安易に使わない。所有関係を `unique_ptr` で整理できないか先に検討する。

### raw pointer (`T*`) — 非所有の参照のみ

所有権を持たない参照に使う。指す先のライフタイムを所有側 (unique_ptr / shared_ptr) が必ず保証していること。

```cpp
// Component は自分を持つ GameObject を参照するが所有しない
GameObject* m_owner = nullptr;

// Transform は親を参照するが所有しない
Transform* m_parent = nullptr;
```

---

## new / delete の直接使用禁止

```cpp
// NG
Component* c = new MeshRenderer();
delete c;

// OK
auto c = std::make_unique<MeshRenderer>();
auto buf = std::make_shared<DX11Buffer>();
```

---

## Transform の親子関係

`Transform::m_parent` と `m_children` は **非所有の raw pointer**。
子 GameObject が破棄されるとき、自身の Transform デストラクタで必ず親から登録解除する。

```cpp
Transform::~Transform() {
    if (m_parent) {
        m_parent->RemoveChild(this);
    }
    for (Transform* child : m_children) {
        child->m_parent = nullptr;
    }
}
```

---

## IRenderer が生成するリソースの所有権

`IRenderer::CreateVertexBuffer()` 等が返す `shared_ptr` は呼び出し元が所有する。
`IRenderer` 自体はリソースを追跡しない。

```cpp
// MeshRenderer が所有する
std::shared_ptr<IBuffer>  m_vertexBuffer;
std::shared_ptr<IBuffer>  m_indexBuffer;
std::shared_ptr<IShader>  m_shader;
std::shared_ptr<ITexture> m_texture;
```

---

## まとめ

```
所有する         → unique_ptr (デフォルト)
複数で共有する   → shared_ptr (最小限に)
参照するだけ     → T*  (生存期間を所有側が保証すること)
new/delete 直接  → 禁止
```
