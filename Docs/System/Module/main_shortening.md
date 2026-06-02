# main.cpp 短縮 設計メモ

`IModule` ([overview.md](overview.md)) と既存クリーンアップ ([cleanup.md](cleanup.md)) に加えて、  
main.cpp をさらに短くするための設計案。全適用で **~800 行 → ~40 行** を目標とする。

---

## 1. スクリプト自己登録マクロ `FBZZ_REGISTER_SCRIPT`

### 現状の問題

```cpp
// main.cpp — 新スクリプトを追加するたびにここを修正しなければならない
void RegisterSandboxScripts()
{
    scene::ScriptFactory::Register<PlayerControllerComponent>();
    scene::ScriptFactory::Register<PlayerWorldSpaceUIComponent>();
    scene::ScriptFactory::Register<TpsCameraComponent>();
}
```

### 改善案

`ScriptFactory.hpp` にマクロを追加し、各スクリプトのヘッダ末尾に置く。

```cpp
// Engine/include/Engine/Scene/ScriptFactory.hpp に追加
//
// スクリプトクラスを ScriptFactory に自動登録するマクロ。
// WHY: main.cpp の RegisterXxxScripts() を不要にし、
//      新スクリプト追加時に main.cpp を触らなくて済む。
//      静的初期化子は TU (翻訳単位) のロード時に実行されるため、
//      main() より前に登録が完了することが保証される。
#define FBZZ_REGISTER_SCRIPT(T) \
    namespace { \
        const bool s_script_registered_##T = [] { \
            ::fbzz::scene::ScriptFactory::Register<T>(); \
            return true; \
        }(); \
    }
```

各スクリプトの **`.cpp` 末尾** に置く:

```cpp
// Scripts/PlayerControllerComponent.cpp
#include "PlayerControllerComponent.hpp"
#include <Engine/Scene/ScriptFactory.hpp>

// ... メソッド定義 ...

FBZZ_REGISTER_SCRIPT(sandbox::PlayerControllerComponent)
```

main.cpp から `RegisterSandboxScripts()` の **定義と呼び出しが両方消える**。  
新スクリプト追加時は `.cpp` に 1 行書くだけで登録完了。

> **WHY — `.hpp` ではなく `.cpp` に置く**:  
> MSVC Release ビルドの `/OPT:REF` は外部から参照されていないシンボルを削除する。  
> `.hpp` に置いた無名名前空間の `const bool` は外部参照がなく、  
> **リンカが静的初期化子ごと除去して登録がスキップされる**可能性がある。  
> `.cpp` に置けば、クラスのメソッド定義が同一 TU にあるため  
> リンカが TU を必ず取り込み、初期化子が実行される。

---

## 2. `LaunchArgs::Parse()` 静的メソッド化

### 現状の問題

`ParseArgs()` と `FindDefaultSandboxProjectPath()` が main.cpp の無名名前空間に埋もれている。  
main.cpp の行数を押し上げているだけでなく、テストも書けない。

### 改善案

```cpp
// Projects/Sandbox/src/LaunchArgs.hpp
#pragma once
#include <filesystem>

namespace fbzz::sandbox {

/// コマンドライン引数を保持する値型。
/// Parse() で CommandLineToArgvW を呼び出してフィールドを埋める。
struct LaunchArgs {
    std::filesystem::path projectPath;
    bool                  standalone = false;

    /// コマンドライン引数を解析して LaunchArgs を返す。
    /// 引数なし起動のフォールバック探索 (FindDefaultSandboxProjectPath) も内包する。
    [[nodiscard]] static LaunchArgs Parse();
};

} // namespace fbzz::sandbox
```

main.cpp での呼び出し:

```cpp
// 変更前
const LaunchArgs args = ParseArgs();

// 変更後
const auto args = LaunchArgs::Parse();
```

`FindDefaultSandboxProjectPath()` も `LaunchArgs.cpp` に移動し、main.cpp から消える。

---

## 3. `Run()` と `main()` の統合

### 現状の問題

`fbzz::sandbox::Run()` と `main()` が分離している。  
`Run()` を別名前空間に置く理由は名前空間の整理だが、実質 `main()` のラッパーに過ぎない。

### 改善案

`IModule` / `ProjectResolver` / `LaunchArgs` がそれぞれ独立したファイルに出た後は、  
`main()` に直接書いても行数が増えない。中間関数を 1 つ減らせる。

```cpp
// main.cpp (全適用後のイメージ)
#include "LaunchArgs.hpp"
#include "ProjectResolver.hpp"
#include "StandaloneModule.hpp"
#include "EditorModule.hpp"
// ...

int main()
{
    const auto args = fbzz::sandbox::LaunchArgs::Parse();

    fbzz::sandbox::ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(), L"FBZZ Sandbox", MB_OK | MB_ICONERROR);
        return 1;
    }
    const auto& project = resolver.Get();

    SetCurrentDirectoryW(fbzz::sandbox::util::GetExecutableDirectory().wstring().c_str());

    auto& app = fbzz::core::Application::Get();

    if (args.standalone) {
        fbzz::ProjectSettings settings;
        if (!settings.Load(fbzz::sandbox::util::PathToUtf8(project.settingsFile))) return 1;
        if (!app.Init(fbzz::sandbox::BuildWindowConfig(settings))) return 1;

        fbzz::renderer::ResourceManager resources(app.GetRenderer());
        fbzz::asset::AssetManager::Init(resources, fbzz::sandbox::util::PathToUtf8(project.root / L"Assets") + "/");

        fbzz::sandbox::StandaloneModule module(project, settings);
        app.Run(module);
    } else {
        if (!app.Init()) return 1;

        fbzz::renderer::ResourceManager resources(app.GetRenderer());
        fbzz::asset::AssetManager::Init(resources, fbzz::sandbox::util::PathToUtf8(project.root / L"Assets") + "/");

        fbzz::sandbox::EditorModule module(project);
        app.Run(module);
    }

    fbzz::asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}
```

---

## 適用後のファイル構成

```
Projects/Sandbox/src/
  main.cpp              ─ ~40 行 (起動分岐のみ)
  LaunchArgs.hpp/.cpp   ─ コマンドライン解析
  ProjectResolver.hpp/.cpp ─ .fbzz_proj / ProjectSettings 解析
  StandaloneModule.hpp/.cpp ─ スタンドアロンゲームループ
  EditorModule.hpp/.cpp ─ エディタループ
  Util/
    PathUtil.hpp        ─ Utf8ToWide / WideToUtf8 / PathToUtf8 等
    FileUtil.hpp        ─ ReadText / Exists 等
```

---

## 優先度まとめ

| # | 対象 | 行数削減 | インパクト |
|---|------|---------|-----------|
| 1 | `FBZZ_REGISTER_SCRIPT` マクロ | ~10 行 + 保守コスト大幅減 | 大 |
| 2 | `LaunchArgs::Parse()` 静的メソッド | ~50 行 | 中 |
| 3 | `Run()` と `main()` の統合 | ~5 行 | 小 |

`IModule` + `cleanup.md` の施策と合わせると **800 行 → 約 40 行** になる。
