// FBZZ Engine
// AnimatorSystem.cpp | fbzz::scene
// スケルタルアニメーションのサンプリングとスキニングパレットのアップロード
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <Engine/Profiler/ProfileScope.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

struct NodeLocalPose {
    math::Vector3 translation = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3 scale = math::Vector3::ONE;
};

float WrapTime(float time, float duration)
{
    if (duration <= 0.0f) return 0.0f;
    float wrapped = std::fmod(time, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
}

math::Vector3 SampleVectorKeys(const std::vector<asset::VectorKey>& keys,
                               double ticks,
                               const math::Vector3& fallback,
                               asset::AnimInterp interp = asset::AnimInterp::Linear)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const asset::VectorKey& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == asset::AnimInterp::Step) return a.value;
    const double span = b.time - a.time;
    const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
    return math::Vector3::Lerp(a.value, b.value, t);
}

math::Quaternion SampleQuaternionKeys(const std::vector<asset::QuaternionKey>& keys,
                                       double ticks,
                                       const math::Quaternion& fallback,
                                       asset::AnimInterp interp = asset::AnimInterp::Linear)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const asset::QuaternionKey& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == asset::AnimInterp::Step) return a.value;
    const double span = b.time - a.time;
    const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
    return math::Quaternion::Slerp(a.value, b.value, t).Normalized();
}

const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName)
{
    for (const auto& track : clip.tracks)
        if (track.nodeName == nodeName)
            return &track;

    // FBX は DCC / exporter の設定により、同じボーンでも
    //   mixamorig:RightFoot
    //   RightFoot
    //   mixamorig:RightFoot_$AssimpFbx$_PreRotation
    // のようにチャンネル名が揺れることがある。
    // exact match を優先した上で、補助ノード suffix と namespace 差だけを吸収する。
    // 正規化規則は asset::CanonicalNodeName に集約し、インポーター側と共有する。
    const std::string canonicalNodeName = asset::CanonicalNodeName(nodeName);
    for (const auto& track : clip.tracks)
        if (asset::CanonicalNodeName(track.nodeName) == canonicalNodeName)
            return &track;

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Root Motion
// ─────────────────────────────────────────────────────────────────────────────

// あるクリップに対して確定したルートモーション設定。
// トラック解決とクリップ側フラグの上書きを 1 か所で済ませ、
// 「ポーズから抜く軸」と「delta に載せる軸」が食い違わないようにする。
struct ResolvedRootMotion {
    uint32_t trackIndex = UINT32_MAX;
    // delta に載せる軸。mode == None のときは全て false。
    bool applyXZ       = false;
    bool applyY        = false;
    bool applyRotation = false;
    // ポーズ (骨のローカル姿勢) からルート成分を除去する軸。
    // 抽出する軸は必ず除去する。除去しないと Transform 移動とポーズ移動で二重に進む。
    bool stripXZ       = false;
    bool stripY        = false;
    bool stripRotation = false;

    bool HasTrack() const { return trackIndex != UINT32_MAX; }
    bool ExtractsAnyAxis() const { return applyXZ || applyY || applyRotation; }
    bool StripsAnyAxis() const { return stripXZ || stripY || stripRotation; }
};

// RootMotionAxisOverride を、クリップ側フラグへ適用して最終値を決める。
bool ResolveAxis(RootMotionAxisOverride axisOverride, bool clipValue)
{
    switch (axisOverride) {
    case RootMotionAxisOverride::Disabled: return false;
    case RootMotionAxisOverride::Enabled:  return true;
    case RootMotionAxisOverride::UseClip:
    default:                               return clipValue;
    }
}

// Animator の設定とクリップの内容から、そのクリップのルートモーション構成を確定する。
//
// WHY: 旧実装はクリップ側フラグ (rootMotionApplyXZ 等) をポーズサンプリングが直接読み、
//      Animator 側の applyRootMotion は見ていなかった。その結果 applyRootMotion=false は
//      「ポーズからは抜かれるが Transform も delta も動かない」= 前進が消える状態になっていた。
//      抽出可否と除去可否をここで一元的に決め、両者が必ず整合するようにする。
ResolvedRootMotion ResolveRootMotion(const AnimatorComponent& animator,
                                     const asset::AnimationClip& clip)
{
    ResolvedRootMotion resolved;
    const RootMotionSettings& settings = animator.rootMotion;

    switch (settings.source) {
    case RootMotionSource::NodeName:
        resolved.trackIndex =
            asset::FindRootMotionTrackIndexByName(clip, settings.nodeName);
        break;
    case RootMotionSource::AutoDetect:
        // クリップ自身の指定を最優先し、無ければ候補名 → スケルトンルートで探す。
        resolved.trackIndex =
            clip.hasRootMotion && clip.rootMotionTrackIndex < clip.tracks.size()
            ? clip.rootMotionTrackIndex
            : asset::AutoDetectRootMotionTrackIndex(clip, animator.skeletonRootNodeName);
        break;
    case RootMotionSource::ClipDefined:
    default:
        if (clip.hasRootMotion) resolved.trackIndex = clip.rootMotionTrackIndex;
        break;
    }
    if (resolved.trackIndex >= clip.tracks.size()) {
        resolved.trackIndex = UINT32_MAX;
        return resolved;
    }

    // 軸マスク: クリップに焼かれた値を Animator 側の三値オーバーライドで上書きする。
    const bool axisXZ = ResolveAxis(settings.applyXZ, clip.rootMotionApplyXZ);
    const bool axisY = ResolveAxis(settings.applyY, clip.rootMotionApplyY);
    const bool axisRotation =
        ResolveAxis(settings.applyRotation, clip.rootMotionApplyRotation);

    if (settings.Extracts()) {
        // 抽出する軸はそのまま除去する軸でもある。
        resolved.applyXZ = resolved.stripXZ = axisXZ;
        resolved.applyY = resolved.stripY = axisY;
        resolved.applyRotation = resolved.stripRotation = axisRotation;
        return resolved;
    }

    // mode == None。delta には何も載せず、ポーズをどうするかだけ poseMode が決める。
    //   Strip: ルート成分を除去して「その場再生」にする (旧 applyRootMotion=false 相当)
    //   Keep : クリップのまま残す。ルートごと前進する DCC そのままの見た目になる
    if (settings.poseMode == RootMotionPoseMode::Strip) {
        resolved.stripXZ = axisXZ;
        resolved.stripY = axisY;
        resolved.stripRotation = axisRotation;
    } else {
        resolved.trackIndex = UINT32_MAX; // サンプリング側の分岐を丸ごと省く
    }
    return resolved;
}

// 階層パス ("Armature/Hips/Spine/Head") から対象 GameObject を引く。
//
// WHY string_view: この関数は「毎フレーム × アニメーター数 × クリップのトラック数」で呼ばれる。
//     ボーン 80 本のキャラなら 1 体あたり毎フレーム 80 回、パスは 5〜7 階層あるので
//     以前の substr 実装では 1 フレームに数千回の std::string ヒープ確保が発生していた。
//     区間を string_view で切り出せば確保はゼロになり、比較結果は完全に同じ。
GameObject* FindAnimationTarget(GameObject& root, const std::string& path)
{
    const std::string_view pathView{ path };
    if (pathView.empty() || pathView == "." || pathView == std::string_view{ root.name })
        return &root;

    GameObject* current = &root;
    size_t begin = 0;
    while (begin < pathView.size()) {
        const size_t end = pathView.find('/', begin);
        const std::string_view part = pathView.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        begin = end == std::string_view::npos ? pathView.size() : end + 1;
        if (part.empty() || (current == &root && part == std::string_view{ root.name })) continue;
        GameObject* next = nullptr;
        for (int i = 0; i < current->GetChildCount(); ++i) {
            GameObject* child = current->GetChild(i);
            if (child && std::string_view{ child->name } == part) {
                next = child;
                break;
            }
        }
        if (!next) return nullptr;
        current = next;
    }
    return current;
}

// マテリアルアニメーションの適用先 GameObject を探す。
// WHY: 以前は submesh ごとの子 GO を meshIndex == materialSlot で探していたが、
//      1 GameObject = モデル全体になり、submesh は MaterialComponent のスロットで
//      表現されるようになった。ここではスロットを持つ Renderer 側の GO を返し、
//      どのスロットへ書くかは ApplyMaterialProperty が track.materialSlot で決める。
GameObject* FindMaterialSlotTarget(GameObject& root)
{
    if (root.GetComponent<SkinnedMeshRenderer>() && root.GetComponent<MaterialComponent>())
        return &root;
    for (int i = 0; i < root.GetChildCount(); ++i) {
        GameObject* child = root.GetChild(i);
        if (!child) continue;
        if (GameObject* found = FindMaterialSlotTarget(*child)) return found;
    }
    return nullptr;
}

float SampleFloatKeys(const std::vector<asset::FloatKey>& keys,
                      double ticks,
                      asset::AnimInterp interp)
{
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const asset::FloatKey& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == asset::AnimInterp::Step) return a.value;
    const double span = b.time - a.time;
    const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
    if (interp != asset::AnimInterp::Cubic) return a.value + (b.value - a.value) * t;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float duration = static_cast<float>(span);
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * a.value
         + (t3 - 2.0f * t2 + t) * a.outTangent * duration
         + (-2.0f * t3 + 3.0f * t2) * b.value
         + (t3 - t2) * b.inTangent * duration;
}

math::Vector2 SampleVector2Keys(const std::vector<asset::Vector2Key>& keys,
                                double ticks,
                                asset::AnimInterp interp)
{
    if (keys.empty()) return math::Vector2::ZERO;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const asset::Vector2Key& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == asset::AnimInterp::Step) return a.value;
    const float t = static_cast<float>((ticks - a.time) / (b.time - a.time));
    return math::Vector2::Lerp(a.value, b.value, t);
}

math::Vector4 SampleVector4Keys(const std::vector<asset::Vector4Key>& keys,
                                double ticks,
                                asset::AnimInterp interp)
{
    if (keys.empty()) return math::Vector4::ZERO;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const asset::Vector4Key& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == asset::AnimInterp::Step) return a.value;
    const float t = static_cast<float>((ticks - a.time) / (b.time - a.time));
    return a.value + (b.value - a.value) * t;
}

template<typename Key>
const Key& SampleDiscreteKey(const std::vector<Key>& keys, double ticks)
{
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const Key& key) { return value < key.time; });
    return upper == keys.begin() ? keys.front() : *(upper - 1);
}

// Reflect() を一度だけ巡回して一致する永続キーへ値を書き込む。
class AnimationPropertyReflector final : public IReflector {
public:
    AnimationPropertyReflector(const asset::PropertyAnimationTrack& track, double ticks)
        : m_track(track), m_ticks(ticks) {}

    void Field(const char* name, float& value) override {
        if (Matches(name) && !m_track.floatKeys.empty())
            value = SampleFloatKeys(m_track.floatKeys, m_ticks, m_track.interp);
    }
    void Field(const char* name, int& value) override {
        if (Matches(name) && !m_track.intKeys.empty())
            value = SampleDiscreteKey(m_track.intKeys, m_ticks).value;
    }
    void Field(const char* name, bool& value) override {
        if (Matches(name) && !m_track.boolKeys.empty())
            value = SampleDiscreteKey(m_track.boolKeys, m_ticks).value;
    }
    void Field(const char* name, math::Vector2& value) override {
        if (Matches(name) && !m_track.vector2Keys.empty())
            value = SampleVector2Keys(m_track.vector2Keys, m_ticks, m_track.interp);
    }
    void Field(const char* name, math::Vector3& value) override {
        if (Matches(name) && !m_track.vector3Keys.empty())
            value = SampleVectorKeys(m_track.vector3Keys, m_ticks, value, m_track.interp);
    }
    void Field(const char* name, math::Vector4& value) override {
        if (Matches(name) && !m_track.vector4Keys.empty())
            value = SampleVector4Keys(m_track.vector4Keys, m_ticks, m_track.interp);
    }
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

private:
    bool Matches(const char* fallback) const {
        return m_track.propertyName == PersistentKey(fallback);
    }
    const asset::PropertyAnimationTrack& m_track;
    double m_ticks = 0.0;
};

void ApplyTransformTracks(GameObject& root,
                           const asset::AnimationClip& clip,
                           double ticks)
{
    for (const auto& track : clip.tracks) {
        GameObject* target = track.targetPath.empty()
            ? nullptr : FindAnimationTarget(root, track.targetPath);
        if (!target) continue;
        target->transform.position = SampleVectorKeys(
            track.positions, ticks, target->transform.position, track.interp);
        target->transform.rotation = SampleQuaternionKeys(
            track.rotations, ticks, target->transform.rotation, track.interp);
        target->transform.scale = SampleVectorKeys(
            track.scales, ticks, target->transform.scale, track.interp);
    }
}

void ApplyComponentProperty(GameObject& target,
                            const asset::PropertyAnimationTrack& track,
                            double ticks)
{
    bool applied = false;
    ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (applied || track.componentType != Registration::serializedName) return;
        if constexpr (requires(T& component, IReflector& reflector) {
            component.Reflect(reflector);
        }) {
            if (T* component = target.GetComponent<T>()) {
                AnimationPropertyReflector reflector(track, ticks);
                component->Reflect(reflector);
                applied = true;
            }
        }
    });
    if (applied) return;

    if (ScriptComponent* scripts = target.GetComponent<ScriptComponent>()) {
        for (auto& entry : scripts->scripts) {
            if (!entry.script || track.componentType != entry.script->GetTypeName()) continue;
            AnimationPropertyReflector reflector(track, ticks);
            entry.script->Reflect(reflector);
            break;
        }
    }
}

void ApplyMaterialProperty(GameObject& target,
                           const asset::PropertyAnimationTrack& track,
                           double ticks)
{
    MaterialComponent* material = target.GetComponent<MaterialComponent>();
    if (!material || track.propertyName.empty()) return;
    // track.materialSlot が submesh (= マテリアルスロット) を選ぶ。負値は主スロット。
    const size_t slotIndex = track.materialSlot > 0 ? static_cast<size_t>(track.materialSlot) : 0u;
    auto& values = material->SlotAt(slotIndex).paramOverrides[track.propertyName];
    switch (track.valueType) {
    case asset::AnimValueType::Float:
        values = { SampleFloatKeys(track.floatKeys, ticks, track.interp) };
        break;
    case asset::AnimValueType::Vector2: {
        const auto v = SampleVector2Keys(track.vector2Keys, ticks, track.interp);
        values = { v.x, v.y };
        break;
    }
    case asset::AnimValueType::Vector3: {
        const auto v = SampleVectorKeys(track.vector3Keys, ticks, math::Vector3::ZERO, track.interp);
        values = { v.x, v.y, v.z };
        break;
    }
    case asset::AnimValueType::Vector4:
    case asset::AnimValueType::Color: {
        const auto v = SampleVector4Keys(track.vector4Keys, ticks, track.interp);
        values = { v.x, v.y, v.z, v.w };
        break;
    }
    case asset::AnimValueType::Int:
        if (!track.intKeys.empty())
            values = { static_cast<float>(SampleDiscreteKey(track.intKeys, ticks).value) };
        break;
    case asset::AnimValueType::Bool:
        if (!track.boolKeys.empty())
            values = { SampleDiscreteKey(track.boolKeys, ticks).value ? 1.0f : 0.0f };
        break;
    }
}

void ApplyPropertyTracks(GameObject& root,
                         const asset::AnimationClip& clip,
                         double ticks,
                         SkinnedMeshRenderer* smr)
{
    for (const auto& track : clip.propertyTracks) {
        if (track.targetType == asset::AnimTargetType::MorphWeight) {
            if (smr && !track.floatKeys.empty())
                smr->morphWeights[track.propertyName] =
                    SampleFloatKeys(track.floatKeys, ticks, track.interp);
            continue;
        }
        if (track.targetPath.empty()) continue;
        GameObject* target = FindAnimationTarget(root, track.targetPath);
        if (track.targetType == asset::AnimTargetType::MaterialProperty &&
            (!target || !target->GetComponent<MaterialComponent>()))
            target = FindMaterialSlotTarget(root);
        if (!target) continue;
        switch (track.targetType) {
        case asset::AnimTargetType::ComponentProperty:
            ApplyComponentProperty(*target, track, ticks);
            break;
        case asset::AnimTargetType::MaterialProperty:
            ApplyMaterialProperty(*target, track, ticks);
            break;
        case asset::AnimTargetType::MorphWeight:
            break;
        }
    }
}

void DispatchAnimationEvents(GameObject& owner,
                              AnimatorComponent& animator,
                              const asset::AnimationClip& clip,
                             float previousTime,
                             float currentTime,
                             bool looped,
                             bool reverse)
{
    ScriptComponent* scripts = owner.GetComponent<ScriptComponent>();
    if (clip.events.empty()) return;
    const float duration = static_cast<float>(clip.GetDurationSeconds());
    for (const auto& event : clip.events) {
        const float eventTime = static_cast<float>(event.time);
        bool crossed = false;
        if (!reverse)
            crossed = looped ? (eventTime > previousTime || eventTime <= currentTime)
                             : (eventTime > previousTime && eventTime <= currentTime);
        else
            crossed = looped ? (eventTime < previousTime || eventTime >= currentTime)
                             : (eventTime < previousTime && eventTime >= currentTime);
        if (!crossed || eventTime < 0.0f || eventTime > duration) continue;
        const AnimationEventInfo info{
            event.name.c_str(), event.intParam, event.floatParam, eventTime
        };
        animator.firedEvents.push_back({
            event.name, event.intParam, event.floatParam, eventTime, Time::frameCount });
        if (scripts != nullptr)
            for (auto& entry : scripts->scripts)
                if (entry.script)
                    entry.script->InvokeAnimationEvent(info);
    }
}

// 1 クリップ分のルートモーション移動量。
struct RootMotionDelta {
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
};

// クリップの前フレームサンプルを取り出す (無ければ新規作成)。
// clip ポインタを同一性キーにする。LoadClips で clips 配列を作り直したときは
// AnimatorSystem 側でキャッシュごと破棄するため、無効ポインタは残らない。
RootMotionClipSample& AcquireRootMotionSample(AnimatorComponent& animator,
                                              const asset::AnimationClip& clip)
{
    for (auto& sample : animator.rootMotionSamples)
        if (sample.clip == &clip) return sample;
    animator.rootMotionSamples.push_back(RootMotionClipSample{ &clip });
    return animator.rootMotionSamples.back();
}

// 前フレームからの差分としてクリップ 1 本のルートモーションを取り出す。
//
// WHY: 旧実装は「ステート時刻の前後差分 + ループ判定」で計算していたため、
//      BlendTree の位相同期・Motion ごとの speed・遷移によるステート切り替えを
//      すべて呼び出し側で辻褄合わせする必要があり、実際には支配クリップ 1 本しか
//      扱えていなかった。クリップが自分の前回サンプル tick を覚えていれば、
//      ブレンド構成が毎フレーム変わっても各クリップの delta は連続する。
RootMotionDelta SampleClipRootMotion(AnimatorComponent& animator,
                                     const asset::AnimationClip& clip,
                                     const ResolvedRootMotion& resolved,
                                     double ticks,
                                     bool reverse,
                                     std::uint64_t frame)
{
    RootMotionDelta delta;
    if (!resolved.HasTrack() || !resolved.ExtractsAnyAxis()) return delta;

    const auto& track = clip.tracks[resolved.trackIndex];
    const auto samplePosition = [&](double t) {
        return SampleVectorKeys(track.positions, t, math::Vector3::ZERO, track.interp);
    };
    const auto sampleRotation = [&](double t) {
        return SampleQuaternionKeys(
            track.rotations, t, math::Quaternion::Identity(), track.interp);
    };

    RootMotionClipSample& sample = AcquireRootMotionSample(animator, clip);
    const math::Vector3 currentPosition = samplePosition(ticks);
    const math::Quaternion currentRotation = sampleRotation(ticks);

    // 前フレームに評価されていないクリップ (フェードインした直後など) は差分を取れない。
    // 最初の 1 フレームだけ寄与ゼロになるが、その時点の weight はほぼ 0 なので影響しない。
    const bool continuous = sample.frame + 1 == frame;
    if (continuous) {
        const double endTicks = clip.durationTicks > 0.0
            ? clip.durationTicks
            : clip.GetDurationSeconds() *
              (clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0);
        // 時刻が巻き戻っていればループ 1 周ぶんを跨いだとみなす。
        // 逆再生 (speed < 0) では大小関係が反転するため reverse で判定を切り替える。
        const bool wrapped = reverse ? (ticks > sample.ticks) : (ticks < sample.ticks);
        if (!wrapped) {
            delta.position = currentPosition - sample.position;
            delta.rotation = sample.rotation.Inverse() * currentRotation;
        } else if (!reverse) {
            delta.position = (samplePosition(endTicks) - sample.position)
                           + (currentPosition - samplePosition(0.0));
            delta.rotation = (sample.rotation.Inverse() * sampleRotation(endTicks))
                           * (sampleRotation(0.0).Inverse() * currentRotation);
        } else {
            delta.position = (samplePosition(0.0) - sample.position)
                           + (currentPosition - samplePosition(endTicks));
            delta.rotation = (sample.rotation.Inverse() * sampleRotation(0.0))
                           * (sampleRotation(endTicks).Inverse() * currentRotation);
        }
    }

    sample.ticks = ticks;
    sample.position = currentPosition;
    sample.rotation = currentRotation;
    sample.frame = frame;

    if (!resolved.applyXZ) delta.position.x = delta.position.z = 0.0f;
    if (!resolved.applyY) delta.position.y = 0.0f;
    if (!resolved.applyRotation) delta.rotation = math::Quaternion::Identity();
    delta.rotation = delta.rotation.Normalized();
    return delta;
}

// 前フレームに評価されなかったクリップのサンプルを捨てる。
// WHY: 放置すると Animator が触った全クリップぶん配列が伸び続ける。
void PruneRootMotionSamples(AnimatorComponent& animator, std::uint64_t frame)
{
    animator.rootMotionSamples.erase(
        std::remove_if(animator.rootMotionSamples.begin(),
                       animator.rootMotionSamples.end(),
                       [frame](const RootMotionClipSample& sample) {
                           return sample.frame != frame;
                       }),
        animator.rootMotionSamples.end());
}

void UpdateMorphVertexBuffers(SkinnedMeshRenderer& smr,
                              renderer::ResourceManager& resources)
{
    if (!smr.model) return;
    if (smr.morphWeights == smr.appliedMorphWeights &&
        smr.morphVertexBuffers.size() == smr.model->meshes.size()) return;
    smr.morphVertexBuffers.resize(smr.model->meshes.size());
    for (size_t meshIndex = 0; meshIndex < smr.model->meshes.size(); ++meshIndex) {
        const auto& mesh = smr.model->meshes[meshIndex];
        if (!mesh || mesh->morphTargets.empty()) continue;

        if (mesh->isSkinned) {
            auto vertices = mesh->cpuSkinnedVertices;
            for (const auto& target : mesh->morphTargets) {
                const auto weightIt = smr.morphWeights.find(target.name);
                if (weightIt == smr.morphWeights.end()) continue;
                const float weight = weightIt->second;
                const size_t count = (std::min)(vertices.size(), target.positionDeltas.size());
                for (size_t i = 0; i < count; ++i) {
                    vertices[i].position += target.positionDeltas[i] * weight;
                    vertices[i].normal += target.normalDeltas[i] * weight;
                    vertices[i].tangent += target.tangentDeltas[i] * weight;
                }
            }
            for (auto& vertex : vertices) {
                vertex.normal = vertex.normal.Normalized();
                vertex.tangent = vertex.tangent.Normalized();
            }
            auto& buffer = smr.morphVertexBuffers[meshIndex];
            if (!buffer.IsValid())
                buffer = resources.CreateVertexBuffer(
                    vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex),
                    sizeof(renderer::SkinnedVertex));
            else
                resources.Update(buffer, vertices.data(),
                                 vertices.size() * sizeof(renderer::SkinnedVertex));
        } else {
            auto vertices = mesh->cpuVertices;
            for (const auto& target : mesh->morphTargets) {
                const auto weightIt = smr.morphWeights.find(target.name);
                if (weightIt == smr.morphWeights.end()) continue;
                const float weight = weightIt->second;
                const size_t count = (std::min)(vertices.size(), target.positionDeltas.size());
                for (size_t i = 0; i < count; ++i) {
                    vertices[i].position += target.positionDeltas[i] * weight;
                    vertices[i].normal += target.normalDeltas[i] * weight;
                    vertices[i].tangent += target.tangentDeltas[i] * weight;
                }
            }
            for (auto& vertex : vertices) {
                vertex.normal = vertex.normal.Normalized();
                vertex.tangent = vertex.tangent.Normalized();
            }
            auto& buffer = smr.morphVertexBuffers[meshIndex];
            if (!buffer.IsValid())
                buffer = resources.CreateVertexBuffer(
                    vertices.data(), vertices.size() * sizeof(renderer::Vertex),
                    sizeof(renderer::Vertex));
            else
                resources.Update(buffer, vertices.data(),
                                 vertices.size() * sizeof(renderer::Vertex));
        }
    }
    smr.appliedMorphWeights = smr.morphWeights;
}

// ポーズからルート成分を除去する。除去する軸は ResolvedRootMotion が決める。
//
// WHY: 旧実装はクリップ側フラグを直接読んでいたため、Animator 側で抽出を止めても
//      ポーズからは抜かれ続け、前進成分がどこにも行かず消えていた。
//      「抜くかどうか」は Animator の設定で決まる、という一点に集約する。
void StripRootMotionFromPose(const asset::SkeletonNode& node,
                             const ResolvedRootMotion& rootMotion,
                             math::Vector3& translation,
                             math::Quaternion& rotation)
{
    if (rootMotion.stripXZ) {
        translation.x = node.bindTranslation.x;
        translation.z = node.bindTranslation.z;
    }
    if (rootMotion.stripY) translation.y = node.bindTranslation.y;
    if (rootMotion.stripRotation) rotation = node.bindRotation;
}

math::Matrix4 SampleNodeLocal(const asset::SkeletonNode& node,
                              const asset::AnimationClip& clip,
                              double ticks,
                              const ResolvedRootMotion& rootMotion)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) return node.localBindTransform;

    math::Vector3 translation = SampleVectorKeys(
        track->positions, ticks, node.bindTranslation, track->interp);
    math::Quaternion rotation = SampleQuaternionKeys(
        track->rotations, ticks, node.bindRotation, track->interp);
    if (rootMotion.HasTrack() && track == &clip.tracks[rootMotion.trackIndex])
        StripRootMotionFromPose(node, rootMotion, translation, rotation);
    const math::Vector3 scale = SampleVectorKeys(track->scales, ticks, node.bindScale, track->interp);
    return math::Matrix4::TRS(translation, rotation, scale);
}

NodeLocalPose SampleNodeLocalPose(const asset::SkeletonNode& node,
                                  const asset::AnimationClip& clip,
                                  double ticks,
                                  const ResolvedRootMotion& rootMotion)
{
    const auto* track = FindTrack(clip, node.name);
    if (!track) {
        return {
            node.bindTranslation,
            node.bindRotation,
            node.bindScale
        };
    }

    math::Vector3 translation = SampleVectorKeys(
        track->positions, ticks, node.bindTranslation, track->interp);
    math::Quaternion rotation = SampleQuaternionKeys(
        track->rotations, ticks, node.bindRotation, track->interp);
    if (rootMotion.HasTrack() && track == &clip.tracks[rootMotion.trackIndex])
        StripRootMotionFromPose(node, rootMotion, translation, rotation);
    return { translation, rotation,
             SampleVectorKeys(track->scales, ticks, node.bindScale, track->interp) };
}

void UpdateWorldTransform(GameObject& go, const Transform& parentTransform)
{
    Transform& tf = go.transform;
    math::Vector3 scaledLocal = {
        tf.position.x * parentTransform.worldScale.x,
        tf.position.y * parentTransform.worldScale.y,
        tf.position.z * parentTransform.worldScale.z
    };
    tf.worldRotation   = (parentTransform.worldRotation * tf.rotation).Normalized();
    tf.worldPosition   = parentTransform.worldPosition + parentTransform.worldRotation * scaledLocal;
    tf.worldScale = {
        parentTransform.worldScale.x * tf.scale.x,
        parentTransform.worldScale.y * tf.scale.y,
        parentTransform.worldScale.z * tf.scale.z
    };
}

void PropagateNonBoneChildTransforms(GameObject& parent)
{
    // WHY: AnimatorSystem は TransformSystem より後で Bone の local pose / world pose を上書きする。
    //      そのままだと Bone 配下にユーザーが置いた Particle / Attachment 用 GameObject は
    //      直前の TransformSystem 結果のままになり、手や武器に追従しない。
    //      BoneComponent を持つ子は Skeleton 再帰側で処理されるため、ここでは通常子だけ更新する。
    for (int i = 0; i < parent.GetChildCount(); ++i) {
        GameObject* child = parent.GetChild(i);
        if (!child) continue;
        if (child->GetComponent<BoneComponent>())
            continue;

        UpdateWorldTransform(*child, parent.transform);
        PropagateNonBoneChildTransforms(*child);
    }
}

GameObject* FindBoneDescendant(GameObject& root, int nodeIndex)
{
    for (int i = 0; i < root.GetChildCount(); ++i) {
        GameObject* child = root.GetChild(i);
        if (!child) continue;

        if (auto* bone = child->GetComponent<BoneComponent>())
            if (bone->nodeIndex == nodeIndex)
                return child;

        if (GameObject* found = FindBoneDescendant(*child, nodeIndex))
            return found;
    }
    return nullptr;
}

// スケルトンの親参照が循環していないかを確認し、生成時の再帰を止める。
// WHY: 通常のインポーターは木構造を作るが、古いキャッシュや破損した .fzasset は
//      親インデックスだけが循環することがあり、Base Layer の初回評価をハングさせる。
bool HasAcyclicParentChain(const asset::Skeleton& skeleton, int nodeIndex)
{
    std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
    int current = nodeIndex;
    while (current >= 0) {
        if (current >= static_cast<int>(skeleton.nodes.size()))
            return false;
        if (visited[static_cast<size_t>(current)] != 0)
            return false;
        visited[static_cast<size_t>(current)] = 1;
        current = skeleton.nodes[static_cast<size_t>(current)].parentIndex;
    }
    return true;
}

GameObject& EnsureBoneObject(Scene& scene,
                             GameObject& owner,
                             SkinnedMeshRenderer& smr,
                             const asset::Skeleton& skeleton,
                             int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return owner;
    auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];

    if (nodeIndex < static_cast<int>(smr.nodeEntities.size())) {
        if (auto* existing = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)])) {
            if (auto* bone = existing->GetComponent<BoneComponent>()) {
                bone->boneName = node.name;
                bone->boneIndex = node.boneIndex;
                bone->skinnedMeshEntity = owner.GetID();
            }
            return *existing;
        }
    }

    // 既存ボーンの捜索範囲。
    //
    // WHY owner の子孫だけでは足りないか (重要):
    //   ノードごとに子 GameObject へ分けた構成では、Renderer は Body / Visor といった
    //   子に付き、ボーンは兄弟の Armature 側に居る。owner の子孫しか見ないと 1 本も
    //   見つからず、Renderer ごとに同じスケルトンを作り直してしまう。
    //   同じキャラの Renderer が 3 つあればボーン階層が 3 セット生えて、
    //   アニメーションはそのうち 1 つにしか効かない、という壊れ方になる。
    //   skeletonRootEntity (Unity の rootBone 相当) が解決できていればその親を範囲にする。
    //   別キャラの兄弟へ誤って束縛しないよう、範囲は「起点ボーンの親」までに限る。
    GameObject* searchScope = &owner;
    if (GameObject* skeletonRoot = scene.GetGameObject(smr.skeletonRootEntity)) {
        if (auto* skeletonParent = skeletonRoot->GetParent())
            searchScope = skeletonParent;
        else
            searchScope = skeletonRoot;
    }

    if (GameObject* found = FindBoneDescendant(*searchScope, nodeIndex)) {
        if (auto* bone = found->GetComponent<BoneComponent>()) {
            bone->boneName = node.name;
            bone->boneIndex = node.boneIndex;
            bone->skinnedMeshEntity = owner.GetID();
        }
        smr.nodeEntities[static_cast<size_t>(nodeIndex)] = found->GetID();
        if (nodeIndex == skeleton.rootNodeIndex)
            smr.skeletonRootEntity = found->GetID();
        return *found;
    }

    GameObject* parent = &owner;
    if (node.parentIndex >= 0 &&
        node.parentIndex < static_cast<int>(skeleton.nodes.size()) &&
        HasAcyclicParentChain(skeleton, node.parentIndex))
        parent = &EnsureBoneObject(scene, owner, smr, skeleton, node.parentIndex);

    GameObject& boneObject = scene.CreateGameObject(node.name);
    boneObject.layer = owner.layer;
    boneObject.transform.position = node.bindTranslation;
    boneObject.transform.rotation = node.bindRotation;
    boneObject.transform.scale = node.bindScale;
    boneObject.SetParent(parent);

    BoneComponent bone{};
    bone.boneName = node.name;
    bone.nodeIndex = nodeIndex;
    bone.boneIndex = node.boneIndex;
    bone.skinnedMeshEntity = owner.GetID();
    bone.generated = true;
    boneObject.AddComponent<BoneComponent>(std::move(bone));

    smr.nodeEntities[static_cast<size_t>(nodeIndex)] = boneObject.GetID();
    if (nodeIndex == skeleton.rootNodeIndex)
        smr.skeletonRootEntity = boneObject.GetID();
    return boneObject;
}

void EnsureBoneHierarchy(Scene& scene,
                         GameObject& owner,
                         SkinnedMeshRenderer& smr,
                         const asset::Skeleton& skeleton)
{
    if (smr.nodeEntities.size() != skeleton.nodes.size())
        smr.nodeEntities.assign(skeleton.nodes.size(), EntityID::INVALID);

    for (size_t i = 0; i < skeleton.nodes.size(); ++i)
        EnsureBoneObject(scene, owner, smr, skeleton, static_cast<int>(i));
}

void ApplyAnimatedPoseToBones(Scene& scene,
                              const asset::Skeleton& skeleton,
                              const asset::AnimationClip& clip,
                              double ticks,
                              SkinnedMeshRenderer& smr,
                              const ResolvedRootMotion& rootMotion)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (i >= smr.nodeEntities.size()) continue;
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;

        const NodeLocalPose pose =
            SampleNodeLocalPose(skeleton.nodes[i], clip, ticks, rootMotion);
        boneObject->transform.position = pose.translation;
        boneObject->transform.rotation = pose.rotation;
        boneObject->transform.scale = pose.scale;
    }
}

void PropagateBoneTransforms(Scene& scene,
                             const asset::Skeleton& skeleton,
                             SkinnedMeshRenderer& smr,
                             int nodeIndex,
                             const Transform& parentTransform,
                             std::vector<uint8_t>& visited)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(smr.nodeEntities.size()))
        return;
    // アセット破損で children が循環していても、Base Layer の全身評価を
    // 無限再帰にしない。通常の Assimp 階層では各ノードは一度だけ通る。
    if (visited[static_cast<size_t>(nodeIndex)] != 0)
        return;
    visited[static_cast<size_t>(nodeIndex)] = 1;
    GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!boneObject) return;

    UpdateWorldTransform(*boneObject, parentTransform);
    PropagateNonBoneChildTransforms(*boneObject);

    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
        PropagateBoneTransforms(scene, skeleton, smr, child, boneObject->transform, visited);
}

void RebuildSkinningFromBoneTransforms(Scene& scene,
                                       GameObject& owner,
                                       const asset::Skeleton& skeleton,
                                       SkinnedMeshRenderer& smr,
                                       AnimatorComponent& animator)
{
    // Animator の評価失敗やリグ差し替え時も、配列外書き込みを起こさず VS 経路へ戻す。
    if (smr.nodeEntities.size() < skeleton.nodes.size() ||
        animator.nodeGlobalTransforms.size() < skeleton.nodes.size())
        return;

    const math::Matrix4 ownerInverse = math::Matrix4::Inverse(owner.transform.GetWorldMatrix());

    for (size_t nodeIndex = 0; nodeIndex < skeleton.nodes.size(); ++nodeIndex) {
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[nodeIndex]);
        if (!boneObject) continue;

        animator.nodeGlobalTransforms[nodeIndex] =
            ownerInverse * boneObject->transform.GetWorldMatrix();
    }

    for (size_t boneIndex = 0; boneIndex < animator.boneMatrices.size(); ++boneIndex) {
        if (boneIndex >= skeleton.bones.size()) continue;
        const auto& bone = skeleton.bones[boneIndex];
        if (bone.nodeIndex < 0 ||
            bone.nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
            continue;

        animator.boneMatrices[boneIndex] =
            skeleton.rootInverseTransform
          * animator.nodeGlobalTransforms[static_cast<size_t>(bone.nodeIndex)]
          * bone.offsetMatrix;
    }
}

void EvaluateNode(const asset::Skeleton& skeleton,
                  const asset::AnimationClip& clip,
                  int nodeIndex,
                  const math::Matrix4& parentGlobal,
                  double ticks,
                  std::vector<math::Matrix4>& palette,
                  std::vector<math::Matrix4>& nodeGlobals,
                  const ResolvedRootMotion& rootMotion)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const math::Matrix4 local = SampleNodeLocal(node, clip, ticks, rootMotion);
    const math::Matrix4 global = parentGlobal * local;

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;

    if (node.boneIndex >= 0 &&
        node.boneIndex < static_cast<int>(palette.size()) &&
        node.boneIndex < static_cast<int>(skeleton.bones.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }

    for (int child : node.children)
        EvaluateNode(skeleton, clip, child, global, ticks, palette, nodeGlobals, rootMotion);
}

// バインドポーズのスキニング行列をノード階層から構築する。
//
// WHY: 単位行列ではない。boneMatrix = rootInverse · nodeGlobal · offsetMatrix の
//   nodeGlobal をバインド TRS で埋めた値が正しい「無アニメ状態」で、これは
//   メッシュノードのグローバルバインド変換に一致する。
//   Mixamo など Y-up で書き出された FBX ではこれがたまたま identity になるため
//   単位行列でも破綻しなかったが、Blender 製 FBX は頂点が Z-up 生データのままで
//   Y-up への変換 (-90°X) をアーマチュアノードが担っている。単位行列を入れると
//   その回転が失われ、モデルが X 軸まわりに 90° 倒れて描画される。
// 無アニメ時はスケルトンのリファレンスポーズを送る。単位行列は
// skeleton = nullptr (スケルトン未解決の汎用アニメータ経路) のときだけ。
//
// WHY animator 側の配列も埋める: boneMatrices は IKSystem・MeshTrailRenderPass・
//   ParticlePass・AnimatorDebugPass が読む。GPU にだけリファレンスポーズを送って
//   CPU 側を identity のままにすると、トレイルの発生位置や IK の初期姿勢が描画とズレる。
void UploadBindPose(AnimatorComponent& animator,
                    renderer::ResourceManager& resources,
                    const asset::Skeleton* skeleton = nullptr)
{
    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();

    if (skeleton && !skeleton->referencePose.empty()) {
        const size_t boneCount = (std::min)(skeleton->referencePose.size(),
                                            static_cast<size_t>(asset::MAX_SKINNING_BONES));
        animator.boneMatrices.assign(skeleton->referencePose.begin(),
                                     skeleton->referencePose.begin()
                                         + static_cast<std::ptrdiff_t>(boneCount));
        for (size_t i = 0; i < boneCount; ++i)
            cb.boneMatrices[i] = skeleton->referencePose[i];
    }

    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

void LoadClips(AnimatorComponent& animator)
{
    animator.clips.clear();
    animator.clipSourcePaths.clear();
    // clips を作り直すとポインタが無効になる。ルートモーションのサンプルキャッシュは
    // clip ポインタをキーにしているため、ここで必ず捨てる。
    animator.rootMotionSamples.clear();

    // Node が直接参照する Source を収集する。
    std::vector<std::string> sources;
    auto addSource = [&sources](const std::string& sourcePath) {
        if (sourcePath.empty()) return;
        if (std::find(sources.begin(), sources.end(), sourcePath) == sources.end())
            sources.push_back(sourcePath);
    };
    const auto addStateSources = [&addSource](const AnimationState& state) {
        addSource(state.sourcePath);
        for (const auto& motion : state.blendTree1D.motions) addSource(motion.sourcePath);
        for (const auto& motion : state.blendTree2D.motions) addSource(motion.sourcePath);
    };

    // Base Layer だけでなく、追加 Layer の Draw/Holster や攻撃モーションも同じ
    // Animator クリップ配列へ登録する。Layer 側だけに接続された Motion を見落とすと、
    // Base Layer を有効にした後に「Layer のステート名は合っているのに動かない」状態になる。
    for (const auto& state : animator.states)
        addStateSources(state);
    for (const auto& layer : animator.layers)
        for (const auto& state : layer.states)
            addStateSources(state);

    for (const auto& src : sources) {
        if (src.empty()) continue;

        // Controller は .anim を GUID で保存するため、GUID 文字列そのものには
        // 拡張子が無い。解決前に ModelImporter へ渡すと、追加レイヤーの .anim を
        // FBX / .fzasset として解釈してクラッシュする経路になる。
        const std::string resolvedSource = asset::AssetManager::ResolveAssetPath(src);
        const auto hasAnimExtension = [](const std::string& path) {
            if (path.size() < 5) return false;
            const size_t offset = path.size() - 5;
            for (size_t i = 0; i < 5; ++i) {
                const char c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(path[offset + i])));
                constexpr char suffix[] = ".anim";
                if (c != suffix[i]) return false;
            }
            return true;
        };

        // .anim ファイルは AnimationClip として直接ロードする。
        // WHY: FBX インポート時のアニメーションクリップは .anim に分離されており、
        //      .fzasset (モデルファイル) には clips が含まれないため。
        if (hasAnimExtension(resolvedSource)) {
            auto h = asset::AssetManager::Load<asset::AnimationClip>(src);
            if (!h.IsValid()) {
                FBZZ_LOG_WARN("AnimatorSystem: .anim source '%s' failed to load", src.c_str());
                continue;
            }
            const auto* clip = asset::AssetManager::Get<asset::AnimationClip>(h);
            if (clip) {
                animator.clips.push_back(*clip);
                animator.clipSourcePaths.push_back(src);
            }
            continue;
        }

        auto model = asset::AssetManager::LoadModel(src);
        if (!model) {
            FBZZ_LOG_WARN("AnimatorSystem: clip source '%s' failed to load", src.c_str());
            continue;
        }
        for (const auto& clip : model->clips) {
            animator.clips.push_back(clip);
            animator.clipSourcePaths.push_back(src);
        }
    }
    animator.clipsLoaded = true;
    animator.clipsAttemptGeneration = asset::AssetManager::GetFlushGeneration();
}

// ── ステートマシン用ヘルパー ──────────────────────────────────────────────────

// animator.clips からクリップ名で探す。
// 完全一致 → 大文字小文字無視の含有一致 の順でフォールバックする。
// WHY: FBX エクスポーターによっては "Walk" → "Armature|Walk" のように
//      オブジェクト名がプレフィックスとして付くため、完全一致だけでは取得できない。
const asset::AnimationClip* FindClipByName(const AnimatorComponent& animator,
                                           const std::string& clipName)
{
    if (clipName.empty() || animator.clips.empty()) return nullptr;

    // 1st pass: 完全一致
    for (const auto& c : animator.clips)
        if (c.name == clipName) return &c;

    // 2nd pass: 大文字小文字無視の部分一致
    //   FBX の clip.name が clipName を含んでいれば採用
    auto toLower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string lowerTarget = toLower(clipName);
    for (const auto& c : animator.clips) {
        if (toLower(c.name).find(lowerTarget) != std::string::npos) return &c;
    }

    return nullptr;
}

const asset::AnimationClip* FindClipBySource(const AnimatorComponent& animator,
                                             const std::string& sourcePath,
                                             const std::string& clipName);

// ステートに対応するクリップを返す。
// clipName 名前検索 → clipIndex 直接指定 の順でフォールバックする。
// WHY: Mixamo 等は FBX 内クリップ名を "mixamo.com" にするため名前検索が失敗する。
//      clipIndex を明示することで任意の FBX でも確実に動作させる。
const asset::AnimationClip* FindClipForState(const AnimatorComponent& animator,
                                             const AnimationState& state)
{
    if (const auto* clip =
            FindClipBySource(animator, state.sourcePath, state.clipName))
        return clip;
    if (!state.clipName.empty()) {
        if (const auto* c = FindClipByName(animator, state.clipName)) return c;
    }
    const int idx = state.clipIndex;
    if (idx >= 0 && idx < static_cast<int>(animator.clips.size()))
        return &animator.clips[idx];
    return nullptr;
}

// animator.states からステート名で探す。見つからなければ nullptr。
const AnimationState* FindState(const AnimatorComponent& animator,
                                const std::string& stateName)
{
    for (const auto& s : animator.states)
        if (s.name == stateName) return &s;
    return nullptr;
}

AnimationState* FindMutableState(AnimatorComponent& animator,
                                 const std::string& stateName)
{
    for (auto& state : animator.states)
        if (state.name == stateName) return &state;
    return nullptr;
}

// animator.parameters からパラメーター名で探す。見つからなければ nullptr。
AnimatorParameter* FindParam(AnimatorComponent& animator, const std::string& paramName)
{
    for (auto& p : animator.parameters)
        if (p.name == paramName) return &p;
    return nullptr;
}

// Source Path と Clip Name の組でクリップを解決する。
// WHY: 異なる FBX が同名クリップを持っていても Node の参照先を一意に保つため。
const asset::AnimationClip* FindClipBySource(const AnimatorComponent& animator,
                                             const std::string& sourcePath,
                                             const std::string& clipName)
{
    if (sourcePath.empty()) return nullptr;
    const asset::AnimationClip* firstFromSource = nullptr;
    for (size_t i = 0; i < animator.clips.size(); ++i) {
        if (i >= animator.clipSourcePaths.size() ||
            animator.clipSourcePaths[i] != sourcePath) continue;
        if (!firstFromSource) firstFromSource = &animator.clips[i];
        if (!clipName.empty() && animator.clips[i].name == clipName)
            return &animator.clips[i];
    }
    return firstFromSource;
}

const asset::AnimationClip* FindClipForMotion(const AnimatorComponent& animator,
                                              const BlendTreeMotion& motion)
{
    if (const auto* clip =
            FindClipBySource(animator, motion.sourcePath, motion.clipName))
        return clip;
    if (!motion.clipName.empty()) {
        if (const auto* clip = FindClipByName(animator, motion.clipName)) return clip;
    }
    if (motion.clipIndex >= 0 &&
        motion.clipIndex < static_cast<int>(animator.clips.size()))
        return &animator.clips[static_cast<size_t>(motion.clipIndex)];
    return nullptr;
}

struct WeightedMotion {
    const BlendTreeMotion* motion = nullptr;
    float                  weight = 0.0f;
};

struct WeightedClip {
    const asset::AnimationClip* clip   = nullptr;
    const BlendTreeMotion*      motion = nullptr;
    double                      ticks  = 0.0;
    float                       weight = 0.0f;
    float                       ikWeight = 1.0f;
    // このクリップのルートモーション構成。ポーズ除去と delta 抽出の両方がここを見る。
    // WHY: 旧実装は「最大 weight のクリップ 1 本」からしかルートモーションを取れず、
    //      BlendTree の Walk↔Run が 0.5 を跨いだ瞬間に移動量が段差状に飛んでいた。
    //      クリップごとに持たせて weight で加重合成する。
    ResolvedRootMotion          rootMotion{};
    // 逆再生中か。ループ跨ぎの判定方向がひっくり返る。
    bool                        reverse = false;
};

std::vector<WeightedMotion> Compute1DWeights(const AnimatorComponent& animator,
                                             const BlendTree1D& tree)
{
    std::vector<WeightedMotion> result;
    if (tree.motions.empty()) return result;

    std::vector<const BlendTreeMotion*> sorted;
    sorted.reserve(tree.motions.size());
    for (const auto& motion : tree.motions) sorted.push_back(&motion);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) { return a->threshold < b->threshold; });

    const float value = tree.dampTime > math::EPSILON &&
                        tree.dampedValueInitialized
        ? tree.dampedValue
        : animator.GetFloat(tree.paramName);
    if (sorted.size() == 1 || value <= sorted.front()->threshold)
        return { { sorted.front(), 1.0f } };
    if (value >= sorted.back()->threshold)
        return { { sorted.back(), 1.0f } };

    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
        const float a = sorted[i]->threshold;
        const float b = sorted[i + 1]->threshold;
        if (value < a || value > b) continue;
        const float span = b - a;
        const float t = span > math::EPSILON ? (value - a) / span : 0.0f;
        return { { sorted[i], 1.0f - t }, { sorted[i + 1], t } };
    }
    return { { sorted.back(), 1.0f } };
}

std::vector<float> ComputeGradientBandWeights(
    const std::vector<const BlendTreeMotion*>& motions,
    float px,
    float py)
{
    std::vector<float> weights(motions.size(), 0.0f);
    if (motions.empty()) return weights;
    if (motions.size() == 1) {
        weights[0] = 1.0f;
        return weights;
    }

    for (size_t i = 0; i < motions.size(); ++i) {
        float score = 1.0f;
        bool compared = false;
        for (size_t j = 0; j < motions.size(); ++j) {
            if (i == j) continue;
            const float dx = motions[i]->posX - motions[j]->posX;
            const float dy = motions[i]->posY - motions[j]->posY;
            const float denominator = dx * dx + dy * dy;
            if (denominator <= math::EPSILON) continue;
            const float band =
                ((px - motions[j]->posX) * dx + (py - motions[j]->posY) * dy)
                / denominator;
            score = (std::min)(score, band);
            compared = true;
        }
        weights[i] = compared ? (std::max)(0.0f, score) : 1.0f;
    }

    const float sum = std::accumulate(weights.begin(), weights.end(), 0.0f);
    if (sum > math::EPSILON) {
        for (float& weight : weights) weight /= sum;
    } else {
        size_t nearest = 0;
        float nearestDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < motions.size(); ++i) {
            const float dx = px - motions[i]->posX;
            const float dy = py - motions[i]->posY;
            const float distance = dx * dx + dy * dy;
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = i;
            }
        }
        weights[nearest] = 1.0f;
    }
    return weights;
}

std::vector<WeightedMotion> Compute2DWeights(const AnimatorComponent& animator,
                                             const BlendTree2D& tree)
{
    std::vector<WeightedMotion> result;
    if (tree.motions.empty()) return result;

    float px = animator.GetFloat(tree.paramX);
    float py = animator.GetFloat(tree.paramY);
    std::vector<const BlendTreeMotion*> motions;
    motions.reserve(tree.motions.size());
    for (const auto& motion : tree.motions) motions.push_back(&motion);

    std::vector<float> weights;
    if (tree.type == BlendTree2DType::SimpleDirectional) {
        size_t origin = motions.size();
        float originDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < motions.size(); ++i) {
            const float distance =
                motions[i]->posX * motions[i]->posX + motions[i]->posY * motions[i]->posY;
            if (distance < originDistance) {
                originDistance = distance;
                origin = i;
            }
        }

        const float magnitude = std::sqrt(px * px + py * py);
        if (magnitude <= math::EPSILON) {
            weights.assign(motions.size(), 0.0f);
            weights[origin] = 1.0f;
        } else {
            std::vector<const BlendTreeMotion*> directional;
            std::vector<BlendTreeMotion> normalizedDirectional;
            std::vector<size_t> directionalIndices;
            normalizedDirectional.reserve(motions.size());
            for (size_t i = 0; i < motions.size(); ++i) {
                if (i == origin && originDistance <= math::EPSILON) continue;
                const float motionMagnitude = std::sqrt(
                    motions[i]->posX * motions[i]->posX +
                    motions[i]->posY * motions[i]->posY);
                if (motionMagnitude <= math::EPSILON) continue;
                normalizedDirectional.push_back(*motions[i]);
                normalizedDirectional.back().posX /= motionMagnitude;
                normalizedDirectional.back().posY /= motionMagnitude;
                directionalIndices.push_back(i);
            }
            directional.reserve(normalizedDirectional.size());
            for (const auto& motion : normalizedDirectional)
                directional.push_back(&motion);

            weights.assign(motions.size(), 0.0f);
            const float directionAmount = std::clamp(magnitude, 0.0f, 1.0f);
            if (directional.empty()) {
                weights[origin] = 1.0f;
            } else {
                const auto directionalWeights =
                    ComputeGradientBandWeights(
                        directional, px / magnitude, py / magnitude);
                if (originDistance <= math::EPSILON)
                    weights[origin] = 1.0f - directionAmount;
                for (size_t i = 0; i < directionalWeights.size(); ++i)
                    weights[directionalIndices[i]] =
                        directionalWeights[i] * directionAmount;
            }
        }
    } else {
        weights = ComputeGradientBandWeights(motions, px, py);
    }

    for (size_t i = 0; i < motions.size(); ++i)
        if (weights[i] > math::EPSILON)
            result.push_back({ motions[i], weights[i] });
    return result;
}

std::vector<WeightedMotion> ComputeStateWeights(const AnimatorComponent& animator,
                                                const AnimationState& state)
{
    if (state.mode == AnimationStateMode::BlendTree1D)
        return Compute1DWeights(animator, state.blendTree1D);
    if (state.mode == AnimationStateMode::BlendTree2D)
        return Compute2DWeights(animator, state.blendTree2D);
    return {};
}

std::vector<WeightedClip> BuildStateClips(const AnimatorComponent& animator,
                                          const AnimationState& state,
                                          float stateTime)
{
    std::vector<WeightedClip> result;
    // 実効再生方向。Animator / State / Motion の speed すべての符号で決まる。
    const auto isReverse = [&animator, &state](float motionSpeed) {
        return animator.speed * state.speed * motionSpeed < 0.0f;
    };

    if (state.mode == AnimationStateMode::Clip) {
        if (const auto* clip = FindClipForState(animator, state)) {
            const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
            result.push_back({
                clip, nullptr, static_cast<double>(stateTime) * tps, 1.0f, state.ikWeight,
                ResolveRootMotion(animator, *clip), isReverse(1.0f)
            });
        }
        return result;
    }

    for (const auto& weighted : ComputeStateWeights(animator, state)) {
        const auto* clip = FindClipForMotion(animator, *weighted.motion);
        if (!clip) continue;
        const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        const float duration = static_cast<float>(clip->GetDurationSeconds());
        float motionTime = stateTime * weighted.motion->speed;
        if (state.mode == AnimationStateMode::BlendTree1D &&
            state.blendTree1D.syncNormalizedTime &&
            state.blendTree1D.normalizedPhaseInitialized) {
            // WHY: Motionごとの秒数で個別Wrapすると、Weight 0から復帰したWalkが別位相で現れる。
            //      同じ0..1位相を各Clip長へ写像し、Idle/Walk/Runの足運びを連続させる。
            motionTime = state.blendTree1D.normalizedPhase * duration;
        } else {
            motionTime = state.loop
                ? WrapTime(motionTime, duration)
                : std::clamp(motionTime, 0.0f, duration);
        }
        result.push_back({
            clip,
            weighted.motion,
            static_cast<double>(motionTime) * tps,
            weighted.weight,
            // State を全体係数、Motion をクリップ固有係数として扱う。
            // WHY: BlendTree 全体を一括調整しつつ、Run だけ足IKを弱められるようにする。
            state.ikWeight * weighted.motion->ikWeight,
            ResolveRootMotion(animator, *clip),
            isReverse(weighted.motion->speed)
        });
    }

    const float sum = std::accumulate(
        result.begin(), result.end(), 0.0f,
        [](float total, const WeightedClip& clip) { return total + clip.weight; });
    if (sum > math::EPSILON)
        for (auto& clip : result) clip.weight /= sum;
    return result;
}

float GetStateDuration(const AnimatorComponent& animator, const AnimationState& state)
{
    if (state.mode == AnimationStateMode::Clip) {
        const auto* clip = FindClipForState(animator, state);
        if (!clip) return 0.0f;
        return static_cast<float>(clip->GetDurationSeconds());
    }

    // BlendTree の再生周期は現在 Weight に依存させず、全 Motion の最大実効 Length で固定する。
    // WHY: Damping 中は Weight が毎フレーム変わる。加重平均 Length を WrapTime に使うと、
    //      周期が途中で短くなった瞬間に stateTime / blendToTime が巻き戻り、Walk が再生し直されるため。
    float duration = 0.0f;
    const auto accumulateMotionDuration = [&](const BlendTreeMotion& motion) {
        const auto* clip = FindClipForMotion(animator, motion);
        if (!clip) return;
        const float speed = (std::max)(std::abs(motion.speed), 1e-4f);
        duration = (std::max)(
            duration,
            static_cast<float>(clip->GetDurationSeconds()) / speed);
    };

    if (state.mode == AnimationStateMode::BlendTree1D) {
        for (const auto& motion : state.blendTree1D.motions)
            accumulateMotionDuration(motion);
    } else if (state.mode == AnimationStateMode::BlendTree2D) {
        for (const auto& motion : state.blendTree2D.motions)
            accumulateMotionDuration(motion);
    }
    return duration;
}

NodeLocalPose BlendNodePose(const asset::SkeletonNode& node,
                            const std::vector<WeightedClip>& clips)
{
    NodeLocalPose blended{};
    blended.translation = math::Vector3::ZERO;
    blended.scale = math::Vector3::ZERO;
    bool hasRotation = false;
    float accumulatedRotationWeight = 0.0f;

    for (const auto& weighted : clips) {
        if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
        const NodeLocalPose pose = SampleNodeLocalPose(
            node, *weighted.clip, weighted.ticks, weighted.rootMotion);
        blended.translation += pose.translation * weighted.weight;
        blended.scale += pose.scale * weighted.weight;
        if (!hasRotation) {
            blended.rotation = pose.rotation;
            accumulatedRotationWeight = weighted.weight;
            hasRotation = true;
        } else {
            const float total = accumulatedRotationWeight + weighted.weight;
            const float t = total > math::EPSILON ? weighted.weight / total : 0.0f;
            blended.rotation =
                math::Quaternion::Slerp(blended.rotation, pose.rotation, t).Normalized();
            accumulatedRotationWeight = total;
        }
    }
    if (!hasRotation) {
        blended.translation = node.bindTranslation;
        blended.rotation = node.bindRotation;
        blended.scale = node.bindScale;
    }
    return blended;
}

void EvaluateNBlendedNodeRecursive(const asset::Skeleton& skeleton,
                                   const std::vector<WeightedClip>& clips,
                                   int nodeIndex,
                                   const math::Matrix4& parentGlobal,
                                   std::vector<math::Matrix4>& palette,
                                   std::vector<math::Matrix4>& nodeGlobals,
                                   std::vector<uint8_t>& visited)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
    // Base Layer は全スケルトンを評価するため、壊れたキャッシュの循環参照が
    // あるとここがフレームを返さなくなる。評価済みノードを一度だけ処理する。
    if (visited[static_cast<size_t>(nodeIndex)] != 0)
        return;
    visited[static_cast<size_t>(nodeIndex)] = 1;
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const NodeLocalPose blended = BlendNodePose(node, clips);
    const math::Matrix4 global =
        parentGlobal * math::Matrix4::TRS(
            blended.translation, blended.rotation, blended.scale);

    if (nodeIndex < static_cast<int>(nodeGlobals.size()))
        nodeGlobals[static_cast<size_t>(nodeIndex)] = global;
    if (node.boneIndex >= 0 &&
        node.boneIndex < static_cast<int>(palette.size()) &&
        node.boneIndex < static_cast<int>(skeleton.bones.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }
    for (int child : node.children)
        EvaluateNBlendedNodeRecursive(
            skeleton, clips, child, global, palette, nodeGlobals, visited);
}

void ApplyNBlendedPoseToBones(Scene& scene,
                              const asset::Skeleton& skeleton,
                              const std::vector<WeightedClip>& clips,
                              SkinnedMeshRenderer& smr,
                              const asset::AvatarMaskAsset* baseMask)
{
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (i >= smr.nodeEntities.size()) continue;
        GameObject* boneObject = scene.GetGameObject(smr.nodeEntities[i]);
        if (!boneObject) continue;
        const asset::SkeletonNode& node = skeleton.nodes[i];
        const NodeLocalPose pose = BlendNodePose(node, clips);

        // Base Layer マスク: 重みが 1 未満のボーンはバインドポーズ側へ寄せる。
        // WHY: Base Layer から外したボーンは「誰も動かしていない素の状態」であってほしい。
        //      前フレームの姿勢を残すと、上のレイヤーが weight 0 になった瞬間に
        //      最後のポーズで固まってしまう。バインドへ戻せば常に決定的になる。
        float weight = 1.0f;
        if (baseMask != nullptr) {
            const std::string path = asset::BuildSkeletonNodePath(
                skeleton, static_cast<int>(i));
            weight = asset::EvaluateAvatarMaskWeight(*baseMask, path, node.name);
        }

        if (weight >= 1.0f - math::EPSILON) {
            boneObject->transform.position = pose.translation;
            boneObject->transform.rotation = pose.rotation;
            boneObject->transform.scale = pose.scale;
        } else {
            boneObject->transform.position =
                math::Vector3::Lerp(node.bindTranslation, pose.translation, weight);
            boneObject->transform.rotation =
                math::Quaternion::Slerp(node.bindRotation, pose.rotation, weight).Normalized();
            boneObject->transform.scale =
                math::Vector3::Lerp(node.bindScale, pose.scale, weight);
        }
    }
}

// 遷移条件を1つ評価する。パラメーターが見つからない場合は false。
bool CheckCondition(const AnimatorCondition& cond, const AnimatorParameter* param)
{
    if (!param) return false;

    switch (cond.op) {
    case ConditionOp::Greater:
        if (param->type == ParamType::Int)
            return static_cast<float>(param->intValue) > cond.threshold;
        return param->floatValue > cond.threshold;
    case ConditionOp::Less:
        if (param->type == ParamType::Int)
            return static_cast<float>(param->intValue) < cond.threshold;
        return param->floatValue < cond.threshold;
    case ConditionOp::Equal:
        if (param->type == ParamType::Int)
            return param->intValue == static_cast<int>(cond.threshold);
        return std::abs(param->floatValue - cond.threshold) < 1e-4f;
    case ConditionOp::NotEqual:
        if (param->type == ParamType::Int)
            return param->intValue != static_cast<int>(cond.threshold);
        return std::abs(param->floatValue - cond.threshold) >= 1e-4f;
    case ConditionOp::True:
        return param->boolValue;
    case ConditionOp::False:
        return !param->boolValue;
    }
    return false;
}

// 遷移の全条件（AND）と hasExitTime を評価する。
bool EvaluateTransition(const AnimationTransition& tr,
                        AnimatorComponent& animator,
                        float normalizedTime)
{
    // hasExitTime: 再生位置が exitTime に達していないと遷移しない
    if (tr.hasExitTime && normalizedTime < tr.exitTime) return false;

    // 条件リストが空で hasExitTime=false → 無効定義として遷移しない
    if (tr.conditions.empty()) return tr.hasExitTime;

    // 全条件 AND
    for (const auto& cond : tr.conditions) {
        const AnimatorParameter* param = FindParam(animator, cond.paramName);
        if (!CheckCondition(cond, param)) return false;
    }
    return true;
}

// 遷移に使われた Trigger パラメーターを false にリセットする。
// WHY: Trigger は「1フレームの発火信号」なので、遷移に消費されたら自動で戻す必要がある。
void ConsumeTriggers(AnimatorComponent& animator, const AnimationTransition& tr)
{
    for (const auto& cond : tr.conditions) {
        auto* param = FindParam(animator, cond.paramName);
        if (param && param->type == ParamType::Trigger)
            param->boolValue = false;
    }
}

// ステートマシンを defaultStateName または states[0] で初期化する。
// currentStateName が既に設定されている場合は何もしない。
// ── ステートマシンのスコープ化 ───────────────────────────────────────────────
// WHY: 上半身と下半身を別々に制御するには、レイヤーごとに独立したステートマシンが要る。
//      しかし Base Layer の状態は AnimatorComponent 直下のフィールド
//      (currentStateName / stateTime / blendTo*) にあり、Script・Editor・MCP・VFX が
//      これを直接参照している。フィールドを構造体へ畳むと参照側を全部書き換えることになる。
//      そこで「どの states を、どのランタイム変数で回すか」だけを参照で束ねたビューを作り、
//      評価ロジックはこのビューに対して書く。Base Layer は既存フィールドを、
//      追加レイヤーは AnimationLayer::runtime を指すだけで同じコードが使い回せる。
struct StateMachineScope {
    std::vector<AnimationState>&            states;
    const std::vector<AnimationTransition>& anyStateTransitions;
    const std::string&                      defaultStateName;
    std::string&                            currentStateName;
    float&                                  stateTime;
    std::string&                            blendToState;
    float&                                  blendToTime;
    float&                                  blendWeight;
    float&                                  blendDuration;
};

StateMachineScope BaseScope(AnimatorComponent& animator)
{
    return StateMachineScope{
        animator.states, animator.anyStateTransitions, animator.defaultStateName,
        animator.currentStateName, animator.stateTime, animator.blendToState,
        animator.blendToTime, animator.blendWeight, animator.blendDuration
    };
}

StateMachineScope LayerScope(AnimationLayer& layer)
{
    return StateMachineScope{
        layer.states, layer.anyStateTransitions, layer.defaultStateName,
        layer.runtime.currentStateName, layer.runtime.stateTime, layer.runtime.blendToState,
        layer.runtime.blendToTime, layer.runtime.blendWeight, layer.runtime.blendDuration
    };
}

const AnimationState* FindStateIn(const std::vector<AnimationState>& states,
                                  const std::string& stateName)
{
    for (const auto& s : states)
        if (s.name == stateName) return &s;
    return nullptr;
}

AnimationState* FindMutableStateIn(std::vector<AnimationState>& states,
                                   const std::string& stateName)
{
    for (auto& s : states)
        if (s.name == stateName) return &s;
    return nullptr;
}

void InitStateMachineScoped(StateMachineScope scope)
{
    if (!scope.currentStateName.empty()) return;
    if (scope.states.empty()) return;

    if (!scope.defaultStateName.empty() &&
        FindStateIn(scope.states, scope.defaultStateName))
        scope.currentStateName = scope.defaultStateName;
    else
        scope.currentStateName = scope.states[0].name;

    scope.stateTime    = 0.0f;
    scope.blendToState = "";
    scope.blendWeight  = 0.0f;
}

void InitStateMachine(AnimatorComponent& animator)
{
    InitStateMachineScoped(BaseScope(animator));
}

// 遷移条件の評価は animator.parameters を見る。パラメーターは Unity 同様
// レイヤーをまたいで共有されるため、scope ではなく animator をそのまま渡す。
bool TryStartTransitionScoped(AnimatorComponent& animator,
                              StateMachineScope scope,
                              const std::vector<AnimationTransition>& transitions,
                              float normalizedTime)
{
    for (const auto& tr : transitions) {
        if (tr.toStateName.empty()) continue;
        // 現在ステート自身へ戻る遷移は開始しない。
        // WHY: Fall 条件のような継続条件で毎フレーム自己遷移すると、再生時刻が0へ戻り続けるため。
        if (tr.toStateName == scope.currentStateName) continue;
        if (!FindStateIn(scope.states, tr.toStateName)) continue;
        if (!EvaluateTransition(tr, animator, normalizedTime)) continue;

        scope.blendToState = tr.toStateName;
        scope.blendToTime  = 0.0f;
        scope.blendWeight  = 0.0f;
        if (auto* targetState = FindMutableStateIn(scope.states, tr.toStateName)) {
            targetState->blendTree1D.normalizedPhase = 0.0f;
            targetState->blendTree1D.normalizedPhaseInitialized = false;
        }
        // 正規化指定は遷移元ステートの Length を基準に実秒へ変換する。
        // WHY: クリップを差し替えても同じ割合の Motion Blend を維持できる。
        const AnimationState* currentState =
            FindStateIn(scope.states, scope.currentStateName);
        const float sourceDuration = currentState
            ? GetStateDuration(animator, *currentState)
            : 0.0f;
        scope.blendDuration = tr.fixedDuration
            ? tr.transitionDuration
            : tr.transitionDuration * sourceDuration;
        scope.blendDuration = (std::max)(scope.blendDuration, 0.0f);
        ConsumeTriggers(animator, tr);
        return true;
    }
    return false;
}

// 1D BlendTree の入力値を時定数ベースで平滑化する。
// WHY: Script が Speed を 0 / 4 / 7.2 と離散的に設定しても、姿勢 Weight は連続変化させる。
void UpdateBlendTree1DDampingIn(AnimatorComponent& animator,
                                std::vector<AnimationState>& states,
                                float dt)
{
    if (!animator.playing) return;
    for (auto& state : states) {
        if (state.mode != AnimationStateMode::BlendTree1D) continue;
        auto& tree = state.blendTree1D;
        const float target = animator.GetFloat(tree.paramName);
        if (!tree.dampedValueInitialized || tree.dampTime <= math::EPSILON) {
            tree.dampedValue = target;
            tree.dampedValueInitialized = true;
            continue;
        }
        const float alpha =
            1.0f - std::exp(
                -(std::max)(dt, 0.0f) / (std::max)(tree.dampTime, 1e-4f));
        tree.dampedValue += (target - tree.dampedValue) * std::clamp(alpha, 0.0f, 1.0f);
        // 指数補間は理論上目標へ到達しないため、近傍で確定して不要な2Clip評価を終了する。
        // WHY: 極小WeightのClipも全ボーンをサンプリングすると、定常時のCPU負荷が倍増する。
        const float snapEpsilon =
            (std::max)(0.001f, std::abs(target) * 0.001f);
        if (std::abs(target - tree.dampedValue) <= snapEpsilon)
            tree.dampedValue = target;
    }
}

void UpdateBlendTree1DDamping(AnimatorComponent& animator, float dt)
{
    UpdateBlendTree1DDampingIn(animator, animator.states, dt);
}

// 現在のBlend Weightからサイクル周波数を補間し、共通の正規化位相を積分する。
// WHY: 最長Clipへ全Motionを引き伸ばす位相同期ではWalk本来の再生速度が失われるため、
//      純粋なMotionでは元速度、ブレンド中は両者の中間テンポになるよう周波数を合成する。
void AdvanceBlendTreePhase(AnimatorComponent& animator,
                           AnimationState& state,
                           float stateTime,
                           float dt)
{
    if (!animator.playing || state.mode != AnimationStateMode::BlendTree1D ||
        !state.blendTree1D.syncNormalizedTime) return;

    float cycleFrequency = 0.0f;
    float validWeight = 0.0f;
    for (const auto& weighted : ComputeStateWeights(animator, state)) {
        const asset::AnimationClip* clip =
            FindClipForMotion(animator, *weighted.motion);
        if (!clip) continue;
        const float duration = static_cast<float>(clip->GetDurationSeconds());
        if (duration <= math::EPSILON) continue;
        cycleFrequency += weighted.weight * weighted.motion->speed / duration;
        validWeight += weighted.weight;
    }
    if (validWeight <= math::EPSILON) return;
    cycleFrequency /= validWeight;

    auto& tree = state.blendTree1D;
    if (!tree.normalizedPhaseInitialized) {
        tree.normalizedPhase = state.loop
            ? stateTime * cycleFrequency -
                std::floor(stateTime * cycleFrequency)
            : math::Clamp01(stateTime * cycleFrequency);
        tree.normalizedPhaseInitialized = true;
        return;
    }

    tree.normalizedPhase += dt * state.speed * animator.speed * cycleFrequency;
    if (state.loop) {
        tree.normalizedPhase -= std::floor(tree.normalizedPhase);
    } else {
        tree.normalizedPhase = math::Clamp01(tree.normalizedPhase);
    }
}

// ステートマシンを1フレーム分更新する。
// 遷移中のブレンド進行 → stateTime 進行 → 遷移条件チェックの順で処理する。
void UpdateStateMachineScoped(AnimatorComponent& animator, StateMachineScope scope, float dt)
{
    // ── ブレンド進行 ───────────────────────────────────────────────────────
    if (!scope.blendToState.empty()) {
        if (!animator.playing) return;

        scope.blendWeight += dt / (std::max)(scope.blendDuration, 1e-4f);

        // クロスフェード中も遷移元のポーズを進め、静止ポーズへのフェードを防ぐ。
        const AnimationState* currentSt =
            FindStateIn(scope.states, scope.currentStateName);
        if (currentSt) {
            const float dur = GetStateDuration(animator, *currentSt);
            if (dur > 0.0f) {
                scope.stateTime += dt * currentSt->speed * animator.speed;
                scope.stateTime = currentSt->loop
                    ? WrapTime(scope.stateTime, dur)
                    : std::clamp(scope.stateTime, 0.0f, dur);
            }
        }

        // 遷移先ステートの時刻も進める
        const AnimationState* nextSt = FindStateIn(scope.states, scope.blendToState);
        if (nextSt) {
            const float dur = GetStateDuration(animator, *nextSt);
            if (dur > 0.0f) {
                scope.blendToTime += dt * nextSt->speed * animator.speed;
                if (nextSt->loop)
                    scope.blendToTime = WrapTime(scope.blendToTime, dur);
                else
                    scope.blendToTime = std::clamp(scope.blendToTime, 0.0f, dur);
            }
        }

        if (scope.blendWeight >= 1.0f) {
            // 遷移完了: 現ステートを次ステートに切り替える
            scope.currentStateName = scope.blendToState;
            scope.stateTime        = scope.blendToTime;
            scope.blendToState     = "";
            scope.blendWeight      = 0.0f;
        }
        // 遷移中は新たな遷移チェックをしない
        return;
    }

    // ── 現ステートの時刻進行 ──────────────────────────────────────────────
    const AnimationState* curSt = FindStateIn(scope.states, scope.currentStateName);
    if (!curSt) return;

    const float duration = GetStateDuration(animator, *curSt);
    if (duration > 0.0f) {
        if (animator.playing) {
            scope.stateTime += dt * curSt->speed * animator.speed;
            if (curSt->loop)
                scope.stateTime = WrapTime(scope.stateTime, duration);
            else
                scope.stateTime = std::clamp(scope.stateTime, 0.0f, duration);
        }
    }

    // ── 遷移条件チェック ──────────────────────────────────────────────────
    const float normalizedTime = (duration > 0.0f)
        ? std::clamp(scope.stateTime / duration, 0.0f, 1.0f)
        : 0.0f;

    // 通常遷移を優先し、成立しなかった場合だけ AnyState を評価する。
    if (!TryStartTransitionScoped(animator, scope, curSt->transitions, normalizedTime))
        TryStartTransitionScoped(animator, scope, scope.anyStateTransitions, normalizedTime);
}

void UpdateStateMachine(AnimatorComponent& animator, float dt)
{
    UpdateStateMachineScoped(animator, BaseScope(animator), dt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Root Motion の合成と適用
// ─────────────────────────────────────────────────────────────────────────────

// 加重クリップ集合からルートモーションを合成する。
// 位置は weight で線形加重、回転は姿勢ブレンドと同じ逐次 Slerp で合成する。
// WHY: Walk 0.6 / Run 0.4 のとき、ポーズは 6:4 で混ざっているのに移動量だけ
//      Walk 100% では足が滑る。ポーズと同じ比率で移動量も混ぜる。
RootMotionDelta AccumulateRootMotion(AnimatorComponent& animator,
                                     const std::vector<WeightedClip>& clips,
                                     std::uint64_t frame)
{
    RootMotionDelta total;
    float accumulatedRotationWeight = 0.0f;
    bool hasRotation = false;

    for (const auto& weighted : clips) {
        if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
        const RootMotionDelta delta = SampleClipRootMotion(
            animator, *weighted.clip, weighted.rootMotion,
            weighted.ticks, weighted.reverse, frame);

        total.position += delta.position * weighted.weight;

        if (!hasRotation) {
            total.rotation = delta.rotation;
            accumulatedRotationWeight = weighted.weight;
            hasRotation = true;
        } else {
            const float sum = accumulatedRotationWeight + weighted.weight;
            const float t = sum > math::EPSILON ? weighted.weight / sum : 0.0f;
            total.rotation =
                math::Quaternion::Slerp(total.rotation, delta.rotation, t).Normalized();
            accumulatedRotationWeight = sum;
        }
    }
    if (!hasRotation) total.rotation = math::Quaternion::Identity();
    return total;
}

// ルートモーションの適用先 GameObject を解決する。
// 空文字列なら Animator 自身。".." で親を遡り、それ以外は owner からの子孫パス。
// WHY: Animator がメッシュ側の子 GameObject に付いていると、旧実装はその子だけを
//      動かしてしまい、コライダを持つ親から見た目がずれていった。
GameObject& ResolveRootMotionTarget(GameObject& owner, const std::string& path)
{
    if (path.empty()) return owner;

    GameObject* current = &owner;
    size_t begin = 0;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        const std::string part = path.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = end == std::string::npos ? path.size() : end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (GameObject* parent = current->GetParent()) current = parent;
            continue;
        }
        GameObject* next = nullptr;
        for (int i = 0; i < current->GetChildCount(); ++i) {
            GameObject* child = current->GetChild(i);
            if (child && child->name == part) { next = child; break; }
        }
        if (!next) return owner; // 解決できないパスは自身へフォールバックする
        current = next;
    }
    return *current;
}

// target から owner までの親子チェーンのワールド Transform を更新する。
//
// WHY: AnimatorSystem は TransformLateUpdate の後に走るため、ここで Transform を
//      書き換えても worldPosition は前の値のまま残る。旧実装はそれを放置していたので、
//      ルートモーションで進んだ分だけ「見た目 (ボーンのワールド行列) が 1 フレーム遅れる」
//      状態になっていた。ボーン伝播の前にチェーンだけ作り直す。
void RefreshWorldChain(GameObject& target, GameObject& owner)
{
    std::vector<GameObject*> chain;
    for (GameObject* go = &owner;; go = go->GetParent()) {
        chain.push_back(go);
        if (go == &target) break;
        if (!go->GetParent()) {
            // target が owner の祖先ではない (別枝)。target 単体だけ更新する。
            chain.assign(1, &target);
            break;
        }
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        GameObject* go = *it;
        if (GameObject* parent = go->GetParent())
            UpdateWorldTransform(*go, parent->transform);
        else {
            go->transform.worldPosition = go->transform.position;
            go->transform.worldRotation = go->transform.rotation;
            go->transform.worldScale = go->transform.scale;
        }
        PropagateNonBoneChildTransforms(*go);
    }
}

// 合成済みの delta を Animator の出力へ書き、mode に応じて適用する。
// ポーズ評価より前に呼ぶこと (適用後のワールド行列でボーンを伝播させるため)。
void ApplyRootMotionResult(AnimatorComponent& animator,
                           GameObject& owner,
                           const RootMotionDelta& rawDelta,
                           float dt)
{
    const RootMotionSettings& settings = animator.rootMotion;

    animator.rootMotionDeltaPosition = math::Vector3::ZERO;
    animator.rootMotionDeltaRotation = math::Quaternion::Identity();
    animator.rootMotionWorldDelta = math::Vector3::ZERO;
    animator.rootMotionWorldVelocity = math::Vector3::ZERO;
    animator.rootMotionDeltaTime = dt;
    animator.rootMotionAppliedByEngine = false;
    if (!settings.Extracts()) return;

    GameObject& target = ResolveRootMotionTarget(owner, settings.targetPath);

    const math::Vector3 localDelta = rawDelta.position * settings.positionScale;
    const math::Quaternion localRotation = math::Quaternion::Slerp(
        math::Quaternion::Identity(), rawDelta.rotation,
        std::clamp(settings.rotationScale, 0.0f, 1.0f)).Normalized();

    // ワールド空間の移動量。適用前の worldRotation を基準にする。
    const math::Vector3 worldDelta = target.transform.worldRotation * localDelta;

    animator.rootMotionDeltaPosition = localDelta;
    animator.rootMotionDeltaRotation = localRotation;
    animator.rootMotionWorldDelta = worldDelta;
    animator.rootMotionWorldVelocity =
        dt > math::EPSILON ? worldDelta / dt : math::Vector3::ZERO;

    switch (settings.mode) {
    case RootMotionMode::ApplyToTransform:
        target.transform.position += target.transform.rotation * localDelta;
        target.transform.rotation =
            (target.transform.rotation * localRotation).Normalized();
        RefreshWorldChain(target, owner);
        animator.rootMotionAppliedByEngine = true;
        break;

    case RootMotionMode::ApplyToRigidBody: {
        // 水平成分だけ速度へ渡し、鉛直成分は重力と衝突解決に任せる。
        // WHY: Transform 直書きは PhysicsSystem の SetPosition() でテレポートになり、
        //      スイープも押し戻しも無いまま壁を抜ける。速度として渡せばソルバーが解く。
        auto* rb = target.GetComponent<RigidBodyComponent>();
        if (rb && rb->enabled && rb->rigidBody && dt > math::EPSILON) {
            math::Vector3 velocity = rb->rigidBody->GetVelocity();
            velocity.x = worldDelta.x / dt;
            velocity.z = worldDelta.z / dt;
            // Y はクリップが明示的に指定しているときだけ上書きする。
            if (std::fabs(worldDelta.y) > math::EPSILON)
                velocity.y = worldDelta.y / dt;
            rb->rigidBody->SetVelocity(velocity);
            animator.rootMotionAppliedByEngine = true;
        } else {
            // RigidBody が無い / dt が 0 のフレームは Transform へフォールバックする。
            // WHY: 何もしないと「設定ミスで移動だけ消える」不可解な挙動になる。
            target.transform.position += target.transform.rotation * localDelta;
            animator.rootMotionAppliedByEngine = true;
        }
        // 回転は物理ボディに任せず Transform 側で回す。次フレームの
        // PhysicsSystem::SyncRigidBodies が worldRotation をボディへ書き戻す。
        target.transform.rotation =
            (target.transform.rotation * localRotation).Normalized();
        RefreshWorldChain(target, owner);
        break;
    }

    case RootMotionMode::ExtractOnly:
        // エンジンは何も動かさない。移動の適用は OnAnimatorMove を受けた Script の責任。
        break;

    case RootMotionMode::None:
    default:
        break;
    }
}

// 抽出した結果を同一フレーム内で Script へ通知する。
// WHY: ScriptAnimatorProxy のポーリングだけだと、AnimatorSystem が Phase::LateUpdate に
//      居るため OnUpdate は常に 1 フレーム前の delta を読むことになる。
void DispatchAnimatorMove(GameObject& owner, const AnimatorComponent& animator)
{
    if (!animator.rootMotion.Extracts()) return;
    ScriptComponent* scripts = owner.GetComponent<ScriptComponent>();
    if (!scripts) return;

    RootMotionInfo info{};
    info.deltaPosition = animator.rootMotionDeltaPosition;
    info.deltaRotation = animator.rootMotionDeltaRotation;
    info.worldDeltaPosition = animator.rootMotionWorldDelta;
    info.worldVelocity = animator.rootMotionWorldVelocity;
    info.deltaTime = animator.rootMotionDeltaTime;
    info.appliedByEngine = animator.rootMotionAppliedByEngine;

    for (auto& entry : scripts->scripts)
        if (entry.script && entry.script->enabled)
            entry.script->InvokeAnimatorMove(info);
}

// 加重クリップ集合からルートモーションを抽出し、適用して Script へ通知するまでを行う。
// ポーズ評価より前に呼ぶ。
void ProcessRootMotion(AnimatorComponent& animator,
                       GameObject& owner,
                       const std::vector<WeightedClip>& clips,
                       float dt)
{
    const std::uint64_t frame = Time::frameCount;
    const RootMotionDelta delta = AccumulateRootMotion(animator, clips, frame);
    PruneRootMotionSamples(animator, frame);
    ApplyRootMotionResult(animator, owner, delta, dt);
    DispatchAnimatorMove(owner, animator);
}

} // namespace

// ── 後方互換パス（states が空のとき）────────────────────────────────────────
// ── ステートマシンパス（states が存在するとき）──────────────────────────────
static const asset::AnimationClip* ResolveStateMachineEffectClip(AnimatorComponent& animator)
{
    const AnimationState* state = FindState(animator, animator.currentStateName);
    if (!state) return nullptr;
    auto clips = BuildStateClips(animator, *state, animator.stateTime);
    const auto best = std::max_element(clips.begin(), clips.end(),
        [](const WeightedClip& a, const WeightedClip& b) { return a.weight < b.weight; });
    return best != clips.end() ? best->clip : nullptr;
}

static const asset::AnimationClip* AdvanceStateMachineAnimator(AnimatorComponent& animator, float dt)
{
    InitStateMachine(animator);
    UpdateBlendTree1DDamping(animator, dt);
    UpdateStateMachine(animator, dt);
    if (auto* state = FindMutableState(animator, animator.currentStateName))
        AdvanceBlendTreePhase(animator, *state, animator.stateTime, dt);
    return ResolveStateMachineEffectClip(animator);
}

static void ApplyClipSideEffects(GameObject& owner,
                                 AnimatorComponent& animator,
                                 const asset::AnimationClip& clip,
                                 SkinnedMeshRenderer* smr,
                                 float previousTime,
                                  float currentTime)
{
    const double ticksPerSecond = clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0;
    const double ticks = static_cast<double>(currentTime) * ticksPerSecond;
    // トラック適用はクリップのトラック数ぶん階層探索を回すため、単独で計測する。
    {
        FBZZ_PROFILE_SCOPE("AnimatorSystem::ApplyTransformTracks");
        ApplyTransformTracks(owner, clip, ticks);
    }
    {
        FBZZ_PROFILE_SCOPE("AnimatorSystem::ApplyPropertyTracks");
        ApplyPropertyTracks(owner, clip, ticks, smr);
    }
    const bool reverse = animator.speed < 0.0f;
    const bool looped = reverse ? currentTime > previousTime : currentTime < previousTime;
    // Event の区間判定だけは「同じクリップを続けて再生している」ことが前提になる。
    // ルートモーションはクリップ単位のサンプル差分へ移したため、この分岐から外れた。
    if (animator.previousEventClipName == clip.name)
        DispatchAnimationEvents(owner, animator, clip, previousTime, currentTime, looped, reverse);
    animator.previousEventTime = currentTime;
    animator.previousEventClipName = clip.name;
}

// ── アニメーションレイヤー ────────────────────────────────────────────────────
// 上半身 / 下半身の出し分けはここが実装本体。Base Layer が全身ポーズを作った後に、
// レイヤーごとの姿勢をボーン単位のウェイトで上書き (Override) / 加算 (Additive) する。

// .mask アセットを必要なときだけ読み込む。Base Layer と各 AnimationLayer で共用する。
// WHY: 毎フレーム TOML をパースすると数十体のキャラで即破綻する。パスが変わったときだけ読み直す。
static void EnsureMaskLoaded(AnimationMaskRef& ref)
{
    if (ref.path.empty()) {
        if (ref.loaded) {
            ref.loaded = false;
            ref.asset = asset::AvatarMaskAsset{};
            ref.loadedPath.clear();
        }
        return;
    }
    if (ref.loaded && ref.loadedPath == ref.path) return;

    ref.loadedPath = ref.path;
    ref.asset      = asset::AvatarMaskAsset{};
    const std::string resolved = asset::AssetManager::ResolveAssetPath(ref.path);
    ref.loaded = asset::LoadAvatarMaskAsset(resolved, ref.asset);
    if (!ref.loaded)
        FBZZ_LOG_WARN("AnimatorSystem: avatar mask load failed [%s]", ref.path.c_str());
}

// このレイヤーが対象ボーンへ効く割合 0..1 を返す。
// .mask アセットが無ければ全身に適用する。
// WHY: 0/1 の二値だと、上半身レイヤーの境界ボーン (Spine 等) でポーズが折れる。
//      .mask の blendDepth により数階層かけて立ち上げられるようにした。
static float LayerBoneWeight(const AnimationLayer& layer,
                             const std::string& path,
                             const std::string& nodeName)
{
    if (layer.mask.loaded)
        return asset::EvaluateAvatarMaskWeight(layer.mask.asset, path, nodeName);

    (void)path;
    (void)nodeName;
    return 1.0f;
}

// 加算レイヤーの基準ポーズを解決する。
// additiveReference が未設定なら nullptr を返し、呼び出し側は従来どおり
// 「加算クリップ自身の先頭キー」を基準に使う。
static const asset::AnimationClip* ResolveAdditiveReferenceClip(
    const AnimatorComponent& animator, const AnimationLayer& layer)
{
    if (layer.additiveReference.sourcePath.empty() &&
        layer.additiveReference.clipName.empty()) return nullptr;
    if (const auto* clip = FindClipBySource(animator,
            layer.additiveReference.sourcePath, layer.additiveReference.clipName))
        return clip;
    if (!layer.additiveReference.clipName.empty())
        return FindClipByName(animator, layer.additiveReference.clipName);
    return nullptr;
}

// 基準クリップから、対象ボーンの基準 TRS を取り出す。
// referenceClip が無い場合は track 自身の先頭キーを基準にする (従来動作)。
static void ResolveAdditiveReferencePose(const asset::AnimationClip* referenceClip,
                                         float referenceTime,
                                         const asset::NodeAnimationTrack& track,
                                         math::Vector3& outPosition,
                                         math::Quaternion& outRotation,
                                         math::Vector3& outScale)
{
    if (referenceClip) {
        const double tps = referenceClip->ticksPerSecond > 0.0
            ? referenceClip->ticksPerSecond : 30.0;
        const double ticks = static_cast<double>(referenceTime) * tps;
        for (const auto& refTrack : referenceClip->tracks) {
            const bool sameTarget =
                (!track.targetPath.empty() && refTrack.targetPath == track.targetPath) ||
                (!track.nodeName.empty()   && refTrack.nodeName   == track.nodeName);
            if (!sameTarget) continue;
            outPosition = SampleVectorKeys(
                refTrack.positions, ticks, math::Vector3::ZERO, refTrack.interp);
            outRotation = SampleQuaternionKeys(
                refTrack.rotations, ticks, math::Quaternion::Identity(), refTrack.interp);
            outScale = SampleVectorKeys(
                refTrack.scales, ticks, math::Vector3::ONE, refTrack.interp);
            return;
        }
    }
    // フォールバック: 加算クリップ自身の先頭キー。
    outPosition = track.positions.empty() ? math::Vector3::ZERO : track.positions.front().value;
    outRotation = track.rotations.empty() ? math::Quaternion::Identity() : track.rotations.front().value;
    outScale    = track.scales.empty()    ? math::Vector3::ONE : track.scales.front().value;
}

// Slot (ワンショット差し込み) のフェードと再生位置を進める。
// WHY: 「移動を流したまま上半身に攻撃を割り込ませ、終わったら自動で戻る」を
//      ステートマシンへ専用ステートと復帰遷移を足さずに成立させる。
static const asset::AnimationClip* UpdateLayerSlot(
    AnimatorComponent& animator, AnimationLayer& layer, float dt)
{
    AnimationSlotPlayback& slot = layer.slot;
    if (!slot.active) { slot.weight = 0.0f; return nullptr; }

    const asset::AnimationClip* clip =
        FindClipBySource(animator, slot.sourcePath, slot.clipName);
    if (!clip && !slot.clipName.empty()) clip = FindClipByName(animator, slot.clipName);
    if (!clip) {
        // 参照が解決できない Slot は鳴らしっぱなしにせず畳む。
        slot.active = false;
        slot.weight = 0.0f;
        return nullptr;
    }

    const float duration = static_cast<float>(clip->GetDurationSeconds());
    if (animator.playing) {
        slot.time += dt * slot.speed * animator.speed;
        if (slot.loop && duration > 0.0f) {
            slot.time = WrapTime(slot.time, duration);
        } else if (duration > 0.0f) {
            slot.time = std::clamp(slot.time, 0.0f, duration);
            // 末尾に到達したら自動でフェードアウトへ移す。
            // WHY: ワンショットは「終わったら戻る」まで含めて 1 操作であってほしい。
            const float fadeOutStart = (std::max)(duration - slot.fadeOutDuration, 0.0f);
            if (slot.time >= fadeOutStart) slot.stopping = true;
        }
    }

    // フェード進行。stopping なら 0 へ、そうでなければ 1 へ向かう。
    const float fadeDuration = slot.stopping ? slot.fadeOutDuration : slot.fadeInDuration;
    const float step = fadeDuration > math::EPSILON ? dt / fadeDuration : 1.0f;
    slot.weight += slot.stopping ? -step : step;
    slot.weight = std::clamp(slot.weight, 0.0f, 1.0f);

    if (slot.stopping && slot.weight <= math::EPSILON) {
        slot.active = false;
        slot.weight = 0.0f;
        return nullptr;
    }
    return clip;
}

// レイヤー内で 1 ボーンぶんのポーズを重み付き平均で積む作業バッファ。
// WHY: 従来は clip ごとに target->transform へ順次 Lerp していたため、
//      3 つ以上の Motion をブレンドすると後ろの Clip ほど強く出る偏りがあった。
//      「累積ウェイトに対する比率」で積めば、順序に依存しない正しい加重平均になる。
struct LayerBonePose {
    GameObject*      target       = nullptr;
    math::Vector3    position     = math::Vector3::ZERO;
    math::Quaternion rotation     = math::Quaternion::Identity();
    math::Vector3    scale        = math::Vector3::ZERO;
    math::Vector3    deltaPosition= math::Vector3::ZERO;
    math::Quaternion deltaRotation= math::Quaternion::Identity();
    math::Vector3    deltaScale   = math::Vector3::ZERO;
    float            accumWeight  = 0.0f;
    bool             hasRotation  = false;
    float            boneWeight   = 1.0f;
};

// 1 レイヤーぶんのクリップ集合を評価して、ボーンごとのポーズを積む。
static void AccumulateLayerClips(AnimatorComponent& animator,
                                 AnimationLayer& layer,
                                 GameObject& owner,
                                 SkinnedMeshRenderer& smr,
                                 const std::vector<WeightedClip>& clips,
                                 float clipSetWeight,
                                 const asset::AnimationClip* additiveReferenceClip,
                                 std::vector<LayerBonePose>& poses)
{
    if (clipSetWeight <= math::EPSILON) return;

    for (const auto& weighted : clips) {
        if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
        const float clipWeight = weighted.weight * clipSetWeight;

        for (const auto& track : weighted.clip->tracks) {
            std::string targetPath = track.targetPath;
            std::string targetName = track.nodeName;
            const RetargetBoneMapping* retarget = nullptr;
            for (const auto& mapping : layer.retargetMappings) {
                if (mapping.sourcePath == targetPath || mapping.sourcePath == targetName) {
                    retarget = &mapping;
                    targetPath = mapping.targetPath;
                    const size_t slash = targetPath.find_last_of('/');
                    targetName = slash == std::string::npos
                        ? targetPath : targetPath.substr(slash + 1);
                    break;
                }
            }

            const float boneWeight = LayerBoneWeight(layer, targetPath, targetName);
            if (boneWeight <= math::EPSILON) continue;

            if (targetPath.empty()) continue;
            GameObject* target = FindAnimationTarget(owner, targetPath);
            if (!target || !target->GetComponent<BoneComponent>()) continue;

            math::Vector3 sampledPosition = SampleVectorKeys(
                track.positions, weighted.ticks, target->transform.position, track.interp);
            math::Quaternion sampledRotation = SampleQuaternionKeys(
                track.rotations, weighted.ticks, target->transform.rotation, track.interp);
            math::Vector3 sampledScale = SampleVectorKeys(
                track.scales, weighted.ticks, target->transform.scale, track.interp);
            if (retarget) {
                sampledPosition = sampledPosition * retarget->translationScale;
                sampledRotation = (retarget->rotationOffset * sampledRotation).Normalized();
            }

            // 同じボーンの既存エントリを探す (レイヤー内のクリップ数は多くて数本)。
            LayerBonePose* pose = nullptr;
            for (auto& p : poses)
                if (p.target == target) { pose = &p; break; }
            if (!pose) {
                poses.push_back(LayerBonePose{});
                pose = &poses.back();
                pose->target = target;
                pose->boneWeight = boneWeight;
            }

            if (layer.mode == AnimationLayerMode::Override) {
                // 累積ウェイトに対する比率で積み、順序に依存しない加重平均にする。
                const float total = pose->accumWeight + clipWeight;
                const float t = total > math::EPSILON ? clipWeight / total : 0.0f;
                pose->position = math::Vector3::Lerp(pose->position, sampledPosition, t);
                pose->scale    = math::Vector3::Lerp(pose->scale, sampledScale, t);
                if (!pose->hasRotation) {
                    pose->rotation = sampledRotation;
                    pose->hasRotation = true;
                } else {
                    pose->rotation =
                        math::Quaternion::Slerp(pose->rotation, sampledRotation, t).Normalized();
                }
                pose->accumWeight = total;
            } else {
                math::Vector3    refPosition;
                math::Quaternion refRotation;
                math::Vector3    refScale;
                ResolveAdditiveReferencePose(
                    additiveReferenceClip, layer.additiveReference.time, track,
                    refPosition, refRotation, refScale);

                pose->deltaPosition += (sampledPosition - refPosition) * clipWeight;
                pose->deltaScale    += (sampledScale - refScale) * clipWeight;
                const math::Quaternion rotationDelta =
                    refRotation.Inverse() * sampledRotation;
                pose->deltaRotation = (pose->deltaRotation * math::Quaternion::Slerp(
                    math::Quaternion::Identity(), rotationDelta, clipWeight)).Normalized();
                pose->accumWeight += clipWeight;
                pose->hasRotation = true;
            }
        }
        ApplyPropertyTracks(owner, *weighted.clip, weighted.ticks, &smr);
    }
}

// レイヤーのステートマシンを進め、評価対象クリップを返す。
// クロスフェード中は遷移元と遷移先を blendWeight で混ぜたリストになる。
static std::vector<WeightedClip> BuildLayerStateClips(
    AnimatorComponent& animator, AnimationLayer& layer, float dt)
{
    std::vector<WeightedClip> result;

    if (layer.states.empty()) return result;

    // 新形式: レイヤー専用のステートマシンを 1 フレーム進める。
    StateMachineScope scope = LayerScope(layer);
    InitStateMachineScoped(scope);
    UpdateBlendTree1DDampingIn(animator, layer.states, dt);
    UpdateStateMachineScoped(animator, scope, dt);

    if (auto* currentState = FindMutableStateIn(layer.states, scope.currentStateName))
        AdvanceBlendTreePhase(animator, *currentState, scope.stateTime, dt);
    if (!scope.blendToState.empty()) {
        if (auto* nextState = FindMutableStateIn(layer.states, scope.blendToState))
            AdvanceBlendTreePhase(animator, *nextState, scope.blendToTime, dt);
    }

    const AnimationState* curSt = FindStateIn(layer.states, scope.currentStateName);
    if (!curSt) return result;

    result = BuildStateClips(animator, *curSt, scope.stateTime);
    if (scope.blendToState.empty()) return result;

    // クロスフェード: 遷移元を (1-w)、遷移先を w で混ぜる。
    const float w = std::clamp(scope.blendWeight, 0.0f, 1.0f);
    for (auto& clip : result) clip.weight *= (1.0f - w);
    if (const AnimationState* nextSt = FindStateIn(layer.states, scope.blendToState)) {
        for (auto clip : BuildStateClips(animator, *nextSt, scope.blendToTime)) {
            clip.weight *= w;
            result.push_back(clip);
        }
    }
    return result;
}

static void ApplyAnimationLayers(AnimatorComponent& animator,
                                 const asset::Skeleton& skeleton,
                                 Scene& scene,
                                 GameObject& owner,
                                 SkinnedMeshRenderer& smr,
                                 float dt)
{
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        smr.nodeEntities.size() < skeleton.nodes.size() ||
        animator.nodeGlobalTransforms.size() < skeleton.nodes.size())
        return;

    bool poseChanged = false;
    std::vector<LayerBonePose> poses;

    for (auto& layer : animator.layers) {
        if (!layer.enabled) continue;
        // Layer を追加した直後はステートも Slot も無い。空 Layer で Mask のロードや
        // Slot の更新まで行うと、まだ何も接続していない編集操作が再生経路へ副作用を
        // 持ち込むため、定義と一時再生の両方が空なら評価を完全に省略する。
        if (layer.states.empty() && !layer.slot.active) continue;
        EnsureMaskLoaded(layer.mask);

        // Slot はレイヤー weight が 0 でも時間を進める。
        // WHY: weight を 0 にして一時的に黙らせている間も、割り込みモーションの
        //      再生位置とフェードは進んでいてほしい (復帰時に途中から鳴る)。
        const asset::AnimationClip* slotClip = UpdateLayerSlot(animator, layer, dt);
        const float slotWeight = slotClip ? layer.slot.weight : 0.0f;

        auto stateClips = BuildLayerStateClips(animator, layer, dt);
        if (layer.weight <= math::EPSILON) continue;
        if (stateClips.empty() && !slotClip) continue;

        const asset::AnimationClip* additiveReference =
            layer.mode == AnimationLayerMode::Additive
                ? ResolveAdditiveReferenceClip(animator, layer)
                : nullptr;

        poses.clear();
        // ステートマシン出力は Slot に押しのけられる分だけ弱める。
        AccumulateLayerClips(animator, layer, owner, smr, stateClips,
                             1.0f - slotWeight, additiveReference, poses);

        if (slotClip && slotWeight > math::EPSILON) {
            const double tps = slotClip->ticksPerSecond > 0.0 ? slotClip->ticksPerSecond : 30.0;
            std::vector<WeightedClip> slotClips;
            // 割り込みクリップも Base と同じルートモーション設定でポーズを扱う。
            // WHY: Base だけルート成分を抜き、Slot は抜かないと、Slot が乗った瞬間に
            //      キャラクターがルートごと飛ぶ。
            slotClips.push_back(WeightedClip{
                slotClip, nullptr, static_cast<double>(layer.slot.time) * tps, 1.0f, 1.0f,
                ResolveRootMotion(animator, *slotClip), animator.speed < 0.0f });
            AccumulateLayerClips(animator, layer, owner, smr, slotClips,
                                 slotWeight, additiveReference, poses);
        }

        // 積んだポーズを、レイヤー weight × ボーン weight で実際の Transform へ適用する。
        for (const auto& pose : poses) {
            if (!pose.target || pose.accumWeight <= math::EPSILON) continue;
            const float alpha = std::clamp(layer.weight * pose.boneWeight, 0.0f, 1.0f);
            if (alpha <= math::EPSILON) continue;

            if (layer.mode == AnimationLayerMode::Override) {
                pose.target->transform.position = math::Vector3::Lerp(
                    pose.target->transform.position, pose.position, alpha);
                if (pose.hasRotation) {
                    pose.target->transform.rotation = math::Quaternion::Slerp(
                        pose.target->transform.rotation, pose.rotation, alpha).Normalized();
                }
                pose.target->transform.scale = math::Vector3::Lerp(
                    pose.target->transform.scale, pose.scale, alpha);
            } else {
                pose.target->transform.position += pose.deltaPosition * alpha;
                pose.target->transform.rotation =
                    (pose.target->transform.rotation * math::Quaternion::Slerp(
                        math::Quaternion::Identity(), pose.deltaRotation, alpha)).Normalized();
                pose.target->transform.scale += pose.deltaScale * alpha;
            }
            poseChanged = true;
        }
    }

    if (poseChanged) {
        std::vector<uint8_t> visited(skeleton.nodes.size(), 0);
        PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootNodeIndex,
                                owner.transform, visited);
        RebuildSkinningFromBoneTransforms(scene, owner, skeleton, smr, animator);
    }
}

static void RunStateMachineAnimatorPath(AnimatorComponent& animator,
                                        const asset::Skeleton& skeleton,
                                        Scene& scene,
                                        GameObject& go,
                                        SkinnedMeshRenderer& smr,
                                        renderer::ResourceManager& resources,
                                        float dt)
{
    InitStateMachine(animator);
    UpdateBlendTree1DDamping(animator, dt);
    UpdateStateMachine(animator, dt);
    if (auto* currentState = FindMutableState(animator, animator.currentStateName))
        AdvanceBlendTreePhase(animator, *currentState, animator.stateTime, dt);
    if (!animator.blendToState.empty()) {
        if (auto* nextState = FindMutableState(animator, animator.blendToState))
            AdvanceBlendTreePhase(animator, *nextState, animator.blendToTime, dt);
    }

    const AnimationState* curSt = FindState(animator, animator.currentStateName);
    animator.currentBlendWeights.clear();
    animator.currentBlendDuration = 0.0f;

    // クリップ未設定でも後段の Layer / IK が参照できるよう、スケルトンの配列と
    // ボーン階層は早期 return より前に必ず準備する。
    const size_t boneCount = (std::min)(skeleton.bones.size(),
                                        static_cast<size_t>(asset::MAX_SKINNING_BONES));
    animator.boneMatrices.assign(boneCount, math::Matrix4::Identity());
    animator.nodeGlobalTransforms.assign(skeleton.nodes.size(), math::Matrix4::Identity());
    if (skeleton.rootNodeIndex < 0 ||
        skeleton.rootNodeIndex >= static_cast<int>(skeleton.nodes.size())) {
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }
    EnsureBoneHierarchy(scene, go, smr, skeleton);

    // 評価できるクリップが無いフレームは移動量ゼロを公開する。
    // WHY: 前フレームの delta が残ると、ExtractOnly の Script が止まった値で動き続ける。
    if (!curSt) {
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    auto currentClips = BuildStateClips(animator, *curSt, animator.stateTime);
    if (currentClips.empty()) {
        UploadBindPose(animator, resources, &skeleton);
        ProcessRootMotion(animator, go, {}, dt);
        return;
    }

    animator.currentBlendDuration = GetStateDuration(animator, *curSt);
    bool exposeBlendWeights = curSt->mode != AnimationStateMode::Clip;

    // Clip / BlendTree を共通の加重クリップ集合として評価する。

    if (!animator.blendToState.empty()) {
        // ── クロスフェードモード ─────────────────────────────────────────
        const AnimationState* nextSt = FindState(animator, animator.blendToState);
        // Clip 同士の遷移も含め、最終姿勢への実寄与率を Editor へ公開する。
        exposeBlendWeights = true;
        auto nextClips = nextSt
            ? BuildStateClips(animator, *nextSt, animator.blendToTime)
            : std::vector<WeightedClip>{};

        if (!nextClips.empty()) {
            const float w = std::clamp(animator.blendWeight, 0.0f, 1.0f);
            for (auto& clip : currentClips) clip.weight *= 1.0f - w;
            for (auto& clip : nextClips) clip.weight *= w;
            currentClips.insert(currentClips.end(), nextClips.begin(), nextClips.end());
        } else {
            // 遷移先クリップが見つからなければ現クリップ単独で続ける
            animator.blendToState.clear();
            animator.blendWeight = 0.0f;
        }
    } else {
        // ── 単一クリップモード ───────────────────────────────────────────
        // 遷移していない場合は currentClips をそのまま評価する。
    }

    // デバッグ API には、クロスフェードを含む最終姿勢への実寄与率を公開する。
    // WHY: 遷移元・遷移先で同じクリップを使う場合は、別項目ではなく合算値が必要になる。
    if (exposeBlendWeights) {
        for (const auto& weighted : currentClips) {
            if (!weighted.clip || weighted.weight <= math::EPSILON) continue;
            const std::string& name =
                weighted.motion && !weighted.motion->clipName.empty()
                ? weighted.motion->clipName
                : weighted.clip->name;
            const auto existing = std::find_if(
                animator.currentBlendWeights.begin(),
                animator.currentBlendWeights.end(),
                [&name](const auto& entry) { return entry.first == name; });
            if (existing != animator.currentBlendWeights.end())
                existing->second += weighted.weight;
            else
                animator.currentBlendWeights.emplace_back(name, weighted.weight);
        }
    }

    animator.currentIKWeight = 0.0f;
    for (const auto& weighted : currentClips)
        animator.currentIKWeight += weighted.ikWeight * weighted.weight;
    animator.currentIKWeight = std::clamp(animator.currentIKWeight, 0.0f, 1.0f);

    // ルートモーションはポーズ評価より前に確定・適用する。
    // 遷移中は currentClips に遷移元・遷移先の両方が weight 付きで入っているため、
    // 移動量も同じ比率で混ざる (旧実装は支配クリップ 1 本しか見ていなかった)。
    ProcessRootMotion(animator, go, currentClips, dt);

    std::vector<uint8_t> evaluationVisited(skeleton.nodes.size(), 0);
    EvaluateNBlendedNodeRecursive(
        skeleton, currentClips, skeleton.rootNodeIndex,
        math::Matrix4::Identity(),
        animator.boneMatrices, animator.nodeGlobalTransforms, evaluationVisited);
    // Base Layer マスク: 未設定なら nullptr を渡し、従来どおり全ボーンへ適用する。
    EnsureMaskLoaded(animator.baseLayerMask);
    ApplyNBlendedPoseToBones(scene, skeleton, currentClips, smr,
                             animator.baseLayerMask.loaded ? &animator.baseLayerMask.asset : nullptr);

    std::vector<uint8_t> propagationVisited(skeleton.nodes.size(), 0);
    PropagateBoneTransforms(scene, skeleton, smr, skeleton.rootNodeIndex,
                            go.transform, propagationVisited);
    RebuildSkinningFromBoneTransforms(scene, go, skeleton, smr, animator);
}

ComponentAccess AnimatorSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<AnimatorComponent>()
        .Writes<AnimatorComponent, BoneComponent>();
}

OrderingHints AnimatorSystem::GetOrder() const
{
    return OrderingHints{}.After<TransformLateUpdate>();
}

void AnimatorSystem::Update(SystemContext& ctx)
{
    // WHY: このファイルには計測スコープが 1 つも無かったため、アニメーション評価の
    //      コストが Profiler のどこにも現れず、フレーム時間の未帰属分に紛れていた。
    //      スキンドメッシュを出した瞬間に重くなる症状の切り分けに必要なので入れる。
    FBZZ_PROFILE_SCOPE("AnimatorSystem");
    if (!ctx.resources) return;
    Scene& scene = ctx.scene;
    renderer::ResourceManager& resources = *ctx.resources;
    const float dt = ctx.dt;
    const auto animatorSpan = scene.GetEntities<AnimatorComponent>();
    const auto animatorEntities = std::vector<EntityID>(
        animatorSpan.begin(),
        animatorSpan.end());

    for (EntityID id : animatorEntities) {
        GameObject* gameObject = scene.GetGameObject(id);
        if (!gameObject) continue;
        GameObject& go = *gameObject;

        auto* animator = go.GetComponent<AnimatorComponent>();
        if (!animator || !animator->enabled) continue;
        animator->firedEvents.clear();

        if (!animator->controllerPath.empty() &&
            animator->loadedControllerPath != animator->controllerPath) {
            asset::AnimatorControllerAsset controller;
            if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller)) {
                asset::ApplyAnimatorControllerAsset(controller, *animator);
            }
            animator->loadedControllerPath = animator->controllerPath;
        }

        // FlushFailed() が呼ばれて世代が進んだときだけ再試行する。
        // WHY: clips.empty() だけを条件にすると毎フレーム WARN スパムが発生する。
        //      世代番号で「FlushFailed() 以降に未試行」の場合のみ再試行を許可する。
        const bool needsRetry = !animator->clipsLoaded ||
            (animator->clips.empty() &&
             !animator->states.empty() &&
             asset::AssetManager::GetFlushGeneration() > animator->clipsAttemptGeneration);
        if (needsRetry)
            LoadClips(*animator);

        if (!animator->skinningBuffer.IsValid())
            animator->skinningBuffer = resources.CreateConstantBuffer(sizeof(SkinningCB));

        // SMR が自 GO になければ子 GO を探す (sub-mesh 分割ヒエラルキー対応)。
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr) {
            for (int ci = 0, cn = go.GetChildCount(); ci < cn; ++ci) {
                if (auto* child = go.GetChild(ci)) {
                    if (auto* s = child->GetComponent<SkinnedMeshRenderer>()) { smr = s; break; }
                }
            }
        }
        if (smr && !smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadModel(smr->modelPath);

        const asset::Skeleton* skeleton = nullptr;
        if (smr && smr->model && smr->model->skeleton)
            skeleton = smr->model->skeleton.get();

        // ルートモーションの AutoDetect が使うスケルトンルート名を焼いておく。
        // WHY: トラック解決は Skeleton を持たない BuildStateClips からも呼ばれる。
        animator->skeletonRootNodeName.clear();
        if (skeleton && skeleton->rootNodeIndex >= 0 &&
            static_cast<size_t>(skeleton->rootNodeIndex) < skeleton->nodes.size()) {
            animator->skeletonRootNodeName =
                skeleton->nodes[static_cast<size_t>(skeleton->rootNodeIndex)].name;
        }

        // ── 再生は Play Mode のみ ──────────────────────────────────────────
        // WHY: エディタ停止中にクリップが進むと、シーンビューのポーズが
        //   「最後に流れたフレーム」で固定されて編集の基準にならない。
        //   モーション確認は Inspector の Animation Preview で行う方針なので、
        //   停止中はリファレンスポーズ (= バインドポーズ) で静止させる。
        //   セットアップ (コントローラ / クリップ読み込み、スキニングバッファ生成、
        //   EnsureBoneHierarchy) は停止中も必要なため、ここまでは通す。
        if (!ctx.simulating) {
            // ボーン GameObject 階層だけは停止中にも用意する。
            // WHY: ソケットの親付けや Inspector からのボーン選択は
            //      Play Mode に入る前から使えている必要がある。
            if (smr && skeleton && skeleton->rootNodeIndex >= 0 &&
                skeleton->rootNodeIndex < static_cast<int>(skeleton->nodes.size()))
                EnsureBoneHierarchy(scene, go, *smr, *skeleton);
            UploadBindPose(*animator, resources, skeleton);
            // 停止中は移動量ゼロを公開する。前フレームの delta が残ると
            // Inspector の表示や Script のポーリングが止まった値を掴み続ける。
            animator->rootMotionDeltaPosition = math::Vector3::ZERO;
            animator->rootMotionDeltaRotation = math::Quaternion::Identity();
            animator->rootMotionWorldDelta = math::Vector3::ZERO;
            animator->rootMotionWorldVelocity = math::Vector3::ZERO;
            animator->rootMotionDeltaTime = 0.0f;
            animator->rootMotionAppliedByEngine = false;
            animator->rootMotionSamples.clear();
            continue;
        }

        const float previousTime = animator->stateTime;
        if (!skeleton) {
            const asset::AnimationClip* clip = AdvanceStateMachineAnimator(*animator, dt);
            if (clip) {
                const float currentTime = animator->stateTime;
                // スケルトンを持たない Animator でもルートモーションは取り出せる。
                const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
                ProcessRootMotion(*animator, go,
                    { WeightedClip{ clip, nullptr,
                                    static_cast<double>(currentTime) * tps, 1.0f, 1.0f,
                                    ResolveRootMotion(*animator, *clip),
                                    animator->speed < 0.0f } },
                    dt);
                ApplyClipSideEffects(go, *animator, *clip, smr,
                                     previousTime, currentTime);
                if (smr) UpdateMorphVertexBuffers(*smr, resources);
            }
            UploadBindPose(*animator, resources);
            continue;
        }

        RunStateMachineAnimatorPath(*animator, *skeleton, scene, go, *smr, resources, dt);

        ApplyAnimationLayers(*animator, *skeleton, scene, go, *smr, dt);

        const asset::AnimationClip* effectClip = ResolveStateMachineEffectClip(*animator);
        if (effectClip) {
            const float currentTime = animator->stateTime;
            ApplyClipSideEffects(go, *animator, *effectClip, smr,
                                 previousTime, currentTime);
        }
        UpdateMorphVertexBuffers(*smr, resources);

        SkinningCB cb{};
        for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
            cb.boneMatrices[i] = math::Matrix4::Identity();
        for (size_t i = 0; i < animator->boneMatrices.size(); ++i)
            cb.boneMatrices[i] = animator->boneMatrices[i];
        resources.Update(animator->skinningBuffer, &cb, sizeof(SkinningCB));
    }
}

} // namespace fbzz::scene
