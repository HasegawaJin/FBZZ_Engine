# Script System — 改善点まとめ

> 現状調査日: 2026-06-09  
> 対象ブランチ: fix/rendering

---

## 現状の構成

```
Script (基底)
├── ScriptProxy群 (transform / input / scene / animator / ...)
├── IReflector (Inspector・Serializer 共通インターフェース)
│   ├── ImGuiReflector      — Editor 描画
│   ├── TomlWriteReflector  — .fbzz 書き込み
│   └── TomlReadReflector   — .fbzz 読み込み
└── ScriptComponent (1 GO に複数 Script を保持する std::vector<ScriptEntry>)
```

`Assets/Scripts/` 下の実スクリプトは sandbox DLL としてビルドされ、`ScriptFactory` 経由でインスタンス化される。

---

## 改善項目

### 1. Inspector から GameObject 参照をアサインできない

**問題**  
`IReflector` がサポートする型は `float / int / bool / Vector2/3/4 / string / Quaternion` のみ。  
別 GameObject を参照したいスクリプトは現状 `scene.Find(name)` や `scene.FindWithTag(tag)` で文字列解決するしかなく、スペルミスや名前変更でサイレントに失敗する。

```cpp
// 現状 (TpsCameraComponent.hpp)
void FindTarget() {
    m_target = scene.FindWithTag(targetTag);  // 実行時文字列依存
}
```

**改善案**  
`EntityID` 型フィールドを `IReflector` に追加し、Editor 側でドラッグ&ドロップにより参照をアサインできるようにする。

```cpp
// IReflector に追加
virtual void Field(const char* name, EntityID& v) = 0;
```

```cpp
// スクリプト側
EntityID targetID = EntityID::INVALID;

void Reflect(IReflector& r) override {
    r.Field("Target", targetID);  // Inspector でGOをD&Dアサイン
}

void OnStart() override {
    m_target = scene.GetGameObject(targetID);
}
```

```cpp
// ImGuiReflector — ボタン表示 + Scene Hierarchy からのドロップ受付
void Field(const char* name, EntityID& v) override {
    auto* go = scene->GetGameObject(v);
    const char* label = go ? go->GetName().c_str() : "(None)";
    ImGui::Button(label, { -1, 0 });
    if (ImGui::BeginDragDropTarget()) {
        if (auto* payload = ImGui::AcceptDragDropPayload("GAMEOBJECT_ID"))
            v = *static_cast<EntityID*>(payload->Data);
        ImGui::EndDragDropTarget();
    }
}
```

シリアライズは `uint64_t` として TOML に保存・復元する（シーン内 EntityID は固定値）。

---

### 2. Prefab 参照と Instantiate がない

**問題**  
`scene.Create()` は空 GO の生成のみ。テンプレート GO（プレハブ）を Inspector でアサインして実行時に複製する手段がない。  
`PlayerWorldSpaceUIComponent` では `OnStart` でコードにより子 GO を構築しているが、エディタでレイアウトを確認できず保守性が低い。

**改善案 (段階的)**

| フェーズ | 内容 |
|---------|------|
| Step A | `PrefabRef` 型を `string`（アセットパス）のラッパーとして定義し `IReflector::Field` に追加。Inspector でアセットブラウザから `.fbzz` をD&D設定できる |
| Step B | `scene.Instantiate(prefabPath)` / `scene.Instantiate(prefabPath, pos, rot)` を `ScriptSceneProxy` に追加。内部で `SceneSerializer` の部分ロードを呼ぶ |
| Step C | Editor 上で既存 GO を選択して「Save as Prefab」でアセット保存できるメニューを追加 |

```cpp
// スクリプト側イメージ
PrefabRef bulletPrefab;

void Reflect(IReflector& r) override {
    r.Field("Bullet Prefab", bulletPrefab);
}

void OnUpdate(float dt) override {
    if (input.GetKeyDown(KeyCode::SPACE))
        scene.Instantiate(bulletPrefab, transform->position, transform->rotation);
}
```

---

### 3. IReflector の表現力不足

**問題**  
全フィールドが `DragFloat / DragInt / Checkbox / InputText` のみで表現の幅が狭い。具体的な不足点:

| 不足している表現 | 現状の回避策 | 問題 |
|----------------|-------------|------|
| スライダー (min/max 付き float) | DragFloat のみ | 値域が Inspector から見えない |
| Enum / ドロップダウン | int + 手動コメント | Inspector 上で数値が表示される |
| セクションヘッダ / セパレータ | なし | フィールドが多い Script で視認性が低い |
| KeyCode 選択 UI | int として DragInt 表示 | キー番号が直接表示されて非直感的 |
| string 長上限 | 固定 256 バイト | 長いパス等で切り捨てられる |
| 配列 / `vector<T>` | なし | Waypoint 列や複数参照を持てない |
| Tooltip (説明文) | なし | フィールドの意図を伝えられない |

**改善案 — `IReflector` 拡張 API**

```cpp
struct IReflector {
    // 既存 ---
    virtual void Field(const char* name, float& v) = 0;
    virtual void Field(const char* name, int& v) = 0;
    // ...

    // 追加 ---
    // スライダー付き float
    virtual void FloatRange(const char* name, float& v, float min, float max) {
        Field(name, v);  // デフォルト実装は通常 Field にフォールバック
    }
    // Enum — labels は {"Walk","Run","Fly"} などの文字列配列
    virtual void Enum(const char* name, int& v,
                      std::span<const char* const> labels) {
        Field(name, v);
    }
    // セクションヘッダ (値を持たない)
    virtual void Header(const char* label) {}
    // KeyCode 選択 (int ラッパー)
    virtual void KeyCodeField(const char* name, int& v) {
        Field(name, v);
    }
    // Tooltip 付き float
    virtual void FieldWithTooltip(const char* name, float& v,
                                  const char* tooltip) {
        Field(name, v);
    }
    // GameObject 参照
    virtual void Field(const char* name, EntityID& v) {}
    // Prefab 参照
    virtual void Field(const char* name, PrefabRef& v) {}
};
```

`ImGuiReflector` でオーバーライドして本来の ImGui ウィジェットを描画し、`TomlWriteReflector` / `TomlReadReflector` は型に合わせて保存・復元する。  
デフォルト実装が既存 `Field` にフォールバックするため、Toml 側は型が同じなら何も変更不要。

---

### 4. 別 GO の Script へのアクセスが文字列依存

**問題**  
別 GO に乗った Script を取得するには以下のコンボが必要で煩雑。

```cpp
auto* go = scene.Find("Enemy");       // 文字列で GO を探す
if (go) {
    auto* enemy = scene.GetScript<EnemyScript>(go);
}
```

**改善案**  
`EntityID` フィールド（改善案 1）と組み合わせた直接アクセス API を `ScriptSceneProxy` に追加。

```cpp
// ScriptSceneProxy に追加
template<typename T> T* GetScript(EntityID id) const;
```

```cpp
// スクリプト側
EntityID m_enemyID = EntityID::INVALID;

void OnUpdate(float dt) override {
    auto* enemy = scene.GetScript<EnemyScript>(m_enemyID);
    if (enemy) enemy->TakeDamage(10);
}
```

---

### 5. Editor での Script 追加 UI が不明確

**問題**  
Inspector 下部に ScriptComponent を追加するボタンはあるが、`ScriptFactory` に登録された型一覧から選択・検索する UI が未整備。  
現状は SceneHierarchyPanel から ScriptComponent を追加するフローになっているが、どの Script 型をアタッチするかの指定が Inspector 上から直感的にできない。

**改善案**

```
Inspector 下部に "Add Script" ボタン
    → ポップアップで型名を入力するサーチフィールド
    → ScriptFactory::GetRegisteredTypeNames() の一覧をインクリメンタル検索
    → 選択すると ScriptComponent に ScriptEntry が追加される
```

```cpp
// ScriptFactory に追加
static std::vector<std::string> GetRegisteredTypeNames();
```

---

### 6. 1 GO に複数 Script をアタッチできることのドキュメント不足

**問題**  
`ScriptComponent` は内部に `std::vector<ScriptEntry> scripts` を持ち、1 GO に複数 Script をアタッチできる設計になっている。  
しかし `PlayerWorldSpaceUIComponent.hpp` のコメントに「エンジンが 1 GO につき 1 ScriptComponent の設計のため … 別 GO に追加して使う」と誤解を招く記述がある。

```cpp
// WHY: このスクリプト自体は Player とは別の空 GameObject に追加して使う。
//      エンジンが 1 GO につき 1 ScriptComponent の設計のため、
//      Player に直接乗せると PlayerControllerComponent と競合する。
```

**実際の制約**  
`ComponentArray` の制約は「1 GO につき同一型の Component は 1 つ」。`ScriptComponent` 自体は 1 つだが、その内部で複数の `Script` 派生クラスを保持できる。  
→ `PlayerControllerComponent` と `PlayerWorldSpaceUIComponent` は同一 GO にアタッチ可能。

**アクション**  
- 上記コメントを修正し、正しい制約（「同じ Script 型を同一 GO に 2 つ付けることはできない」）を記述する
- AGENTS.md または Script API コメントに複数 Script アタッチの使い方を追記する

---

## 優先度まとめ

| 優先 | 項目 | 理由 |
|-----|------|------|
| 高 | 1. EntityID フィールド + D&D | スクリプト間参照の安全性・利便性に直結 |
| 高 | 5. Add Script UI | 毎回の開発で踏む操作フロー |
| 中 | 3. IReflector 拡張 (Header/Range/Enum) | スクリプトの Inspector 視認性向上 |
| 中 | 6. 複数 Script アタッチのドキュメント修正 | 誤解の解消、即対応可能 |
| 低 | 2. Prefab Instantiate | 設計が大きく、Step A から段階的に |
| 低 | 4. GetScript(EntityID) | 1 の EntityID フィールド実装後に追加 |
