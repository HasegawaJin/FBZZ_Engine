// FBZZ Engine
// FzAssetLoader.cpp | fbzz::asset
// .fzasset マニフェスト → Model ランタイムロード
// Assimp 不要。fz* バイナリを直接デシリアライズする。
#include <Engine/Asset/FzAssetLoader.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <toml++/toml.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace fbzz::asset {

namespace {

// ── バイナリ読み込みヘルパー ──────────────────────────────────────────────
struct BinaryReader {
    std::vector<uint8_t> data;
    size_t pos = 0;

    bool Open(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        const auto size = static_cast<size_t>(f.tellg());
        data.resize(size);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        return f.good() || f.eof();
    }

    template<typename T>
    bool Read(T& out) {
        if (pos + sizeof(T) > data.size()) return false;
        std::memcpy(&out, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool ReadBytes(void* dst, size_t bytes) {
        if (pos + bytes > data.size()) return false;
        std::memcpy(dst, data.data() + pos, bytes);
        pos += bytes;
        return true;
    }

    bool Skip(size_t bytes) {
        if (pos + bytes > data.size()) return false;
        pos += bytes;
        return true;
    }
};

// ── float[16] → math::Matrix4 ────────────────────────────────────────────
math::Matrix4 FromFloatArray(const float src[16])
{
    math::Matrix4 m;
    std::memcpy(&m.m[0][0], src, 16 * sizeof(float));
    return m;
}

// ── .fzmesh → renderer::Mesh ─────────────────────────────────────────────
std::unique_ptr<renderer::Mesh> LoadFzMesh(const std::string& path,
                                            renderer::ResourceManager& resources)
{
    using namespace asset;
    BinaryReader r;
    if (!r.Open(path)) {
        FBZZ_LOG_ERROR("FzAssetLoader: cannot open mesh [%s]", path.c_str());
        return nullptr;
    }

    FzMeshHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'M' || hdr.magic[3] != 'H') {
        FBZZ_LOG_ERROR("FzAssetLoader: bad mesh magic [%s]", path.c_str());
        return nullptr;
    }

    const bool skinned = (hdr.flags & FZMESH_FLAG_SKINNED) != 0;
    auto mesh = std::make_unique<renderer::Mesh>();
    mesh->isSkinned    = skinned;
    mesh->vertexCount  = hdr.vertexCount;
    mesh->indexCount   = hdr.indexCount;
    mesh->boundsCenter = { hdr.boundsCenter[0], hdr.boundsCenter[1], hdr.boundsCenter[2] };
    mesh->boundsRadius = hdr.boundsRadius;

    if (!skinned) {
        mesh->cpuVertices.resize(hdr.vertexCount);
        if (!r.ReadBytes(mesh->cpuVertices.data(),
                         hdr.vertexCount * sizeof(renderer::Vertex))) {
            FBZZ_LOG_ERROR("FzAssetLoader: truncated vertex data [%s]", path.c_str());
            return nullptr;
        }
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            mesh->cpuVertices.data(),
            hdr.vertexCount * sizeof(renderer::Vertex),
            sizeof(renderer::Vertex));
    } else {
        mesh->cpuSkinnedVertices.resize(hdr.vertexCount);
        if (!r.ReadBytes(mesh->cpuSkinnedVertices.data(),
                         hdr.vertexCount * sizeof(renderer::SkinnedVertex))) {
            FBZZ_LOG_ERROR("FzAssetLoader: truncated skinned vertex data [%s]", path.c_str());
            return nullptr;
        }
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            mesh->cpuSkinnedVertices.data(),
            hdr.vertexCount * sizeof(renderer::SkinnedVertex),
            sizeof(renderer::SkinnedVertex));
    }

    mesh->cpuIndices.resize(hdr.indexCount);
    if (!r.ReadBytes(mesh->cpuIndices.data(),
                     hdr.indexCount * sizeof(uint32_t))) {
        FBZZ_LOG_ERROR("FzAssetLoader: truncated index data [%s]", path.c_str());
        return nullptr;
    }
    mesh->indexBuffer = resources.CreateIndexBuffer(
        mesh->cpuIndices.data(), hdr.indexCount);

    return mesh;
}

// ── .fzskel → Skeleton ───────────────────────────────────────────────────
std::unique_ptr<Skeleton> LoadFzSkel(const std::string& path)
{
    using namespace asset;
    BinaryReader r;
    if (!r.Open(path)) {
        FBZZ_LOG_ERROR("FzAssetLoader: cannot open skeleton [%s]", path.c_str());
        return nullptr;
    }

    FzSkelHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'S' || hdr.magic[3] != 'K') {
        FBZZ_LOG_ERROR("FzAssetLoader: bad skeleton magic [%s]", path.c_str());
        return nullptr;
    }

    auto skel = std::make_unique<Skeleton>();
    skel->rootNodeIndex       = hdr.rootNodeIndex;
    skel->rootInverseTransform = FromFloatArray(hdr.rootInverse);
    skel->nodes.resize(hdr.nodeCount);

    for (uint32_t ni = 0; ni < hdr.nodeCount; ++ni) {
        FzSkeletonNodeData nd{};
        if (!r.Read(nd)) return nullptr;

        SkeletonNode& node = skel->nodes[ni];
        node.name        = nd.name;
        node.parentIndex = nd.parentIndex;
        node.boneIndex   = nd.boneIndex;
        node.bindTranslation = { nd.bindTranslation[0], nd.bindTranslation[1], nd.bindTranslation[2] };
        node.bindRotation    = { nd.bindRotation[0], nd.bindRotation[1], nd.bindRotation[2], nd.bindRotation[3] };
        node.bindScale       = { nd.bindScale[0], nd.bindScale[1], nd.bindScale[2] };
        node.localBindTransform = FromFloatArray(nd.localBindTransform);
        skel->nodeMap[node.name] = static_cast<int>(ni);

        node.children.resize(nd.childCount);
        if (!r.ReadBytes(node.children.data(), nd.childCount * sizeof(int32_t)))
            return nullptr;
    }

    skel->bones.resize(hdr.boneCount);
    for (uint32_t bi = 0; bi < hdr.boneCount; ++bi) {
        FzBoneData bd{};
        if (!r.Read(bd)) return nullptr;

        Bone& bone = skel->bones[bi];
        bone.name       = bd.name;
        bone.nodeIndex  = bd.nodeIndex;
        bone.offsetMatrix = FromFloatArray(bd.offsetMatrix);
        skel->boneMap[bone.name] = static_cast<int>(bi);
    }

    return skel;
}

// ── .fzanim → AnimationClip ──────────────────────────────────────────────
AnimationClip LoadFzAnim(const std::string& path)
{
    using namespace asset;
    AnimationClip clip;
    BinaryReader r;
    if (!r.Open(path)) {
        FBZZ_LOG_ERROR("FzAssetLoader: cannot open anim [%s]", path.c_str());
        return clip;
    }

    FzAnimHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'A' || hdr.magic[3] != 'N') {
        FBZZ_LOG_ERROR("FzAssetLoader: bad anim magic [%s]", path.c_str());
        return clip;
    }

    clip.name           = hdr.name;
    clip.durationTicks  = hdr.durationTicks;
    clip.ticksPerSecond = (hdr.ticksPerSecond > 0.0) ? hdr.ticksPerSecond : 30.0;
    clip.tracks.resize(hdr.trackCount);

    for (uint32_t ti = 0; ti < hdr.trackCount; ++ti) {
        FzAnimTrackHeader th{};
        if (!r.Read(th)) return clip;

        NodeAnimationTrack& track = clip.tracks[ti];
        track.nodeName = th.nodeName;

        track.positions.resize(th.positionCount);
        for (uint32_t ki = 0; ki < th.positionCount; ++ki) {
            FzVectorKey vk{};
            if (!r.Read(vk)) return clip;
            track.positions[ki] = { vk.time, { vk.x, vk.y, vk.z } };
        }
        track.rotations.resize(th.rotationCount);
        for (uint32_t ki = 0; ki < th.rotationCount; ++ki) {
            FzQuaternionKey qk{};
            if (!r.Read(qk)) return clip;
            track.rotations[ki] = { qk.time, { qk.x, qk.y, qk.z, qk.w } };
        }
        track.scales.resize(th.scaleCount);
        for (uint32_t ki = 0; ki < th.scaleCount; ++ki) {
            FzVectorKey vk{};
            if (!r.Read(vk)) return clip;
            track.scales[ki] = { vk.time, { vk.x, vk.y, vk.z } };
        }
    }
    return clip;
}

// ── .fzmat → renderer::Material ──────────────────────────────────────────
std::unique_ptr<renderer::Material> LoadFzMat(const std::string& matPath,
                                               const std::string& texDir,
                                               renderer::ResourceManager& resources)
{
    std::ifstream f(matPath);
    if (!f) {
        FBZZ_LOG_ERROR("FzAssetLoader: cannot open material [%s]", matPath.c_str());
        return std::make_unique<renderer::Material>();
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    toml::parse_result parsed = toml::parse(buf.str());
    if (!parsed) {
        FBZZ_LOG_ERROR("FzAssetLoader: material TOML parse failed [%s]", matPath.c_str());
        return std::make_unique<renderer::Material>();
    }

    auto mat = std::make_unique<renderer::Material>();
    const toml::table& tbl = parsed.table();

    // textures テーブル → スロット 0 にアルベドをセット
    // WHY: MaterialComponent が ShaderDescriptor ベースでスロットを管理するため、
    //      ここでは最低限のテクスチャ解決のみ行い、スロット番号は 0 固定とする。
    //      将来 ShaderDescriptor のスロット名対応が整備されたら拡張する。
    const auto* texTbl = tbl["textures"].as_table();
    if (texTbl) {
        static const char* kSlotOrder[] = {
            "albedo", "normal", "metallic", "roughness", "ao", "emissive"
        };
        for (size_t si = 0; si < std::size(kSlotOrder); ++si) {
            auto pathNode = (*texTbl)[kSlotOrder[si]].value<std::string>();
            if (!pathNode) continue;

            const std::string fullPath = (fs::path(texDir) / *pathNode).string();
            auto handle = resources.LoadTexture(fullPath);
            if (handle.IsValid()) {
                if (mat->textures.size() <= si)
                    mat->textures.resize(si + 1);
                mat->textures[si] = handle;
            }
        }
    }

    return mat;
}

// ── TOML 文字列配列を std::vector<std::string> に展開 ───────────────────
std::vector<std::string> TomlArrayToStrings(const toml::array* arr)
{
    std::vector<std::string> out;
    if (!arr) return out;
    for (const auto& node : *arr) {
        if (auto s = node.value<std::string>()) out.push_back(*s);
    }
    return out;
}

} // namespace

// ── FzAssetLoader::Load ───────────────────────────────────────────────────
std::unique_ptr<Model> FzAssetLoader::Load(const std::string& fzassetPath,
                                            renderer::ResourceManager& resources)
{
    std::ifstream f(fzassetPath);
    if (!f) {
        FBZZ_LOG_WARN("FzAssetLoader: cannot open [%s]", fzassetPath.c_str());
        return nullptr;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    toml::parse_result parsed = toml::parse(buf.str());
    if (!parsed) {
        FBZZ_LOG_ERROR("FzAssetLoader: TOML parse failed [%s]: %s",
                        fzassetPath.c_str(), parsed.error().description().data());
        return nullptr;
    }
    const toml::table& tbl = parsed.table();



    const fs::path assetDir = fs::path(fzassetPath).parent_path();
    const std::string texDir = (assetDir / "textures").string();

    const auto meshPaths = TomlArrayToStrings(tbl["meshes"].as_array());
    const auto matPaths  = TomlArrayToStrings(tbl["materials"].as_array());
    const auto animPaths = TomlArrayToStrings(tbl["animations"].as_array());
    const auto skelPath  = tbl["skeleton"].value_or(std::string{});

    // meshes が空でも animations だけのマニフェスト (アニメーション専用 FBX 由来) は valid
    auto model = std::make_unique<Model>();

    // ── メッシュ + マテリアル ─────────────────────────────────────────────
    for (size_t i = 0; i < meshPaths.size(); ++i) {
        const std::string fullMesh = (assetDir / meshPaths[i]).string();
        auto mesh = LoadFzMesh(fullMesh, resources);
        if (!mesh) return nullptr;
        model->meshes.push_back(std::move(mesh));

        std::unique_ptr<renderer::Material> mat;
        if (i < matPaths.size() && !matPaths[i].empty()) {
            const std::string fullMat = (assetDir / matPaths[i]).string();
            mat = LoadFzMat(fullMat, texDir, resources);
        } else {
            mat = std::make_unique<renderer::Material>();
        }
        model->materials.push_back(std::move(mat));
    }

    // ── スケルトン ────────────────────────────────────────────────────────
    if (!skelPath.empty()) {
        const std::string fullSkel = (assetDir / skelPath).string();
        model->skeleton = LoadFzSkel(fullSkel);
        if (!model->skeleton)
            FBZZ_LOG_WARN("FzAssetLoader: skeleton load failed [%s]", fullSkel.c_str());
    }

    // ── アニメーション ────────────────────────────────────────────────────
    for (const auto& animRelPath : animPaths) {
        const std::string fullAnim = (assetDir / animRelPath).string();
        model->clips.push_back(LoadFzAnim(fullAnim));
    }

    return model;
}

} // namespace fbzz::asset
