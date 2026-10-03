/// @file    SceneSerializer.hpp
/// @brief   TOML ベースのシーン保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note GameObject 階層と登録済み Component を .fbzz に変換する。 未知の型は ScriptFactory / ComponentRegistry との対応を見て扱う。
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

/// @brief ScriptComponent を Script 実体ごと複製する (Duplicate / Copy&Paste / Prefab 展開用)。
/// @note フィールド値は `.fbzz` 保存と同じリフレクタ (SceneWriteReflector/SceneReadReflector) を往復するため、値型・入れ子型・GameObject 参照・アセット参照は保存・復元と同じ規則で引き継がれる。複製だけ別経路にすると片方に足したフィールドがもう片方で黙って落ちる。
/// @note GameObject 参照は複製元が指していた GameObject をそのまま指す。階層ごと複製しても内部参照は張り替わらない (通常 Component の EntityID と同じ挙動)。
ScriptComponent CloneScriptComponent(const ScriptComponent& src,
                                     const Scene* srcScene,
                                     Scene* dstScene,
                                     GameObject* dstOwner);

class SceneSerializer {
public:
    /// @brief Scene を `.fbzz` ファイルに保存する。
    /// @note 設計書と異なり Scene& を取る。GetComponent が const 未対応のため。
    static bool Save(Scene& scene, const std::string& path);

    /// @brief Scene を TOML テキストへ直列化する。失敗時は空文字。
    /// @param scenePath シーンの置き場所。Terrain はこの隣へ `.terrain`/`.mat` の実体を書き出す副作用を持つ (参照だけ保存すると実体が古いまま残り再起動後に白地形になる)。省略すると副作用を起こさず純粋にテキストだけを返す。
    /// @note 失敗時も戻り値は空文字になるため「中身が空」と区別できない。読み込み側の LoadDataFromText と対。
    static std::string SaveToText(Scene& scene, const std::string& scenePath = {});

    /// @brief `.fbzz` ファイルから Scene を復元する。
    /// @param resources nullptr ならデータだけ復元する。詳細は LoadData を参照。
    static std::unique_ptr<Scene> Load(const std::string& path,
                                       renderer::ResourceManager* resources);

    static std::unique_ptr<Scene> Load(const std::string& path,
                                       renderer::ResourceManager& resources)
    {
        return Load(path, &resources);
    }

    /// @brief TOML テキストから直接復元する。
    /// @return パース失敗または容量超過なら nullptr。
    /// @param sourcePath このテキストの出所 (`.scene`/`.prefab`/`.vfx` のパス)。パースエラー表示に加え、TerrainComponent の terrainAssetPath など「シーンからの相対パス」を解決する基準になる。空だと相対参照が解けない。
    /// @note プレファブ展開のように断片を一度きり読む呼び出しでも、Load へ渡すためだけの一時ファイルが要らない。
    static std::unique_ptr<Scene> LoadFromText(const std::string& tomlText,
                                               renderer::ResourceManager* resources,
                                               const std::string& sourcePath);

    static std::unique_ptr<Scene> LoadFromText(const std::string& tomlText,
                                               renderer::ResourceManager& resources,
                                               const std::string& sourcePath)
    {
        return LoadFromText(tomlText, &resources, sourcePath);
    }

    /// @brief GPU リソースを作らず、シーンのデータだけを復元する。
    /// @note TOML から GameObject/Component を組み立てることと Mesh を GPU へ載せることは別の関心事。デバイスを要求するのは後者だけなので、ヘッドレス (CI・アセット変換ツール) でも読める。
    /// @note 復元後 MeshRenderer::mesh は nullptr のまま meshPath だけが残る。描画に使うなら ResolveMeshes() を通すこと。
    static std::unique_ptr<Scene> LoadData(const std::string& path)
    {
        return Load(path, nullptr);
    }

    static std::unique_ptr<Scene> LoadDataFromText(const std::string& tomlText,
                                                   const std::string& sourcePath)
    {
        return LoadFromText(tomlText, nullptr, sourcePath);
    }

    /// @brief データだけ復元した Scene へ、後から GPU リソースを結び直す。
    /// @note 既に mesh を持つ MeshRenderer は触らない (二重に載せない)。
    /// @return 解決できた MeshRenderer の数。
    static int ResolveMeshes(Scene& scene, renderer::ResourceManager& resources);

    /// @brief 既存の Scene を path の内容で上書きする (PlayMode Stop / File > Open)。
    static bool LoadInPlace(Scene& scene, const std::string& path,
                            renderer::ResourceManager& resources);

    /// @brief 既存 Scene に tomlText の GameObject を追記する (既存 GO は破棄しない)。
    /// @return 容量不足なら false。既存 Scene は変更せず outRoots を空にする。
    /// @note Script::OnUpdate 内の Instantiate でシーン全体を再構築すると呼び出し元 Script が解放され use-after-free になる。AppendObjects は破棄せず追記のみ行う。
    static bool AppendObjects(Scene& scene, const std::string& tomlText,
                              renderer::ResourceManager* resources,
                              std::vector<EntityID>& outRoots);

    static bool AppendObjects(Scene& scene, const std::string& tomlText,
                              renderer::ResourceManager& resources,
                              std::vector<EntityID>& outRoots)
    {
        return AppendObjects(scene, tomlText, &resources, outRoots);
    }
};

} /// @note namespace fbzz::scene
