/// @file    ModelStreamChannel.cpp
/// @brief   Model / ModelAsset / MaterialAsset の非同期経路。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include "ModelStreamChannel.hpp"
#include "BundledModel.hpp"

#include <Engine/Asset/AssetStreamChannel.hpp>
#include <Engine/Asset/MatAssetImporter.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/ModelAssetImporter.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace fbzz::asset {

namespace {

/// 落とせる品質段。実際の段数は LOD の数で頭打ちになる。
constexpr AssetQuality kLowestModelQuality = 3;

bool EndsWithCI(const std::string& text, std::string_view suffix)
{
    if (text.size() < suffix.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto a = std::tolower(static_cast<unsigned char>(text[text.size() - suffix.size() + i]));
        const auto b = std::tolower(static_cast<unsigned char>(suffix[i]));
        if (a != b) return false;
    }
    return true;
}

std::uint64_t MeshBytes(const renderer::Mesh& mesh)
{
    return mesh.cpuVertices.size() * sizeof(renderer::Vertex)
         + mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex)
         + mesh.cpuIndices.size() * sizeof(uint32_t);
}

/// @brief 同期 importer (ModelAssetImporter) と同じ作り方で頂点・インデックスバッファを作る。
/// @note バッファは UPLOAD ヒープへの書き込みで、GPU の完了を待つ必要が無い (転送トークンは 0)。
void CreateMeshBuffers(renderer::ResourceManager& resources, renderer::Mesh& mesh)
{
    if (mesh.isSkinned && !mesh.cpuSkinnedVertices.empty()) {
        mesh.vertexBuffer = resources.CreateVertexBuffer(
            mesh.cpuSkinnedVertices.data(),
            mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex),
            sizeof(renderer::SkinnedVertex));
    } else if (!mesh.cpuVertices.empty()) {
        mesh.vertexBuffer = resources.CreateVertexBuffer(
            mesh.cpuVertices.data(),
            mesh.cpuVertices.size() * sizeof(renderer::Vertex),
            sizeof(renderer::Vertex));
    }
    if (!mesh.cpuIndices.empty())
        mesh.indexBuffer = resources.CreateIndexBuffer(mesh.cpuIndices.data(),
                                                       static_cast<uint32_t>(mesh.cpuIndices.size()));
}

/// @brief 同期でしか読めない形式 (.mesh / DCC 直読み) かどうかを、メインスレッドで決めて渡す。
struct ModelJobContext final : AssetJobContext {
    bool syncOnly = false;
};

template<typename T>
struct DecodedModel final : IDecodedAsset {
    std::unique_ptr<T> asset;
    std::string        resolvedPath;
    bool               syncOnly = false;
    std::uint64_t      bytes = 0;

    [[nodiscard]] std::size_t CpuBytes() const override { return static_cast<std::size_t>(bytes); }
};

/// @brief Model / ModelAsset 共通の部分。T ごとの違いは «解析結果の組み立て» と «メッシュの列挙» だけ。
template<typename T>
class ModelChannelBase : public AssetStreamChannel<T> {
public:
    explicit ModelChannelBase(renderer::ResourceManager& resources) : m_resources(resources) {}

    [[nodiscard]] bool Snapshot(const std::string& key, AssetJobInput& out, std::string& outError) override
    {
        /// @note .fbx は Library/Baked の .fzasset へ寄せる (同期 Load と同じ規則)。
        std::string resolved = AssetManager::ResolveImportPath(key, true);
        if (resolved.empty()) {
            outError = "モデルを解決できません: " + key;
            return false;
        }
        auto context = std::make_shared<ModelJobContext>();
        context->syncOnly = !EndsWithCI(resolved, ".fzasset");
        out.resolvedPath = std::move(resolved);
        out.context = std::move(context);
        return true;
    }

    [[nodiscard]] AssetDecodeResult Decode(const AssetJobInput& input) override
    {
        AssetDecodeResult result;
        auto decoded = std::make_unique<DecodedModel<T>>();
        decoded->resolvedPath = input.resolvedPath;
        decoded->syncOnly = static_cast<const ModelJobContext&>(*input.context).syncOnly;
        if (!decoded->syncOnly) {
            /// @note resources を渡さない = CPU の解析だけ。GPU バッファは BeginUpload がメインスレッドで作る。
            std::unique_ptr<ModelAsset> parsed = Parse(input.resolvedPath, input.quality);
            if (!parsed) {
                result.error = AssetLoadError::DecodeFailed;
                result.message = "モデルを解析できません: " + input.resolvedPath;
                return result;
            }
            decoded->asset = Build(std::move(parsed), input.quality);
            if (!decoded->asset) {
                result.error = AssetLoadError::DecodeFailed;
                result.message = "モデルにメッシュがありません: " + input.resolvedPath;
                return result;
            }
            ForEachMesh(*decoded->asset, [&](renderer::Mesh& mesh) { decoded->bytes += MeshBytes(mesh); });
        }
        result.decoded = std::move(decoded);
        return result;
    }

    [[nodiscard]] std::uint64_t DeviceEpoch() const override { return m_resources.GetResetVersion(); }

    [[nodiscard]] std::unique_ptr<IAssetCandidate> BeginUpload(
        IDecodedAsset& decoded, std::uint64_t& outToken, std::string& outError) override
    {
        auto& model = static_cast<DecodedModel<T>&>(decoded);
        outToken = 0;
        auto candidate = std::make_unique<AssetCandidate<T>>();
        if (model.syncOnly) {
            /// @note 索引の無い形式は同期 importer に任せる (設計の «索引のない旧形式は全体ロード»)。
            auto& importer = AssetStore<T>::Get().importer;
            candidate->asset = importer ? importer->Import(model.resolvedPath, &m_resources) : nullptr;
        } else if (model.asset) {
            /// @note デバイスが作り直されたら同じ CPU 成果物からもう一度呼ばれる。古いハンドルは捨てて作り直す。
            ForEachMesh(*model.asset, [&](renderer::Mesh& mesh) {
                mesh.vertexBuffer = {};
                mesh.indexBuffer = {};
                CreateMeshBuffers(m_resources, mesh);
            });
            candidate->asset = std::move(model.asset);
        }
        if (!candidate->asset) {
            outError = "モデルを作れません: " + model.resolvedPath;
            return nullptr;
        }
        return candidate;
    }

    [[nodiscard]] std::uint64_t EstimateResidentBytes(RawAssetHandle handle) const override
    {
        std::uint64_t bytes = 0;
        if (T* asset = this->TypedGet(handle)) ForEachMesh(*asset, [&](renderer::Mesh& mesh) { bytes += MeshBytes(mesh); });
        return bytes;
    }

protected:
    /// @brief ワーカー: ファイルを解析する。既定は全体を読む。
    [[nodiscard]] virtual std::unique_ptr<ModelAsset> Parse(const std::string& path, AssetQuality /*quality*/) const
    {
        ModelAssetImporter importer;
        return importer.Import(path, nullptr);
    }
    [[nodiscard]] virtual std::unique_ptr<T> Build(std::unique_ptr<ModelAsset> parsed, AssetQuality quality) const = 0;

    template<typename Fn>
    static void ForEachMesh(T& asset, Fn&& fn)
    {
        if constexpr (std::is_same_v<T, Model>) {
            for (auto& mesh : asset.meshes) if (mesh) fn(*mesh);
        } else {
            for (LodLevel& lod : asset.lods)
                for (SubmeshEntry& submesh : lod.submeshes)
                    if (submesh.mesh) fn(*submesh.mesh);
        }
    }

    renderer::ResourceManager& m_resources;
};

class ModelStreamChannel final : public ModelChannelBase<Model> {
public:
    using ModelChannelBase::ModelChannelBase;

    /// @note 公開中の Model は差し替えない (MeshRenderer が Mesh* を持っている)。更新失敗として記録させる。
    [[nodiscard]] bool Publish(RawAssetHandle handle, IAssetCandidate& candidate) override
    {
        if (TypedGet(handle) != nullptr) return false;
        return ModelChannelBase::Publish(handle, candidate);
    }

protected:
    [[nodiscard]] std::unique_ptr<Model> Build(std::unique_ptr<ModelAsset> parsed, AssetQuality) const override
    {
        return detail::BuildBundledModel(std::move(parsed));
    }
};

class ModelAssetStreamChannel final : public ModelChannelBase<ModelAsset> {
public:
    using ModelChannelBase::ModelChannelBase;

    [[nodiscard]] bool SupportsQuality() const override { return true; }
    [[nodiscard]] AssetQuality LowestQuality() const override { return kLowestModelQuality; }

protected:
    /// @note 部分 I/O: 品質段より高品質な LOD の本体はディスクから読まない (シークで飛ばす)。
    [[nodiscard]] std::unique_ptr<ModelAsset> Parse(const std::string& path, AssetQuality quality) const override
    {
        ModelAssetImporter importer;
        return importer.ImportPartial(path, nullptr, quality);
    }

    /// @note 品質段より高品質な LOD のメッシュを捨てる (submesh の枠と名前は残す)。最低品質の LOD は必ず残す。
    [[nodiscard]] std::unique_ptr<ModelAsset> Build(std::unique_ptr<ModelAsset> parsed, AssetQuality quality) const override
    {
        if (parsed->lods.empty()) return nullptr;
        const std::size_t firstResident = (std::min)(static_cast<std::size_t>(quality), parsed->lods.size() - 1);
        for (std::size_t lod = 0; lod < firstResident; ++lod)
            for (SubmeshEntry& submesh : parsed->lods[lod].submeshes)
                submesh.mesh.reset();
        return parsed;
    }
};

struct DecodedMaterial final : IDecodedAsset {
    std::unique_ptr<MaterialAsset> asset;
};

class MaterialStreamChannel final : public AssetStreamChannel<MaterialAsset> {
public:
    [[nodiscard]] bool Snapshot(const std::string& key, AssetJobInput& out, std::string& outError) override
    {
        out.resolvedPath = AssetManager::ResolveImportPath(key, false);
        if (out.resolvedPath.empty()) {
            outError = "マテリアルを解決できません: " + key;
            return false;
        }
        return true;
    }

    /// @note MatAssetImporter はファイルの解析だけで AssetDatabase もストアも触らないので、ワーカーで呼べる。
    [[nodiscard]] AssetDecodeResult Decode(const AssetJobInput& input) override
    {
        AssetDecodeResult result;
        MatAssetImporter importer;
        auto decoded = std::make_unique<DecodedMaterial>();
        decoded->asset = importer.Import(input.resolvedPath, nullptr);
        if (!decoded->asset) {
            result.error = AssetLoadError::DecodeFailed;
            result.message = "マテリアルを解析できません: " + input.resolvedPath;
            return result;
        }
        const std::string textureType(AssetStreamTypeName<TextureAsset>());
        for (const auto& [slot, reference] : decoded->asset->textures) {
            if (reference.empty()) continue;
            /// @note Sprite 参照は親テクスチャを要求する。接尾辞込みだと Sprite の数だけ同じ画像を読む。
            std::string texturePath;
            std::string spriteToken;
            decoded->dependencies.push_back({ textureType,
                ParseSpriteReference(reference, texturePath, spriteToken) ? texturePath : reference, false });
        }
        result.decoded = std::move(decoded);
        return result;
    }

    [[nodiscard]] std::unique_ptr<IAssetCandidate> BeginUpload(
        IDecodedAsset& decoded, std::uint64_t& outToken, std::string&) override
    {
        outToken = 0;
        auto candidate = std::make_unique<AssetCandidate<MaterialAsset>>();
        candidate->asset = std::move(static_cast<DecodedMaterial&>(decoded).asset);
        return candidate;
    }
};

} // namespace

std::shared_ptr<IAssetStreamChannel> CreateMaterialStreamChannel()
{
    return std::make_shared<MaterialStreamChannel>();
}

std::shared_ptr<IAssetStreamChannel> CreateModelStreamChannel(renderer::ResourceManager& resources)
{
    return std::make_shared<ModelStreamChannel>(resources);
}

std::shared_ptr<IAssetStreamChannel> CreateModelAssetStreamChannel(renderer::ResourceManager& resources)
{
    return std::make_shared<ModelAssetStreamChannel>(resources);
}

} // namespace fbzz::asset
