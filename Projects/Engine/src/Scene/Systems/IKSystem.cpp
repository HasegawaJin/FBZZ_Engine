// FBZZ Engine
// IKSystem.cpp | fbzz::scene
// AnimatorSystem の FK 結果に解析的 2-Bone IK を後処理として適用する。
// WHY: IK はアニメーションの後段で骨行列だけを補正し、既存の GameObject 階層と
//      AnimatorSystem の責務を崩さずに足接地と膝方向制御を実現する。
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/MathUtils.hpp>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace fbzz::scene {

namespace {

struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

struct GroundHit {
    math::Vector3 point  = math::Vector3::ZERO;
    math::Vector3 normal = math::Vector3::UP;
    float         distance = 0.0f;
};

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ArbitraryPerpendicular(const math::Vector3& axis)
{
    math::Vector3 perp = math::Vector3::Cross(math::Vector3::RIGHT, axis);
    if (perp.LengthSq() < math::EPSILON * math::EPSILON)
        perp = math::Vector3::Cross(math::Vector3::UP, axis);
    return perp.Normalized();
}

math::Quaternion FromToRotation(const math::Vector3& from, const math::Vector3& to)
{
    const math::Vector3 f = from.Normalized();
    const math::Vector3 t = to.Normalized();
    const float d = math::Vector3::Dot(f, t);

    if (d >= 1.0f - math::EPSILON)
        return math::Quaternion::Identity();

    if (d <= -1.0f + math::EPSILON) {
        math::Vector3 axis = math::Vector3::Cross(math::Vector3::RIGHT, f);
        if (axis.LengthSq() < math::EPSILON * math::EPSILON)
            axis = math::Vector3::Cross(math::Vector3::UP, f);
        return math::Quaternion::FromAxisAngle(axis.Normalized(), math::PI);
    }

    const math::Vector3 axis = math::Vector3::Cross(f, t);
    const float w = 1.0f + d;
    return math::Quaternion{ axis.x, axis.y, axis.z, w }.Normalized();
}

// 地面候補コライダーを判定する。
// WHY: キャラクター自身や動的オブジェクトを足 IK の接地面にすると揺れや自己ヒットが起きるため、
//      静的な有効コライダーだけを地面として扱う。
bool IsGroundCandidate(GameObject& go, ColliderComponent& collider)
{
    if (!go.activeSelf() || !collider.enabled || collider.isTrigger)
        return false;
    auto* rb = go.GetComponent<RigidBodyComponent>();
    return !rb || !rb->enabled || !rb->rigidBody || rb->rigidBody->IsStatic();
}

bool RaycastAABB(const math::Vector3& origin,
                 const math::Vector3& dir,
                 float maxDistance,
                 const math::Vector3& center,
                 const math::Vector3& halfExtents,
                 GroundHit& out)
{
    float tMin = 0.0f;
    float tMax = maxDistance;
    math::Vector3 hitNormal = math::Vector3::UP;

    auto TestAxis = [&](float originValue, float dirValue, float minValue, float maxValue,
                        const math::Vector3& negativeNormal,
                        const math::Vector3& positiveNormal) -> bool
    {
        constexpr float EPS = 1e-6f;
        if (std::abs(dirValue) < EPS)
            return originValue >= minValue && originValue <= maxValue;

        float t1 = (minValue - originValue) / dirValue;
        float t2 = (maxValue - originValue) / dirValue;
        math::Vector3 axisNormal = negativeNormal;
        if (t1 > t2) {
            std::swap(t1, t2);
            axisNormal = positiveNormal;
        }

        if (t1 > tMin) {
            tMin = t1;
            hitNormal = axisNormal;
        }
        tMax = std::min(tMax, t2);
        return tMin <= tMax;
    };

    const math::Vector3 min = center - halfExtents;
    const math::Vector3 max = center + halfExtents;
    if (!TestAxis(origin.x, dir.x, min.x, max.x, -math::Vector3::RIGHT, math::Vector3::RIGHT)) return false;
    if (!TestAxis(origin.y, dir.y, min.y, max.y, -math::Vector3::UP,    math::Vector3::UP))    return false;
    if (!TestAxis(origin.z, dir.z, min.z, max.z, -math::Vector3::FORWARD, math::Vector3::FORWARD)) return false;
    if (tMin < 0.0f || tMin > maxDistance) return false;

    out.point    = origin + dir * tMin;
    out.normal   = hitNormal;
    out.distance = tMin;
    return true;
}

bool RaycastOBB(const math::Vector3& origin,
                const math::Vector3& dir,
                float maxDistance,
                const math::Vector3& center,
                const math::Quaternion& rotation,
                const math::Vector3& halfExtents,
                GroundHit& out)
{
    const math::Vector3 axes[3] = {
        rotation * math::Vector3::RIGHT,
        rotation * math::Vector3::UP,
        rotation * math::Vector3::FORWARD
    };

    const math::Vector3 relOrigin = origin - center;
    const math::Vector3 localOrigin = {
        math::Vector3::Dot(relOrigin, axes[0]),
        math::Vector3::Dot(relOrigin, axes[1]),
        math::Vector3::Dot(relOrigin, axes[2])
    };
    const math::Vector3 localDir = {
        math::Vector3::Dot(dir, axes[0]),
        math::Vector3::Dot(dir, axes[1]),
        math::Vector3::Dot(dir, axes[2])
    };

    GroundHit localHit;
    if (!RaycastAABB(localOrigin, localDir, maxDistance,
                     math::Vector3::ZERO, halfExtents, localHit))
        return false;

    out.point    = origin + dir * localHit.distance;
    out.distance = localHit.distance;

    const math::Vector3 worldNormal =
        axes[0] * localHit.normal.x +
        axes[1] * localHit.normal.y +
        axes[2] * localHit.normal.z;
    out.normal = worldNormal.LengthSq() > math::EPSILON * math::EPSILON
        ? worldNormal.Normalized()
        : math::Vector3::UP;
    return true;
}

bool RaycastCollider(GameObject& go,
                     AabbColliderComponent& collider,
                     const math::Vector3& origin,
                     const math::Vector3& dir,
                     float maxDistance,
                     GroundHit& out)
{
    if (!IsGroundCandidate(go, collider)) return false;
    const math::Vector3 center =
        go.transform.position +
        go.transform.rotation * ComponentScale(collider.center, go.transform.worldScale);
    return RaycastAABB(origin, dir, maxDistance, center, collider.size * 0.5f, out);
}

bool RaycastCollider(GameObject& go,
                     BoxColliderComponent& collider,
                     const math::Vector3& origin,
                     const math::Vector3& dir,
                     float maxDistance,
                     GroundHit& out)
{
    if (!IsGroundCandidate(go, collider)) return false;
    const math::Vector3 center =
        go.transform.position +
        go.transform.rotation * ComponentScale(collider.center, go.transform.worldScale);
    return RaycastOBB(origin, dir, maxDistance, center, go.transform.rotation,
                      collider.size * 0.5f, out);
}

bool RaycastGround(Scene& scene,
                   const math::Vector3& origin,
                   const math::Vector3& dir,
                   float maxDistance,
                   GroundHit& out)
{
    bool hit = false;
    GroundHit best;
    best.distance = std::numeric_limits<float>::max();

    auto TryHit = [&](GroundHit candidate)
    {
        if (candidate.distance < best.distance) {
            best = candidate;
            hit  = true;
        }
    };

    for (GameObject& go : scene.GameObjects()) {
        GroundHit candidate;
        if (auto* aabb = go.GetComponent<AabbColliderComponent>())
            if (RaycastCollider(go, *aabb, origin, dir, maxDistance, candidate))
                TryHit(candidate);
        if (auto* box = go.GetComponent<BoxColliderComponent>())
            if (RaycastCollider(go, *box, origin, dir, maxDistance, candidate))
                TryHit(candidate);
    }

    if (hit) out = best;
    return hit;
}

// C: Blender 風の Soft IK 距離変換。
// WHAT: ゴール距離が上限へ近づくほど指数関数で減速し、膝が伸び切る直前の跳ねを抑える。
// WHY: 線形クランプだけでは最大伸長付近で急に止まり、膝が「ピン」と伸びた見た目になりやすい。
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

void SetWorldPosition(GameObject& go, const math::Vector3& worldPosition)
{
    if (auto* parent = go.GetParent()) {
        const math::Quaternion invParentRot = parent->transform.rotation.Inverse();
        const math::Vector3 rel = invParentRot * (worldPosition - parent->transform.position);
        const math::Vector3 parentScale = parent->transform.worldScale;
        go.transform.localPosition = {
            std::abs(parentScale.x) > math::EPSILON ? rel.x / parentScale.x : rel.x,
            std::abs(parentScale.y) > math::EPSILON ? rel.y / parentScale.y : rel.y,
            std::abs(parentScale.z) > math::EPSILON ? rel.z / parentScale.z : rel.z
        };
    } else {
        go.transform.localPosition = worldPosition;
    }
    // IK は同じフレーム内で TransformSystem の再実行前に読むため、world 値も同期する。
    go.transform.position = worldPosition;
}

// E: 脚長ベースの動的レイ高さで地面を問い合わせる。
// WHAT: 脚長から開始高さと最大距離を決め、スケールに依存しない接地判定を行う。
// WHY: 固定距離だとモデルサイズの違いで空振りや誤検出が起きるため、脚長比率で扱う。
bool QueryGroundHit(Scene& scene,
                    const math::Vector3& footFkPosition,
                    float legLength,
                    GroundHit& outHit)
{
    // 開始点は脚長の 40% 上、レイ長は 130% にして斜面や段差でも足元を拾う。
    constexpr float RAY_UP_RATIO   = 0.4f;
    constexpr float RAY_DOWN_RATIO = 1.3f;
    const float rayStartHeight = legLength * RAY_UP_RATIO;
    const float rayDistance    = legLength * RAY_DOWN_RATIO;
    const math::Vector3 rayOrigin = footFkPosition + math::Vector3::UP * rayStartHeight;
    return RaycastGround(scene, rayOrigin, -math::Vector3::UP, rayDistance, outHit);
}

// 地面スナップでターゲット位置を接地点へ更新し、ヒット情報を返す。
// outHit は足首傾き補正 (footNormalAxis が非ゼロのとき) に使用する。
bool UpdateFootTargetFromGround(Scene& scene,
                                GameObject& target,
                                const math::Vector3& footFkPosition,
                                float legLength,
                                GroundHit& outHit)
{
    // 足首ジョイントは足裏より内側にあるため、接地点から法線方向へ少し浮かせる。
    constexpr float FOOT_SURFACE_OFFSET = 0.06f;
    if (!QueryGroundHit(scene, footFkPosition, legLength, outHit)) return false;
    const math::Vector3 targetPosition = outHit.point + outHit.normal * FOOT_SURFACE_OFFSET;
    SetWorldPosition(target, targetPosition);
    return true;
}

void RecalcBoneMatrix(const asset::Skeleton& skeleton,
                      const std::vector<math::Matrix4>& nodeGlobalTransforms,
                      std::vector<math::Matrix4>& boneMatrices,
                      int nodeIndex)
{
    const int boneIndex = skeleton.nodes[static_cast<size_t>(nodeIndex)].boneIndex;
    if (boneIndex < 0 || boneIndex >= static_cast<int>(boneMatrices.size()))
        return;

    const auto& bone = skeleton.bones[static_cast<size_t>(boneIndex)];
    boneMatrices[static_cast<size_t>(boneIndex)] =
        skeleton.rootInverseTransform
      * nodeGlobalTransforms[static_cast<size_t>(nodeIndex)]
      * bone.offsetMatrix;
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

// TipBone 以下の子孫を、TipBone の移動量だけ平行移動して FK ワールド回転を維持する。
// WHY: Knee Pole や Foot の接地回転は膝/足首の補正であり、ToeBase/Toe_End の向きまで
//      直接変えるとつま先が Pole 方向へ引っ張られて見える。子孫は足首の位置移動には
//      追従させるが、回転は AnimatorSystem が確定した FK ワールド姿勢を保つ。
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

        const auto& tf = childGo->transform;
        animator.nodeGlobalTransforms[static_cast<size_t>(childIdx)] =
            ownerInv * math::Matrix4::TRS(
                tf.position + worldDelta,
                tf.rotation,
                tf.worldScale);
        RecalcBoneMatrix(skeleton, animator.nodeGlobalTransforms, animator.boneMatrices, childIdx);

        TranslateDescendantsKeepFkRotation(
            scene, skeleton, smr, animator, ownerInv, childIdx, worldDelta);
    }
}

} // namespace

void IKSystem(Scene& scene, renderer::ResourceManager& resources, float /*dt*/)
{
    const auto span     = scene.GetEntities<IKSolverComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go) continue;

        auto* ik       = go->GetComponent<IKSolverComponent>();
        auto* animator = go->GetComponent<AnimatorComponent>();
        auto* smr      = go->GetComponent<SkinnedMeshRenderer>();
        if (!ik || !ik->enabled || !animator || !smr) continue;
        if (!smr->model || !smr->model->skeleton) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        bool anyChainModified = false;

        // ================================================================
        // D: Hip 鬮倥＆陬懈ｭ｣ pre-pass
        // WHAT: isLeg=true 縺ｮ閼壹メ繧ｧ繝ｼ繝ｳ蜈ｨ縺ｦ縺ｮ謗･蝨ｰ繧ｪ繝輔そ繝・ヨ繧貞庶髮・＠縲・        //       縺昴・蟷ｳ蝮・､蛻・□縺鷹ｪｨ逶､繝懊・繝ｳ繧貞桙逶ｴ遘ｻ蜍輔＆縺帙ｋ縲・        //       縺薙・蛟､縺ｯ蠕檎ｶ壹・ IK 繧ｽ繝ｫ繝悶〒 pA (螟ｧ閻ｿ鬪ｨ譬ｹ) 縺ｫ繧ょ刈邂励☆繧九・        // WHY: 蟾ｦ蜿ｳ縺ｮ雜ｳ繧ｿ繝ｼ繧ｲ繝・ヨ鬮倥＆縺碁撼蟇ｾ遘ｰ縺ｪ蝮る％縺ｧ縺ｯ迚・・縺御ｼｸ縺ｳ蛻・▲縺ｦ
        //      閹昴Ο繝・け縺瑚ｵｷ縺阪ｋ縲るｪｨ逶､繧貞・縺ｫ荳九￡繧九％縺ｨ縺ｧ荳｡閼壹・蜿ｯ蜍募沺繧堤｢ｺ菫昴☆繧九・        // ================================================================
        float hipOffsetY = 0.0f;
        int   hipNodeIdx = -1;

        if (!ik->hipBoneName.empty()) {
            const auto itHip = skeleton.nodeMap.find(ik->hipBoneName);
            if (itHip != skeleton.nodeMap.end()) {
                hipNodeIdx = itHip->second;

                float offsetSum = 0.0f;
                float legLenSum = 0.0f;
                int   legCount  = 0;

                for (const auto& chain : ik->chains) {
                    if (!chain.enabled || !chain.isLeg || chain.weight <= 0.0f) continue;

                    const auto itA = skeleton.nodeMap.find(chain.rootBoneName);
                    const auto itB = skeleton.nodeMap.find(chain.midBoneName);
                    const auto itC = skeleton.nodeMap.find(chain.tipBoneName);
                    if (itA == skeleton.nodeMap.end() ||
                        itB == skeleton.nodeMap.end() ||
                        itC == skeleton.nodeMap.end()) continue;

                    const size_t nA = static_cast<size_t>(itA->second);
                    const size_t nB = static_cast<size_t>(itB->second);
                    const size_t nC = static_cast<size_t>(itC->second);
                    if (nA >= smr->nodeEntities.size() ||
                        nB >= smr->nodeEntities.size() ||
                        nC >= smr->nodeEntities.size()) continue;

                    const GameObject* bA = scene.GetGameObject(smr->nodeEntities[nA]);
                    const GameObject* bB = scene.GetGameObject(smr->nodeEntities[nB]);
                    const GameObject* bC = scene.GetGameObject(smr->nodeEntities[nC]);
                    if (!bA || !bB || !bC) continue;

                    const float LA_pre = (bB->transform.position - bA->transform.position).Length();
                    const float LB_pre = (bC->transform.position - bB->transform.position).Length();
                    if (LA_pre < math::EPSILON || LB_pre < math::EPSILON) continue;

                    const float legLen = LA_pre + LB_pre;
                    GroundHit hit;
                    if (!QueryGroundHit(scene, bC->transform.position, legLen, hit)) continue;

                    offsetSum += hit.point.y - bC->transform.position.y;
                    legLenSum += legLen;
                    ++legCount;
                }

                if (legCount > 0) {
                    hipOffsetY = offsetSum / static_cast<float>(legCount);

                    // 平均脚長の 40% を上限にして、補正による骨格破綻を防ぐ。
                    const float avgLegLen  = legLenSum / static_cast<float>(legCount);
                    const float maxHipMove = avgLegLen * 0.4f;
                    hipOffsetY = math::Clamp(hipOffsetY, -maxHipMove, maxHipMove);

                    // Hip ボーンの nodeGlobalTransforms と boneMatrices を先行更新する。
                    // WHY: IK ソルブ前に骨盤より上の姿勢を確定し、脚の基点を正しい高さに置く。
                    const size_t nHip = static_cast<size_t>(hipNodeIdx);
                    if (nHip < smr->nodeEntities.size() &&
                        nHip < animator->nodeGlobalTransforms.size()) {
                        const GameObject* bHip = scene.GetGameObject(smr->nodeEntities[nHip]);
                        if (bHip) {
                            math::Vector3 hipWorldPos = bHip->transform.position;
                            hipWorldPos.y += hipOffsetY;
                            animator->nodeGlobalTransforms[nHip] =
                                ownerInv * math::Matrix4::TRS(
                                    hipWorldPos,
                                    bHip->transform.rotation,
                                    bHip->transform.worldScale);
                            RecalcBoneMatrix(skeleton,
                                             animator->nodeGlobalTransforms,
                                             animator->boneMatrices,
                                             hipNodeIdx);
                            anyChainModified = true;
                        }
                    }
                }
            }
        }

        // ================================================================
        // Main IK solve: チェーンごとに解析的 2-Bone IK を解く。
        // ================================================================
        for (auto& chain : ik->chains) {
            if (!chain.enabled || chain.weight <= 0.0f) continue;

            const auto itA = skeleton.nodeMap.find(chain.rootBoneName);
            const auto itB = skeleton.nodeMap.find(chain.midBoneName);
            const auto itC = skeleton.nodeMap.find(chain.tipBoneName);
            assert(itA != skeleton.nodeMap.end() && "IK root bone was not found");
            assert(itB != skeleton.nodeMap.end() && "IK mid bone was not found");
            assert(itC != skeleton.nodeMap.end() && "IK tip bone was not found");

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

            // FK ポーズの位置。骨長は必ず FK 基準から計算する。
            // WHY: Hip オフセット後の pA で骨長を取ると、モデル固有の長さが姿勢依存で揺れる。
            const math::Vector3 pA_fk = boneGoA->transform.position;
            const math::Vector3 pB_fk = boneGoB->transform.position;
            const math::Vector3 pC_fk = boneGoC->transform.position;

            // D: 脚チェーンのみ Hip 補正オフセットを Root 位置に加算する。
            const math::Vector3 pA = pA_fk
                + (chain.isLeg
                    ? math::Vector3{ 0.0f, hipOffsetY, 0.0f }
                    : math::Vector3::ZERO);

            // 骨長は Hip オフセット前の FK 位置から計算する。
            const float LA = (pB_fk - pA_fk).Length();
            const float LB = (pC_fk - pB_fk).Length();
            if (LA < math::EPSILON || LB < math::EPSILON) continue;
            const float totalLength = LA + LB;

            // E: 動的レイ高さで地面スナップを実行してターゲット位置を更新する。
            GroundHit groundHit;
            const bool hasGroundHit =
                chain.useGroundSnap &&
                UpdateFootTargetFromGround(scene, *targetGO, pC_fk, totalLength, groundHit);

            const math::Vector3 pT = targetGO->transform.position + chain.targetOffset;

            math::Vector3 pP     = math::Vector3::ZERO;
            bool          hasPole = chain.poleEntity.IsValid();
            if (hasPole) {
                GameObject* poleGO = scene.GetGameObject(chain.poleEntity);
                if (poleGO) pP = poleGO->transform.position;
                else        hasPole = false;
            }

            // ゴール距離の決定。
            //   1) Soft IK で D_max への漸近を滑らかにする。
            //   2) D_max で固くクランプ
            //   3) 三角形成立に必要な最小距離でクランプ
            const float D_max = totalLength * math::Clamp(chain.maxExtension, 0.5f, 1.0f);

            const math::Vector3 vecAT    = pT - pA;
            const float         vecATLen = vecAT.Length();

            // C: Soft IK を適用し、D_max に近づくほど段階的に減速する。
            float D = ApplySoftIK(vecATLen, D_max, chain.softness);

            const float dMin = math::Abs(LA - LB) + math::EPSILON;
            D = math::Clamp(D, dMin, D_max - math::EPSILON);

            math::Vector3 axisAT = math::Vector3::FORWARD;
            if (vecATLen > math::EPSILON)
                axisAT = vecAT * (1.0f / vecATLen);

            const math::Vector3 pTEffective = pA + (axisAT * D);

            // コサイン定理で Root ボーンの曲げ角を求める。
            const float cosA =
                math::Clamp((LA * LA + D * D - LB * LB) / (2.0f * LA * D), -1.0f, 1.0f);
            const float sinA = std::sqrt(math::Max(0.0f, 1.0f - cosA * cosA));

            // 曲げ方向。Pole 指定があれば Pole 側、なければ FK の曲げ方向を維持する。
            math::Vector3 bendDir;
            if (hasPole) {
                const math::Vector3 poleVec = pP - pA;
                const math::Vector3 poleRaw =
                    poleVec - axisAT * math::Vector3::Dot(poleVec, axisAT);
                bendDir = poleRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? poleRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
            } else {
                const math::Vector3 bVec    = pB_fk - pA;
                const math::Vector3 bendRaw =
                    bVec - axisAT * math::Vector3::Dot(bVec, axisAT);
                bendDir = bendRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? bendRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
            }

            const math::Vector3 pB_ik =
                pA + axisAT * (LA * cosA) + bendDir * (LA * sinA);

            const math::Quaternion rotA_fk = boneGoA->transform.rotation;
            const math::Quaternion rotB_fk = boneGoB->transform.rotation;
            const math::Quaternion rotC_fk = boneGoC->transform.rotation;

            // rotA_ik は FK 骨方向から IK 骨方向へ回す。
            // WHY: 骨長は pA_fk 基準で計算しているため、FromToRotation の from も pA_fk 基準にそろえる。
            const math::Quaternion rotA_ik =
                (FromToRotation((pB_fk - pA_fk).Normalized(),
                                (pB_ik - pA).Normalized()) * rotA_fk).Normalized();
            const math::Quaternion rotB_ik =
                (FromToRotation((pC_fk - pB_fk).Normalized(),
                                (pTEffective - pB_ik).Normalized()) * rotB_fk).Normalized();

            const float weight = math::Clamp01(chain.weight);
            const math::Quaternion rotA_final =
                math::Quaternion::Slerp(rotA_fk, rotA_ik, weight);
            const math::Quaternion rotB_final =
                math::Quaternion::Slerp(rotB_fk, rotB_ik, weight);
            const math::Vector3 pB_final =
                math::Vector3::Lerp(pB_fk, pB_ik, weight);

            // TipBone 回転: FK を基準に、footNormalAxis が指定されている場合のみ斜面傾きを適用する。
            // WHAT: chain.footNormalAxis (foot bone ローカル空間での「足裏が向く方向」) を
            //       groundHit.normal に揃える最小回転を FK 回転に合成し、weight でブレンドする。
            // WHY: 軸方向はリグごとに異なるため IKChain 側でユーザーが指定する。
            //      ゼロベクトルのときは FK を完全維持し、既存の挙動を壊さない。
            //      Pole の影響を受けないよう rotC_fk にだけ適用し、rotB_ik とは無関係に計算する。
            math::Quaternion rotC_final = rotC_fk;
            const float footNormalLenSq = chain.footNormalAxis.LengthSq();
            if (hasGroundHit && footNormalLenSq > math::EPSILON * math::EPSILON) {
                // foot bone のローカル軸をワールド空間へ変換して現在の「足裏方向」を得る。
                const math::Vector3 footFloorDir =
                    (rotC_fk * chain.footNormalAxis.Normalized()).Normalized();
                // footFloorDir を groundHit.normal に揃える最小回転を合成する。
                const math::Quaternion groundAlign =
                    FromToRotation(footFloorDir, groundHit.normal);
                const math::Quaternion rotC_ground = (groundAlign * rotC_fk).Normalized();
                rotC_final = math::Quaternion::Slerp(rotC_fk, rotC_ground, weight);
            }

            // B: TipBone 位置は FK ↔ IK ターゲットの直線補間で確定する。
            // WHY: pB_final + rotB_final * localCScaled でも weight=1 では pTEffective に等しいが、
            //      weight<1 では rotB_final が Pole 方向に依存するため pC_final が Pole で引っ張られる。
            //      直線補間にすることで全 weight で Pole 非依存になり、
            //      TranslateDescendantsKeepFkRotation の worldDelta も Pole の影響を受けなくなる。
            //      snapTipToTarget=false のときも同じ式を使う。膝位置から再構築する旧式は不要。
            const math::Vector3 pC_final = math::Vector3::Lerp(pC_fk, pTEffective, weight);

            const math::Vector3 scaleA = boneGoA->transform.worldScale;
            const math::Vector3 scaleB = boneGoB->transform.worldScale;
            const math::Vector3 scaleC = boneGoC->transform.worldScale;

            animator->nodeGlobalTransforms[nA] =
                ownerInv * math::Matrix4::TRS(pA,       rotA_final, scaleA);
            animator->nodeGlobalTransforms[nB] =
                ownerInv * math::Matrix4::TRS(pB_final, rotB_final, scaleB);
            animator->nodeGlobalTransforms[nC] =
                ownerInv * math::Matrix4::TRS(pC_final, rotC_final, scaleC);

            RecalcBoneMatrix(skeleton, animator->nodeGlobalTransforms,
                             animator->boneMatrices, nodeA);
            RecalcBoneMatrix(skeleton, animator->nodeGlobalTransforms,
                             animator->boneMatrices, nodeB);
            RecalcBoneMatrix(skeleton, animator->nodeGlobalTransforms,
                             animator->boneMatrices, nodeC);
            TranslateDescendantsKeepFkRotation(
                scene, skeleton, *smr, *animator, ownerInv, nodeC, pC_final - pC_fk);

            anyChainModified = true;
        }

        if (anyChainModified && animator->skinningBuffer.IsValid())
            UploadBoneMatrices(*animator, resources);
    }
}

} // namespace fbzz::scene
