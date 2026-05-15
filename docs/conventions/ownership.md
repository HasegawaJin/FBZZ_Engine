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

// DX11 内部でのデバイス参照 (DX11Renderer が所有)
ID3D11DeviceContext* m_context = nullptr;
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
    // 子の m_parent をクリアする (子自身は Scene が所有しているので delete しない)
    for (Transform* child : m_children) {
        child->m_parent = nullptr;
    }
}
```

この処理がないと、親の `m_children` に Dangling pointer が残る。

---

## Component::m_owner

Component は自分を所有する GameObject への **非所有の参照** を持つ。
Component のデストラクタが呼ばれるのは必ず GameObject の破棄と同時なので、
`m_owner` が Dangling pointer になることはない。

ただし `OnDestroy()` の中で `m_owner` を使うコードは、
GameObject が破棄中である点を意識して書くこと。

---

## IRenderer が生成するリソースの所有権

`IRenderer::CreateVertexBuffer()` 等が返す `shared_ptr` は呼び出し元が所有する。
`IRenderer` 自体はリソースを追跡しない。
`IRenderer` が先に破棄される場合、GPU リソースは自動解放される (ComPtr の RAII)。

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

---

## 参考ドキュメント

- [std::unique_ptr (cppreference)](https://en.cppreference.com/w/cpp/memory/unique_ptr) — unique_ptr の API・使い方
- [std::shared_ptr (cppreference)](https://en.cppreference.com/w/cpp/memory/shared_ptr) — shared_ptr の API・注意点
- [std::make_unique (cppreference)](https://en.cppreference.com/w/cpp/memory/unique_ptr/make_unique) — 推奨される生成方法
- [std::make_shared (cppreference)](https://en.cppreference.com/w/cpp/memory/shared_ptr/make_shared) — 推奨される生成方法
- [ComPtr クラス](https://learn.microsoft.com/en-us/cpp/cppcx/wrl/comptr-class) — DX11 COM オブジェクト用スマートポインタ
