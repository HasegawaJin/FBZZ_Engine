// FBZZ Engine
// AssetManager.hpp | fbzz::asset
// Model と Texture のロードおよびキャッシュ管理
// Renderer の ResourceManager を経由して GPU リソースを作り、ハンドルを利用側へ渡す。
// 同じ相対パスは共有キャッシュとして扱い、明示的な Unload / UnloadAll まで保持する。
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <Engine/Renderer/ResourceHandle.hpp>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class AssetManager {
public:
    // Application 初期化直後に一度だけ呼ぶ。二重呼び出しは assert で検出する
    static void Init(renderer::ResourceManager& resources, const std::string& basePath = "assets/");

    // UnloadAll → app.Shutdown の順で呼ぶこと
    static void UnloadAll();

    // ロード失敗エントリ (nullptr キャッシュ) を削除して次回 Load で再試行させる。
    // FBX インポート完了後に呼ぶことでエンジン再起動なしにモデルを読み込み直せる。
    static void FlushFailed();
    static renderer::ResourceHandle<renderer::TextureTag> LoadTexture(const std::string& relativePath);

    // Load<T>: 対応型は Model / ITexture のみ
    // 未対応型はヘッダー内の static_assert によりコンパイルエラーになる
    template<typename T>
    static std::shared_ptr<T> Load(const std::string& relativePath) {
        static_assert(sizeof(T) == 0,
            "AssetManager::Load<T>: unsupported type. Use Model or ITexture.");
        return nullptr;
    }

    template<typename T>
    static void Unload(const std::string& relativePath) {
        static_assert(sizeof(T) == 0,
            "AssetManager::Unload<T>: unsupported type.");
    }

private:
    static renderer::ResourceManager* s_resources;
    static std::string          s_basePath;
    static bool                 s_initialized;

    static std::unordered_map<std::string, std::shared_ptr<Model>>    s_models;
    static std::unordered_map<std::string, renderer::ResourceHandle<renderer::TextureTag>> s_textures;

    static std::string Normalize(const std::string& path);
};

template<> std::shared_ptr<Model>    AssetManager::Load<Model>   (const std::string&);
template<> void AssetManager::Unload<Model>   (const std::string&);

} // namespace fbzz::asset
