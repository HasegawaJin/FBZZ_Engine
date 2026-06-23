// FBZZ Engine
// AnimationImporter.cpp | fbzz::asset
// .anim バイナリ v2 → AnimationClip デシリアライザ
// WHAT: v2 の durationSeconds / events / track interp を正とし、旧 v1 形式は読み込まない。
#include <Engine/Asset/AnimationImporter.hpp>
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

namespace {

// .anim v2 固有の追加ヘッダー。
// WHY: 最新形式では再生長を durationSeconds に正規化し、遷移判定が tick rate に依存しないようにする。
struct FzAnimV2Extension {
    double   durationSeconds;
    float    frameRate;
    uint8_t  loop;
    uint8_t  hasRootMotion;
    uint8_t  _pad[2];
    uint32_t rootMotionTrackIndex;
    uint32_t eventCount;
};
static_assert(sizeof(FzAnimV2Extension) == 24, "FzAnimV2Extension size mismatch");

struct FzAnimEventV2 {
    double  time;
    char    name[64];
    int32_t intParam;
    float   floatParam;
};
static_assert(sizeof(FzAnimEventV2) == 80, "FzAnimEventV2 size mismatch");

// v2 トラックヘッダー (v1 FzAnimTrackHeader と互換性なし)
struct FzAnimTrackHeaderV2 {
    char     nodeName[128];
    uint8_t  interp;    // AnimInterp enum
    uint8_t  _pad[3];
    uint32_t positionCount;
    uint32_t rotationCount;
    uint32_t scaleCount;
};
static_assert(sizeof(FzAnimTrackHeaderV2) == 144, "FzAnimTrackHeaderV2 size mismatch");

std::unique_ptr<AnimationClip> LoadV2(BinaryReader& r, const FzAnimHeader& hdr)
{
    FzAnimV2Extension ext{};
    if (!r.Read(ext)) return nullptr;

    auto clip = std::make_unique<AnimationClip>();
    clip->name                 = hdr.name;
    // v2 の再生長は durationSeconds を正とする。ticksPerSecond はキー時刻を秒から tick へ戻すためだけに保持する。
    // WHY: exporter は各キーの time を Assimp tick 単位で保存するため、サンプリング時の変換係数は必要になる。
    clip->ticksPerSecond       = (hdr.ticksPerSecond > 0.0) ? hdr.ticksPerSecond : 30.0;
    clip->durationSeconds      = ext.durationSeconds;
    clip->frameRate            = ext.frameRate;
    clip->loop                 = ext.loop != 0;
    clip->hasRootMotion        = ext.hasRootMotion != 0;
    clip->rootMotionTrackIndex = ext.rootMotionTrackIndex;

    // イベント
    clip->events.resize(ext.eventCount);
    for (uint32_t ei = 0; ei < ext.eventCount; ++ei) {
        FzAnimEventV2 ev{};
        if (!r.Read(ev)) return nullptr;
        clip->events[ei] = { ev.time, ev.name, ev.intParam, ev.floatParam };
    }

    // トラック
    clip->tracks.resize(hdr.trackCount);
    for (uint32_t ti = 0; ti < hdr.trackCount; ++ti) {
        FzAnimTrackHeaderV2 th{};
        if (!r.Read(th)) return nullptr;

        NodeAnimationTrack& track = clip->tracks[ti];
        track.nodeName = th.nodeName;
        track.interp   = static_cast<AnimInterp>(th.interp);

        track.positions.resize(th.positionCount);
        for (uint32_t ki = 0; ki < th.positionCount; ++ki) {
            FzVectorKey vk{};
            if (!r.Read(vk)) return nullptr;
            track.positions[ki] = { vk.time, { vk.x, vk.y, vk.z } };
        }
        track.rotations.resize(th.rotationCount);
        for (uint32_t ki = 0; ki < th.rotationCount; ++ki) {
            FzQuaternionKey qk{};
            if (!r.Read(qk)) return nullptr;
            track.rotations[ki] = { qk.time, { qk.x, qk.y, qk.z, qk.w } };
        }
        track.scales.resize(th.scaleCount);
        for (uint32_t ki = 0; ki < th.scaleCount; ++ki) {
            FzVectorKey vk{};
            if (!r.Read(vk)) return nullptr;
            track.scales[ki] = { vk.time, { vk.x, vk.y, vk.z } };
        }
    }
    return clip;
}

} // namespace

std::unique_ptr<AnimationClip> AnimationImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    BinaryReader r;
    if (!r.Open(absPath)) {
        FBZZ_LOG_ERROR("AnimationImporter: cannot open [%s]", absPath.c_str());
        return nullptr;
    }

    FzAnimHeader hdr{};
    if (!r.Read(hdr) ||
        hdr.magic[0] != 'F' || hdr.magic[1] != 'Z' ||
        hdr.magic[2] != 'A' || hdr.magic[3] != 'N') {
        FBZZ_LOG_ERROR("AnimationImporter: bad magic [%s]", absPath.c_str());
        return nullptr;
    }

    if (hdr.version == 2) return LoadV2(r, hdr);

    FBZZ_LOG_ERROR("AnimationImporter: unsupported .anim version %u (expected v2) [%s]",
                   hdr.version,
                   absPath.c_str());
    return nullptr;
}

} // namespace fbzz::asset
