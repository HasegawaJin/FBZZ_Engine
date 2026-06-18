// FBZZ Engine
// IKSystem.cpp | fbzz::scene
// AnimatorSystem の FK 結果に解析的 2-Bone IK を後処理として適用する。
// WHY: IK はアニメーションの後段で骨行列だけを補正し、既存の GameObject 階層と
//      AnimatorSystem の責務を崩さずにターゲット追従と膝方向制御を実現する。
#include <Engine/Scene/Systems/IKSystem.hpp>
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/AnimatorSystem.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
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
// WHY: TipBone の IK 補正を子孫の回転へ直接伝播させると、末端の子ボーンまで
//      Pole 方向へ引っ張られて見える。子孫は TipBone の位置移動には
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
        // WHY: この行列は owner 空間へ戻す前の「ワールド姿勢」として組むため、
        //      local position を使うと ToeBase などの子ボーンが親基準座標をワールド座標として扱われる。
        animator.nodeGlobalTransforms[static_cast<size_t>(childIdx)] =
            ownerInv * math::Matrix4::TRS(
                tf.worldPosition + worldDelta,
                tf.worldRotation,
                tf.worldScale);
        RecalcBoneMatrix(skeleton, animator.nodeGlobalTransforms, animator.boneMatrices, childIdx);

        TranslateDescendantsKeepFkRotation(
            scene, skeleton, smr, animator, ownerInv, childIdx, worldDelta);
    }
}

} // namespace

ComponentAccess IKSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<IKSolverComponent, BoneComponent>()
        .Writes<BoneComponent>();
}

OrderingHints IKSystem::GetOrder() const
{
    return OrderingHints{}.After<AnimatorSystem>();
}

void IKSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;
    Scene& scene = ctx.scene;
    renderer::ResourceManager& resources = *ctx.resources;
    const auto span     = scene.GetEntities<IKSolverComponent>();
    const auto entities = std::vector<EntityID>(span.begin(), span.end());

    for (EntityID id : entities) {
        GameObject* go = scene.GetGameObject(id);
        if (!go) continue;

        auto* ik       = go->GetComponent<IKSolverComponent>();
        auto* animator = go->GetComponent<AnimatorComponent>();
        auto* smr      = go->GetComponent<SkinnedMeshRenderer>();
        // AnimatorComponent は親 GO に、SMR は子 GO に置く構成を許容する。
        // WHY: AnimatorSystem と同じ子探索パターンで階層分離レイアウトに対応する。
        if (!smr) {
            for (int ci = 0, cn = go->GetChildCount(); ci < cn; ++ci) {
                if (auto* child = go->GetChild(ci)) {
                    if (auto* s = child->GetComponent<SkinnedMeshRenderer>()) { smr = s; break; }
                }
            }
        }
        if (!ik || !ik->enabled || !animator || !smr) continue;
        if (!smr->model || !smr->model->skeleton) continue;

        const asset::Skeleton& skeleton = *smr->model->skeleton;
        const math::Matrix4 ownerInv =
            math::Matrix4::Inverse(go->transform.GetWorldMatrix());
        bool anyChainModified = false;
        // ステートごとの IK Weight をクロスフェードを考慮して取得する。
        // IKChain::weight に乗算することで、ステート設定を chain ごとの細かい調整と独立させる。
        const float stateIKWeight = animator->GetCurrentIKWeight();

        // ================================================================
        //      閹昴Ο繝・け縺瑚ｵｷ縺阪ｋ縲るｪｨ逶､繧貞・縺ｫ荳九￡繧九％縺ｨ縺ｧ荳｡閼壹・蜿ｯ蜍募沺繧堤｢ｺ菫昴☆繧九・        // ================================================================
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
            // WHY: IK 解の前後で骨長が揺れないよう、入力ポーズの骨間距離を基準にする。
            const math::Vector3 pA_fk = boneGoA->transform.worldPosition;
            const math::Vector3 pB_fk = boneGoB->transform.worldPosition;
            const math::Vector3 pC_fk = boneGoC->transform.worldPosition;

            const math::Vector3 pA = pA_fk;

            // 骨長は FK 位置から計算する。
            const float LA = (pB_fk - pA_fk).Length();
            const float LB = (pC_fk - pB_fk).Length();
            if (LA < math::EPSILON || LB < math::EPSILON) continue;
            const float totalLength = LA + LB;

            // 汎用 IK はターゲット GameObject と任意オフセットだけをゴールにする。
            // WHY: 足接地補正は FootIKSystem に分離し、IKSolver は部位非依存の 2-Bone 解法へ戻す。
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

            // Auto Pole: axisAT 確定後、Owner 回転込みの指定方向または FK の曲げ方向から
            // 膝の曲げ方向を毎フレーム算出する。
            // WHY: Player のように見た目を 180 度回転して使う場合、FK 曲げ方向だけでは
            //      キャラクターの前後と一致せず膝が背面へ折れることがある。
            //      poleEntity が有効な場合は hasPole=true のままなので Auto には入らない。
            if (!hasPole && chain.autoPole) {
                math::Vector3 bendRaw = math::Vector3::ZERO;
                if (chain.autoPoleLocalDirection.LengthSq() > math::EPSILON * math::EPSILON) {
                    const math::Vector3 ownerDir =
                        (go->transform.worldRotation *
                         chain.autoPoleLocalDirection.Normalized()).Normalized();
                    bendRaw = ownerDir - axisAT * math::Vector3::Dot(ownerDir, axisAT);
                }
                if (bendRaw.LengthSq() <= math::EPSILON * math::EPSILON) {
                    const math::Vector3 bVec = pB_fk - pA;
                    bendRaw = bVec - axisAT * math::Vector3::Dot(bVec, axisAT);
                }
                autoPoleDir = bendRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? bendRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
                hasAutoPoleDir = true;
            }

            const math::Vector3 pTEffective = pA + (axisAT * D);

            // コサイン定理で Root ボーンの曲げ角を求める。
            const float cosA =
                math::Clamp((LA * LA + D * D - LB * LB) / (2.0f * LA * D), -1.0f, 1.0f);
            const float sinA = std::sqrt(math::Max(0.0f, 1.0f - cosA * cosA));

            // 曲げ方向。Pole 指定があれば Pole 側、なければ FK の曲げ方向を維持する。
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
                const math::Vector3 bVec    = pB_fk - pA;
                const math::Vector3 bendRaw =
                    bVec - axisAT * math::Vector3::Dot(bVec, axisAT);
                bendDir = bendRaw.LengthSq() > math::EPSILON * math::EPSILON
                    ? bendRaw.Normalized()
                    : ArbitraryPerpendicular(axisAT);
            }

            const math::Vector3 pB_ik =
                pA + axisAT * (LA * cosA) + bendDir * (LA * sinA);

            const math::Quaternion rotA_fk = boneGoA->transform.worldRotation;
            const math::Quaternion rotB_fk = boneGoB->transform.worldRotation;

            // rotA_ik は FK 骨方向から IK 骨方向へ回す。
            // WHY: 骨長は pA_fk 基準で計算しているため、FromToRotation の from も pA_fk 基準にそろえる。
            const math::Quaternion rotA_ik =
                (FromToRotation((pB_fk - pA_fk).Normalized(),
                                (pB_ik - pA).Normalized()) * rotA_fk).Normalized();
            const math::Quaternion rotB_ik =
                (FromToRotation((pC_fk - pB_fk).Normalized(),
                                (pTEffective - pB_ik).Normalized()) * rotB_fk).Normalized();

            const float weight = math::Clamp01(chain.weight * stateIKWeight);
            const math::Quaternion rotA_final =
                math::Quaternion::Slerp(rotA_fk, rotA_ik, weight);
            const math::Quaternion rotB_final =
                math::Quaternion::Slerp(rotB_fk, rotB_ik, weight);
            const math::Vector3 pB_final =
                math::Vector3::Lerp(pB_fk, pB_ik, weight);

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
                ownerInv * math::Matrix4::TRS(pC_final, boneGoC->transform.worldRotation, scaleC);

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
