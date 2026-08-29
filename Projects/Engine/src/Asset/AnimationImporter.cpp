/// @file    AnimationImporter.cpp
/// @brief   .anim バイナリ v3 → AnimationClip デシリアライザ。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// WHAT: v3 の型付き Property / Material / Morph Track を復元する。
#include <Engine/Asset/AnimationImporter.hpp>
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

namespace {

struct FzAnimV3Extension {
    double   durationSeconds;
    float    frameRate;
    uint8_t  loop;
    uint8_t  hasRootMotion;
    uint8_t  rootMotionApplyXZ;
    uint8_t  rootMotionApplyY;
    uint8_t  rootMotionApplyRotation;
    uint8_t  optimized;
    uint8_t  _pad[2];
    uint32_t rootMotionTrackIndex;
    uint32_t eventCount;
    uint32_t propertyTrackCount;
    float    positionError;
    float    rotationErrorDegrees;
    float    scaleError;
};
static_assert(sizeof(FzAnimV3Extension) == 48, "FzAnimV3Extension size mismatch");

struct FzAnimEventV3 {
    double  time;
    char    name[64];
    int32_t intParam;
    float   floatParam;
};
static_assert(sizeof(FzAnimEventV3) == 80, "FzAnimEventV3 size mismatch");

struct FzAnimTrackHeaderV3 {
    char     targetPath[256];
    char     nodeName[128];
    uint8_t  interp;
    uint8_t  _pad[3];
    uint32_t positionCount;
    uint32_t rotationCount;
    uint32_t scaleCount;
};
static_assert(sizeof(FzAnimTrackHeaderV3) == 400, "FzAnimTrackHeaderV3 size mismatch");

struct FzPropertyTrackHeaderV3 {
    char     targetPath[256];
    char     componentType[64];
    char     propertyName[128];
    uint8_t  targetType;
    uint8_t  valueType;
    uint8_t  interp;
    uint8_t  _pad;
    int32_t  materialSlot;
    int32_t  meshIndex;
    uint32_t floatCount;
    uint32_t vector2Count;
    uint32_t vector3Count;
    uint32_t vector4Count;
    uint32_t intCount;
    uint32_t boolCount;
};
static_assert(sizeof(FzPropertyTrackHeaderV3) == 484, "FzPropertyTrackHeaderV3 size mismatch");

struct FzFloatKeyV3 {
    double time;
    float value;
    float inTangent;
    float outTangent;
    float _pad;
};
static_assert(sizeof(FzFloatKeyV3) == 24, "FzFloatKeyV3 size mismatch");

struct FzVector2KeyV3 { double time; float x, y; };
struct FzVector4KeyV3 { double time; float x, y, z, w; };
struct FzIntKeyV3 { double time; int32_t value; int32_t _pad; };
struct FzBoolKeyV3 { double time; uint8_t value; uint8_t _pad[7]; };

// rootMotionTrackIndex から人が読めるノード名を復元する。
// WHY: .anim v3 のヘッダーにはノード名を入れる余地が無い。トラック配列から引けば
//      同じ情報が得られるので、フォーマットを変えずに Inspector や
//      RootMotionSource::NodeName の突き合わせへ提示できる。
void ResolveRootMotionNodeName(AnimationClip& clip)
{
    clip.rootMotionNodeName.clear();
    if (clip.rootMotionTrackIndex < clip.tracks.size())
        clip.rootMotionNodeName = clip.tracks[clip.rootMotionTrackIndex].nodeName;
}

std::unique_ptr<AnimationClip> LoadV3(BinaryReader& r, const FzAnimHeader& hdr)
{
    FzAnimV3Extension ext{};
    if (!r.Read(ext)) return nullptr;

    auto clip = std::make_unique<AnimationClip>();
    clip->name = hdr.name;
    clip->durationTicks = hdr.durationTicks;
    clip->ticksPerSecond = hdr.ticksPerSecond > 0.0 ? hdr.ticksPerSecond : 30.0;
    clip->durationSeconds = ext.durationSeconds;
    clip->frameRate = ext.frameRate;
    clip->loop = ext.loop != 0;
    clip->hasRootMotion = ext.hasRootMotion != 0;
    clip->rootMotionApplyXZ = ext.rootMotionApplyXZ != 0;
    clip->rootMotionApplyY = ext.rootMotionApplyY != 0;
    clip->rootMotionApplyRotation = ext.rootMotionApplyRotation != 0;
    clip->rootMotionTrackIndex = ext.rootMotionTrackIndex;
    clip->optimized = ext.optimized != 0;
    clip->positionError = ext.positionError;
    clip->rotationErrorDegrees = ext.rotationErrorDegrees;
    clip->scaleError = ext.scaleError;

    clip->events.resize(ext.eventCount);
    for (uint32_t i = 0; i < ext.eventCount; ++i) {
        FzAnimEventV3 ev{};
        if (!r.Read(ev)) return nullptr;
        clip->events[i] = { ev.time, ev.name, ev.intParam, ev.floatParam };
    }

    clip->tracks.resize(hdr.trackCount);
    for (uint32_t i = 0; i < hdr.trackCount; ++i) {
        FzAnimTrackHeaderV3 th{};
        if (!r.Read(th)) return nullptr;
        auto& track = clip->tracks[i];
        track.targetPath = th.targetPath;
        track.nodeName = th.nodeName;
        track.interp = static_cast<AnimInterp>(th.interp);
        track.positions.resize(th.positionCount);
        track.rotations.resize(th.rotationCount);
        track.scales.resize(th.scaleCount);
        for (auto& key : track.positions) {
            FzVectorKey value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, { value.x, value.y, value.z } };
        }
        for (auto& key : track.rotations) {
            FzQuaternionKey value{};
            if (!r.Read(value)) return nullptr;
            key = {
                value.time,
                math::Quaternion{ value.x, value.y, value.z, value.w }.Normalized()
            };
        }
        for (auto& key : track.scales) {
            FzVectorKey value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, { value.x, value.y, value.z } };
        }
    }

    clip->propertyTracks.resize(ext.propertyTrackCount);
    for (auto& track : clip->propertyTracks) {
        FzPropertyTrackHeaderV3 th{};
        if (!r.Read(th)) return nullptr;
        track.targetPath = th.targetPath;
        track.componentType = th.componentType;
        track.propertyName = th.propertyName;
        track.targetType = static_cast<AnimTargetType>(th.targetType);
        track.valueType = static_cast<AnimValueType>(th.valueType);
        track.interp = static_cast<AnimInterp>(th.interp);
        track.materialSlot = th.materialSlot;
        track.meshIndex = th.meshIndex;

        track.floatKeys.resize(th.floatCount);
        for (auto& key : track.floatKeys) {
            FzFloatKeyV3 value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, value.value, value.inTangent, value.outTangent };
        }
        track.vector2Keys.resize(th.vector2Count);
        for (auto& key : track.vector2Keys) {
            FzVector2KeyV3 value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, { value.x, value.y } };
        }
        track.vector3Keys.resize(th.vector3Count);
        for (auto& key : track.vector3Keys) {
            FzVectorKey value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, { value.x, value.y, value.z } };
        }
        track.vector4Keys.resize(th.vector4Count);
        for (auto& key : track.vector4Keys) {
            FzVector4KeyV3 value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, { value.x, value.y, value.z, value.w } };
        }
        track.intKeys.resize(th.intCount);
        for (auto& key : track.intKeys) {
            FzIntKeyV3 value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, value.value };
        }
        track.boolKeys.resize(th.boolCount);
        for (auto& key : track.boolKeys) {
            FzBoolKeyV3 value{};
            if (!r.Read(value)) return nullptr;
            key = { value.time, value.value != 0 };
        }
    }
    ResolveRootMotionNodeName(*clip);
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

    if (hdr.version == 3) return LoadV3(r, hdr);

    FBZZ_LOG_ERROR("AnimationImporter: unsupported .anim version %u (expected v3) [%s]",
                   hdr.version,
                   absPath.c_str());
    return nullptr;
}

} // namespace fbzz::asset
