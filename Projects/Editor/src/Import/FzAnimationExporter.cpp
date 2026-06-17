// FBZZ Engine
// FzAnimationExporter.cpp | fbzz::editor
// aiAnimation → .anim バイナリ書き出し
#include <Editor/Import/FzAnimationExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/anim.h>
#include <cstring>

namespace fbzz::editor {

bool FzAnimationExporter::Export(const aiAnimation* anim,
                                  const std::string& outputPath,
                                  float unitScale)
{
    using namespace asset;

    auto out = util::FileSystem::OpenBinaryWriter(util::FileSystem::PathFromUtf8(outputPath));
    if (!out) {
        FBZZ_LOG_ERROR("FzAnimationExporter: cannot open [%s]", outputPath.c_str());
        return false;
    }

    // ── ヘッダー ─────────────────────────────────────────────────────────
    FzAnimHeader hdr{};
    hdr.magic[0] = 'F'; hdr.magic[1] = 'Z'; hdr.magic[2] = 'A'; hdr.magic[3] = 'N';
    hdr.version        = FZANIM_VERSION;
    hdr.durationTicks  = anim->mDuration;
    hdr.ticksPerSecond = (anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 25.0;
    hdr.trackCount     = anim->mNumChannels;
    std::strncpy(hdr.name, anim->mName.C_Str(), sizeof(hdr.name) - 1);
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    // ── トラック ─────────────────────────────────────────────────────────
    for (uint32_t ci = 0; ci < anim->mNumChannels; ++ci) {
        const aiNodeAnim* ch = anim->mChannels[ci];

        FzAnimTrackHeader th{};
        std::strncpy(th.nodeName, ch->mNodeName.C_Str(), sizeof(th.nodeName) - 1);
        th.positionCount = ch->mNumPositionKeys;
        th.rotationCount = ch->mNumRotationKeys;
        th.scaleCount    = ch->mNumScalingKeys;
        out.write(reinterpret_cast<const char*>(&th), sizeof(th));

        // 位置キー (Skeleton の bindTranslation と同じ unitScale を適用)
        for (uint32_t ki = 0; ki < ch->mNumPositionKeys; ++ki) {
            const auto& k = ch->mPositionKeys[ki];
            FzVectorKey vk{};
            vk.time = k.mTime;
            vk.x    = k.mValue.x * unitScale;
            vk.y    = k.mValue.y * unitScale;
            vk.z    = k.mValue.z * unitScale;
            out.write(reinterpret_cast<const char*>(&vk), sizeof(vk));
        }

        // 回転キー
        for (uint32_t ki = 0; ki < ch->mNumRotationKeys; ++ki) {
            const auto& k = ch->mRotationKeys[ki];
            FzQuaternionKey qk{};
            qk.time = k.mTime;
            qk.x    = k.mValue.x;
            qk.y    = k.mValue.y;
            qk.z    = k.mValue.z;
            qk.w    = k.mValue.w;
            out.write(reinterpret_cast<const char*>(&qk), sizeof(qk));
        }

        // スケールキー
        for (uint32_t ki = 0; ki < ch->mNumScalingKeys; ++ki) {
            const auto& k = ch->mScalingKeys[ki];
            FzVectorKey vk{};
            vk.time = k.mTime;
            vk.x    = k.mValue.x;
            vk.y    = k.mValue.y;
            vk.z    = k.mValue.z;
            out.write(reinterpret_cast<const char*>(&vk), sizeof(vk));
        }
    }

    return out.good();
}

} // namespace fbzz::editor
