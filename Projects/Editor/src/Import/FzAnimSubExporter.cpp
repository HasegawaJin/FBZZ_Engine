// FBZZ Engine
// FzAnimSubExporter.cpp | fbzz::editor
// FBX → .anim バイナリ v2
// FzAnimImporter.cpp の FzAnimV2Extension / FzAnimTrackHeaderV2 と対応する。
#include <Editor/Import/FzAnimSubExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/anim.h>
#include <assimp/scene.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fbzz::editor {

namespace {

// FzAnimImporter で定義した同一レイアウト (対称性確保)
struct FzAnimV2Extension {
    double   durationSeconds;
    float    frameRate;
    uint8_t  loop;
    uint8_t  hasRootMotion;
    uint8_t  _pad[2];
    uint32_t rootMotionTrackIndex;
    uint32_t eventCount;
};
static_assert(sizeof(FzAnimV2Extension) == 24);

struct FzAnimTrackHeaderV2 {
    char     nodeName[128];
    uint8_t  interp;   // 1 = Linear (デフォルト)
    uint8_t  _pad[3];
    uint32_t positionCount;
    uint32_t rotationCount;
    uint32_t scaleCount;
};
static_assert(sizeof(FzAnimTrackHeaderV2) == 144);

} // namespace

bool FzAnimSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;
    const aiScene* scene = ctx.scene;
    if (!scene || scene->mNumAnimations == 0) return true; // アニメーションなしは正常

    namespace fs = std::filesystem;
    const fs::path animDir = util::FileSystem::PathFromUtf8(ctx.outputDir) / "anims";
    util::FileSystem::EnsureDirectory(animDir);

    for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* anim = scene->mAnimations[ai];
        const std::string animName = anim->mName.C_Str();

        // 選択的インポート
        if (!ctx.selectedAnimNames.empty()) {
            bool found = false;
            for (const auto& n : ctx.selectedAnimNames)
                if (n == animName) { found = true; break; }
            if (!found) continue;
        }

        const std::string animPath = util::FileSystem::PathToUtf8(
            animDir / ("clip_" + std::to_string(ai) + ".anim"));

        std::ofstream out(animPath, std::ios::binary);
        if (!out) return false;

        const double tps = (anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 30.0;
        const double durationSec = anim->mDuration / tps;
        const float  frameRate   = static_cast<float>(tps);

        // FzAnimHeader (version=2)
        FzAnimHeader hdr{};
        hdr.magic[0]='F'; hdr.magic[1]='Z'; hdr.magic[2]='A'; hdr.magic[3]='N';
        hdr.version      = 2;
        hdr.durationTicks  = anim->mDuration;
        hdr.ticksPerSecond = tps;
        hdr.trackCount   = anim->mNumChannels;
        const size_t nameLen = std::min(animName.size(), sizeof(hdr.name)-1);
        std::memcpy(hdr.name, animName.data(), nameLen);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // FzAnimV2Extension
        FzAnimV2Extension ext{};
        ext.durationSeconds      = durationSec;
        ext.frameRate            = frameRate;
        ext.loop                 = 0;
        ext.hasRootMotion        = 0;
        ext.rootMotionTrackIndex = UINT32_MAX;
        ext.eventCount           = 0;
        out.write(reinterpret_cast<const char*>(&ext), sizeof(ext));

        // トラック
        for (uint32_t ti = 0; ti < anim->mNumChannels; ++ti) {
            const aiNodeAnim* ch = anim->mChannels[ti];

            FzAnimTrackHeaderV2 th{};
            const std::string nodeName = ch->mNodeName.C_Str();
            const size_t nlen = std::min(nodeName.size(), sizeof(th.nodeName)-1);
            std::memcpy(th.nodeName, nodeName.data(), nlen);
            th.interp         = 1; // Linear
            th.positionCount  = ch->mNumPositionKeys;
            th.rotationCount  = ch->mNumRotationKeys;
            th.scaleCount     = ch->mNumScalingKeys;
            out.write(reinterpret_cast<const char*>(&th), sizeof(th));

            for (uint32_t ki = 0; ki < ch->mNumPositionKeys; ++ki) {
                const auto& k = ch->mPositionKeys[ki];
                FzVectorKey vk{ k.mTime,
                    k.mValue.x * ctx.unitScale,
                    k.mValue.y * ctx.unitScale,
                    k.mValue.z * ctx.unitScale,
                    0.0f };
                out.write(reinterpret_cast<const char*>(&vk), sizeof(vk));
            }
            for (uint32_t ki = 0; ki < ch->mNumRotationKeys; ++ki) {
                const auto& k = ch->mRotationKeys[ki];
                FzQuaternionKey qk{ k.mTime, k.mValue.x, k.mValue.y, k.mValue.z, k.mValue.w };
                out.write(reinterpret_cast<const char*>(&qk), sizeof(qk));
            }
            for (uint32_t ki = 0; ki < ch->mNumScalingKeys; ++ki) {
                const auto& k = ch->mScalingKeys[ki];
                FzVectorKey vk{ k.mTime, k.mValue.x, k.mValue.y, k.mValue.z, 0.0f };
                out.write(reinterpret_cast<const char*>(&vk), sizeof(vk));
            }
        }

        if (!out.good()) return false;
    }

    return true;
}

} // namespace fbzz::editor
