/// @file    AnimSubExporter.cpp
/// @brief   FBX → .anim バイナリ v3。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// AnimationImporter.cpp の v3 レイアウトと対応し、Node Transform と Morph Weight を同時に保存する。
#include <Editor/Import/AnimSubExporter.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/anim.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {

// AnimationImporter で定義した同一レイアウト (対称性確保)
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
static_assert(sizeof(FzAnimV3Extension) == 48);

struct FzAnimTrackHeaderV3 {
    char     targetPath[256];
    char     nodeName[128];
    uint8_t  interp;   // 1 = Linear (デフォルト)
    uint8_t  _pad[3];
    uint32_t positionCount;
    uint32_t rotationCount;
    uint32_t scaleCount;
};
static_assert(sizeof(FzAnimTrackHeaderV3) == 400);

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
static_assert(sizeof(FzPropertyTrackHeaderV3) == 484);

struct FzFloatKeyV3 {
    double time;
    float value;
    float inTangent;
    float outTangent;
    float _pad;
};
static_assert(sizeof(FzFloatKeyV3) == 24);

struct FzAnimEventV3 {
    double  time;
    char    name[64];
    int32_t intParam;
    float   floatParam;
};
static_assert(sizeof(FzAnimEventV3) == 80);

struct MorphExportTrack {
    std::string targetPath;
    std::string morphName;
    int32_t meshIndex = 0;
    std::vector<FzFloatKeyV3> keys;
};

std::vector<MorphExportTrack> BuildMorphTracks(const aiScene& scene,
                                               const aiAnimation& anim,
                                               double startTicks,
                                               double endTicks)
{
    std::vector<MorphExportTrack> result;
    for (uint32_t ci = 0; ci < anim.mNumMorphMeshChannels; ++ci) {
        const aiMeshMorphAnim* channel = anim.mMorphMeshChannels[ci];
        const std::string channelName = channel->mName.C_Str();
        int32_t meshIndex = -1;
        for (uint32_t mi = 0; mi < scene.mNumMeshes; ++mi) {
            if (channelName == scene.mMeshes[mi]->mName.C_Str()) {
                meshIndex = static_cast<int32_t>(mi);
                break;
            }
        }
        if (meshIndex < 0 && ci < scene.mNumMeshes) meshIndex = static_cast<int32_t>(ci);
        if (meshIndex < 0) continue;

        const aiMesh* mesh = scene.mMeshes[meshIndex];
        std::vector<uint32_t> targetIndices;
        for (uint32_t ki = 0; ki < channel->mNumKeys; ++ki) {
            const aiMeshMorphKey& key = channel->mKeys[ki];
            for (uint32_t vi = 0; vi < key.mNumValuesAndWeights; ++vi) {
                if (std::find(targetIndices.begin(), targetIndices.end(), key.mValues[vi])
                    == targetIndices.end()) {
                    targetIndices.push_back(key.mValues[vi]);
                }
            }
        }

        for (const uint32_t targetIndex : targetIndices) {
            MorphExportTrack track{};
            track.targetPath = channelName;
            track.meshIndex = meshIndex;
            if (targetIndex < mesh->mNumAnimMeshes &&
                mesh->mAnimMeshes[targetIndex]->mName.length > 0) {
                track.morphName = mesh->mAnimMeshes[targetIndex]->mName.C_Str();
            } else {
                track.morphName = "Morph_" + std::to_string(targetIndex);
            }
            track.keys.reserve(channel->mNumKeys);
            for (uint32_t ki = 0; ki < channel->mNumKeys; ++ki) {
                const aiMeshMorphKey& key = channel->mKeys[ki];
                if (key.mTime < startTicks || key.mTime > endTicks) continue;
                float weight = 0.0f;
                for (uint32_t vi = 0; vi < key.mNumValuesAndWeights; ++vi) {
                    if (key.mValues[vi] == targetIndex) {
                        weight = static_cast<float>(key.mWeights[vi]);
                        break;
                    }
                }
                track.keys.push_back({ key.mTime - startTicks, weight, 0.0f, 0.0f, 0.0f });
            }
            if (!track.keys.empty()) result.push_back(std::move(track));
        }
    }
    return result;
}

void OptimizeVectorKeys(std::vector<asset::FzVectorKey>& keys, float error)
{
    if (keys.size() < 3) return;
    std::vector<asset::FzVectorKey> optimized;
    optimized.reserve(keys.size());
    optimized.push_back(keys.front());
    for (size_t i = 1; i + 1 < keys.size(); ++i) {
        const auto& a = optimized.back();
        const auto& b = keys[i];
        const auto& c = keys[i + 1];
        const double span = c.time - a.time;
        const float t = span > 0.0 ? static_cast<float>((b.time - a.time) / span) : 0.0f;
        const float ex = a.x + (c.x - a.x) * t - b.x;
        const float ey = a.y + (c.y - a.y) * t - b.y;
        const float ez = a.z + (c.z - a.z) * t - b.z;
        if (ex * ex + ey * ey + ez * ez > error * error) optimized.push_back(b);
    }
    optimized.push_back(keys.back());
    keys = std::move(optimized);
}

void OptimizeQuaternionKeys(std::vector<asset::FzQuaternionKey>& keys, float errorDegrees)
{
    if (keys.size() < 3) return;
    const float radians = errorDegrees * 3.14159265358979323846f / 180.0f;
    const float minimumDot = std::cos(radians * 0.5f);
    std::vector<asset::FzQuaternionKey> optimized;
    optimized.reserve(keys.size());
    optimized.push_back(keys.front());
    for (size_t i = 1; i + 1 < keys.size(); ++i) {
        const auto& a = optimized.back();
        const auto& b = keys[i];
        const auto& c = keys[i + 1];
        const double span = c.time - a.time;
        const float t = span > 0.0 ? static_cast<float>((b.time - a.time) / span) : 0.0f;
        aiQuaternion qa(a.w, a.x, a.y, a.z);
        aiQuaternion qb(b.w, b.x, b.y, b.z);
        aiQuaternion qc(c.w, c.x, c.y, c.z);
        aiQuaternion sampled;
        aiQuaternion::Interpolate(sampled, qa, qc, t);
        sampled.Normalize();
        const float dot = std::abs(sampled.x * qb.x + sampled.y * qb.y +
                                   sampled.z * qb.z + sampled.w * qb.w);
        if (dot < minimumDot) optimized.push_back(b);
    }
    optimized.push_back(keys.back());
    keys = std::move(optimized);
}

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

// aiNode の実階層から、AnimationClip と Skeleton が共有する正規パスを構築する。
// WHY: nodeName だけでは同名ノードを区別できず、追加レイヤーの対象解決が失敗する。
//      canonical 名も併用し、DCC の namespace / Assimp 補助 suffix を吸収する。
std::string NormalizeAnimationNodeName(std::string_view value)
{
    std::string normalized(value);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

bool FindAnimationNodePath(const aiNode* node,
                           std::string_view requestedName,
                           std::string& outPath)
{
    if (!node) return false;

    const std::string nodeName = NormalizeAnimationNodeName(node->mName.C_Str());
    const std::string normalizedRequested = NormalizeAnimationNodeName(requestedName);
    const bool matches = nodeName == normalizedRequested ||
        asset::CanonicalNodeName(nodeName) == asset::CanonicalNodeName(normalizedRequested);
    if (matches) {
        outPath = nodeName;
        return true;
    }

    for (uint32_t i = 0; i < node->mNumChildren; ++i) {
        std::string childPath;
        if (!FindAnimationNodePath(node->mChildren[i], requestedName, childPath)) continue;
        outPath = nodeName.empty() ? childPath : nodeName + "/" + childPath;
        return true;
    }
    return false;
}

// ルートモーションノードの候補を段階付きで集める。
//
// WHY: 旧実装は "rootmotion" / "root_motion" の完全一致だけを見ており、Mixamo の
//      mixamorig:Hips や Blender の Armature|Hips では 1 件もヒットしなかった。
//      判定規則は asset::ClassifyRootMotionNodeName に集約し、ランタイム側の
//      AutoDetect と同じ結果になるようにする。
struct RootMotionCandidate {
    uint32_t                  trackIndex = UINT32_MAX;
    asset::RootMotionNameTier tier = asset::RootMotionNameTier::None;
    std::string               nodeName;
};

// 明示指定 (インポート設定) があればそれを最優先し、無ければ候補名で選ぶ。
// 明示指定された名前が見つからない場合は候補名へフォールバックする。
void ConsiderRootMotionChannel(RootMotionCandidate& best,
                               const std::string& channelName,
                               uint32_t trackIndex,
                               const std::string& explicitNodeName)
{
    if (!explicitNodeName.empty() &&
        asset::RootMotionNameKey(channelName) ==
            asset::RootMotionNameKey(explicitNodeName)) {
        best.trackIndex = trackIndex;
        best.tier = asset::RootMotionNameTier::Explicit;
        best.nodeName = channelName;
        return;
    }
    // 明示指定が既にヒットしていれば、候補名では上書きしない。
    if (!explicitNodeName.empty() && best.trackIndex != UINT32_MAX) return;

    const asset::RootMotionNameTier tier =
        asset::ClassifyRootMotionNodeName(channelName);
    if (tier == asset::RootMotionNameTier::None) return;
    if (static_cast<int>(tier) <= static_cast<int>(best.tier)) return;
    best.trackIndex = trackIndex;
    best.tier = tier;
    best.nodeName = channelName;
}

} // namespace

bool AnimSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;
    const aiScene* scene = ctx.scene;
    if (!scene || scene->mNumAnimations == 0) return true; // アニメーションなしは正常

    namespace fs = std::filesystem;
    // .anim は Library 側へ出す (隠蔽)。
    //
    // WHY .meta を持たせなくてよいか: GUID は原本 FBX の GUID + "anims/<file>.anim" から
    //     AssetDatabase::DeriveGuid で決定論的に導出される。どの環境でも同じ値になり、
    //     Library を消して再インポートしても復元されるため、git 管理下の .meta が要らない。
    //     (乱数 GUID + .meta 方式のままここを Library へ移すと、クローン直後の再インポートで
    //      別 GUID が振られ、.animcontroller の参照が全部切れる。)
    //
    // WHY (ディレクトリを事前に作らない): 選択的インポート (selectedAnimNames) で全クリップが
    //   除外された場合や、クリップが 1 本も書き出されなかった場合に空の anims/ が残るため、
    //   作成は実際に .anim を書く直前 (下の EnsureParentDirectory) まで遅延させる。
    const fs::path animDir = util::FileSystem::PathFromUtf8(ctx.manifestDir) / "anims";

    std::unordered_map<std::string, uint32_t> usedClipStems;
    for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* anim = scene->mAnimations[ai];
        const std::string animName = anim->mName.C_Str();

        AnimationClipImportSettings clipOptions;
        clipOptions.name = animName;
        for (const AnimationClipImportSettings& settings : ctx.clipSettings) {
            if (settings.name == animName) {
                clipOptions = settings;
                break;
            }
        }

        // 選択的インポート
        if (!ctx.selectedAnimNames.empty()) {
            bool found = false;
            for (const auto& n : ctx.selectedAnimNames)
                if (n == animName) { found = true; break; }
            if (!found) continue;
        }

        const double startTicks = std::clamp(
            std::max(0.0, clipOptions.startFrame), 0.0, anim->mDuration);
        const double requestedEndTicks = clipOptions.endFrame < 0.0
            ? anim->mDuration : clipOptions.endFrame;
        const double endTicks = std::clamp(requestedEndTicks, startTicks, anim->mDuration);
        const double clipDurationTicks = endTicks - startTicks;
        const std::string requestedClipName = clipOptions.outputName.empty()
            ? animName : clipOptions.outputName;
        const std::string clipName = SanitizeClipName(requestedClipName, ai);
        // 出力ファイル名はクリップ名そのものにする (Idle.anim であって Idle@Idle.anim ではない)。
        //
        // WHY 原本名を前置しないか: .anim は Library/Baked/<fbx-guid>/anims/ 配下へ出るため、
        //     ディレクトリが原本 FBX ごとに分かれている。別 FBX 間でファイル名が衝突しようが
        //     なく、接頭辞は「1 クリップ 1 FBX」運用だと Idle@Idle のように同じ語を 2 度
        //     書くだけのノイズになっていた。アセットブラウザでも読みづらい。
        //
        // WHY 同名衝突を心配しなくてよいか: 同一 FBX 内に同名クリップが複数ある場合は、
        //     直下の usedClipStems が _1 / _2 を付けて従来どおり回避する。前置をやめても
        //     衝突回避の責務はそちらに残っている。
        //
        // NOTE: クリップの内部名 (FzAnimHeader::name) は元から clipName で @ を含まない。
        //       ここで変わるのはファイル名だけ。.animcontroller が参照するのは抽出済みの
        //       Assets/Animation/*.anim (独自の .meta GUID を持つ) なので、そちらは無傷。
        //       Library/Baked を直接指す参照だけは導出 GUID が変わるため、再インポート後に
        //       貼り直しが要る。
        std::string clipStem = clipName;
        uint32_t& sameNameCount = usedClipStems[clipStem];
        if (sameNameCount > 0)
            clipStem += "_" + std::to_string(sameNameCount);
        ++sameNameCount;

        const fs::path     animFsPath = animDir / (clipStem + ".anim");
        const std::string  animPath   = util::FileSystem::PathToUtf8(animFsPath);

        // 実際に書き出すクリップが確定したこの時点で初めて anims/ を作る。
        if (!util::FileSystem::EnsureParentDirectory(animFsPath)) return false;

        std::ofstream out(animPath, std::ios::binary);
        if (!out) return false;

        const double tps = (anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 30.0;
        const float  frameRate   = static_cast<float>(tps);
        const std::vector<MorphExportTrack> morphTracks =
            BuildMorphTracks(*scene, *anim, startTicks, endTicks);
        std::vector<const aiNodeAnim*> nodeChannels;
        std::vector<FzAnimEventV3> animationEvents;
        RootMotionCandidate rootMotion;
        for (uint32_t channelIndex = 0; channelIndex < anim->mNumChannels; ++channelIndex) {
            const aiNodeAnim* channel = anim->mChannels[channelIndex];
            const std::string channelName = channel->mNodeName.C_Str();
            static constexpr std::string_view eventPrefix = "FBZZ_EVENT__";
            if (channelName.rfind(eventPrefix.data(), 0) == 0) {
                // イベントチャンネルは「ステップ信号」として解釈する。
                //
                // WHY: DCC 側のアニメーションベイク (Blender の bake_anim_step=1.0 など) は
                //      補助ノードも毎フレームサンプリングするため、キー数 = フレーム数になる。
                //      キーを素直に 1:1 でイベント化すると 26 フレームのクリップから
                //      26 個のイベントが飛ぶ。DCC 側でベイクを切らせるとリグの
                //      コンストレイントまで焼けなくなるので、取り込み側で吸収する。
                //
                // 規約: 先頭キーの値を「静止値 (rest)」とみなし、
                //       静止値と異なる値へ遷移した瞬間だけをイベントとして採用する。
                //       静止値へ戻る遷移は発火しない。これにより
                //         - ベイク済みの重複キーは無視される
                //         - rest → A → rest → A で同じイベントを何度でも打てる
                //         - A → B の直接遷移も B として発火する
                //       DCC 側は「普段は静止値、発火させたいフレームで値を変える」だけでよく、
                //       センチネル値や特別なエクスポート設定を要求しない。
                const std::string eventName = channelName.substr(eventPrefix.size());
                const uint32_t keyCount = channel->mNumPositionKeys;
                if (keyCount == 0) continue;

                constexpr float kEventEpsilon = 1.0e-4f;
                const auto sameValue = [](const aiVector3D& a, const aiVector3D& b) {
                    return std::fabs(a.x - b.x) < kEventEpsilon &&
                           std::fabs(a.y - b.y) < kEventEpsilon;
                };

                const aiVector3D restValue = channel->mPositionKeys[0].mValue;
                aiVector3D previousValue = restValue;
                for (uint32_t keyIndex = 1; keyIndex < keyCount; ++keyIndex) {
                    const aiVector3D& value = channel->mPositionKeys[keyIndex].mValue;
                    const double keyTime = channel->mPositionKeys[keyIndex].mTime;
                    if (keyTime < startTicks || keyTime > endTicks) continue;
                    const bool changed  = !sameValue(value, previousValue);
                    const bool isActive = !sameValue(value, restValue);
                    previousValue = value;
                    if (!changed || !isActive) continue;

                    FzAnimEventV3 event{};
                    event.time = (keyTime - startTicks) / tps;
                    std::memcpy(event.name, eventName.data(),
                                std::min(eventName.size(), sizeof(event.name) - 1));
                    event.intParam   = static_cast<int32_t>(std::lround(value.x));
                    event.floatParam = value.y;
                    animationEvents.push_back(event);
                }
                continue;
            }
            ConsiderRootMotionChannel(rootMotion, channelName,
                                      static_cast<uint32_t>(nodeChannels.size()),
                                      ctx.rootMotionNodeName);
            nodeChannels.push_back(channel);
        }
        std::sort(animationEvents.begin(), animationEvents.end(),
            [](const FzAnimEventV3& a, const FzAnimEventV3& b) { return a.time < b.time; });

        // どのノードをルートモーションとして拾ったかはログに残す。
        // WHY: 「ルートモーションが効かない」の原因は大半が命名不一致で、
        //      候補が見つかったのか否かが分からないと切り分けようがない。
        if (rootMotion.trackIndex == UINT32_MAX) {
            FBZZ_LOG_INFO("AnimSubExporter: [%s] root motion node not found "
                          "(specify FbxImportOptions::rootMotionNodeName if needed)",
                          clipName.c_str());
        } else {
            FBZZ_LOG_INFO("AnimSubExporter: [%s] root motion node '%s' (%s)",
                          clipName.c_str(), rootMotion.nodeName.c_str(),
                          rootMotion.tier == asset::RootMotionNameTier::Explicit
                              ? "explicit — enabled"
                              : "skeletal candidate — enable via Animator Auto Detect");
        }

        // FzAnimHeader (version=3)
        FzAnimHeader hdr{};
        hdr.magic[0]='F'; hdr.magic[1]='Z'; hdr.magic[2]='A'; hdr.magic[3]='N';
        hdr.version      = 3;
        hdr.durationTicks  = clipDurationTicks;
        hdr.ticksPerSecond = tps;
        hdr.trackCount   = static_cast<uint32_t>(nodeChannels.size());
        const size_t nameLen = std::min(clipName.size(), sizeof(hdr.name)-1);
        std::memcpy(hdr.name, clipName.data(), nameLen);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // FzAnimV3Extension
        FzAnimV3Extension ext{};
        ext.durationSeconds      = clipDurationTicks / tps;
        ext.frameRate            = frameRate;
        // Loop Time は .fbx.meta のクリップ設定から焼く。
        // WHY ここで解決するか: .anim は再インポートのたびに上書きされる生成物なので、
        //     設定の権威は原本の横 (.fbx.meta) にある。毎回そこから読み直して焼き込む。
        ext.loop = clipOptions.loop ? 1 : 0;
        // hasRootMotion を立てるのは「ルートモーション専用ノード」が見つかったときだけ。
        //
        // WHY: Hips / Armature のような骨階層のルート相当は候補としては拾いたいが、
        //      既定で有効化すると、その場アニメ (Mixamo の in-place クリップなど) の
        //      腰の揺れまで移動量として抜き出してしまい、キャラクターが漂う。
        //      トラック位置だけ記録しておき、有効化の判断は Animator 側の
        //      RootMotionSource::AutoDetect / NodeName に委ねる。
        ext.hasRootMotion =
            rootMotion.tier == asset::RootMotionNameTier::Explicit ? 1 : 0;
        ext.rootMotionApplyXZ    = 1;
        ext.rootMotionApplyY     = 0;
        ext.rootMotionApplyRotation = 1;
        ext.optimized            = 1;
        ext.rootMotionTrackIndex = rootMotion.trackIndex;
        ext.eventCount           = static_cast<uint32_t>(animationEvents.size());
        ext.propertyTrackCount   = static_cast<uint32_t>(morphTracks.size());
        ext.positionError        = 0.0001f;
        ext.rotationErrorDegrees = 0.05f;
        ext.scaleError           = 0.0001f;
        out.write(reinterpret_cast<const char*>(&ext), sizeof(ext));
        out.write(reinterpret_cast<const char*>(animationEvents.data()),
                  static_cast<std::streamsize>(animationEvents.size() * sizeof(FzAnimEventV3)));

        // DCC 座標系補正 (FbxImportTool が正規化したルートノードと同名のトラックへ適用)。
        // WHY: Blender はルートノードの +90°X / scale100 をアニメトラックでも毎キー再生する。
        //      バインド側 (ノード) からは除去済みのため、トラック側にも同じ F = q⁻¹·(1/s) を
        //      合成しないと骨階層とアニメが 90° / 100 倍ずれてしまう。
        const aiQuaternion axisInvQ;
        const float axisInvS = 1.0f / ctx.axisFixScale;


        // トラック
        for (const aiNodeAnim* ch : nodeChannels) {

            const std::string nodeName = ch->mNodeName.C_Str();
            const bool applyAxisFix =
                std::find(ctx.axisFixNodes.begin(), ctx.axisFixNodes.end(), nodeName)
                != ctx.axisFixNodes.end();

            std::vector<FzVectorKey> positionKeys;
            std::vector<FzQuaternionKey> rotationKeys;
            std::vector<FzVectorKey> scaleKeys;
            positionKeys.reserve(ch->mNumPositionKeys);
            rotationKeys.reserve(ch->mNumRotationKeys);
            scaleKeys.reserve(ch->mNumScalingKeys);
            for (uint32_t ki = 0; ki < ch->mNumPositionKeys; ++ki) {
                const auto& k = ch->mPositionKeys[ki];
                if (k.mTime < startTicks || k.mTime > endTicks) continue;
                aiVector3D v = k.mValue;
                if (applyAxisFix) v = axisInvQ.Rotate(v * axisInvS);
                FzVectorKey vk{ k.mTime - startTicks,
                    v.x * ctx.unitScale,
                    v.y * ctx.unitScale,
                    v.z * ctx.unitScale,
                    0.0f };
                positionKeys.push_back(vk);
            }
            for (uint32_t ki = 0; ki < ch->mNumRotationKeys; ++ki) {
                const auto& k = ch->mRotationKeys[ki];
                if (k.mTime < startTicks || k.mTime > endTicks) continue;
                aiQuaternion q = k.mValue;
                if (applyAxisFix) q = axisInvQ * q; // F の回転を左掛け (バインド側と同じ変換)
                FzQuaternionKey qk{ k.mTime - startTicks, q.x, q.y, q.z, q.w };
                rotationKeys.push_back(qk);
            }
            for (uint32_t ki = 0; ki < ch->mNumScalingKeys; ++ki) {
                const auto& k = ch->mScalingKeys[ki];
                if (k.mTime < startTicks || k.mTime > endTicks) continue;
                aiVector3D v = k.mValue;
                if (applyAxisFix) v = v * axisInvS; // scale100 キー → 1.0
                FzVectorKey vk{ k.mTime - startTicks, v.x, v.y, v.z, 0.0f };
                scaleKeys.push_back(vk);
            }
            OptimizeVectorKeys(positionKeys, ext.positionError);
            OptimizeQuaternionKeys(rotationKeys, ext.rotationErrorDegrees);
            OptimizeVectorKeys(scaleKeys, ext.scaleError);

            FzAnimTrackHeaderV3 th{};
            const size_t nlen = std::min(nodeName.size(), sizeof(th.nodeName)-1);
            std::memcpy(th.nodeName, nodeName.data(), nlen);
            std::string targetPath;
            (void)FindAnimationNodePath(scene->mRootNode, nodeName, targetPath);
            const size_t pathLength = std::min(targetPath.size(), sizeof(th.targetPath)-1);
            std::memcpy(th.targetPath, targetPath.data(), pathLength);
            th.interp = 1;
            th.positionCount = static_cast<uint32_t>(positionKeys.size());
            th.rotationCount = static_cast<uint32_t>(rotationKeys.size());
            th.scaleCount = static_cast<uint32_t>(scaleKeys.size());
            out.write(reinterpret_cast<const char*>(&th), sizeof(th));
            out.write(reinterpret_cast<const char*>(positionKeys.data()),
                      static_cast<std::streamsize>(positionKeys.size() * sizeof(FzVectorKey)));
            out.write(reinterpret_cast<const char*>(rotationKeys.data()),
                      static_cast<std::streamsize>(rotationKeys.size() * sizeof(FzQuaternionKey)));
            out.write(reinterpret_cast<const char*>(scaleKeys.data()),
                      static_cast<std::streamsize>(scaleKeys.size() * sizeof(FzVectorKey)));
        }

        // Assimp が公開する FBX BlendShape / Shape Key Weight を Morph Property Track へ変換する。
        for (const MorphExportTrack& morph : morphTracks) {
            FzPropertyTrackHeaderV3 th{};
            const size_t pathLength = std::min(morph.targetPath.size(), sizeof(th.targetPath) - 1);
            const size_t nameLength = std::min(morph.morphName.size(), sizeof(th.propertyName) - 1);
            std::memcpy(th.targetPath, morph.targetPath.data(), pathLength);
            std::memcpy(th.propertyName, morph.morphName.data(), nameLength);
            th.targetType = 2; // AnimTargetType::MorphWeight
            th.valueType = 0;  // AnimValueType::Float
            th.interp = 1;     // AnimInterp::Linear
            th.meshIndex = morph.meshIndex;
            th.floatCount = static_cast<uint32_t>(morph.keys.size());
            out.write(reinterpret_cast<const char*>(&th), sizeof(th));
            out.write(reinterpret_cast<const char*>(morph.keys.data()),
                      static_cast<std::streamsize>(morph.keys.size() * sizeof(FzFloatKeyV3)));
        }

        if (!out.good()) return false;
    }

    return true;
}

} // namespace fbzz::editor
