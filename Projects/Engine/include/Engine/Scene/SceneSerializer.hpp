// FBZZ Engine
// SceneSerializer.hpp | fbzz::scene
// TOML ベースのシーン保存・復元
// GameObject 階層と登録済み Component を .fbzz に変換する。
// 未知の型は ScriptFactory / ComponentRegistry との対応を見て扱う。
#pragma once
#include <string>
#include <memory>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;

class SceneSerializer {
public:
    // Scene を .fbzz ファイルに保存する
    // 設計書と異なり Scene&。GetComponent が const 未対応のため
    static bool Save(Scene& scene, const std::string& path);

    // .fbzz ファイルから Scene を復元する
    // renderer: Mesh / Material の GPU リソース生成に使う
    static std::unique_ptr<Scene> Load(const std::string& path,
                                       renderer::ResourceManager& resources);

    // 既存の Scene を path の内容で上書きする (PlayMode Stop / File > Open)
    static bool LoadInPlace(Scene& scene, const std::string& path,
                            renderer::ResourceManager& resources);
};

} // namespace fbzz::scene
