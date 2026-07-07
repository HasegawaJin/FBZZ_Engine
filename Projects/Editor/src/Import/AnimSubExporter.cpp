// FBZZ Engine
// AnimSubExporter.cpp | fbzz::editor
// FBX → .anim バイナリ v2
// AnimationImporter.cpp の FzAnimV2Extension / FzAnimTrackHeaderV2 と対応する。
#include <Editor/Import/AnimSubExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/anim.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace fbzz::editor {

namespace {

// AnimationImporter で定義した同一レイアウト (対称性確保)
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

std::string SanitizeClipName(const std::string& name, uint32_t index)
{
    std::string out = name.empty() ? ("Take_" + std::to_string(index)) : name;
    for (char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }
    return out;
}

} // namespace

bool AnimSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;
    const aiScene* scene = ctx.scene;
    if (!scene || scene->mNumAnimations == 0) return true; // アニメーションなしは正常

    namespace fs = std::filesystem;
    const fs::path animDir = util::FileSystem::PathFromUtf8(ctx.outputDir) / "anims";
    util::FileSystem::EnsureDirectory(animDir);

    std::unordered_map<std::string, uint32_t> usedClipStems;
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

        const std::string clipName = SanitizeClipName(animName, ai);
        std::string clipStem = ctx.baseName + "@" + clipName;
        uint32_t& sameNameCount = usedClipStems[clipStem];
        if (sameNameCount > 0)
            clipStem += "_" + std::to_string(sameNameCount);
        ++sameNameCount;

        const std::string animPath = util::FileSystem::PathToUtf8(
            animDir / (clipStem + ".anim"));

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

        // DCC 座標系補正 (FbxImportTool が正規化したルートノードと同名のトラックへ適用)。
        // WHY: Blender はルートノードの +90°X / scale100 をアニメトラックでも毎キー再生する。
        //      バインド側 (ノード) からは除去済みのため、トラック側にも同じ F = q⁻¹·(1/s) を
        //      合成しないと骨階層とアニメが 90° / 100 倍ずれてしまう。
        const aiQuaternion axisInvQ(ctx.axisFixRotation[3], -ctx.axisFixRotation[0],
                                    -ctx.axisFixRotation[1], -ctx.axisFixRotation[2]);
        const float axisInvS = 1.0f / ctx.axisFixScale;

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

            const bool applyAxisFix =
                std::find(ctx.axisFixNodes.begin(), ctx.axisFixNodes.end(), nodeName)
                != ctx.axisFixNodes.end();

            for (uint32_t ki = 0; ki < ch->mNumPositionKeys; ++ki) {
                const auto& k = ch->mPositionKeys[ki];
                aiVector3D v = k.mValue;
                if (applyAxisFix) v = axisInvQ.Rotate(v * axisInvS);
                FzVectorKey vk{ k.mTime,
                    v.x * ctx.unitScale,
                    v.y * ctx.unitScale,
                    v.z * ctx.unitScale,
                    0.0f };
                out.write(reinterpret_cast<const char*>(&vk), sizeof(vk));
            }
            for (uint32_t ki = 0; ki < ch->mNumRotationKeys; ++ki) {
                const auto& k = ch->mRotationKeys[ki];
                aiQuaternion q = k.mValue;
                if (applyAxisFix) q = axisInvQ * q; // F の回転を左掛け (バインド側と同じ変換)
                FzQuaternionKey qk{ k.mTime, q.x, q.y, q.z, q.w };
                out.write(reinterpret_cast<const char*>(&qk), sizeof(qk));
            }
            for (uint32_t ki = 0; ki < ch->mNumScalingKeys; ++ki) {
                const auto& k = ch->mScalingKeys[ki];
                aiVector3D v = k.mValue;
                if (applyAxisFix) v = v * axisInvS; // scale100 キー → 1.0
                FzVectorKey vk{ k.mTime, v.x, v.y, v.z, 0.0f };
                out.write(reinterpret_cast<const char*>(&vk), sizeof(vk));
            }
        }

        if (!out.good()) return false;
    }

    return true;
}

} // namespace fbzz::editor
