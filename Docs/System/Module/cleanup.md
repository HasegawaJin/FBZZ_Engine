# main.cpp クリーンアップ 設計メモ

`IModule` 導入 ([overview.md](overview.md)) に加えて検討できるリファクタリング。  
優先度順に列挙する。

---

## 1. `ResolveProject()` → `ProjectResolver` クラス

### 現状の問題

`ResolveProject()` が 70 行の単一関数で、以下が混在している。

- `.fbzz_proj` の TOML 解析
- `ProjectSettings.toml` の TOML 解析
- パス解決 (`ResolvePathUnderRoot`)
- エラーメッセージ生成 (`std::wstring&` で out 引数)

### 改善案

```cpp
// Projects/Sandbox/src/ProjectResolver.hpp
namespace fbzz::sandbox {

class ProjectResolver {
public:
    struct Result {
        std::filesystem::path root;
        std::filesystem::path projectFile;
        std::filesystem::path settingsFile;
        std::filesystem::path sceneFile;
    };

    /// @return 解決に成功すれば true。失敗時は ErrorMessage() を参照。
    [[nodiscard]] bool Resolve(const std::filesystem::path& projectPath);

    const Result&       Get()          const { return m_result; }
    const std::wstring& ErrorMessage() const { return m_error; }

private:
    Result      m_result;
    std::wstring m_error;
};

} // namespace fbzz::sandbox
```

**WHY**: 責務が明確になりテストを書きやすくなる。`out 引数` が消えて呼び出し側もシンプルになる。

---

## 2. `ResolveGameCamera()` をエンジン側へ

### 現状の問題

```cpp
// Sandbox/main.cpp (無名名前空間)
renderer::Camera ResolveGameCamera(scene::Scene& scene, float aspectRatio);
```

- `CameraComponent → renderer::Camera` の変換は Sandbox 固有ではない
- `RunStandaloneLoop` と `RunEditorLoop` の両方に同じロジックが重複している

### 改善案

`CameraComponent` のメンバ関数として持たせる。

```cpp
// Engine/Scene/Components/CameraComponent.hpp
struct CameraComponent {
    // ...既存フィールド...

    /// isMain フラグは無視し、このコンポーネントの値を Camera に変換する。
    renderer::Camera ToCamera(const math::Vector3& position,
                              const math::Quaternion& rotation,
                              float aspectRatio) const;
};
```

呼び出し側:

```cpp
// StandaloneModule::OnRender()
for (auto& go : m_scene->GameObjects()) {
    auto* cam = go.GetComponent<scene::CameraComponent>();
    if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;
    gameCamera = cam->ToCamera(go.transform.position, go.transform.rotation, aspect);
    break;
}
```

**WHY**: 変換ロジックがデータを持つクラスに集まる。`StandaloneModule` / `EditorModule` どちらからも共有できる。

---

## 3. Win32 ユーティリティの分離

### 現状の問題

以下が `main.cpp` の無名名前空間に孤立している。

```cpp
std::wstring Utf8ToWide(const std::string&);
std::string  WideToUtf8(const std::wstring&);
std::string  PathToUtf8(const std::filesystem::path&);
std::string  ReadText(const std::filesystem::path&);
bool         Exists(const std::filesystem::path&);
std::filesystem::path MakeAbsolute(const std::filesystem::path&);
std::filesystem::path GetExecutableDirectory();
```

`StandaloneModule` / `EditorModule` を別ファイルに切り出すと、これらをまた持ち込まなければならない。

### 改善案

```
Projects/Sandbox/src/Util/
  PathUtil.hpp   ─ Utf8ToWide / WideToUtf8 / PathToUtf8 / MakeAbsolute / GetExecutableDirectory
  FileUtil.hpp   ─ ReadText / Exists
```

```cpp
// Util/PathUtil.hpp
namespace fbzz::sandbox::util {

std::wstring          Utf8ToWide(const std::string& text);
std::string           WideToUtf8(const std::wstring& text);
std::string           PathToUtf8(const std::filesystem::path& path);
std::filesystem::path MakeAbsolute(const std::filesystem::path& path);
std::filesystem::path GetExecutableDirectory();

} // namespace fbzz::sandbox::util
```

**WHY**: `StandaloneModule.cpp` / `EditorModule.cpp` から `#include "Util/PathUtil.hpp"` で参照できる。重複コピーを防ぐ。

---

## 4. `ApplyPhysicsSettings` / `ApplyUISettings` を呼び出し先のメンバへ

### 現状の問題

```cpp
// Sandbox/main.cpp (無名名前空間)
void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings);
void ApplyUISettings(const ProjectSettings& settings);
```

設定を受け取る側のクラスが自分に適用する方法を知らず、外側から強制される形になっている。

### 改善案

`physics::World` に個別セッターを呼ぶ形に留める。

```cpp
// Sandbox 側の ApplyPhysicsSettings (free function のまま)
// WHY: physics::World に ProjectSettings を渡すと
//      依存方向 (sandbox → engine → physics → math) に対して
//      physics → engine の逆方向依存が生じる。
//      ProjectSettings は engine 層に属するため World からは参照できない。
static void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings)
{
    world.SetGravity(settings.physics.gravity);
    world.SetSubsteps(settings.physics.substeps);
}
```

`physics::World` 側には `SetGravity` / `SetSubsteps` の個別セッターを用意するだけでよい。  
`ProjectSettings` を World が直接知る必要はない。

UISystem も同様:

```cpp
// 現状の UISystemSetDefaultFontPath を ApplySettings に変えるのではなく
// フリー関数のまま Sandbox 側に置く
static void ApplyUISettings(const ProjectSettings& settings)
{
    scene::UISystemSetDefaultFontPath(settings.ui.defaultFontPath);
}
```

**WHY**: 「設定の適用方法」を呼び出し先のクラスに持たせると依存方向が逆転する。  
Sandbox 層のフリー関数として残すことで、physics / scene ライブラリを汚染しない。

---

## 優先度まとめ

| # | 対象 | インパクト | 難易度 |
|---|------|-----------|-------|
| 1 | `ProjectResolver` クラス | 大 (責務分離・テスト容易化) | 中 |
| 2 | `ResolveGameCamera` → `CameraComponent::ToCamera` | 中 (重複排除) | 小 |
| 3 | Win32 ユーティリティ分離 | 中 (モジュール分割の前提) | 小 |
| 4 | `ApplyPhysicsSettings` → `World::ApplySettings` | 小 (局所的整理) | 小 |

`IModule` 導入と同時に 2・3 を実施し、`ProjectResolver` は独立したタスクとして扱うのが現実的。
