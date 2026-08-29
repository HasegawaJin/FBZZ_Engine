/// @file    SceneSerializer.hpp
/// @brief   TOML ベースのシーン保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// GameObject 階層と登録済み Component を .fbzz に変換する。
/// 未知の型は ScriptFactory / ComponentRegistry との対応を見て扱う。
#pragma once
#include <string>
#include <memory>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

class Scene;
class GameObject;
struct EntityID;
struct ScriptComponent;

// ScriptComponent を Script 実体ごと複製する (Duplicate / Copy&Paste / Prefab 展開用)。
// フィールド値は .fbzz 保存と同じリフレクタを往復させるため、値型・入れ子型・
// GameObject 参照・アセット参照が保存・復元とまったく同じ規則で引き継がれる。
//
// WHY 複製がシリアライザ側にあるか: Script のフィールドは Reflect() 経由でしか
//     触れず、その解釈 (参照の guid 化、アセット参照、入れ子スコープ) は
//     SceneWriteReflector / SceneReadReflector が唯一の実装。複製だけ別経路にすると
//     片方に足したフィールドがもう片方で黙って落ちる。
//
// NOTE: GameObject 参照は複製元が指していた GameObject をそのまま指す。
//       階層ごと複製しても階層内部への参照は張り替わらない (値コピーされる
//       通常 Component の EntityID と同じ挙動)。
ScriptComponent CloneScriptComponent(const ScriptComponent& src,
                                     const Scene* srcScene,
                                     Scene* dstScene,
                                     GameObject* dstOwner);

class SceneSerializer {
public:
    // Scene を .fbzz ファイルに保存する
    // 設計書と異なり Scene&。GetComponent が const 未対応のため
    static bool Save(Scene& scene, const std::string& path);

    // .fbzz ファイルから Scene を復元する
    // renderer: Mesh / Material の GPU リソース生成に使う
    static std::unique_ptr<Scene> Load(const std::string& path,
                                       renderer::ResourceManager& resources);

    // TOML テキストから直接復元する。
    // WHY: プレファブ展開のように「メモリ上の断片を一度きり読む」呼び出しが、
    //      Load へ渡すためだけに一時ファイルを書いていた。
    // sourcePath: このテキストの出所 (.scene / .prefab / .vfx のパス)。
    //   パースエラーの表示だけでなく、TerrainComponent の terrainAssetPath など
    //   「シーンからの相対パス」を解決する基準として使う。空にすると相対参照が解けない。
    static std::unique_ptr<Scene> LoadFromText(const std::string& tomlText,
                                               renderer::ResourceManager& resources,
                                               const std::string& sourcePath);

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
