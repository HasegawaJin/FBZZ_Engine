// FBZZ Engine
// SceneSerializer.hpp | fbzz::scene
// TOML ベースのシーン保存・復元
// GameObject 階層と登録済み Component を .fbzz に変換する。
// 未知の型は ScriptFactory / ComponentRegistry との対応を見て扱う。
#pragma once
#include <string>
#include <memory>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;
struct EntityID;

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

    // 既存 Scene に tomlText の GameObject を追記する (既存 GO は破棄しない)
    // WHY: Script::OnUpdate 内の Instantiate でシーン全体を再構築すると
    //      呼び出し元 Script が解放され use-after-free になる。
    //      AppendObjects はシーンを破棄せず新規 GO の追記のみ行う。
    static bool AppendObjects(Scene& scene, const std::string& tomlText,
                              renderer::ResourceManager& resources,
                              std::vector<EntityID>& outRoots);
};

} // namespace fbzz::scene
