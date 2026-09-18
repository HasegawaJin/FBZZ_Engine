/// @file    SkeletonImporter.cpp
/// @brief   .skel バイナリ v1 → Skeleton デシリアライザ。
/// @author  Hasegawa Jin
/// @date    2026-06-19
///
/// フォーマット: FzSkelHeader (magic "FZSK") + FzSkeletonNodeData[] + FzBoneData[]
/// ModelAssetImporter::ReadSkeleton と同じ読み込みロジックを持つが、.skel は単独ファイル
/// として存在し、メッシュなしのスケルトン参照を可能にする。
#include <Engine/Asset/SkeletonImporter.hpp>
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <array>
#include <cstring>

namespace fbzz::asset {

namespace {

math::Matrix4 FromFloatArray(const float src[16])
{
    math::Matrix4 m;
    std::memcpy(&m.m[0][0], src, 16 * sizeof(float));
    return m;
}

} // namespace

std::unique_ptr<Skeleton> SkeletonImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    FBZZ_LOG_INFO("SkeletonImporter: importing [%s]", absPath.c_str());

    BinaryReader r;
    if (!r.Open(absPath)) {
        FBZZ_LOG_ERROR("SkeletonImporter: cannot open [%s]", absPath.c_str());
        return nullptr;
    }

    FzSkelHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'S' || hdr.magic[3] != 'K') {
        FBZZ_LOG_ERROR("SkeletonImporter: bad magic [%s]", absPath.c_str());
        return nullptr;
    }
    if (hdr.version != FZSKEL_VERSION) {
        FBZZ_LOG_ERROR("SkeletonImporter: unsupported version %u [%s]",
                       hdr.version, absPath.c_str());
        return nullptr;
    }

    auto skel = std::make_unique<Skeleton>();
    skel->rootNodeIndex        = hdr.rootNodeIndex;
    skel->rootInverseTransform = FromFloatArray(hdr.rootInverse);
    skel->nodes.resize(hdr.nodeCount);

    for (uint32_t ni = 0; ni < hdr.nodeCount; ++ni) {
        FzSkeletonNodeData nd{};
        if (!r.Read(nd)) {
            FBZZ_LOG_ERROR("SkeletonImporter: truncated node %u [%s]", ni, absPath.c_str());
            return nullptr;
        }

        SkeletonNode& node      = skel->nodes[ni];
        node.name               = nd.name;
        node.parentIndex        = nd.parentIndex;
        node.boneIndex          = nd.boneIndex;
        node.bindTranslation    = { nd.bindTranslation[0], nd.bindTranslation[1], nd.bindTranslation[2] };
        node.bindRotation       = { nd.bindRotation[0], nd.bindRotation[1], nd.bindRotation[2], nd.bindRotation[3] };
        node.bindScale          = { nd.bindScale[0], nd.bindScale[1], nd.bindScale[2] };
        node.localBindTransform = FromFloatArray(nd.localBindTransform);
        skel->nodeMap[node.name] = static_cast<int>(ni);

        node.children.resize(nd.childCount);
        if (!r.ReadBytes(node.children.data(), nd.childCount * sizeof(int32_t))) {
            FBZZ_LOG_ERROR("SkeletonImporter: truncated children %u [%s]", ni, absPath.c_str());
            return nullptr;
        }
    }

    skel->bones.resize(hdr.boneCount);
    for (uint32_t bi = 0; bi < hdr.boneCount; ++bi) {
        FzBoneData bd{};
        if (!r.Read(bd)) {
            FBZZ_LOG_ERROR("SkeletonImporter: truncated bone %u [%s]", bi, absPath.c_str());
            return nullptr;
        }

        Bone& bone        = skel->bones[bi];
        bone.name         = bd.name;
        bone.nodeIndex    = bd.nodeIndex;
        bone.offsetMatrix = FromFloatArray(bd.offsetMatrix);
        skel->boneMap[bone.name] = static_cast<int>(bi);
    }

    /// @note 無アニメ時の既定パレット。単位行列を使わないための前提データ。
    BuildReferencePose(*skel);
    return skel;
}

std::span<const std::string_view> SkeletonImporter::SupportedExtensions() const
{
    static constexpr std::array<std::string_view, 1> exts{ ".skel" };
    return exts;
}

} // namespace fbzz::asset
