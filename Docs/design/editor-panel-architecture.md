# Editorパネル拡張方針

## 共通基底

制作ツールは `EditorToolPanel` を継承する。`IPanel` のWindowライフサイクルに加えて、
`GetEditorType()` と共通ツールバー、`Editors` メニューカテゴリを提供する。

```cpp
class TerrainEditorPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "Terrain Editor"; }
    const char* GetEditorType() const override { return "Terrain Editor"; }
};
```

## 登録手順

1. `include/Editor/Panels/` に `.hpp` を追加する。
2. `src/Panels/` に `.cpp` を追加する。
3. `EditorApp.cpp` のPanel生成リストへ `make_unique` を1行追加する。
4. CMakeの `GLOB_RECURSE` が自動的にソースを収集する。

Map EditorとEffect Editorはこの方式に統一済み。将来のAnimation、Material、VFX Graph、Terrain
専用Editorも同じ基底クラスから追加できる。
