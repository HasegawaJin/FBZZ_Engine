/// @file    IKSystem.cpp
/// @brief   AnimatorSystem の FK 結果に複数種の IK Solver を順序付きで適用する。
/// @author  Hasegawa Jin
/// @date    2026-05-30

/// @note IK はアニメーション後段で骨行列だけを補正し、足接地から全身 IK までを
/// @note 同一の依存順で処理して AnimatorSystem の責務を崩さない。
#include <Engine/Scene/Systems/IKSystem.hpp>
#include "BonePoseHelpers.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ArbitraryPerpendicular(const math::Vector3& axis)
{
    /// @note 反平行時の回転軸を RIGHT 固定にすると、骨軸が RIGHT 近傍を通過する瞬間に
    /// @note RIGHT -> UP へ切り替わり、腕を振り切る箇所で姿勢が跳ねたり一時停止したように見える。
    /// @note 軸と最も直交する基底を選ぶことで、不要な特異点と符号反転を減らす。
    const math::Vector3 absoluteAxis{
        std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)
    };
    math::Vector3 basis = math::Vector3::RIGHT;
    if (absoluteAxis.y <= absoluteAxis.x && absoluteAxis.y <= absoluteAxis.z)
        basis = math::Vector3::UP;
    else if (absoluteAxis.z <= absoluteAxis.x && absoluteAxis.z <= absoluteAxis.y)
        basis = math::Vector3::FORWARD;
    return math::Vector3::Cross(basis, axis).Normalized();
}

math::Quaternion FromToRotation(const math::Vector3& from, const math::Vector3& to)
{
    const math::Vector3 f = from.Normalized();
    const math::Vector3 t = to.Normalized();
    const float d = math::Vector3::Dot(f, t);

    if (d >= 1.0f - math::EPSILON)
        return math::Quaternion::Identity();

    if (d <= -1.0f + math::EPSILON) {
        return math::Quaternion::FromAxisAngle(ArbitraryPerpendicular(f), math::PI);
    }

    const math::Vector3 axis = math::Vector3::Cross(f, t);
    const float w = 1.0f + d;
    return math::Quaternion{ axis.x, axis.y, axis.z, w }.Normalized();
}

/// @note Blender 風の Soft IK 距離変換。
/// @note ゴール距離が上限へ近づくほど指数関数で減速し、膝が伸び切る直前の跳ねを抑える。
/// @note 線形クランプだけだと最大伸長付近で急に止まり、膝が「ピン」と伸びて見える。
float ApplySoftIK(float dist, float dMax, float softness)
{
    if (softness <= 0.0f || dist <= 0.0f) return dist;
    const float softLimit = dMax * softness;
    if (softLimit < math::EPSILON) return dist;
    const float softZone = dMax - softLimit;
    if (dist <= softZone) return dist;
    const float excess = dist - softZone;
    return softZone + softLimit * (1.0f - std::exp(-excess / softLimit));
}

/// @note 末端の重み付け後に固定骨長で中間関節を再計算し、位置と回転の独立補間による節の収縮を防ぐ。
/// @see https://www.ryanjuckett.com/analytic-two-bone-ik-in-2d/ The solvable domain / Law of cosines.
void BlendTwoBonePositions(const math::Vector3& root,
                           const math::Vector3& sourceMid,
                           const math::Vector3& sourceTip,
                           const math::Vector3& solvedMid,
                           const math::Vector3& solvedTip,
                           float upperLength,
                           float lowerLength,
                           float weight,
                           math::Vector3& outMid,
                           math::Vector3& outTip)
{
    const math::Vector3 rootToTip = math::Vector3::Lerp(sourceTip, solvedTip, weight) - root;
    const float distance = math::Clamp(rootToTip.Length(),
        std::abs(upperLength - lowerLength) + math::EPSILON,
        upperLength + lowerLength - math::EPSILON);
    const math::Vector3 fallbackAxis = (sourceTip - root).NormalizedOr(
        (sourceMid - root).NormalizedOr(math::Vector3::FORWARD));
    const math::Vector3 axis = rootToTip.NormalizedOr(fallbackAxis);
    const math::Vector3 sourceReference = sourceMid - root;
    const math::Vector3 solvedReference = solvedMid - root;
    const math::Vector3 sourceProjection = sourceReference - axis * math::Vector3::Dot(sourceReference, axis);
    const math::Vector3 solvedProjection = solvedReference - axis * math::Vector3::Dot(solvedReference, axis);
    const math::Vector3 sourceBend = sourceProjection.NormalizedOr(
        solvedProjection.NormalizedOr(ArbitraryPerpendicular(axis)));
    const math::Vector3 solvedBend = solvedProjection.NormalizedOr(sourceBend);
    const float bendCosine = math::Clamp(math::Vector3::Dot(sourceBend, solvedBend), -1.0f, 1.0f);
    const float bendSine = math::Vector3::Dot(axis, math::Vector3::Cross(sourceBend, solvedBend));
    /// @note 反対向きの Pole も円周上で補間し、中間 weight で曲げ方向が消えて膝が反転するのを防ぐ。
    const float bendAngle = std::abs(bendSine) <= math::EPSILON && bendCosine < 0.0f
        ? math::PI : std::atan2(bendSine, bendCosine);
    const math::Vector3 bend = math::Quaternion::FromAxisAngle(axis, bendAngle * weight) * sourceBend;
    const float cosine = math::Clamp(
        (upperLength * upperLength + distance * distance - lowerLength * lowerLength) /
        (2.0f * upperLength * distance), -1.0f, 1.0f);
    const float sine = std::sqrt(math::Max(0.0f, 1.0f - cosine * cosine));
    outMid = root + axis * (upperLength * cosine) + bend * (upperLength * sine);
    outTip = root + axis * distance;
}

void UploadBoneMatrices(AnimatorComponent& animator, renderer::ResourceManager& resources)
{
    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();

    for (size_t i = 0; i < animator.boneMatrices.size(); ++i)
        cb.boneMatrices[i] = animator.boneMatrices[i];

    resources.Update(animator.skinningBuffer, &cb, sizeof(SkinningCB));
}

/// @note TipBone 以下の子孫を、TipBone の移動量だけ平行移動して FK ワールド回転を維持する。
/// @note IK 補正を子孫の回転へ直接伝播させると末端の子ボーンまで Pole 方向へ引っ張られて
/// @note 見えるため、位置移動には追従させても回転は FK のワールド姿勢を保つ。
void TranslateDescendantsKeepFkRotation(Scene& scene,
                                        const asset::Skeleton& skeleton,
                                        const SkinnedMeshRenderer& smr,
                                        AnimatorComponent& animator,
                                        const math::Matrix4& ownerInv,
                                        int nodeIndex,
                                        const math::Vector3& worldDelta)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return;

    for (int childIdx : skeleton.nodes[static_cast<size_t>(nodeIndex)].children) {
        if (childIdx < 0 || childIdx >= static_cast<int>(skeleton.nodes.size()))
            continue;
        if (childIdx >= static_cast<int>(smr.nodeEntities.size()))
            continue;
        if (childIdx >= static_cast<int>(animator.nodeGlobalTransforms.size()))
            continue;

        GameObject* childGo = scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(childIdx)]);
        if (!childGo)
            continue;

        /// @note ワールド姿勢として組む (local position だと ToeBase 等の子ボーンで
        /// @note 親基準座標がワールド座標として扱われる)。親は再帰の 1 段上で確定済みだが
        /// @note 子の world は先行する腰補正を含むため、足首に今回追加した差分だけを足す。
        /// @note 値をコピーしてから CommitBoneWorldPose へ渡す。参照のまま渡すと
        /// @note Commit が同じ Transform を書き換え、引数が書き込み途中の値を指す。
        const math::Vector3    fkPosition = childGo->transform.worldPosition;
        const math::Quaternion fkRotation = childGo->transform.worldRotation;
        const math::Vector3    fkScale    = childGo->transform.worldScale;
        bone_pose_detail::CommitBoneWorldPose(scene, skeleton, smr, animator, ownerInv, childIdx,
                            fkPosition + worldDelta, fkRotation, fkScale);

        TranslateDescendantsKeepFkRotation(
            scene, skeleton, smr, animator, ownerInv, childIdx, worldDelta);
    }
}

/// @note FootPlace がフレーム内で後続 Solver へ渡す全身補正状態。
/// @note 足→腰→脊椎の依存を Component の永続状態ではなく 1 フレーム限定で明示する。
struct IKBodyState {
    float appliedHipOffset = 0.0f;
    /// @note 両足のうち大きいほうの地形補正量 (m)。Spine auto-weight の駆動値として使う。
    /// @note 平地では ≈0、坂道では足高さ差に比例して増加するため weight の自動変調に適する。
    float terrainSlopeMetric = 0.0f;
    bool leftFootGrounded = false;
    bool rightFootGrounded = false;
    std::vector<math::Vector3> solvedPositions;
    std::vector<math::Quaternion> solvedRotations;
    std::vector<bool> hasSolvedPose;
};

/// @note 同じフレームの反復は同じ FK 基準と前フレーム履歴から再評価し、weight と応答時間を累積しない。
struct LookAtFrameState {
    math::Quaternion sourceRotation = math::Quaternion::Identity();
    math::Quaternion historyRotation = math::Quaternion::Identity();
    bool initialized = false;
};

struct HumanoidLegNames {
    const char* root;
    const char* mid;
    const char* foot;
    const char* toe;
};

struct LegNodes {
    int root = -1;
    int mid  = -1;
    int foot = -1;
    int toe  = -1;
    float upperLength = 0.0f;
    float lowerLength = 0.0f;
    GameObject* rootGo = nullptr;
    GameObject* midGo  = nullptr;
    GameObject* footGo = nullptr;
    float surfaceOffset = 0.05f;
    bool physicsShape = false;
    bool physicsSupported = false;
    float physicsSupportWeight = 0.0f;
    math::Vector3 physicsFootPosition = math::Vector3::ZERO;
    math::Vector3 physicsSupportPoint = math::Vector3::ZERO;
    math::Vector3 physicsSupportNormal = math::Vector3::UP;
    const physics::Collider* physicsSupportCollider = nullptr;
    math::Quaternion surfaceRotation = math::Quaternion::Identity();

    /// @note ResolveLeg 時点 (= このフレームの IK がまだ何も書いていない時点) の FK ワールド姿勢。
    /// @note 脚 Solver は Transform が素の FK である前提で hipDelta を自前で足し込む。
    /// @note 同フレームの ApplyNodeAndDescendantsOffset がヒップ subtree の Transform を
    /// @note 書き換えた後に読み直すと hipDelta が二重に乗るため、ここで FK を凍結する。
    math::Vector3    rootFkPosition = math::Vector3::ZERO;
    math::Vector3    midFkPosition  = math::Vector3::ZERO;
    math::Vector3    footFkPosition = math::Vector3::ZERO;
    math::Quaternion rootFkRotation = math::Quaternion::Identity();
    math::Quaternion midFkRotation  = math::Quaternion::Identity();
    math::Quaternion footFkRotation = math::Quaternion::Identity();
    math::Vector3    rootFkScale    = math::Vector3::ONE;
    math::Vector3    midFkScale     = math::Vector3::ONE;
    math::Vector3    footFkScale    = math::Vector3::ONE;
};

using GroundHit = physics::World::RaycastHit;

const physics::World::ColliderFilter kGroundFilter = [](const physics::ColliderInstance& instance) {
    return !instance.isTrigger && (!instance.body || instance.body->IsStatic());
};

std::string CanonicalBoneName(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    const std::string helper = "_$AssimpFbx$_";
    if (const size_t helperPos = name.find(helper); helperPos != std::string::npos)
        name = name.substr(0, helperPos);
    if (const size_t pathPos = name.find_last_of("/|"); pathPos != std::string::npos)
        name = name.substr(pathPos + 1);
    if (const size_t namespacePos = name.find_last_of(':'); namespacePos != std::string::npos)
        name = name.substr(namespacePos + 1);
    return name;
}

int FindHumanoidNode(const asset::Skeleton& skeleton, const std::string& humanoidName)
{
    if (humanoidName.empty()) return -1;
    if (const auto exact = skeleton.nodeMap.find(humanoidName); exact != skeleton.nodeMap.end())
        return exact->second;
    for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
        if (CanonicalBoneName(skeleton.nodes[i].name) == humanoidName)
            return static_cast<int>(i);
    }
    return -1;
}

bool ResolveLeg(Scene& scene,
                const asset::Skeleton& skeleton,
                const SkinnedMeshRenderer& renderer,
                const AnimatorComponent& animator,
                const HumanoidLegNames& names,
                LegNodes& out)
{
    out.root = FindHumanoidNode(skeleton, names.root);
    out.mid  = FindHumanoidNode(skeleton, names.mid);
    out.foot = FindHumanoidNode(skeleton, names.foot);
    out.toe  = FindHumanoidNode(skeleton, names.toe);
    if (out.root < 0 || out.mid < 0 || out.foot < 0) return false;

    const size_t root = static_cast<size_t>(out.root);
    const size_t mid  = static_cast<size_t>(out.mid);
    const size_t foot = static_cast<size_t>(out.foot);
    if (root >= renderer.nodeEntities.size() || mid >= renderer.nodeEntities.size() ||
        foot >= renderer.nodeEntities.size() || root >= animator.nodeGlobalTransforms.size() ||
        mid >= animator.nodeGlobalTransforms.size() || foot >= animator.nodeGlobalTransforms.size())
        return false;

    out.rootGo = scene.GetGameObject(renderer.nodeEntities[root]);
    out.midGo  = scene.GetGameObject(renderer.nodeEntities[mid]);
    out.footGo = scene.GetGameObject(renderer.nodeEntities[foot]);
    if (!out.rootGo || !out.midGo || !out.footGo) return false;

    /// @note ここが「このフレームの FK」を凍結する唯一の地点。以降 Solver は Transform を
    /// @note 読み直さず、このスナップショットだけを入力にする。
    out.rootFkPosition = out.rootGo->transform.worldPosition;
    out.midFkPosition  = out.midGo->transform.worldPosition;
    out.footFkPosition = out.footGo->transform.worldPosition;
    out.rootFkRotation = out.rootGo->transform.worldRotation;
    out.midFkRotation  = out.midGo->transform.worldRotation;
    out.footFkRotation = out.footGo->transform.worldRotation;
    out.rootFkScale    = out.rootGo->transform.worldScale;
    out.midFkScale     = out.midGo->transform.worldScale;
    out.footFkScale    = out.footGo->transform.worldScale;

    out.upperLength = (out.midFkPosition - out.rootFkPosition).Length();
    out.lowerLength = (out.footFkPosition - out.midFkPosition).Length();
    return out.upperLength > math::EPSILON && out.lowerLength > math::EPSILON;
}

void ApplyNodeAndDescendantsOffset(const asset::Skeleton& skeleton,
                                   const SkinnedMeshRenderer& renderer,
                                   Scene& scene,
                                   AnimatorComponent& animator,
                                   const math::Matrix4& ownerInv,
                                   int nodeIndex,
                                   const math::Vector3& worldDelta)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(renderer.nodeEntities.size()) ||
        nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
        return;
    const GameObject* bone = scene.GetGameObject(renderer.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!bone) return;
    /// @note Commit が同じ Transform を書き換えるため、FK 値はコピーしてから渡す。
    const math::Vector3    fkPosition = bone->transform.worldPosition;
    const math::Quaternion fkRotation = bone->transform.worldRotation;
    const math::Vector3    fkScale    = bone->transform.worldScale;
    bone_pose_detail::CommitBoneWorldPose(scene, skeleton, renderer, animator, ownerInv, nodeIndex,
                        fkPosition + worldDelta, fkRotation, fkScale);
    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
        ApplyNodeAndDescendantsOffset(skeleton, renderer, scene, animator, ownerInv, child, worldDelta);
}

/// @note 足首直下の地面を取得し、上下両方向の接地補正量を返す。
/// @note 段差の低い側を 0 扱いすると脚 Solver が走らず、膝を曲げる余地も作れない。
bool QueryFootCorrection(physics::World& world,
                         const IKChain& chain,
                         LegNodes& leg,
                         const RagdollRig* rig,
                         math::Vector3& smoothedSupportOffset,
                         math::Vector3& smoothedSoleNormal,
                         float alpha,
                         GroundHit& outHit,
                         float& outCorrection)
{
    outCorrection = 0.0f;
    const float legLength = leg.upperLength + leg.lowerLength;
    leg.physicsSupported=rig && leg.physicsSupportWeight>0.0f
        && rig->TryGetBoneSupport(leg.footGo->name,leg.physicsFootPosition,leg.physicsSupportPoint,
            leg.physicsSupportNormal,leg.physicsSupportCollider);
    auto supportOffset=leg.physicsSupported ? leg.physicsFootPosition-leg.footFkPosition : math::Vector3::ZERO;
    supportOffset.y=0.0f;
    const float offsetLength=supportOffset.Length();
    if (offsetLength>chain.maxCorrection && offsetLength>math::EPSILON)
        supportOffset=supportOffset*(chain.maxCorrection/offsetLength);
    /// @note 接触が瞬断しても目標速度を不連続にしない。大きく転倒した足を無制限に追従しない。
    smoothedSupportOffset=leg.physicsSupportWeight>0.0f
        ? math::Vector3::Lerp(smoothedSupportOffset,supportOffset,alpha) : math::Vector3::ZERO;
    const float smoothedLength = smoothedSupportOffset.Length();
    if (smoothedLength > chain.maxCorrection && smoothedLength > math::EPSILON)
        smoothedSupportOffset = smoothedSupportOffset * (chain.maxCorrection / smoothedLength);
    leg.physicsFootPosition=leg.footFkPosition+smoothedSupportOffset;
    const auto rayPosition=leg.footFkPosition+smoothedSupportOffset*leg.physicsSupportWeight;
    const math::Vector3 origin = rayPosition +
                                 math::Vector3::UP * (legLength * chain.rayUpRatio);
    if (leg.physicsSupported) {
        /// @note SAT の代表法線を地形の面法線と混同しない。支持中の同じ collider へ照会し、動的支持も含める。
        const auto supportFilter=[&](const physics::ColliderInstance& instance) {
            return !instance.isTrigger && instance.collider==leg.physicsSupportCollider;
        };
        if (!world.Raycast(origin,-math::Vector3::UP,legLength*chain.rayDownRatio,outHit,supportFilter)) {
            /// @note つま先だけが縁に掛かり足首直下が面外のときは、支持面を足首の XZ へ延長する。
            outHit.normal=leg.physicsSupportNormal;
            outHit.point={rayPosition.x,leg.physicsSupportPoint.y
                -(outHit.normal.x*(rayPosition.x-leg.physicsSupportPoint.x)
                    +outHit.normal.z*(rayPosition.z-leg.physicsSupportPoint.z))/outHit.normal.y,rayPosition.z};
        }
    } else if (!world.Raycast(origin, -math::Vector3::UP,
                       legLength * chain.rayDownRatio, outHit, kGroundFilter))
        return false;

    smoothedSoleNormal=rig && leg.physicsSupportWeight>0.0f
        ? math::Vector3::Lerp(smoothedSoleNormal,outHit.normal,alpha).NormalizedOr(outHit.normal)
        : outHit.normal;
    leg.surfaceOffset = chain.footSurfaceOffset;
    leg.surfaceRotation = leg.footFkRotation;
    leg.physicsShape = rig && rig->TryGetFootPlacement(
        leg.footGo->name, leg.footFkRotation, outHit.normal, leg.surfaceRotation, leg.surfaceOffset,&smoothedSoleNormal);
    const float raw = outHit.point.y + leg.surfaceOffset - leg.footFkPosition.y;
    const float magnitude = std::abs(raw);
    const float deadZone = leg.physicsShape ? 0.0f : chain.correctionDeadZone;
    if (magnitude <= deadZone) return true;

    const float signedCorrection = raw > 0.0f
        ? magnitude - deadZone
        : -(magnitude - deadZone);
    outCorrection = math::Clamp(
        signedCorrection, -chain.maxCorrection, chain.maxCorrection);
    return true;
}

/// @note 地面高さをゴールに解析的 2-Bone IK を解き、膝位置と足首位置を同時に更新する。
bool SolveFootLeg(const IKChain& chain,
                  Scene& scene,
                  const asset::Skeleton& skeleton,
                  const SkinnedMeshRenderer& renderer,
                  AnimatorComponent& animator,
                  const math::Matrix4& ownerInv,
                  const math::Quaternion& ownerRotation,
                  const LegNodes& leg,
                  float correction,
                  float effectiveWeight,
                  const math::Vector3& hipDelta,
                  const GroundHit* groundHit,
                  bool grounded)
{
    /// @note 接地中は高さ補正が 0 でも足裏の姿勢と水平支持を更新する。
    if (!grounded && std::abs(correction) <= math::EPSILON &&
        hipDelta.LengthSq() <= math::EPSILON * math::EPSILON)
        return false;

    /// @note ヒップ補正は直前に Transform へ書き戻されているため、ここでライブ値を読むと
    /// @note hipDelta が root / foot の両方へ二重に加算される。ResolveLeg の FK スナップショットを使う。
    const math::Vector3 rootFk = leg.rootFkPosition;
    const math::Vector3 midFk  = leg.midFkPosition;
    const math::Vector3 footFk = leg.footFkPosition;
    const math::Vector3 root   = rootFk + hipDelta;
    math::Vector3 target = footFk + math::Vector3{ 0.0f, correction, 0.0f };
    /// @note 接地中の水平位置を物理から戻し、腰・膝も同じ足目標へ解く。FK の末端だけで接触面を引っ張らない。
    if (leg.physicsShape && leg.physicsSupportWeight>0.0f) {
        target.x=math::Lerp(target.x,leg.physicsFootPosition.x,leg.physicsSupportWeight);
        target.z=math::Lerp(target.z,leg.physicsFootPosition.z,leg.physicsSupportWeight);
    }
    const math::Vector3 rootToTarget = target - root;
    const float rawDistance = rootToTarget.Length();
    if (rawDistance <= math::EPSILON) return false;

    const float maxDistance = leg.upperLength + leg.lowerLength;
    const float minDistance = math::Abs(leg.upperLength - leg.lowerLength) + math::EPSILON;
    /// @note FK の脚長を維持し、足の目標だけを到達可能域へ制限する。腰の上下限を超えて追従しない。
    const float distance = math::Clamp(rawDistance, minDistance, maxDistance - math::EPSILON);
    const math::Vector3 axis = rootToTarget * (1.0f / rawDistance);
    const math::Vector3 effectiveTarget = root + axis * distance;

    /// @note FK 曲げ方向を常に計算する。autoPole で完全上書きすると膝が常に同じ方向を向き
    /// @note 固定化して見えるため、FK 方向を優先し逆半球になる場合のみ preferred で補正する。
    const math::Vector3 fkBendRaw = (midFk + hipDelta) - root;
    const math::Vector3 fkBend    = fkBendRaw - axis * math::Vector3::Dot(fkBendRaw, axis);

    math::Vector3 bendRaw = math::Vector3::ZERO;
    if (chain.autoPole &&
        chain.autoPoleLocalDirection.LengthSq() > math::EPSILON * math::EPSILON) {
        const math::Vector3 preferredDirection =
            (ownerRotation * chain.autoPoleLocalDirection.Normalized()).Normalized();
        const math::Vector3 preferredBend =
            preferredDirection - axis * math::Vector3::Dot(preferredDirection, axis);
        if (fkBend.LengthSq() > math::EPSILON * math::EPSILON &&
            preferredBend.LengthSq() > math::EPSILON * math::EPSILON) {
            /// @note fkBend の信頼度は sin²θ = |fkBend|²/|fkBendRaw|²。膝が axis とほぼ平行になる
            /// @note スイング相では fkBend がほぼゼロで正規化後の方向が任意になるため、
            /// @note ハードな閾値ではなく信頼度ウェイトで FK と preferred をブレンドする。
            const float fkBendRawSq = fkBendRaw.LengthSq();
            const float sinSq   = fkBendRawSq > math::EPSILON * math::EPSILON
                                  ? fkBend.LengthSq() / fkBendRawSq : 0.0f;
            /// @note sin²θ > 0.04 (≒ θ > 11.5°) で FK を完全に信頼し、それ未満は preferred へ漸近。
            const float fkWeight = math::Clamp01(sinSq / 0.04f);
            const float agreement = math::Vector3::Dot(
                fkBend.Normalized(), preferredBend.Normalized());
            if (agreement >= 0.0f) {
                /// @note FK 方向が preferred と同じ半球: 信頼度ウェイトで FK と preferred をブレンド。
                bendRaw = fkBend * fkWeight + preferredBend * (1.0f - fkWeight);
            } else {
                /// @note 逆半球 (後方折れ) → preferred で補正。
                bendRaw = preferredBend;
            }
        } else {
            bendRaw = preferredBend.LengthSq() > math::EPSILON * math::EPSILON
                ? preferredBend : fkBend;
        }
    }
    if (bendRaw.LengthSq() <= math::EPSILON * math::EPSILON)
        bendRaw = fkBend;
    const math::Vector3 bend = bendRaw.LengthSq() > math::EPSILON * math::EPSILON
        ? bendRaw.Normalized() : ArbitraryPerpendicular(axis);
    const float cosine = math::Clamp(
        (leg.upperLength * leg.upperLength + distance * distance - leg.lowerLength * leg.lowerLength) /
        (2.0f * leg.upperLength * distance), -1.0f, 1.0f);
    const float sine = std::sqrt(math::Max(0.0f, 1.0f - cosine * cosine));
    const math::Vector3 solvedMid = root + axis * (leg.upperLength * cosine) +
                                    bend * (leg.upperLength * sine);

    math::Vector3 finalMid;
    math::Vector3 finalFoot;
    BlendTwoBonePositions(root, midFk + hipDelta, footFk + hipDelta,
        solvedMid, effectiveTarget, leg.upperLength, leg.lowerLength, effectiveWeight,
        finalMid, finalFoot);
    const math::Quaternion rootRotation =
        (FromToRotation((midFk - rootFk).Normalized(), (finalMid - root).Normalized()) *
         leg.rootFkRotation).Normalized();
    const math::Quaternion midRotation =
        (FromToRotation((footFk - midFk).Normalized(), (finalFoot - finalMid).Normalized()) *
         leg.midFkRotation).Normalized();
    math::Quaternion footRotation = leg.footFkRotation;
    if (leg.physicsShape) {
        footRotation = math::Quaternion::Slerp(footRotation, leg.surfaceRotation, effectiveWeight);
    } else if (groundHit && chain.footNormalAxis.LengthSq() > math::EPSILON * math::EPSILON) {
        const math::Vector3 sole =
            (footRotation * chain.footNormalAxis.Normalized()).Normalized();
        const math::Quaternion aligned =
            (FromToRotation(sole, groundHit->normal) * footRotation).Normalized();
        footRotation = math::Quaternion::Slerp(footRotation, aligned, effectiveWeight);
    }

    if (leg.root < 0 || leg.mid < 0 || leg.foot < 0 ||
        leg.root >= static_cast<int>(skeleton.nodes.size()) ||
        leg.mid >= static_cast<int>(skeleton.nodes.size()) ||
        leg.foot >= static_cast<int>(skeleton.nodes.size()) ||
        leg.root >= static_cast<int>(animator.nodeGlobalTransforms.size()) ||
        leg.mid >= static_cast<int>(animator.nodeGlobalTransforms.size()) ||
        leg.foot >= static_cast<int>(animator.nodeGlobalTransforms.size()))
        return false;

    /// @note 腿 → 脛 → 足首の順に確定させる。Commit は local を親のワールド姿勢から逆算するため、
    /// @note 親を先に書かないと子の local が古い親基準で求まる。
    const math::Vector3 rootScale = leg.rootGo->transform.worldScale;
    const math::Vector3 midScale  = leg.midGo->transform.worldScale;
    const math::Vector3 footScale = leg.footGo->transform.worldScale;
    bone_pose_detail::CommitBoneWorldPose(scene, skeleton, renderer, animator, ownerInv, leg.root,
                        root, rootRotation, rootScale);
    bone_pose_detail::CommitBoneWorldPose(scene, skeleton, renderer, animator, ownerInv, leg.mid,
                        finalMid, midRotation, midScale);
    bone_pose_detail::CommitBoneWorldPose(scene, skeleton, renderer, animator, ownerInv, leg.foot,
                        finalFoot, footRotation, footScale);
    TranslateDescendantsKeepFkRotation(
        scene, skeleton, renderer, animator, ownerInv, leg.foot, finalFoot - (footFk + hipDelta));
    return true;
}

/// @note 両足の接地、ヒップ補正、脚の 2-Bone IK を 1 チェーンとして処理する。
bool SolveFootPlace(IKChain& chain,
                    Scene& scene,
                    physics::World& world,
                    const asset::Skeleton& skeleton,
                    const SkinnedMeshRenderer& renderer,
                    AnimatorComponent& animator,
                    const math::Matrix4& ownerInv,
                    const math::Quaternion& ownerRotation,
                    float stateWeight,
                    float deltaTime,
                    IKBodyState& bodyState)
{
    /// @note Inspector の直接入力と旧保存データも SDK と同じ有限範囲に収める。
    auto bounded = [](float value, float fallback, float maximum) {
        return std::isfinite(value) ? std::clamp(value, 0.0f, maximum) : fallback;
    };
    chain.rayUpRatio = bounded(chain.rayUpRatio, 0.5f, 2.0f);
    chain.rayDownRatio = bounded(chain.rayDownRatio, 1.2f, 4.0f);
    chain.footSurfaceOffset = bounded(chain.footSurfaceOffset, 0.05f, 0.5f);
    chain.correctionDeadZone = bounded(chain.correctionDeadZone, 0.025f, 0.25f);
    chain.maxCorrection = bounded(chain.maxCorrection, 0.12f, 1.0f);
    chain.footPlantDistance = bounded(chain.footPlantDistance, 0.06f, 1.0f);
    chain.smoothTime = bounded(chain.smoothTime, 0.10f, 1.0f);
    chain.hipWeight = bounded(chain.hipWeight, 1.0f, 1.0f);
    chain.maxHipLowering = bounded(chain.maxHipLowering, 0.10f, 1.0f);
    chain.maxHipRaising = bounded(chain.maxHipRaising, 0.0f, 1.0f);
    chain.hipBendReserve = bounded(chain.hipBendReserve, 0.0f, 0.25f);
    const HumanoidLegNames leftNames{ "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase" };
    const HumanoidLegNames rightNames{ "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase" };
    LegNodes left;
    LegNodes right;
    const bool hasLeft  = ResolveLeg(scene, skeleton, renderer, animator, leftNames, left);
    const bool hasRight = ResolveLeg(scene, skeleton, renderer, animator, rightNames, right);
    if (!hasLeft && !hasRight) return false;

    const float effectiveWeight = math::Clamp01(
        chain.weight * (chain.useAnimatorIKWeight ? stateWeight : 1.0f));
    if (effectiveWeight <= math::EPSILON) {
        /// @note ジャンプへ遷移したフレームで平滑化残量を適用すると、片脚だけ旧Pole方向へねじれる。
        chain.smoothedLeft = 0.0f;
        chain.smoothedRight = 0.0f;
        chain.smoothedHip = 0.0f;
        chain.smoothedTerrainOnlyHip = 0.0f;
        chain.smoothedLeftSupportOffset=chain.smoothedRightSupportOffset=math::Vector3::ZERO;
        chain.smoothedLeftSoleNormal=chain.smoothedRightSoleNormal=math::Vector3::UP;
        chain.leftPlantWeight = 0.0f;
        chain.rightPlantWeight = 0.0f;
        bodyState.leftFootGrounded = false;
        bodyState.rightFootGrounded = false;
        bodyState.terrainSlopeMetric = 0.0f;
        return false;
    }
    GroundHit leftHit{};
    GroundHit rightHit{};
    float leftRaw = 0.0f;
    float rightRaw = 0.0f;
    const RagdollRig* physicsRig = nullptr;
    if (chain.useRagdollFootShape) {
        auto* owner = hasLeft ? left.rootGo : right.rootGo;
        while (owner) {
            if (const auto* component = owner->GetComponent<RagdollComponent>(); component && component->enabled) {
                physicsRig = component->runtime.rig.get();
                left.physicsSupportWeight=right.physicsSupportWeight=component->mode==RagdollMode::Active
                    ? math::Clamp01(component->weight) : 0.0f;
                break;
            }
            owner = owner->GetParent();
        }
    }
    const float alpha = chain.smoothTime > 0.0f
        ? 1.0f - std::exp(-std::max(deltaTime, 0.0f) / chain.smoothTime) : 1.0f;
    const bool leftHasGround = hasLeft &&
        QueryFootCorrection(world, chain, left, physicsRig, chain.smoothedLeftSupportOffset,
            chain.smoothedLeftSoleNormal,alpha,leftHit,leftRaw);
    const bool rightHasGround = hasRight &&
        QueryFootCorrection(world, chain, right, physicsRig, chain.smoothedRightSupportOffset,
            chain.smoothedRightSoleNormal,alpha,rightHit,rightRaw);
    chain.runtimePhysicsFootShape = left.physicsShape || right.physicsShape;
    /// @note 接地相は FK の足裏高の差で選ぶ。地形高を差し引くと、段差の低い側を遊脚と誤判定する。
    /// @note 低い足を必ず接地候補に残し、そこから一定以上高い足だけをスイング相として解放する。
    const float leftClearance = leftHasGround
        ? left.footGo->transform.worldPosition.y -
            left.surfaceOffset
        : std::numeric_limits<float>::max();
    const float rightClearance = rightHasGround
        ? right.footGo->transform.worldPosition.y -
            right.surfaceOffset
        : std::numeric_limits<float>::max();
    float minimumClearance = std::numeric_limits<float>::max();
    if (leftHasGround) minimumClearance = std::min(minimumClearance, leftClearance);
    if (rightHasGround) minimumClearance = std::min(minimumClearance, rightClearance);
    const float plantTolerance = std::max(chain.footPlantDistance, 0.0f);
    /// @note 接地中は解除側の閾値を広げ、境界付近で接地/非接地が毎フレーム反転するのを防ぐ。
    const float leftTolerance = plantTolerance *
        (chain.leftPlantWeight > 0.01f ? 1.5f : 1.0f);
    const float rightTolerance = plantTolerance *
        (chain.rightPlantWeight > 0.01f ? 1.5f : 1.0f);
    const bool leftGrounded = leftHasGround &&
        (leftClearance <= minimumClearance + leftTolerance);
    const bool rightGrounded = rightHasGround &&
        (rightClearance <= minimumClearance + rightTolerance);
    bodyState.leftFootGrounded = chain.runtimeLeftGrounded = leftGrounded;
    bodyState.rightFootGrounded = chain.runtimeRightGrounded = rightGrounded;
    if (!leftGrounded) chain.smoothedLeftSupportOffset=math::Vector3::ZERO;
    if (!rightGrounded) chain.smoothedRightSupportOffset=math::Vector3::ZERO;
    chain.leftPlantWeight = math::Lerp(
        chain.leftPlantWeight, leftGrounded ? 1.0f : 0.0f, alpha);
    chain.rightPlantWeight = math::Lerp(
        chain.rightPlantWeight, rightGrounded ? 1.0f : 0.0f, alpha);
    chain.smoothedLeft = std::clamp(math::Lerp(
        chain.smoothedLeft, leftGrounded ? leftRaw : 0.0f, alpha), -chain.maxCorrection, chain.maxCorrection);
    chain.smoothedRight = std::clamp(math::Lerp(
        chain.smoothedRight, rightGrounded ? rightRaw : 0.0f, alpha), -chain.maxCorrection, chain.maxCorrection);

    math::Vector3 hipDelta = math::Vector3::ZERO;
    bool modified = false;
    chain.runtimeHipTarget = 0.0f;
    chain.runtimeHipLimited = false;
    if (chain.adjustHip && chain.hipWeight > 0.0f) {
        float lowestCorrection = std::numeric_limits<float>::max();
        if (leftGrounded) lowestCorrection = std::min(lowestCorrection, chain.smoothedLeft);
        if (rightGrounded) lowestCorrection = std::min(lowestCorrection, chain.smoothedRight);
        if (!leftGrounded && !rightGrounded) lowestCorrection = 0.0f;
        const float reserve = (leftGrounded || rightGrounded)
            ? chain.hipBendReserve * effectiveWeight : 0.0f;
        const float requested = (lowestCorrection * effectiveWeight - reserve) * chain.hipWeight;
        const float lower = -chain.maxHipLowering * effectiveWeight;
        const float upper = chain.maxHipRaising * effectiveWeight;
        const float hipTarget = std::clamp(requested, lower, upper);
        chain.runtimeHipTarget = hipTarget;
        chain.runtimeHipLimited = std::abs(requested - hipTarget) > math::EPSILON;
        /// @note 上限を縮めたフレームから制限を守る。平滑化の履歴で旧上限を超過させない。
        chain.smoothedHip = std::clamp(math::Lerp(chain.smoothedHip, hipTarget, alpha), lower, upper);
        chain.smoothedTerrainOnlyHip = std::clamp(
            math::Lerp(chain.smoothedTerrainOnlyHip, lowestCorrection * effectiveWeight * chain.hipWeight, alpha), lower, upper);
        const int hipNode = FindHumanoidNode(skeleton, chain.hipBoneName);
        if (hipNode >= 0) {
            hipDelta.y = chain.smoothedHip;
            if (std::abs(hipDelta.y) > math::EPSILON) {
                ApplyNodeAndDescendantsOffset(
                    skeleton, renderer, scene, animator, ownerInv, hipNode, hipDelta);
                modified = true;
            }
        }
    } else {
        chain.smoothedHip = 0.0f;
        chain.smoothedTerrainOnlyHip = 0.0f;
    }
    bodyState.appliedHipOffset = chain.runtimeAppliedHip = hipDelta.y;

    /// @note 地形傾斜メトリクス: 両足のうち大きいほうの補正量を Spine auto-weight の駆動値にする。
    /// @note 平地ではほぼ 0 だが坂道では足高さ差に応じて増加するため weight 変調に適している。
    bodyState.terrainSlopeMetric = std::max(
        std::abs(chain.smoothedLeft), std::abs(chain.smoothedRight));

    if (hasLeft && leftGrounded) {
        modified |= SolveFootLeg(chain, scene, skeleton, renderer, animator, ownerInv, ownerRotation,
                                 left, chain.smoothedLeft,
                                 effectiveWeight * chain.leftPlantWeight, hipDelta,
                                 &leftHit, true);
    }
    if (hasRight && rightGrounded) {
        modified |= SolveFootLeg(chain, scene, skeleton, renderer, animator, ownerInv, ownerRotation,
                                 right, chain.smoothedRight,
                                 effectiveWeight * chain.rightPlantWeight, hipDelta,
                                 &rightHit, true);
    }
    return modified;
}

/// @note Solver が確定したワールド姿勢を骨行列と後続チェーン共有状態へ反映する。
void WriteSolvedPose(const asset::Skeleton& skeleton,
                     const SkinnedMeshRenderer& renderer,
                     Scene& scene,
                     AnimatorComponent& animator,
                     const math::Matrix4& ownerInv,
                     int nodeIndex,
                     const math::Vector3& position,
                     const math::Quaternion& rotation,
                     IKBodyState& bodyState)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(renderer.nodeEntities.size()) ||
        nodeIndex >= static_cast<int>(animator.nodeGlobalTransforms.size()))
        return;
    const GameObject* bone = scene.GetGameObject(renderer.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!bone) return;

    /// @note Commit がこの Transform を書き換えるため、スケールはコピーしてから渡す。
    const math::Vector3 boneScale = bone->transform.worldScale;
    bone_pose_detail::CommitBoneWorldPose(scene, skeleton, renderer, animator, ownerInv, nodeIndex,
                        position, rotation, boneScale);

    const size_t index = static_cast<size_t>(nodeIndex);
    if (index < bodyState.hasSolvedPose.size()) {
        bodyState.solvedPositions[index] = position;
        bodyState.solvedRotations[index] = rotation;
        bodyState.hasSolvedPose[index] = true;
    }
}

/// @note 親ボーンの剛体差分を枝全体へ適用し、Spine/LookAt 配下の肩・腕・目などを追従させる。
void ApplyRigidSubtree(const asset::Skeleton& skeleton,
                       const SkinnedMeshRenderer& renderer,
                       Scene& scene,
                       AnimatorComponent& animator,
                       const math::Matrix4& ownerInv,
                       int nodeIndex,
                       const math::Vector3& originalPivot,
                       const math::Vector3& solvedPivot,
                       const math::Quaternion& rotationDelta,
                       const math::Vector3& inheritedDisplacement,
                       IKBodyState& bodyState)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()) ||
        nodeIndex >= static_cast<int>(renderer.nodeEntities.size()))
        return;
    const GameObject* bone = scene.GetGameObject(renderer.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!bone) return;

    const size_t index = static_cast<size_t>(nodeIndex);
    const bool hasPriorPose = index < bodyState.hasSolvedPose.size() && bodyState.hasSolvedPose[index];
    const math::Vector3 originalPosition = hasPriorPose
        ? bodyState.solvedPositions[index]
        : bone->transform.worldPosition + inheritedDisplacement;
    const math::Quaternion originalRotation = hasPriorPose
        ? bodyState.solvedRotations[index]
        : bone->transform.worldRotation;
    const math::Vector3 solvedPosition = solvedPivot +
        rotationDelta * (originalPosition - originalPivot);
    const math::Quaternion solvedRotation =
        (rotationDelta * originalRotation).Normalized();
    WriteSolvedPose(skeleton, renderer, scene, animator, ownerInv,
                    nodeIndex, solvedPosition, solvedRotation, bodyState);

    for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children) {
        ApplyRigidSubtree(skeleton, renderer, scene, animator, ownerInv, child,
                          originalPivot, solvedPivot, rotationDelta,
                          inheritedDisplacement, bodyState);
    }
}

/// @note boneNames の順序を維持したまま Skeleton ノード番号へ解決する。
bool ResolveChainNodes(const asset::Skeleton& skeleton,
                       const std::vector<std::string>& boneNames,
                       std::vector<int>& outNodes)
{
    outNodes.clear();
    outNodes.reserve(boneNames.size());
    for (const auto& boneName : boneNames) {
        const int nodeIndex = FindHumanoidNode(skeleton, boneName);
        if (nodeIndex < 0) return false;
        outNodes.push_back(nodeIndex);
    }
    return !outNodes.empty();
}

/// @note from から to への最短回転を指定角度以内に制限して返す。
math::Quaternion ClampedFromToRotation(const math::Vector3& from,
                                       const math::Vector3& to,
                                       float maxAngleRadians)
{
    const math::Vector3 source = from.Normalized();
    const math::Vector3 target = to.Normalized();
    const float cosine = math::Clamp(math::Vector3::Dot(source, target), -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    if (angle <= maxAngleRadians) return FromToRotation(source, target);

    math::Vector3 axis = math::Vector3::Cross(source, target);
    if (axis.LengthSq() <= math::EPSILON * math::EPSILON)
        axis = ArbitraryPerpendicular(source);
    return math::Quaternion::FromAxisAngle(axis.Normalized(), maxAngleRadians);
}

/// @note 指定ボーンのローカル注視軸をターゲットへ向け、子孫を剛体追従させる。
bool SolveLookAt(IKChain& chain,
                 Scene& scene,
                 const asset::Skeleton& skeleton,
                 const SkinnedMeshRenderer& renderer,
                 AnimatorComponent& animator,
                 const math::Matrix4& ownerInv,
                 float stateWeight,
                 float deltaTime,
                  IKBodyState& bodyState,
                  LookAtFrameState& frameState)
{
    if (chain.boneNames.size() != 1 ||
        chain.lookAtAxis.LengthSq() <= math::EPSILON * math::EPSILON)
        return false;
    GameObject* target = scene.GetGameObject(chain.targetEntity);
    if (!target) return false;

    const int nodeIndex = FindHumanoidNode(skeleton, chain.boneNames[0]);
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(renderer.nodeEntities.size())) return false;
    GameObject* bone = scene.GetGameObject(renderer.nodeEntities[static_cast<size_t>(nodeIndex)]);
    if (!bone) return false;

    const size_t index = static_cast<size_t>(nodeIndex);
    const bool hasPriorPose = index < bodyState.hasSolvedPose.size() && bodyState.hasSolvedPose[index];
    const math::Vector3 sourcePosition = hasPriorPose
        ? bodyState.solvedPositions[index]
        : bone->transform.worldPosition;
    const math::Quaternion sourceRotation = hasPriorPose
        ? bodyState.solvedRotations[index]
        : bone->transform.worldRotation;
    if (!frameState.initialized) {
        frameState.sourceRotation = sourceRotation;
        frameState.historyRotation = chain.lookAtHasState ? chain.lookAtSmoothedRotation : sourceRotation;
        frameState.initialized = true;
    }
    const math::Vector3 desiredDirection =
        target->transform.worldPosition + chain.targetOffset - sourcePosition;
    if (desiredDirection.LengthSq() <= math::EPSILON * math::EPSILON) return false;

    const math::Vector3 currentAxis =
        (frameState.sourceRotation * chain.lookAtAxis.Normalized()).Normalized();
    const float maxAngle = math::Clamp(chain.lookAtClampAngle, 0.0f, 180.0f) *
                           (math::PI / 180.0f);
    math::Quaternion desiredRotation =
        (ClampedFromToRotation(currentAxis, desiredDirection.Normalized(), maxAngle) *
         frameState.sourceRotation).Normalized();

    /// @note Up 軸のロールをワールド Up へ寄せ、注視中の首・頭の横倒しを抑える。
    if (chain.lookAtUpAxis.LengthSq() > math::EPSILON * math::EPSILON) {
        const math::Vector3 lookDirection = desiredDirection.Normalized();
        const math::Vector3 currentUp =
            (desiredRotation * chain.lookAtUpAxis.Normalized()).Normalized();
        const math::Vector3 projectedCurrent =
            currentUp - lookDirection * math::Vector3::Dot(currentUp, lookDirection);
        const math::Vector3 projectedTarget =
            math::Vector3::UP - lookDirection * math::Vector3::Dot(math::Vector3::UP, lookDirection);
        if (projectedCurrent.LengthSq() > math::EPSILON * math::EPSILON &&
            projectedTarget.LengthSq() > math::EPSILON * math::EPSILON) {
            desiredRotation =
                (FromToRotation(projectedCurrent.Normalized(), projectedTarget.Normalized()) *
                 desiredRotation).Normalized();
        }
    }

    const float weight = math::Clamp01(chain.weight * stateWeight);
    const math::Quaternion weightedRotation =
        math::Quaternion::Slerp(frameState.sourceRotation, desiredRotation, weight);
    chain.lookAtHasState = true;
    const float response = 1.0f - std::exp(
        -std::max(deltaTime, 0.0001f) * std::max(chain.lookAtSpeed, 0.0f));
    chain.lookAtSmoothedRotation = math::Quaternion::Slerp(
        frameState.historyRotation, weightedRotation, response);

    const math::Quaternion rotationDelta =
        (chain.lookAtSmoothedRotation * sourceRotation.Inverse()).Normalized();
    WriteSolvedPose(skeleton, renderer, scene, animator, ownerInv, nodeIndex,
                    sourcePosition, chain.lookAtSmoothedRotation, bodyState);
    for (int child : skeleton.nodes[index].children) {
        ApplyRigidSubtree(skeleton, renderer, scene, animator, ownerInv, child,
                          sourcePosition, sourcePosition, rotationDelta,
                          math::Vector3::ZERO, bodyState);
    }
    return true;
}

/// @note 可変長ボーン列を FABRIK でターゲットへ収束させ、各節の長さを維持する。
bool SolveSpine(IKChain& chain,
                Scene& scene,
                const asset::Skeleton& skeleton,
                const SkinnedMeshRenderer& renderer,
                AnimatorComponent& animator,
                const math::Matrix4& ownerInv,
                float stateWeight,
                IKBodyState& bodyState)
{
    if (chain.boneNames.size() < 2) return false;
    GameObject* target = scene.GetGameObject(chain.targetEntity);
    if (!target) return false;

    std::vector<int> nodes;
    if (!ResolveChainNodes(skeleton, chain.boneNames, nodes)) return false;
    const size_t count = nodes.size();
    for (size_t i = 1; i < count; ++i) {
        if (skeleton.nodes[static_cast<size_t>(nodes[i])].parentIndex != nodes[i - 1])
            return false;
    }
    std::vector<GameObject*> bones(count, nullptr);
    std::vector<math::Vector3> sourcePositions(count);
    std::vector<math::Quaternion> sourceRotations(count);
    std::vector<float> lengths(count - 1, 0.0f);
    float totalLength = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        const int nodeIndex = nodes[i];
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(renderer.nodeEntities.size())) return false;
        bones[i] = scene.GetGameObject(renderer.nodeEntities[static_cast<size_t>(nodeIndex)]);
        if (!bones[i]) return false;
        const size_t index = static_cast<size_t>(nodeIndex);
        const bool hasPriorPose = index < bodyState.hasSolvedPose.size() && bodyState.hasSolvedPose[index];
        sourcePositions[i] = hasPriorPose
            ? bodyState.solvedPositions[index]
            : bones[i]->transform.worldPosition;
        sourceRotations[i] = hasPriorPose
            ? bodyState.solvedRotations[index]
            : bones[i]->transform.worldRotation;
        if (i > 0) {
            lengths[i - 1] = (sourcePositions[i] - sourcePositions[i - 1]).Length();
            if (lengths[i - 1] <= math::EPSILON) return false;
            totalLength += lengths[i - 1];
        }
    }

    std::vector<math::Vector3> solved = sourcePositions;
    const math::Vector3 rootPosition = sourcePositions.front();
    const math::Vector3 targetPosition = target->transform.worldPosition + chain.targetOffset;
    const float rootToTarget = (targetPosition - rootPosition).Length();

    if (rootToTarget >= totalLength) {
        const math::Vector3 direction = (targetPosition - rootPosition).Normalized();
        for (size_t i = 1; i < count; ++i)
            solved[i] = solved[i - 1] + direction * lengths[i - 1];
    } else {
        constexpr int MAX_ITERATIONS = 12;
        constexpr float TOLERANCE_SQ = 0.000001f;
        for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
            solved.back() = targetPosition;
            for (size_t i = count - 1; i > 0; --i) {
                const math::Vector3 direction = (solved[i - 1] - solved[i]).Normalized();
                solved[i - 1] = solved[i] + direction * lengths[i - 1];
            }
            solved.front() = rootPosition;
            for (size_t i = 1; i < count; ++i) {
                const math::Vector3 direction = (solved[i] - solved[i - 1]).Normalized();
                solved[i] = solved[i - 1] + direction * lengths[i - 1];
            }
            if ((solved.back() - targetPosition).LengthSq() <= TOLERANCE_SQ) break;
        }
    }

    /// @note spineAutoWeight が有効なとき、地形傾斜に応じて spineFlatWeight ↔ chain.weight を補間する。
    /// @note 平地では低 weight で FK をほぼ維持し、坂道で自動的に補正量を増やすため。
    float effectiveChainWeight = chain.weight;
    if (chain.spineAutoWeight && chain.spineSlopeRampMeters > math::EPSILON) {
        const float slopeFactor =
            math::Clamp01(bodyState.terrainSlopeMetric / chain.spineSlopeRampMeters);
        effectiveChainWeight =
            math::Lerp(chain.spineFlatWeight, chain.weight, slopeFactor);
    }
    const float weight = math::Clamp01(effectiveChainWeight * stateWeight);
    std::vector<math::Vector3> finalPositions(count);
    std::vector<math::Quaternion> finalRotations(count);
    for (size_t i = 0; i < count; ++i)
        finalPositions[i] = math::Vector3::Lerp(sourcePositions[i], solved[i], weight);
    /// @note FK/IK 位置の単純補間は中間 Weight で節長を縮めるため、Root から再投影する。
    for (size_t i = 1; i < count; ++i) {
        math::Vector3 direction = finalPositions[i] - finalPositions[i - 1];
        if (direction.LengthSq() <= math::EPSILON * math::EPSILON)
            direction = sourcePositions[i] - sourcePositions[i - 1];
        finalPositions[i] = finalPositions[i - 1] + direction.Normalized() * lengths[i - 1];
    }
    for (size_t i = 0; i + 1 < count; ++i) {
        const math::Quaternion delta = FromToRotation(
            (sourcePositions[i + 1] - sourcePositions[i]).Normalized(),
            (finalPositions[i + 1] - finalPositions[i]).Normalized());
        finalRotations[i] = (delta * sourceRotations[i]).Normalized();
    }
    const math::Quaternion tipDelta = count > 1
        ? (finalRotations[count - 2] * sourceRotations[count - 2].Inverse()).Normalized()
        : math::Quaternion::Identity();
    finalRotations.back() = (tipDelta * sourceRotations.back()).Normalized();

    for (size_t i = 0; i < count; ++i) {
        WriteSolvedPose(skeleton, renderer, scene, animator, ownerInv, nodes[i],
                        finalPositions[i], finalRotations[i], bodyState);
    }

    /// @note チェーン外の枝は、最寄りの Spine ボーンの剛体差分で追従させる。
    for (size_t i = 0; i < count; ++i) {
        const int nextNode = i + 1 < count ? nodes[i + 1] : -1;
        const math::Quaternion delta =
            (finalRotations[i] * sourceRotations[i].Inverse()).Normalized();
        for (int child : skeleton.nodes[static_cast<size_t>(nodes[i])].children) {
            if (child == nextNode) continue;
            ApplyRigidSubtree(skeleton, renderer, scene, animator, ownerInv, child,
                              sourcePositions[i], finalPositions[i], delta,
                              math::Vector3::ZERO, bodyState);
        }
    }
    return true;
}

}

/// @note 骨 GameObject の Transform を書き、smr を読む。宣言から漏らすと、それらを触る他の
/// @note System と同じバッチに入って並列に走る (理由は AnimatorSystem::GetAccess を参照)。
ComponentAccess IKSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<SkinnedMeshRenderer, RagdollComponent>()
        .Writes<IKSolverComponent, AnimatorComponent, BoneComponent, Transform>();
}

OrderingHints IKSystem::GetOrder() const
{
    return OrderingHints{}.After<AnimatorSystem>();
}

void IKSystem::Update(SystemContext& ctx)
{
    /// @note Pause 中に最終姿勢を再び入力へ使うと、腰補正と平滑化が累積する。
    if (ctx.playing && !ctx.simulating) return;
    if (!ctx.resources) return;
    Scene& scene = ctx.scene;
    physics::World& world = ctx.world;
    renderer::ResourceManager& resources = *ctx.resources;
    const float deltaTime = std::max(ctx.dt, 0.0001f);
    const auto span     = scene.GetEntities<IKSolverComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go || !go->activeInHierarchy()) continue;

        auto* ik       = go->GetComponent<IKSolverComponent>();
        auto* animator = go->GetComponent<AnimatorComponent>();
        auto* smr      = bone_pose_detail::FindSkinnedMeshRenderer(*go);
        if (!ik) continue;
        ResolveIKTargetReferences(scene, *ik);
        ++ik->runtimeUpdateCount;
        ik->runtimeSolvedChainCount = 0;
        ik->runtimeAnimatorWeight = animator ? animator->GetCurrentIKWeight() : 0.0f;
        ik->runtimeLeftFootGrounded = false;
        ik->runtimeRightFootGrounded = false;
        ik->runtimeHipOffset = 0.0f;
        ik->runtimeSkinningUploaded = false;
        ik->runtimeFullBodyIterations = 0;
        ik->runtimeFullBodyError = 0.0f;
        ik->runtimeFullBodyConverged = false;
        for (auto& chain : ik->chains) {
            chain.runtimeHipTarget = chain.runtimeAppliedHip = 0.0f;
            chain.runtimePhysicsFootShape = false;
            chain.runtimeHipLimited = chain.runtimeLeftGrounded = chain.runtimeRightGrounded = false;
            if (!ik->enabled || !chain.enabled || chain.weight <= math::EPSILON) {
                chain.smoothedLeft = chain.smoothedRight = chain.smoothedHip = chain.smoothedTerrainOnlyHip = 0.0f;
                chain.leftPlantWeight = chain.rightPlantWeight = 0.0f;
                chain.smoothedLeftSupportOffset=chain.smoothedRightSupportOffset=math::Vector3::ZERO;
                chain.smoothedLeftSoleNormal=chain.smoothedRightSoleNormal=math::Vector3::UP;
            }
        }
        if (!ik->enabled || !animator || !smr) continue;
        if (!smr->model || !smr->model->skeleton) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        bool anyChainModified = false;
        /// @note ステートごとの IK Weight をクロスフェードを考慮して取得する。
        /// @note IKChain::weight に乗算することで、ステート設定を chain ごとの細かい調整と独立させる。
        const float stateIKWeight = animator->GetCurrentIKWeight();

        /// @note order が同じ場合は登録順を維持し、編集時に予測可能な Solver 順序にする。
        std::vector<IKChain*> sortedChains;
        sortedChains.reserve(ik->chains.size());
        for (auto& chain : ik->chains) sortedChains.push_back(&chain);
        std::stable_sort(sortedChains.begin(), sortedChains.end(),
            [](const IKChain* lhs, const IKChain* rhs) { return lhs->order < rhs->order; });
        IKChain* fullBodyBiped = nullptr;
        for (IKChain* chain : sortedChains) {
            if (chain->enabled && chain->type == IKSolverType::FullBodyBiped) {
                fullBodyBiped = chain;
                break;
            }
        }
        if (fullBodyBiped) {
            auto solverPriority = [](IKSolverType type) {
                switch (type) {
                case IKSolverType::FootPlace: return 0;
                case IKSolverType::TwoBone:   return 10;
                case IKSolverType::FABRIK:    return 20;
                case IKSolverType::AimAt:     return 30;
                case IKSolverType::HandPlace: return 40;
                default:                      return 50;
                }
            };
            std::stable_sort(sortedChains.begin(), sortedChains.end(),
                [&](const IKChain* lhs, const IKChain* rhs) {
                    return solverPriority(lhs->type) < solverPriority(rhs->type);
                });
        }
        IKBodyState bodyState{};
        bodyState.solvedPositions.resize(skeleton.nodes.size(), math::Vector3::ZERO);
        bodyState.solvedRotations.resize(skeleton.nodes.size(), math::Quaternion::Identity());
        bodyState.hasSolvedPose.resize(skeleton.nodes.size(), false);

        /// @note 位置エフェクターの最大残差を測定し、十分収束した時点で反復を終了する。固定回数
        /// @note だけでは軽いポーズにも無駄な反復を行い、難しいポーズの失敗も検出できない。
        auto calculateEffectorError = [&]() {
            float maximumError = 0.0f;
            for (const IKChain* chain : sortedChains) {
                if (!chain->enabled || chain->weight <= 0.0f ||
                    !chain->targetEntity.IsValid()) continue;
                size_t tipNameIndex = 0;
                if (chain->type == IKSolverType::TwoBone ||
                    chain->type == IKSolverType::HandPlace) {
                    if (chain->boneNames.size() != 3) continue;
                    tipNameIndex = 2;
                } else if (chain->type == IKSolverType::FABRIK) {
                    if (chain->boneNames.size() < 2) continue;
                    tipNameIndex = chain->boneNames.size() - 1;
                } else {
                    continue;
                }

                const int tipNode = FindHumanoidNode(
                    skeleton, chain->boneNames[tipNameIndex]);
                if (tipNode < 0 ||
                    tipNode >= static_cast<int>(smr->nodeEntities.size())) continue;
                const size_t tipIndex = static_cast<size_t>(tipNode);
                const GameObject* target = scene.GetGameObject(chain->targetEntity);
                const GameObject* tip = scene.GetGameObject(smr->nodeEntities[tipIndex]);
                if (!target || !tip) continue;
                const math::Vector3 tipPosition = bodyState.hasSolvedPose[tipIndex]
                    ? bodyState.solvedPositions[tipIndex]
                    : tip->transform.worldPosition;
                const math::Vector3 targetPosition =
                    target->transform.worldPosition + chain->targetOffset;
                maximumError = std::max(
                    maximumError, (targetPosition - tipPosition).Length());
            }
            return maximumError;
        };

        /// @note 膝ロックが起きる。骨盤を先に下げることで両脚の可動域を確保する。
        /// @note Main IK solve: チェーンごとに解析的 2-Bone IK を解く。
        const int solverPassCount = fullBodyBiped
            ? std::clamp(fullBodyBiped->fullBodyIterations, 1, 16)
            : 1;
        const float solverStateWeight = stateIKWeight *
            (fullBodyBiped ? math::Clamp01(fullBodyBiped->weight) : 1.0f);
        const float maxJointCorrection = fullBodyBiped
            ? math::Clamp(fullBodyBiped->fullBodyMaxRotationDegrees, 1.0f, 180.0f) *
                (math::PI / 180.0f)
            : math::PI;
        std::vector<IKChain*> solvedChains;
        auto markSolved = [&](IKChain* solvedChain) {
            if (std::find(solvedChains.begin(), solvedChains.end(), solvedChain) ==
                solvedChains.end()) {
                solvedChains.push_back(solvedChain);
                ik->runtimeSolvedChainCount = static_cast<int>(solvedChains.size());
            }
        };

        const size_t solverInvocationCount =
            static_cast<size_t>(solverPassCount) * sortedChains.size();
        std::vector<LookAtFrameState> lookAtFrameStates(sortedChains.size());
        int completedFullBodyPasses = 0;
        float fullBodyError = 0.0f;
        bool fullBodyConverged = false;
        for (size_t invocation = 0; invocation < solverInvocationCount; ++invocation) {
            if (fullBodyBiped && invocation > 0 &&
                invocation % sortedChains.size() == 0) {
                completedFullBodyPasses =
                    static_cast<int>(invocation / sortedChains.size());
                fullBodyError = calculateEffectorError();
                if (fullBodyError <= std::max(fullBodyBiped->fullBodyTolerance, 0.0001f)) {
                    fullBodyConverged = true;
                    break;
                }
            }
            const int solverPass =
                static_cast<int>(invocation / sortedChains.size());
            IKChain* chainPtr = sortedChains[invocation % sortedChains.size()];
            IKChain& chain = *chainPtr;
            if (!chain.enabled || chain.weight <= 0.0f) continue;
            if (chain.type == IKSolverType::FullBodyBiped) continue;

            /// @note 同じ Weight を反復回数分そのまま適用すると、0.6 を4回で実効0.974まで増幅する。
            /// @note 1-(1-w)^(1/N) を1パス分の合成率に使い、全反復後の実効Weightを w に保つ。
            float chainStateWeight = solverStateWeight;
            const bool distributesWeight =
                chain.type == IKSolverType::FABRIK ||
                chain.type == IKSolverType::TwoBone ||
                chain.type == IKSolverType::HandPlace;
            if (fullBodyBiped && distributesWeight && solverPassCount > 1) {
                const float effectiveWeight = math::Clamp01(chain.weight * solverStateWeight);
                const float passWeight = 1.0f - std::pow(
                    1.0f - effectiveWeight,
                    1.0f / static_cast<float>(solverPassCount));
                chainStateWeight = chain.weight > math::EPSILON
                    ? passWeight / chain.weight
                    : 0.0f;
            }

            if (chain.type == IKSolverType::FootPlace) {
                if (solverPass > 0) continue;
                const bool modified = SolveFootPlace(
                    chain, scene, world, skeleton, *smr, *animator,
                    ownerInv, go->transform.worldRotation,
                    solverStateWeight, deltaTime, bodyState);
                anyChainModified |= modified;
                if (modified) markSolved(chainPtr);
                continue;
            }
            if (chain.type == IKSolverType::FABRIK) {
                const bool modified = SolveSpine(
                    chain, scene, skeleton, *smr, *animator,
                    ownerInv, chainStateWeight, bodyState);
                anyChainModified |= modified;
                if (modified) markSolved(chainPtr);
                continue;
            }
            if (chain.type == IKSolverType::AimAt) {
                const bool modified = SolveLookAt(
                    chain, scene, skeleton, *smr, *animator,
                    ownerInv, chainStateWeight, deltaTime, bodyState,
                    lookAtFrameStates[invocation % sortedChains.size()]);
                anyChainModified |= modified;
                if (modified) markSolved(chainPtr);
                continue;
            }
            if (chain.type != IKSolverType::TwoBone &&
                chain.type != IKSolverType::HandPlace) continue;
            if (chain.boneNames.size() != 3) continue;

            const auto itA = skeleton.nodeMap.find(chain.boneNames[0]);
            const auto itB = skeleton.nodeMap.find(chain.boneNames[1]);
            const auto itC = skeleton.nodeMap.find(chain.boneNames[2]);
            /// @note 回復可能なリグ設定ミスでEditor全体を停止させず、診断値を未収束として残す。
            if (itA == skeleton.nodeMap.end() ||
                itB == skeleton.nodeMap.end() ||
                itC == skeleton.nodeMap.end()) continue;

            const int    nodeA = itA->second;
            const int    nodeB = itB->second;
            const int    nodeC = itC->second;
            const size_t nA    = static_cast<size_t>(nodeA);
            const size_t nB    = static_cast<size_t>(nodeB);
            const size_t nC    = static_cast<size_t>(nodeC);

            if (nA >= smr->nodeEntities.size()              ||
                nB >= smr->nodeEntities.size()              ||
                nC >= smr->nodeEntities.size()              ||
                nA >= animator->nodeGlobalTransforms.size() ||
                nB >= animator->nodeGlobalTransforms.size() ||
                nC >= animator->nodeGlobalTransforms.size()) continue;

            GameObject* targetGO = scene.GetGameObject(chain.targetEntity);
            if (!targetGO) continue;

            GameObject* boneGoA = scene.GetGameObject(smr->nodeEntities[nA]);
            GameObject* boneGoB = scene.GetGameObject(smr->nodeEntities[nB]);
            GameObject* boneGoC = scene.GetGameObject(smr->nodeEntities[nC]);
            if (!boneGoA || !boneGoB || !boneGoC) continue;

            /// @note FK ポーズの位置。骨長は必ず FK 基準から計算し、IK 解の前後で骨長が
            /// @note 揺れないよう入力ポーズの骨間距離を基準にする。
            const math::Vector3 pA_fk = boneGoA->transform.worldPosition;
            const math::Vector3 pB_fk = boneGoB->transform.worldPosition;
            const math::Vector3 pC_fk = boneGoC->transform.worldPosition;
            const math::Vector3 pA = bodyState.hasSolvedPose[nA]
                ? bodyState.solvedPositions[nA] : pA_fk;
            const math::Vector3 pBSource = bodyState.hasSolvedPose[nB]
                ? bodyState.solvedPositions[nB] : pB_fk;
            const math::Vector3 pCSource = bodyState.hasSolvedPose[nC]
                ? bodyState.solvedPositions[nC] : pC_fk;

            /// @note 骨長は FK 位置から計算する。
            const float LA = (pBSource - pA).Length();
            const float LB = (pCSource - pBSource).Length();
            if (LA < math::EPSILON || LB < math::EPSILON) continue;
            const float totalLength = LA + LB;

            /// @note TwoBone はターゲット GameObject と任意オフセットをゴールにする。FootPlace
            /// @note 固有の地形判定は混ぜず、汎用チェーンの入力を部位非依存に保つ。
            const math::Vector3 pT = targetGO->transform.worldPosition + chain.targetOffset;

            math::Vector3 pP     = math::Vector3::ZERO;
            bool          hasPole = chain.poleEntity.IsValid();
            math::Vector3 autoPoleDir = math::Vector3::ZERO;
            bool          hasAutoPoleDir = false;
            if (hasPole) {
                GameObject* poleGO = scene.GetGameObject(chain.poleEntity);
                if (poleGO) pP = poleGO->transform.worldPosition;
                else        hasPole = false;
            }

            /// @note ゴール距離の決定。
            /// @note 1) Soft IK で D_max への漸近を滑らかにする。
            /// @note 2) D_max で固くクランプ
            /// @note 3) 三角形成立に必要な最小距離でクランプ
            const float D_max = totalLength * math::Clamp(chain.maxExtension, 0.5f, 1.0f);

            const math::Vector3 vecAT    = pT - pA;
            const float         vecATLen = vecAT.Length();

            /// @note C: Soft IK を適用し、D_max に近づくほど段階的に減速する。
            float D = ApplySoftIK(vecATLen, D_max, chain.softness);

            const float dMin = math::Abs(LA - LB) + math::EPSILON;
            D = math::Clamp(D, dMin, D_max - math::EPSILON);

            /// @note 関節の屈曲角から到達距離を逆算し、肘・膝の過伸展と逆折れを防ぐ。0 度を
            /// @note 完全伸展として余弦定理 D^2=LA^2+LB^2+2*LA*LB*cos(theta) を使う。
            const float minBend = math::Clamp(
                std::min(chain.minBendAngleDegrees, chain.maxBendAngleDegrees),
                0.0f, 179.0f) * (math::PI / 180.0f);
            const float maxBend = math::Clamp(
                std::max(chain.minBendAngleDegrees, chain.maxBendAngleDegrees),
                0.0f, 179.0f) * (math::PI / 180.0f);
            const float bendCosine = math::Clamp(
                (D * D - LA * LA - LB * LB) / (2.0f * LA * LB), -1.0f, 1.0f);
            const float bendAngle = math::Clamp(std::acos(bendCosine), minBend, maxBend);
            const float constrainedDistance = std::sqrt(math::Max(
                0.0f, LA * LA + LB * LB + 2.0f * LA * LB * std::cos(bendAngle)));
            D = math::Clamp(constrainedDistance, dMin, D_max - math::EPSILON);

            math::Vector3 axisAT = math::Vector3::FORWARD;
            if (vecATLen > math::EPSILON)
                axisAT = vecAT * (1.0f / vecATLen);

            /// @note Auto Pole: axisAT 確定後、Owner 回転込みの指定方向または FK の曲げ方向から
            /// @note 膝の曲げ方向を毎フレーム算出する。Player のように見た目を 180 度回転して
            /// @note 使う場合、FK 曲げ方向だけではキャラクターの前後と一致せず膝が背面へ折れる
            /// @note ことがある (poleEntity が有効なら hasPole=true のままで Auto には入らない)。
            if (!hasPole && chain.autoPole) {
                math::Vector3 bendRaw = math::Vector3::ZERO;
                if (chain.autoPoleLocalDirection.LengthSq() > math::EPSILON * math::EPSILON) {
                    const math::Vector3 ownerDir =
                        (go->transform.worldRotation *
                         chain.autoPoleLocalDirection.Normalized()).Normalized();
                    bendRaw = ownerDir - axisAT * math::Vector3::Dot(ownerDir, axisAT);
                }
                if (bendRaw.LengthSq() <= math::EPSILON * math::EPSILON) {
                    const math::Vector3 bVec = pBSource - pA;
                    bendRaw = bVec - axisAT * math::Vector3::Dot(bVec, axisAT);
                }
                autoPoleDir = bendRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? bendRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
                hasAutoPoleDir = true;
            }

            const math::Vector3 pTEffective = pA + (axisAT * D);

            /// @note コサイン定理で Root ボーンの曲げ角を求める。
            const float cosA =
                math::Clamp((LA * LA + D * D - LB * LB) / (2.0f * LA * D), -1.0f, 1.0f);
            const float sinA = std::sqrt(math::Max(0.0f, 1.0f - cosA * cosA));

            /// @note 曲げ方向。Pole 指定があれば Pole 側、なければ FK の曲げ方向を維持する。
            math::Vector3 bendDir;
            if (hasAutoPoleDir) {
                bendDir = autoPoleDir;
            } else if (hasPole) {
                const math::Vector3 poleVec = pP - pA;
                const math::Vector3 poleRaw =
                    poleVec - axisAT * math::Vector3::Dot(poleVec, axisAT);
                bendDir = poleRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? poleRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
            } else {
                const math::Vector3 bVec    = pBSource - pA;
                const math::Vector3 bendRaw =
                    bVec - axisAT * math::Vector3::Dot(bVec, axisAT);
                bendDir = bendRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? bendRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
            }

            const math::Vector3 pB_ik =
                pA + axisAT * (LA * cosA) + bendDir * (LA * sinA);

            const math::Quaternion rotA_fk = bodyState.hasSolvedPose[nA]
                ? bodyState.solvedRotations[nA] : boneGoA->transform.worldRotation;
            const math::Quaternion rotB_fk = bodyState.hasSolvedPose[nB]
                ? bodyState.solvedRotations[nB] : boneGoB->transform.worldRotation;

            const float weight = math::Clamp01(chain.weight * chainStateWeight);
            math::Vector3 pB_final;
            math::Vector3 pC_final;
            BlendTwoBonePositions(pA, pBSource, pCSource, pB_ik, pTEffective,
                LA, LB, weight, pB_final, pC_final);
            const math::Quaternion deltaA = ClampedFromToRotation(
                (pBSource - pA).Normalized(), (pB_final - pA).Normalized(), maxJointCorrection);
            const math::Quaternion deltaB = ClampedFromToRotation(
                (pCSource - pBSource).Normalized(), (pC_final - pB_final).Normalized(), maxJointCorrection);
            /// @note 反復ごとの角度上限で目標から外れる場合も、位置を同じ回転から組み直して骨長を維持する。
            pB_final = pA + deltaA * (pBSource - pA);
            pC_final = pB_final + deltaB * (pCSource - pBSource);
            const math::Quaternion rotA_final =
                (deltaA * rotA_fk).Normalized();
            const math::Quaternion rotB_final =
                (deltaB * rotB_fk).Normalized();
            const math::Quaternion rotC_fk = bodyState.hasSolvedPose[nC]
                ? bodyState.solvedRotations[nC] : boneGoC->transform.worldRotation;
            math::Quaternion rotC_final = rotC_fk;
            if (chain.type == IKSolverType::HandPlace) {
                const math::Quaternion targetRotation =
                    (targetGO->transform.worldRotation * chain.handRotationOffset).Normalized();
                const float rotationWeight =
                    math::Clamp01(weight * chain.handRotationWeight);
                rotC_final = math::Quaternion::Slerp(
                    rotC_fk, targetRotation, rotationWeight);
            }
            WriteSolvedPose(skeleton, *smr, scene, *animator, ownerInv,
                            nodeA, pA, rotA_final, bodyState);
            WriteSolvedPose(skeleton, *smr, scene, *animator, ownerInv,
                            nodeB, pB_final, rotB_final, bodyState);
            WriteSolvedPose(skeleton, *smr, scene, *animator, ownerInv,
                            nodeC, pC_final, rotC_final, bodyState);

            const math::Quaternion tipRotationDelta = chain.type == IKSolverType::HandPlace
                ? (rotC_final * rotC_fk.Inverse()).Normalized()
                : math::Quaternion::Identity();
            const math::Vector3 inheritedDisplacement = pCSource - pC_fk;
            for (int child : skeleton.nodes[nC].children) {
                ApplyRigidSubtree(skeleton, *smr, scene, *animator, ownerInv, child,
                                  pCSource, pC_final, tipRotationDelta,
                                  inheritedDisplacement, bodyState);
            }

            anyChainModified = true;
            markSolved(chainPtr);
        }

        if (fullBodyBiped) {
            if (!fullBodyConverged) {
                completedFullBodyPasses = solverPassCount;
                fullBodyError = calculateEffectorError();
                fullBodyConverged =
                    fullBodyError <= std::max(fullBodyBiped->fullBodyTolerance, 0.0001f);
            }
            ik->runtimeFullBodyIterations = completedFullBodyPasses;
            ik->runtimeFullBodyError = fullBodyError;
            ik->runtimeFullBodyConverged = fullBodyConverged;
        }

        ik->runtimeLeftFootGrounded = bodyState.leftFootGrounded;
        ik->runtimeRightFootGrounded = bodyState.rightFootGrounded;
        ik->runtimeHipOffset = bodyState.appliedHipOffset;
        ik->runtimeSkinningUploaded = anyChainModified && animator->skinningBuffer.IsValid();
        if (ik->runtimeSkinningUploaded)
            UploadBoneMatrices(*animator, resources);
    }
}

}
