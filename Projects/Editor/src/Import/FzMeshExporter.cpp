// FBZZ Engine
// FzMeshExporter.cpp | fbzz::editor
// aiMesh → .mesh バイナリ書き出し
#include <Editor/Import/FzMeshExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/mesh.h>
#include <assimp/scene.h>
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {

// ── 頂点変換ヘルパー ─────────────────────────────────────────────────────
// WHY: ModelImporterInternal.hpp は Engine src/ のプライベートヘッダーのため
//      Editor からは直接インクルードできない。同等の変換をここで定義する。

struct Vertex {
    float position[3];
    float normal[3];
    float tangent[3];
    float uv[2];
};
static_assert(sizeof(Vertex) == 44, "Vertex layout must match renderer::Vertex");

struct SkinnedVertex {
    float    position[3];
    float    normal[3];
    float    tangent[3];
    float    uv[2];
    uint32_t boneIndices[4];
    float    boneWeights[4];
};
static_assert(sizeof(SkinnedVertex) == 76, "SkinnedVertex layout must match renderer::SkinnedVertex");

Vertex ConvertVertex(const aiMesh* mesh, uint32_t i, float unitScale)
{
    Vertex v{};
    v.position[0] = mesh->mVertices[i].x * unitScale;
    v.position[1] = mesh->mVertices[i].y * unitScale;
    v.position[2] = mesh->mVertices[i].z * unitScale;

    if (mesh->mNormals) {
        v.normal[0] = mesh->mNormals[i].x;
        v.normal[1] = mesh->mNormals[i].y;
        v.normal[2] = mesh->mNormals[i].z;
    } else {
        v.normal[1] = 1.0f;
    }

    if (mesh->mTangents) {
        v.tangent[0] = mesh->mTangents[i].x;
        v.tangent[1] = mesh->mTangents[i].y;
        v.tangent[2] = mesh->mTangents[i].z;
    } else {
        v.tangent[0] = 1.0f;
    }

    if (mesh->mTextureCoords[0]) {
        v.uv[0] = mesh->mTextureCoords[0][i].x;
        v.uv[1] = mesh->mTextureCoords[0][i].y;
    }

    return v;
}

// 1 頂点に最大 4 ボーンを保持する作業用構造体
struct VertexInfluences {
    std::array<uint32_t, 4> indices{};
    std::array<float, 4>    weights{};

    void Add(uint32_t boneIdx, float w)
    {
        if (w <= 0.0f) return;
        for (int i = 0; i < 4; ++i) {
            if (w > weights[static_cast<size_t>(i)]) {
                for (int j = 3; j > i; --j) {
                    weights[static_cast<size_t>(j)] = weights[static_cast<size_t>(j - 1)];
                    indices[static_cast<size_t>(j)] = indices[static_cast<size_t>(j - 1)];
                }
                weights[static_cast<size_t>(i)] = w;
                indices[static_cast<size_t>(i)] = boneIdx;
                return;
            }
        }
    }

    void Normalize()
    {
        float sum = 0.0f;
        for (float w : weights) sum += w;
        if (sum <= 0.0f) { weights[0] = 1.0f; return; }
        for (float& w : weights) w /= sum;
    }
};

// バウンディング球をミニマックスから計算する (Ritter の近似)
void ComputeBounds(const std::vector<Vertex>& verts,
                   float outCenter[3], float& outRadius)
{
    if (verts.empty()) { outCenter[0] = outCenter[1] = outCenter[2] = 0.0f; outRadius = 0.0f; return; }
    float mn[3] = { verts[0].position[0], verts[0].position[1], verts[0].position[2] };
    float mx[3] = { verts[0].position[0], verts[0].position[1], verts[0].position[2] };
    for (const auto& v : verts) {
        for (int i = 0; i < 3; ++i) {
            if (v.position[i] < mn[i]) mn[i] = v.position[i];
            if (v.position[i] > mx[i]) mx[i] = v.position[i];
        }
    }
    for (int i = 0; i < 3; ++i) outCenter[i] = (mn[i] + mx[i]) * 0.5f;
    outRadius = 0.0f;
    for (const auto& v : verts) {
        float dx = v.position[0] - outCenter[0];
        float dy = v.position[1] - outCenter[1];
        float dz = v.position[2] - outCenter[2];
        float r2 = dx*dx + dy*dy + dz*dz;
        if (r2 > outRadius * outRadius) outRadius = std::sqrt(r2);
    }
}

void ComputeBoundsSkinned(const std::vector<SkinnedVertex>& verts,
                           float outCenter[3], float& outRadius)
{
    if (verts.empty()) { outCenter[0] = outCenter[1] = outCenter[2] = 0.0f; outRadius = 0.0f; return; }
    float mn[3] = { verts[0].position[0], verts[0].position[1], verts[0].position[2] };
    float mx[3] = { verts[0].position[0], verts[0].position[1], verts[0].position[2] };
    for (const auto& v : verts) {
        for (int i = 0; i < 3; ++i) {
            if (v.position[i] < mn[i]) mn[i] = v.position[i];
            if (v.position[i] > mx[i]) mx[i] = v.position[i];
        }
    }
    for (int i = 0; i < 3; ++i) outCenter[i] = (mn[i] + mx[i]) * 0.5f;
    outRadius = 0.0f;
    for (const auto& v : verts) {
        float dx = v.position[0] - outCenter[0];
        float dy = v.position[1] - outCenter[1];
        float dz = v.position[2] - outCenter[2];
        float r2 = dx*dx + dy*dy + dz*dz;
        if (r2 > outRadius * outRadius) outRadius = std::sqrt(r2);
    }
}

} // namespace

bool FzMeshExporter::Export(const aiMesh* mesh,
                             const aiScene* /*scene*/,
                             float unitScale,
                             bool isSkinned,
                             const std::string& outputPath)
{
    using namespace asset;

    auto out = util::FileSystem::OpenBinaryWriter(util::FileSystem::PathFromUtf8(outputPath));
    if (!out) {
        FBZZ_LOG_ERROR("FzMeshExporter: cannot open [%s]", outputPath.c_str());
        return false;
    }

    // ── インデックス ─────────────────────────────────────────────────────
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
    for (uint32_t fi = 0; fi < mesh->mNumFaces; ++fi) {
        const aiFace& f = mesh->mFaces[fi];
        if (f.mNumIndices != 3) continue;
        indices.push_back(f.mIndices[0]);
        indices.push_back(f.mIndices[1]);
        indices.push_back(f.mIndices[2]);
    }

    // ── ヘッダー書き出し ─────────────────────────────────────────────────
    FzMeshHeader hdr{};
    hdr.magic[0] = 'F'; hdr.magic[1] = 'Z'; hdr.magic[2] = 'M'; hdr.magic[3] = 'H';
    hdr.version      = FZMESH_VERSION;
    hdr.flags        = isSkinned ? FZMESH_FLAG_SKINNED : 0u;
    hdr.vertexCount  = mesh->mNumVertices;
    hdr.indexCount   = static_cast<uint32_t>(indices.size());

    if (!isSkinned)
    {
        // ── 静的メッシュ ─────────────────────────────────────────────────
        std::vector<Vertex> verts(mesh->mNumVertices);
        for (uint32_t i = 0; i < mesh->mNumVertices; ++i)
            verts[i] = ConvertVertex(mesh, i, unitScale);

        ComputeBounds(verts, hdr.boundsCenter, hdr.boundsRadius);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        out.write(reinterpret_cast<const char*>(verts.data()),
                  static_cast<std::streamsize>(verts.size() * sizeof(Vertex)));
    }
    else
    {
        // ── スキンメッシュ ───────────────────────────────────────────────
        // ボーン名 → グローバルインデックスのマップを構築する
        std::unordered_map<std::string, uint32_t> boneIndexMap;
        if (mesh->HasBones()) {
            for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi)
                boneIndexMap[mesh->mBones[bi]->mName.C_Str()] = bi;
        }

        std::vector<VertexInfluences> influences(mesh->mNumVertices);
        if (mesh->HasBones()) {
            for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi) {
                const aiBone* bone = mesh->mBones[bi];
                for (uint32_t wi = 0; wi < bone->mNumWeights; ++wi)
                    influences[bone->mWeights[wi].mVertexId].Add(bi, bone->mWeights[wi].mWeight);
            }
        }
        for (auto& inf : influences) inf.Normalize();

        std::vector<SkinnedVertex> verts(mesh->mNumVertices);
        for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
            const Vertex base = ConvertVertex(mesh, i, unitScale);
            std::memcpy(verts[i].position, base.position, sizeof(base.position));
            std::memcpy(verts[i].normal,   base.normal,   sizeof(base.normal));
            std::memcpy(verts[i].tangent,  base.tangent,  sizeof(base.tangent));
            std::memcpy(verts[i].uv,       base.uv,       sizeof(base.uv));
            std::memcpy(verts[i].boneIndices, influences[i].indices.data(), 4 * sizeof(uint32_t));
            std::memcpy(verts[i].boneWeights, influences[i].weights.data(), 4 * sizeof(float));
        }

        ComputeBoundsSkinned(verts, hdr.boundsCenter, hdr.boundsRadius);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        out.write(reinterpret_cast<const char*>(verts.data()),
                  static_cast<std::streamsize>(verts.size() * sizeof(SkinnedVertex)));
    }

    // ── インデックス書き出し ─────────────────────────────────────────────
    out.write(reinterpret_cast<const char*>(indices.data()),
              static_cast<std::streamsize>(indices.size() * sizeof(uint32_t)));

    return out.good();
}

} // namespace fbzz::editor
