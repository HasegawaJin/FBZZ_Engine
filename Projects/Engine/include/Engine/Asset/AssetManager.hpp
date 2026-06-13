// FBZZ Engine
// AssetManager.hpp | fbzz::asset
// Model と Texture のロードおよびキャッシュ管理
// Renderer の ResourceManager を経由して GPU リソースを作り、ハンドルを利用側へ渡す。
// 同じ相対パスは共有キャッシュとして扱い、明示的な Unload / UnloadAll まで保持する。
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <Engine/Renderer/ResourceHandle.hpp>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset    { struct MaterialAsset; }
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

    // FlushFailed() を呼ぶたびにインクリメントされる世代番号。
    // AnimatorSystem はこれを見てインポート後の再試行タイミングを判断する。
    static int GetFlushGeneration();
    // Assets 起点パスを実ファイルパスへ解決する。CPU 側独自アセットのローダーで共有する。
    [[nodiscard]] static std::string ResolveAssetPath(const std::string& path);

    static renderer::ResourceHandle<renderer::TextureTag> LoadTexture(const std::string& relativePath);

    // MaterialAsset: CPU 側アセットをスロットプールで管理し ResourceHandle を返す。
    // WHY: shared_ptr ではキャッシュキーが絶対/相対パスで分岐して別インスタンスが生まれる。
    //      ハンドル化することで AssetManager が唯一の所有者となり参照が一本化される。
    static renderer::ResourceHandle<renderer::MaterialAssetTag> LoadMaterial(const std::string& relativePath);
    static MaterialAsset* GetMaterial(renderer::ResourceHandle<renderer::MaterialAssetTag> h);
    static void UnloadMaterial(const std::string& relativePath);

    // Load<T>: Model のみ対応。MaterialAsset は LoadMaterial() を使うこと。
    // 未対応型はヘッダー内の static_assert によりコンパイルエラーになる
    template<typename T>
    static std::shared_ptr<T> Load(const std::string& relativePath) {
        static_assert(sizeof(T) == 0,
            "AssetManager::Load<T>: unsupported type. Use Model or LoadMaterial for MaterialAsset.");
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

    // MaterialAsset スロットプール
    // WHY: ResourcePool (renderer 側) は GPU リソース向けのため MemoryDebug が必要。
    //      CPU アセットには軽量な独立プールを持ち、ResourceHandle<MaterialAssetTag> を発行する。
    struct MatSlot {
        std::unique_ptr<MaterialAsset> asset;
        uint32_t gen     = 1; // 0 は ResourceHandle デフォルト値なので 1 から始める
        bool     occupied = false;
    };
    static std::vector<MatSlot>   s_materialSlots;
    static std::vector<uint32_t>  s_materialFreeList;
    static std::unordered_map<std::string, renderer::ResourceHandle<renderer::MaterialAssetTag>> s_materials;

    static std::string Normalize(const std::string& path);

    // スロット操作ヘルパー
    static renderer::ResourceHandle<renderer::MaterialAssetTag> AllocMaterialSlot(std::unique_ptr<MaterialAsset> asset);
    static bool IsMaterialLive(renderer::ResourceHandle<renderer::MaterialAssetTag> h);
};

template<> std::shared_ptr<Model> AssetManager::Load<Model>(const std::string&);
template<> void AssetManager::Unload<Model>(const std::string&);

} // namespace fbzz::asset
