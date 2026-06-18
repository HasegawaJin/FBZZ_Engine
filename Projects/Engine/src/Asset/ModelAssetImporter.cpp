// FBZZ Engine
// ModelAssetImporter.cpp | fbzz::asset
// .fzasset バイナリ → ModelAsset デシリアライザ
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Asset/FzModelFormat.hpp>
#include <Engine/Asset/ModelAssetImporter.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstring>
#include <vector>

namespace fbzz::asset {

namespace {

math::Matrix4 FromFloatArray(const float src[16])
{
    math::Matrix4 m;
    std::memcpy(&m.m[0][0], src, 16 * sizeof(float));
    return m;
}

bool ReadSkeleton(BinaryReader& r, ModelAsset& out, const std::string& path)
{
    FzSkelHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'S' || hdr.magic[3] != 'K') {
        FBZZ_LOG_ERROR("ModelAssetImporter: bad skeleton magic [%s]", path.c_str());
        return false;
    }

    out.skeleton = std::make_unique<Skeleton>();
    Skeleton& skel = *out.skeleton;
    skel.rootNodeIndex        = hdr.rootNodeIndex;
    skel.rootInverseTransform = FromFloatArray(hdr.rootInverse);
    skel.nodes.resize(hdr.nodeCount);

    for (uint32_t ni = 0; ni < hdr.nodeCount; ++ni) {
        FzSkeletonNodeData nd{};
        if (!r.Read(nd)) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated skeleton node %u [%s]", ni, path.c_str());
            return false;
        }

        SkeletonNode& node   = skel.nodes[ni];
        node.name            = nd.name;
        node.parentIndex     = nd.parentIndex;
        node.boneIndex       = nd.boneIndex;
        node.bindTranslation = { nd.bindTranslation[0], nd.bindTranslation[1], nd.bindTranslation[2] };
        node.bindRotation    = { nd.bindRotation[0], nd.bindRotation[1], nd.bindRotation[2], nd.bindRotation[3] };
        node.bindScale       = { nd.bindScale[0], nd.bindScale[1], nd.bindScale[2] };
        node.localBindTransform = FromFloatArray(nd.localBindTransform);
        skel.nodeMap[node.name] = static_cast<int>(ni);

        node.children.resize(nd.childCount);
        if (!r.ReadBytes(node.children.data(), nd.childCount * sizeof(int32_t))) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated skeleton children %u [%s]", ni, path.c_str());
            return false;
        }
    }

    skel.bones.resize(hdr.boneCount);
    for (uint32_t bi = 0; bi < hdr.boneCount; ++bi) {
        FzBoneData bd{};
        if (!r.Read(bd)) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated bone %u [%s]", bi, path.c_str());
            return false;
        }

        Bone& bone       = skel.bones[bi];
        bone.name        = bd.name;
        bone.nodeIndex   = bd.nodeIndex;
        bone.offsetMatrix = FromFloatArray(bd.offsetMatrix);
        skel.boneMap[bone.name] = static_cast<int>(bi);
    }
    return true;
}

} // namespace

std::unique_ptr<ModelAsset> ModelAssetImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* resources)
{
    FBZZ_LOG_INFO("ModelAssetImporter: importing [%s]", absPath.c_str());
    BinaryReader r;
    if (!r.Open(absPath)) {
        FBZZ_LOG_ERROR("ModelAssetImporter: cannot open [%s]", absPath.c_str());
        return nullptr;
    }

    FzModelHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'M' || hdr.magic[3] != 'D') {
        FBZZ_LOG_ERROR("ModelAssetImporter: bad magic [%s] (%02X %02X %02X %02X)",
                       absPath.c_str(),
                       static_cast<unsigned char>(hdr.magic[0]),
                       static_cast<unsigned char>(hdr.magic[1]),
                       static_cast<unsigned char>(hdr.magic[2]),
                       static_cast<unsigned char>(hdr.magic[3]));
        return nullptr;
    }

    if (hdr.version != FZMODEL_VERSION || hdr.lodCount == 0) {
        FBZZ_LOG_ERROR("ModelAssetImporter: unsupported header version=%u lodCount=%u [%s]",
                       hdr.version, hdr.lodCount, absPath.c_str());
        return nullptr;
    }

    auto model = std::make_unique<ModelAsset>();
    const bool skinned = (hdr.flags & FZMODEL_FLAG_SKINNED) != 0;

    // マテリアルスロット名
    model->materialSlotNames.resize(hdr.materialSlotCount);
    for (uint32_t i = 0; i < hdr.materialSlotCount; ++i) {
        char nameBuf[FZMODEL_SLOT_NAME_LEN]{};
        if (!r.ReadBytes(nameBuf, FZMODEL_SLOT_NAME_LEN)) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated material slot %u [%s]", i, absPath.c_str());
            return nullptr;
        }
        model->materialSlotNames[i] = nameBuf;
    }

    // LOD ループ
    model->lods.resize(hdr.lodCount);
    for (uint32_t li = 0; li < hdr.lodCount; ++li) {
        FzLodHeader lodHdr{};
        if (!r.Read(lodHdr)) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated LOD header %u [%s]", li, absPath.c_str());
            return nullptr;
        }

        LodLevel& lod = model->lods[li];
        lod.screenSizeThreshold = lodHdr.screenSizeThreshold;
        lod.submeshes.resize(lodHdr.submeshCount);

        for (uint32_t si = 0; si < lodHdr.submeshCount; ++si) {
            FzSubmeshHeader smHdr{};
            if (!r.Read(smHdr)) {
                FBZZ_LOG_ERROR("ModelAssetImporter: truncated submesh header lod=%u submesh=%u [%s]",
                               li, si, absPath.c_str());
                return nullptr;
            }

            SubmeshEntry& entry = lod.submeshes[si];
            entry.materialSlotIndex = smHdr.materialSlotIndex;

            auto mesh = std::make_unique<renderer::Mesh>();
            mesh->vertexCount  = smHdr.vertexCount;
            mesh->indexCount   = smHdr.indexCount;
            mesh->boundsCenter = { smHdr.boundsCenter[0], smHdr.boundsCenter[1], smHdr.boundsCenter[2] };
            mesh->boundsRadius = smHdr.boundsRadius;
            if (smHdr.vertexFormat > 1) {
                FBZZ_LOG_ERROR("ModelAssetImporter: unknown vertex format %u lod=%u submesh=%u [%s]",
                               smHdr.vertexFormat, li, si, absPath.c_str());
                return nullptr;
            }
            mesh->isSkinned    = (smHdr.vertexFormat == 1);

            if (!mesh->isSkinned) {
                mesh->cpuVertices.resize(smHdr.vertexCount);
                if (!r.ReadBytes(mesh->cpuVertices.data(),
                                 smHdr.vertexCount * sizeof(renderer::Vertex))) {
                    FBZZ_LOG_ERROR("ModelAssetImporter: truncated static vertices lod=%u submesh=%u count=%u [%s]",
                                   li, si, smHdr.vertexCount, absPath.c_str());
                    return nullptr;
                }
                if (resources)
                    mesh->vertexBuffer = resources->CreateVertexBuffer(
                        mesh->cpuVertices.data(),
                        smHdr.vertexCount * sizeof(renderer::Vertex),
                        sizeof(renderer::Vertex));
            } else {
                mesh->cpuSkinnedVertices.resize(smHdr.vertexCount);
                if (!r.ReadBytes(mesh->cpuSkinnedVertices.data(),
                                 smHdr.vertexCount * sizeof(renderer::SkinnedVertex))) {
                    FBZZ_LOG_ERROR("ModelAssetImporter: truncated skinned vertices lod=%u submesh=%u count=%u [%s]",
                                   li, si, smHdr.vertexCount, absPath.c_str());
                    return nullptr;
                }
                if (resources)
                    mesh->vertexBuffer = resources->CreateVertexBuffer(
                        mesh->cpuSkinnedVertices.data(),
                        smHdr.vertexCount * sizeof(renderer::SkinnedVertex),
                        sizeof(renderer::SkinnedVertex));
            }

            mesh->cpuIndices.resize(smHdr.indexCount);
            if (!r.ReadBytes(mesh->cpuIndices.data(),
                             smHdr.indexCount * sizeof(uint32_t))) {
                FBZZ_LOG_ERROR("ModelAssetImporter: truncated indices lod=%u submesh=%u count=%u [%s]",
                               li, si, smHdr.indexCount, absPath.c_str());
                return nullptr;
            }
            if (resources)
                mesh->indexBuffer = resources->CreateIndexBuffer(
                    mesh->cpuIndices.data(), smHdr.indexCount);

            entry.mesh = std::move(mesh);
        }
    }

    // スケルトン
    if (skinned) {
        if (!ReadSkeleton(r, *model, absPath)) {
            FBZZ_LOG_WARN("ModelAssetImporter: skeleton read failed [%s]", absPath.c_str());
        }
    }

    return model;
}

} // namespace fbzz::asset
