/// @file    ModelAssetImporter.cpp
/// @brief   .fzasset バイナリ → ModelAsset デシリアライザ。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Asset/FzModelFormat.hpp>
#include <Engine/Asset/FzVertexCompat.hpp>
#include <Engine/Asset/ModelAssetImporter.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
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
    /// @note 無アニメ時の既定パレット。単位行列を使わないための前提データ。
    BuildReferencePose(skel);
    return true;
}

/// v4 のノード階層チャンクを読む。ファイル末尾に置かれている。
bool ReadNodes(BinaryReader& r, ModelAsset& out, const std::string& path)
{
    FzModelNodeChunkHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'N' || hdr.magic[3] != 'D') {
        FBZZ_LOG_ERROR("ModelAssetImporter: bad node chunk magic [%s]", path.c_str());
        return false;
    }

    out.rootNodeIndex = hdr.rootNodeIndex;
    out.nodes.resize(hdr.nodeCount);

    for (uint32_t ni = 0; ni < hdr.nodeCount; ++ni) {
        FzModelNodeData nd{};
        if (!r.Read(nd)) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated node %u [%s]", ni, path.c_str());
            return false;
        }

        ModelNode& node       = out.nodes[ni];
        node.name             = nd.name;
        node.parentIndex      = nd.parentIndex;
        node.localTranslation = { nd.localTranslation[0], nd.localTranslation[1],
                                  nd.localTranslation[2] };
        node.localRotation    = { nd.localRotation[0], nd.localRotation[1],
                                  nd.localRotation[2], nd.localRotation[3] };
        node.localScale       = { nd.localScale[0], nd.localScale[1], nd.localScale[2] };

        node.meshIndices.resize(nd.meshCount);
        if (nd.meshCount > 0 &&
            !r.ReadBytes(node.meshIndices.data(), nd.meshCount * sizeof(uint32_t))) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated node meshes %u [%s]", ni, path.c_str());
            return false;
        }

        node.children.resize(nd.childCount);
        if (nd.childCount > 0 &&
            !r.ReadBytes(node.children.data(), nd.childCount * sizeof(int32_t))) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated node children %u [%s]", ni, path.c_str());
            return false;
        }
    }
    return true;
}

/// @brief submesh の本体 (頂点・インデックス・モーフ) を読まずに飛ばす。大きさはヘッダーから決まる。
bool SkipSubmeshPayload(BinaryReader& r, const FzSubmeshHeader& header,
                        const FzSubmeshExtensionV3& extension, bool hasVertexColor)
{
    const std::size_t vertexStride = header.vertexFormat == 1
        ? sizeof(renderer::SkinnedVertex)
        : (hasVertexColor ? sizeof(renderer::Vertex) : sizeof(FzVertexV1));
    if (!r.Skip(static_cast<std::size_t>(header.vertexCount) * vertexStride
                + static_cast<std::size_t>(header.indexCount) * sizeof(uint32_t)))
        return false;
    for (uint32_t mi = 0; mi < extension.morphTargetCount; ++mi) {
        FzMorphTargetHeader morphHeader{};
        if (!r.Read(morphHeader) || morphHeader.vertexCount != header.vertexCount) return false;
        if (!r.Skip(static_cast<std::size_t>(morphHeader.vertexCount) * sizeof(FzMorphDelta))) return false;
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
    return ImportFromReader(r, absPath, resources, 0);
}

std::unique_ptr<ModelAsset> ModelAssetImporter::ImportPartial(
    const std::string&         absPath,
    renderer::ResourceManager* resources,
    uint32_t                   firstResidentLod,
    uint64_t*                  outBytesRead)
{
    BinaryReader r;
    if (!r.OpenStreaming(absPath)) {
        FBZZ_LOG_ERROR("ModelAssetImporter: cannot open [%s]", absPath.c_str());
        return nullptr;
    }
    auto model = ImportFromReader(r, absPath, resources, firstResidentLod);
    if (outBytesRead) *outBytesRead = r.bytesRead;
    return model;
}

std::unique_ptr<ModelAsset> ModelAssetImporter::ImportFromReader(
    BinaryReader&              r,
    const std::string&         absPath,
    renderer::ResourceManager* resources,
    uint32_t                   firstResidentLod)
{
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

    /// @note v4 以前は静的頂点に色が無いだけで、他のチャンクは同一レイアウト。
    if (hdr.version < FZMODEL_VERSION_PRE_VERTEX_COLOR || hdr.version > FZMODEL_VERSION
        || hdr.lodCount == 0) {
        FBZZ_LOG_ERROR("ModelAssetImporter: unsupported header version=%u lodCount=%u [%s]",
                       hdr.version, hdr.lodCount, absPath.c_str());
        return nullptr;
    }
    const bool hasVertexColor = hdr.version > FZMODEL_VERSION_PRE_VERTEX_COLOR;

    auto model = std::make_unique<ModelAsset>();
    const bool skinned = (hdr.flags & FZMODEL_FLAG_SKINNED) != 0;

    /// @note マテリアルスロット名
    model->materialSlotNames.resize(hdr.materialSlotCount);
    for (uint32_t i = 0; i < hdr.materialSlotCount; ++i) {
        char nameBuf[FZMODEL_SLOT_NAME_LEN]{};
        if (!r.ReadBytes(nameBuf, FZMODEL_SLOT_NAME_LEN)) {
            FBZZ_LOG_ERROR("ModelAssetImporter: truncated material slot %u [%s]", i, absPath.c_str());
            return nullptr;
        }
        model->materialSlotNames[i] = nameBuf;
    }

    /// @note 最低品質の LOD は必ず読む。これより高品質な LOD は本体をシークで飛ばす。
    const uint32_t firstResident = (std::min)(firstResidentLod, hdr.lodCount - 1u);

    /// @note LOD ループ
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
            FzSubmeshExtensionV3 smExtV3{};
            if (!r.Read(smExtV3)) {
                FBZZ_LOG_ERROR("ModelAssetImporter: truncated submesh v3 extension lod=%u submesh=%u [%s]",
                               li, si, absPath.c_str());
                return nullptr;
            }

            SubmeshEntry& entry = lod.submeshes[si];
            entry.materialSlotIndex = smHdr.materialSlotIndex;
            entry.name = smExtV3.name;

            if (li < firstResident) {
                if (!SkipSubmeshPayload(r, smHdr, smExtV3, hasVertexColor)) {
                    FBZZ_LOG_ERROR("ModelAssetImporter: truncated submesh payload lod=%u submesh=%u [%s]",
                                   li, si, absPath.c_str());
                    return nullptr;
                }
                continue;
            }

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
                if (!ReadStaticVertices(r, smHdr.vertexCount, hasVertexColor, mesh->cpuVertices)) {
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
            /// @note ヘッダーは球しか持たないので AABB だけ頂点から補う (遮蔽者に使えるかの判定材料)。
            mesh->ComputeBoundsExtents();
            if (resources)
                mesh->indexBuffer = resources->CreateIndexBuffer(
                    mesh->cpuIndices.data(), smHdr.indexCount);

            const uint32_t morphTargetCount = smExtV3.morphTargetCount;
            mesh->morphTargets.resize(morphTargetCount);
            for (uint32_t mi = 0; mi < morphTargetCount; ++mi) {
                FzMorphTargetHeader morphHeader{};
                if (!r.Read(morphHeader) || morphHeader.vertexCount != smHdr.vertexCount) {
                    FBZZ_LOG_ERROR("ModelAssetImporter: invalid morph header lod=%u submesh=%u morph=%u [%s]",
                                   li, si, mi, absPath.c_str());
                    return nullptr;
                }
                auto& morph = mesh->morphTargets[mi];
                morph.name = morphHeader.name;
                morph.positionDeltas.resize(morphHeader.vertexCount);
                morph.normalDeltas.resize(morphHeader.vertexCount);
                morph.tangentDeltas.resize(morphHeader.vertexCount);
                for (uint32_t vi = 0; vi < morphHeader.vertexCount; ++vi) {
                    FzMorphDelta delta{};
                    if (!r.Read(delta)) return nullptr;
                    morph.positionDeltas[vi] = {
                        delta.position[0], delta.position[1], delta.position[2]
                    };
                    morph.normalDeltas[vi] = {
                        delta.normal[0], delta.normal[1], delta.normal[2]
                    };
                    morph.tangentDeltas[vi] = {
                        delta.tangent[0], delta.tangent[1], delta.tangent[2]
                    };
                }
            }

            entry.mesh = std::move(mesh);
        }
    }

    /// @note スケルトン
    if (skinned) {
        if (!ReadSkeleton(r, *model, absPath)) {
            FBZZ_LOG_WARN("ModelAssetImporter: skeleton read failed [%s]", absPath.c_str());
        }
    }

    /// @note ノード階層 (v4)。読めなくても配置側が「全 submesh を 1 GameObject」へ
    ///       フォールバックできるため、失敗は警告に留めてモデル自体は返す。
    if ((hdr.flags & FZMODEL_FLAG_NODES) != 0u) {
        model->nodeTransformsBaked =
            (hdr.flags & FZMODEL_FLAG_NODE_TRANSFORMS_BAKED) != 0u;
        if (!ReadNodes(r, *model, absPath)) {
            FBZZ_LOG_WARN("ModelAssetImporter: node hierarchy read failed [%s]", absPath.c_str());
            model->nodes.clear();
            model->rootNodeIndex = -1;
        }
    }

    return model;
}

} // namespace fbzz::asset
