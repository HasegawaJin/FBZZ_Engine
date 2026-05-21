// FBZZ Engine
// AssetManager.hpp | fbzz::asset
// Model / ITexture のロードとキャッシュ管理 (static クラス)
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::renderer { class IRenderer; class ITexture; }
namespace fbzz::asset    { struct Model; }

namespace fbzz::asset {

class AssetManager {
public:
    // Application 初期化直後に一度だけ呼ぶ。二重呼び出しは assert で検出する
    static void Init(renderer::IRenderer& renderer, const std::string& basePath = "assets/");

    // UnloadAll → ShaderManager::Shutdown → app.Shutdown の順で呼ぶこと
    static void UnloadAll();

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
    static renderer::IRenderer* s_renderer;
    static std::string          s_basePath;
    static bool                 s_initialized;

    static std::unordered_map<std::string, std::shared_ptr<Model>>              s_models;
    static std::unordered_map<std::string, std::shared_ptr<renderer::ITexture>> s_textures;

    static std::string Normalize(const std::string& path);
};

template<> std::shared_ptr<Model>              AssetManager::Load<Model>(const std::string&);
template<> std::shared_ptr<renderer::ITexture> AssetManager::Load<renderer::ITexture>(const std::string&);
template<> void AssetManager::Unload<Model>(const std::string&);
template<> void AssetManager::Unload<renderer::ITexture>(const std::string&);

} // namespace fbzz::asset
