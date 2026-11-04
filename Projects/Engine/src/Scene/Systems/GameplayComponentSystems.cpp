/// @file    GameplayComponentSystems.cpp
/// @brief   汎用GameObject Componentの追従、曲線移動、Camera制御、Billboard姿勢を評価する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include "RuntimeMeshUpload.hpp"
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Core/Memory/MakeUnique.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Systems/PresentationGeometry.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/SplineCurve.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {
namespace {

constexpr float DEG_TO_RAD = 0.01745329251994329577f;

float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

math::Vector3 Multiply(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 DivideSafe(const math::Vector3& a, const math::Vector3& b)
{
    return {
        std::abs(b.x) > 0.000001f ? a.x / b.x : 0.0f,
        std::abs(b.y) > 0.000001f ? a.y / b.y : 0.0f,
        std::abs(b.z) > 0.000001f ? a.z / b.z : 0.0f
    };
}

/// @note 数学型の近似比較では微小な実移動の同期を落とすため、変更検出には成分の厳密比較を使う。
bool ExactlyEqual(const math::Vector3& a, const math::Vector3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool ExactlyEqual(const math::Quaternion& a, const math::Quaternion& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool FiniteRotation(const math::Quaternion& value)
{
    const float length = value.Length();
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w)
        && std::isfinite(length) && length >= math::EPSILON;
}

bool ValidLocalHierarchy(const GameObject& object)
{
    for (const auto* current = &object; current; current = current->GetParent()) {
        const auto& pose = current->transform;
        if (!Finite(pose.position) || !Finite(pose.scale) || !FiniteRotation(pose.rotation)) return false;
    }
    return true;
}

bool ValidWorldPose(const Transform& pose)
{
    return Finite(pose.worldPosition) && Finite(pose.worldScale) && FiniteRotation(pose.worldRotation);
}

bool ValidInverseParent(const GameObject& object)
{
    const auto* parent = object.GetParent();
    if (!parent) return true;
    const auto& pose = parent->transform;
    return ValidWorldPose(pose) && std::abs(pose.worldScale.x) > 1.0e-6f
        && std::abs(pose.worldScale.y) > 1.0e-6f && std::abs(pose.worldScale.z) > 1.0e-6f;
}

bool ValidFollowTarget(const GameObject& owner, const GameObject& target)
{
    return target.activeInHierarchy() && &owner != &target && !target.IsDescendantOf(owner)
        && ValidLocalHierarchy(owner) && ValidLocalHierarchy(target);
}

bool NormalizeDirection(const math::Vector3& value, math::Vector3& out)
{
    const double lengthSquared = static_cast<double>(value.x) * value.x
        + static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
    if (!Finite(value) || !std::isfinite(lengthSquared) || lengthSquared <= 1.0e-6) return false;
    const double inverse = 1.0 / std::sqrt(lengthSquared);
    out = {static_cast<float>(value.x * inverse), static_cast<float>(value.y * inverse), static_cast<float>(value.z * inverse)};
    return true;
}

/// @note 入力姿勢は段階の最初に一度確定し、次の段階まで書き込み順を変えずに local へ蓄積する。
/// @see Docs/design/script-transform-contract.md
class WorldTransformSynchronization {
public:
    explicit WorldTransformSynchronization(Scene& scene) : m_scene(scene) {}

    void SynchronizeInputs()
    {
        if (m_synchronized) return;
        FlushWorldTransforms(m_scene);
        m_synchronized = true;
    }

    void MarkChanged(bool changed) { m_changed |= changed; }

    void FlushChanges()
    {
        if (!m_changed) return;
        FlushWorldTransforms(m_scene);
        m_synchronized = true;
        m_changed = false;
    }

private:
    Scene& m_scene;
    bool m_synchronized = false;
    bool m_changed = false;
};

template<typename T>
bool HasActivePoseProducer(Scene& scene)
{
    for (EntityID id : scene.GetEntities<T>()) {
        const auto* component = scene.GetComponent<T>(id);
        const auto* object = scene.GetGameObject(id);
        if (component && component->enabled && object && object->activeInHierarchy())
            return true;
    }
    return false;
}

/// @note Animator の任意 local トラックと骨補正後の非骨子孫は、従来の Constraint 前の同期境界を必要とする。
/// @note Ragdoll は GPU 資源なしでも姿勢を書けるため、資源を要求する他の生成側と分ける。
/// @see Docs/design/script-transform-contract.md
bool RequiresAnimationSynchronization(const SystemContext& ctx)
{
    return HasActivePoseProducer<RagdollComponent>(ctx.scene)
        || (ctx.resources && (HasActivePoseProducer<AnimatorComponent>(ctx.scene)
            || HasActivePoseProducer<IKSolverComponent>(ctx.scene)
            || HasActivePoseProducer<SpringBoneComponent>(ctx.scene)));
}

bool SetWorldPosition(GameObject& go, const math::Vector3& value)
{
    const math::Vector3 previous = go.transform.position;
    if (GameObject* parent = go.GetParent()) {
        const math::Vector3 delta = parent->transform.worldRotation.Inverse()
            * (value - parent->transform.worldPosition);
        go.transform.position = DivideSafe(delta, parent->transform.worldScale);
    } else {
        go.transform.position = value;
    }
    return !ExactlyEqual(previous, go.transform.position);
}

bool SetWorldRotation(GameObject& go, const math::Quaternion& value)
{
    const math::Quaternion previous = go.transform.rotation;
    if (GameObject* parent = go.GetParent())
        go.transform.rotation = (parent->transform.worldRotation.Inverse() * value).Normalized();
    else
        go.transform.rotation = value.Normalized();
    return !ExactlyEqual(previous, go.transform.rotation);
}

bool SetWorldScale(GameObject& go, const math::Vector3& value)
{
    const math::Vector3 previous = go.transform.scale;
    if (GameObject* parent = go.GetParent())
        go.transform.scale = DivideSafe(value, parent->transform.worldScale);
    else
        go.transform.scale = value;
    return !ExactlyEqual(previous, go.transform.scale);
}

/// @note FBX の骨名と手動配置ノード名の両方をソケット名として扱う。
[[nodiscard]] bool MatchesSocket(GameObject& node, const std::string& socketName)
{
    if (socketName.empty())
        return false;
    if (node.name == socketName)
        return true;
    const auto* bone = node.GetComponent<BoneComponent>();
    return bone && bone->boneName == socketName;
}

GameObject* FindSocket(GameObject* root, const std::string& socketName)
{
    if (!root)
        return nullptr;
    /// @note 空名は「target 自身に付ける」の意味。従来動作なのでここだけ空を許す。
    if (socketName.empty() || MatchesSocket(*root, socketName))
        return root;
    for (int index = 0; index < root->GetChildCount(); ++index) {
        if (GameObject* result = FindSocket(root->GetChild(index), socketName))
            return result;
    }
    return nullptr;
}

/// @note Prefab 内では追従先を省略でき、同名ソケットは最も近い祖先側から決定的に選ぶ。
/// @note 自分側の部分木は同名ソケットによる自己参照を防ぐため除外する。
GameObject* FindSocketInAncestors(GameObject& self, const std::string& socketName)
{
    if (socketName.empty())
        return nullptr;
    GameObject* visited = &self;
    for (GameObject* node = self.GetParent(); node;
         visited = node, node = node->GetParent()) {
        if (MatchesSocket(*node, socketName))
            return node;
        for (int index = 0; index < node->GetChildCount(); ++index) {
            GameObject* child = node->GetChild(index);
            if (!child || child == visited)
                continue;
            if (GameObject* result = FindSocket(child, socketName))
                return result;
        }
    }
    return nullptr;
}

float SmoothFactor(float damping, float dt)
{
    return damping <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / damping);
}

float HashNoise(float value, int seed)
{
    const float sample = std::sin(value * 12.9898f + static_cast<float>(seed) * 78.233f) * 43758.5453f;
    return (sample - std::floor(sample)) * 2.0f - 1.0f;
}

GameObject* FindMainCamera(Scene& scene)
{
    for (EntityID id : scene.GetEntities<CameraComponent>()) {
        GameObject* go = scene.GetGameObject(id);
        const auto* camera = scene.GetComponent<CameraComponent>(id);
        if (go && camera && go->activeInHierarchy() && camera->enabled && camera->isMain)
            return go;
    }
    return nullptr;
}

} ///< @note namespace

ComponentAccess ConstraintSystem::GetAccess() const
{
    return ComponentAccess{}.Reads<BoneComponent, AnimatorComponent, IKSolverComponent, SpringBoneComponent, RagdollComponent>()
        .Writes<SocketAttachmentComponent, TransformConstraintComponent, Transform>();
}
ComponentAccess SplineSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
ComponentAccess CameraRigSystem::GetAccess() const
{
    return ComponentAccess{}.Writes<CameraComponent, VirtualCameraComponent, CameraFollowComponent,
        CameraBlendComponent, CameraShakeComponent, Transform>();
}
ComponentAccess BillboardSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
ComponentAccess PresentationSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
OrderingHints PresentationSystem::GetOrder() const
{
    return OrderingHints{}.After<TransformLateUpdate>().After<AnimatorSystem>();
}

void ConstraintSystem::Update(SystemContext& ctx)
{
    WorldTransformSynchronization transforms(ctx.scene);
    if (RequiresAnimationSynchronization(ctx)) transforms.SynchronizeInputs();
    for (EntityID id : ctx.scene.GetEntities<SocketAttachmentComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* attachment = ctx.scene.GetComponent<SocketAttachmentComponent>(id);
        if (!go || !attachment || !attachment->enabled || !go->activeInHierarchy())
            continue;
        GameObject* targetRoot = attachment->target.Resolve(ctx.scene);

        /// @note 追従先が書き換わった瞬間だけを「切り替え」として拾う (イベント通知ではなく
        /// @note 差分検出)。呼び出し側は socketName へ代入するだけでよく、通知の取りこぼしで
        /// @note 補間が始まらないまま行き先だけ変わる (瞬間移動する) 事態も起きない。
        if (attachment->socketName != attachment->appliedSocketName) {
            attachment->blendFromSocketName = attachment->appliedSocketName;
            attachment->appliedSocketName   = attachment->socketName;
            /// @note 初回 (補間元が無い) と編集中はスナップする。編集中に補間を進めると、
            /// @note Play していないのに Inspector の値が毎フレーム変わって見える。
            attachment->blendRemaining =
                (attachment->blendFromSocketName.empty() || !ctx.simulating)
                    ? 0.0f
                    : std::max(attachment->blendDuration, 0.0f);
        }

        /// @note target が指定されていればその部分木から、省略されていれば自分の祖先から探す。
        /// @note どちらの経路でも「見つかったソケットのワールド姿勢に合わせる」以降は同一。
        const auto resolveSocket = [&](const std::string& name) -> GameObject* {
            if (attachment->target.IsAssigned()) return targetRoot ? FindSocket(targetRoot, name) : nullptr;
            return FindSocketInAncestors(*go, name);
        };

        GameObject* socket = resolveSocket(attachment->socketName);
        if (!socket) {
            /// @note 見つからない場合は探索経路 (Target subtree / ancestors) まで含めて警告する。
            /// @note 黙って抜けると綴り違い・別枝配置・Target 指定漏れが区別できない。
            /// @note 解決は毎フレーム試みるため、name+socketName の組で 1 度だけ出す
            /// @note (毎フレーム出すと Console が埋まる)。
            if (!attachment->socketName.empty()) {
                static std::unordered_set<std::string> reported;
                if (reported.insert(go->name + '\n' + attachment->socketName).second) {
                    FBZZ_LOG_WARN("SocketAttachment: socket '%s' not found for [%s] (searched %s)",
                                  attachment->socketName.c_str(), go->name.c_str(),
                                  targetRoot ? "Target subtree" : "ancestors");
                }
            }
            continue;
        }
        if (!ValidFollowTarget(*go, *socket) || !Finite(attachment->positionOffset)
            || !Finite(attachment->rotationOffsetDegrees) || !Finite(attachment->scaleMultiplier)) continue;
        transforms.SynchronizeInputs();
        if (!ValidWorldPose(go->transform) || !ValidWorldPose(socket->transform) || !ValidInverseParent(*go)) continue;
        const math::Quaternion offsetRotation =
            math::Quaternion::FromEuler(attachment->rotationOffsetDegrees * DEG_TO_RAD);

        /// @note ソケット 1 つぶんの「合わせたいワールド姿勢」。オフセットまで畳んだ形で返す。
        const auto poseOf = [&](const GameObject& s,
                                math::Vector3& position,
                                math::Quaternion& rotation,
                                math::Vector3& scale) {
            position = s.transform.worldPosition
                + s.transform.worldRotation * attachment->positionOffset;
            rotation = (s.transform.worldRotation * offsetRotation).Normalized();
            scale    = Multiply(s.transform.worldScale, attachment->scaleMultiplier);
        };

        math::Vector3    desiredPosition;
        math::Quaternion desiredRotation;
        math::Vector3    desiredScale;
        poseOf(*socket, desiredPosition, desiredRotation, desiredScale);

        /// @note 切り替え中は旧ソケットと新ソケットの「その瞬間の」姿勢を混ぜる。
        /// @note 両方ともアニメーションで動き続けるので、キャラが歩いていても置き去りにならない。
        if (attachment->blendRemaining > 0.0f) {
            attachment->blendRemaining =
                std::max(0.0f, attachment->blendRemaining - std::max(ctx.dt, 0.0f));
            GameObject* from = resolveSocket(attachment->blendFromSocketName);
            if (from && ValidWorldPose(from->transform) && attachment->blendDuration > 0.0f) {
                const float linear =
                    Clamp01(1.0f - attachment->blendRemaining / attachment->blendDuration);
                /// @note smoothstep。等速で移すと出だしと着地が硬く、手に「置いた」感が出ない。
                const float t = linear * linear * (3.0f - 2.0f * linear);
                math::Vector3    fromPosition;
                math::Quaternion fromRotation;
                math::Vector3    fromScale;
                poseOf(*from, fromPosition, fromRotation, fromScale);
                desiredPosition = math::Vector3::Lerp(fromPosition, desiredPosition, t);
                desiredRotation =
                    math::Quaternion::Slerp(fromRotation, desiredRotation, t).Normalized();
                desiredScale    = math::Vector3::Lerp(fromScale, desiredScale, t);
            } else {
                /// @note 旧ソケットが消えた / 補間時間が 0。追いかけようがないので打ち切る。
                attachment->blendRemaining = 0.0f;
            }
        }

        /// @note 自分側の合わせ点。「この子ソケットが相手ソケットに重なる」ように原点をずらす。
        /// @note 相対姿勢は自分のローカル空間で測るため、自分自身の現在姿勢には依存しない。
        if (!attachment->localSocketName.empty()) {
            GameObject* localSocket = FindSocket(go, attachment->localSocketName);
            if (localSocket && localSocket != go) {
                if (!ValidWorldPose(localSocket->transform)) continue;
                const math::Quaternion selfInverse = go->transform.worldRotation.Inverse();
                const math::Quaternion localRotation =
                    (selfInverse * localSocket->transform.worldRotation).Normalized();
                const math::Vector3 localPosition = selfInverse
                    * (localSocket->transform.worldPosition - go->transform.worldPosition);
                desiredRotation = (desiredRotation * localRotation.Inverse()).Normalized();
                desiredPosition = desiredPosition - desiredRotation * localPosition;
            }
        }

        if (!Finite(desiredPosition) || !Finite(desiredScale) || !FiniteRotation(desiredRotation)) continue;
        if (attachment->followPosition)
            transforms.MarkChanged(SetWorldPosition(*go, desiredPosition));
        if (attachment->followRotation)
            transforms.MarkChanged(SetWorldRotation(*go, desiredRotation));
        if (attachment->followScale)
            transforms.MarkChanged(SetWorldScale(*go, desiredScale));
    }
    transforms.FlushChanges();
    for (EntityID id : ctx.scene.GetEntities<TransformConstraintComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* constraint = ctx.scene.GetComponent<TransformConstraintComponent>(id);
        GameObject* target = constraint ? constraint->target.Resolve(ctx.scene) : nullptr;
        if (!go || !constraint || !constraint->enabled || !target || !go->activeInHierarchy()
            || !ValidFollowTarget(*go, *target) || !std::isfinite(constraint->weight)
            || static_cast<int>(constraint->mode) < 0 || static_cast<int>(constraint->mode) > 5
            || (constraint->maintainOffset && (!Finite(constraint->positionOffset)
                || !Finite(constraint->rotationOffsetDegrees) || !Finite(constraint->scaleMultiplier))))
            continue;
        transforms.SynchronizeInputs();
        if (!ValidWorldPose(go->transform) || !ValidWorldPose(target->transform) || !ValidInverseParent(*go)) continue;
        const float weight = Clamp01(constraint->weight);
        const math::Quaternion rotationOffset = constraint->maintainOffset
            ? math::Quaternion::FromEuler(constraint->rotationOffsetDegrees * DEG_TO_RAD)
            : math::Quaternion::Identity();
        const math::Vector3 positionOffset = constraint->maintainOffset
            ? constraint->positionOffset : math::Vector3::ZERO;
        const math::Vector3 scaleMultiplier = constraint->maintainOffset
            ? constraint->scaleMultiplier : math::Vector3::ONE;
        math::Vector3 nextPosition = go->transform.worldPosition;
        math::Quaternion nextRotation = go->transform.worldRotation;
        math::Vector3 nextScale = go->transform.worldScale;
        bool writePosition = false, writeRotation = false, writeScale = false;
        if (constraint->mode == TransformConstraintMode::Parent
            || constraint->mode == TransformConstraintMode::Position) {
            const math::Vector3 desired = target->transform.worldPosition
                + target->transform.worldRotation * positionOffset;
            nextPosition = math::Vector3::Lerp(go->transform.worldPosition, desired, weight);
            writePosition = true;
        }
        if (constraint->mode == TransformConstraintMode::Parent
            || constraint->mode == TransformConstraintMode::Rotation) {
            const math::Quaternion desired = target->transform.worldRotation * rotationOffset;
            nextRotation = math::Quaternion::Slerp(go->transform.worldRotation, desired, weight);
            writeRotation = true;
        }
        if (constraint->mode == TransformConstraintMode::Parent
            || constraint->mode == TransformConstraintMode::Scale) {
            const math::Vector3 desired = Multiply(target->transform.worldScale, scaleMultiplier);
            nextScale = math::Vector3::Lerp(go->transform.worldScale, desired, weight);
            writeScale = true;
        }
        if (constraint->mode == TransformConstraintMode::Aim
            || constraint->mode == TransformConstraintMode::LookAt) {
            if (!Finite(constraint->aimAxis) || !Finite(constraint->upAxis)) continue;
            math::Vector3 direction;
            if (NormalizeDirection(target->transform.worldPosition - go->transform.worldPosition, direction)) {
                math::Vector3 aimAxis = math::Vector3::FORWARD, upAxis = math::Vector3::UP;
                (void)NormalizeDirection(constraint->aimAxis, aimAxis);
                (void)NormalizeDirection(constraint->upAxis, upAxis);
                const math::Quaternion axisCorrection =
                    math::Quaternion::LookRotation(aimAxis, upAxis).Inverse();
                math::Quaternion desired =
                    math::Quaternion::LookRotation(direction, upAxis)
                    * axisCorrection;
                desired *= rotationOffset;
                nextRotation = math::Quaternion::Slerp(go->transform.worldRotation, desired, weight);
                writeRotation = true;
            }
        }
        if (!Finite(nextPosition) || !Finite(nextScale) || !FiniteRotation(nextRotation)) continue;
        if (writePosition) transforms.MarkChanged(SetWorldPosition(*go, nextPosition));
        if (writeRotation) transforms.MarkChanged(SetWorldRotation(*go, nextRotation));
        if (writeScale) transforms.MarkChanged(SetWorldScale(*go, nextScale));
    }
    transforms.FlushChanges();
}

void SplineSystem::Update(SystemContext& ctx)
{
    WorldTransformSynchronization transforms(ctx.scene);
    struct RuntimeSplineCurve {
        std::vector<math::Vector3> points;
        bool closed = false;
    };
    std::unordered_map<std::uint64_t, RuntimeSplineCurve> resolvedPoints;
    for (EntityID id : ctx.scene.GetEntities<SplineFollowerComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* follower = ctx.scene.GetComponent<SplineFollowerComponent>(id);
        GameObject* splineObject = follower ? follower->spline.Resolve(ctx.scene) : nullptr;
        auto* spline = splineObject ? splineObject->GetComponent<SplineComponent>() : nullptr;
        if (!go || !go->activeInHierarchy() || !follower || !follower->enabled || !spline
            || !spline->enabled || !splineObject->activeInHierarchy())
            continue;
        transforms.SynchronizeInputs();
        const std::uint64_t splineKey = (static_cast<std::uint64_t>(splineObject->GetID().generation) << 32)
            | splineObject->GetID().index;
        auto pointsIt = resolvedPoints.find(splineKey);
        if (pointsIt == resolvedPoints.end()) {
            SplineControlPointSnapshot resolved = ResolveSplinePoints(
                ctx.scene, splineObject->GetID(), *spline,
                ctx.playing || ctx.simulating ? ControlPointReadContext::RUNTIME
                                             : ControlPointReadContext::AUTHORING);
            if (!resolved.valid) continue;
            RuntimeSplineCurve curve;
            curve.closed = resolved.closed;
            curve.points.reserve(resolved.points.size());
            for (const SplineControlPointValue& point : resolved.points)
                curve.points.push_back(point.localPosition);
            pointsIt = resolvedPoints.emplace(splineKey, std::move(curve)).first;
        }
        const std::span<const math::Vector3> points{pointsIt->second.points};
        const bool closed = pointsIt->second.closed;
        if (points.size() < 2) continue;
        if (ctx.simulating && follower->playing) {
            const float direction = follower->reverse ? -1.0f : 1.0f;
            follower->normalizedPosition += direction * follower->speed * ctx.dt
                / EstimateSplineCurveLength(points, closed, splineObject->transform.worldScale);
            if (follower->wrapMode == SplineWrapMode::Loop) {
                follower->normalizedPosition -= std::floor(follower->normalizedPosition);
            } else if (follower->wrapMode == SplineWrapMode::PingPong) {
                if (follower->normalizedPosition > 1.0f || follower->normalizedPosition < 0.0f) {
                    follower->normalizedPosition = std::clamp(follower->normalizedPosition, 0.0f, 1.0f);
                    follower->reverse = !follower->reverse;
                }
            } else if (follower->normalizedPosition >= 1.0f || follower->normalizedPosition <= 0.0f) {
                follower->normalizedPosition = std::clamp(follower->normalizedPosition, 0.0f, 1.0f);
                follower->playing = false;
            }
        }
        const math::Vector3 localPosition = EvaluateSplineCurve(points, closed,
            follower->normalizedPosition);
        const math::Vector3 worldPosition = splineObject->transform.worldPosition
            + splineObject->transform.worldRotation * Multiply(localPosition, splineObject->transform.worldScale);
        transforms.MarkChanged(SetWorldPosition(*go, worldPosition));
        if (follower->orientToPath) {
            const math::Vector3 tangent = splineObject->transform.worldRotation
                * Multiply(EvaluateSplineCurveTangent(points, closed,
                    follower->normalizedPosition), splineObject->transform.worldScale);
            if (tangent.LengthSq() > 0.000001f)
                transforms.MarkChanged(SetWorldRotation(*go,
                    math::Quaternion::LookRotation(tangent.Normalized())));
        }
    }
    transforms.FlushChanges();
}

void CameraRigSystem::Update(SystemContext& ctx)
{
    WorldTransformSynchronization transforms(ctx.scene);
    GameObject* selected = nullptr;
    int selectedPriority = -2147483647;
    for (EntityID id : ctx.scene.GetEntities<VirtualCameraComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* virtualCamera = ctx.scene.GetComponent<VirtualCameraComponent>(id);
        if (!go || !virtualCamera)
            continue;
        virtualCamera->active = false;
        if (virtualCamera->enabled && go->GetComponent<CameraComponent>()
            && go->activeInHierarchy()
            && virtualCamera->priority > selectedPriority) {
            selected = go;
            selectedPriority = virtualCamera->priority;
        }
    }
    if (selected) {
        if (auto* state = selected->GetComponent<VirtualCameraComponent>())
            state->active = true;
        for (EntityID id : ctx.scene.GetEntities<CameraComponent>()) {
            GameObject* go = ctx.scene.GetGameObject(id);
            auto* camera = ctx.scene.GetComponent<CameraComponent>(id);
            if (!go || !camera)
                continue;
            camera->isMain = go == selected;
            if (go == selected)
                camera->fovY = selected->GetComponent<VirtualCameraComponent>()->fovY;
        }
    }
    for (EntityID id : ctx.scene.GetEntities<CameraFollowComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* follow = ctx.scene.GetComponent<CameraFollowComponent>(id);
        GameObject* target = follow ? follow->target.Resolve(ctx.scene) : nullptr;
        if (!go || !go->activeInHierarchy() || !follow || !follow->enabled || !target
            || !ValidFollowTarget(*go, *target) || !Finite(follow->offset)
            || !std::isfinite(follow->positionDamping) || !std::isfinite(follow->rotationDamping)
            || !std::isfinite(ctx.dt))
            continue;
        transforms.SynchronizeInputs();
        if (!ValidWorldPose(go->transform) || !ValidWorldPose(target->transform) || !ValidInverseParent(*go)) continue;
        const math::Vector3 targetPosition = target->transform.worldPosition;
        const math::Quaternion targetRotation = target->transform.worldRotation;
        const math::Vector3 offset = follow->useTargetRotation
            ? targetRotation * follow->offset : follow->offset;
        const math::Vector3 desired = targetPosition + offset;
        const auto nextPosition = math::Vector3::Lerp(go->transform.worldPosition, desired,
            SmoothFactor(follow->positionDamping, ctx.dt));
        math::Quaternion nextRotation = go->transform.worldRotation;
        if (!Finite(desired) || !Finite(nextPosition)) continue;
        if (follow->lookAtTarget) {
            math::Vector3 direction;
            if (NormalizeDirection(targetPosition - desired, direction))
                nextRotation = math::Quaternion::Slerp(go->transform.worldRotation,
                    math::Quaternion::LookRotation(direction), SmoothFactor(follow->rotationDamping, ctx.dt));
        }
        if (!FiniteRotation(nextRotation)) continue;
        transforms.MarkChanged(SetWorldPosition(*go, nextPosition));
        if (follow->lookAtTarget) transforms.MarkChanged(SetWorldRotation(*go, nextRotation));
    }
    for (EntityID id : ctx.scene.GetEntities<CameraBlendComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* blend = ctx.scene.GetComponent<CameraBlendComponent>(id);
        GameObject* from = blend ? blend->fromCamera.Resolve(ctx.scene) : nullptr;
        GameObject* to = blend ? blend->toCamera.Resolve(ctx.scene) : nullptr;
        if (!go || !go->activeInHierarchy() || !blend || !blend->enabled || !blend->playing || !from || !to)
            continue;
        transforms.SynchronizeInputs();
        blend->elapsed += ctx.dt;
        float t = blend->curve == CameraBlendCurve::Cut ? 1.0f
            : Clamp01(blend->elapsed / std::max(blend->duration, 0.0001f));
        if (blend->curve == CameraBlendCurve::EaseInOut)
            t = t * t * (3.0f - 2.0f * t);
        transforms.MarkChanged(SetWorldPosition(*go,
            math::Vector3::Lerp(from->transform.worldPosition, to->transform.worldPosition, t)));
        transforms.MarkChanged(SetWorldRotation(*go,
            math::Quaternion::Slerp(from->transform.worldRotation, to->transform.worldRotation, t)));
        if (auto* camera = go->GetComponent<CameraComponent>()) {
            const auto* fromCamera = from->GetComponent<CameraComponent>();
            const auto* toCamera = to->GetComponent<CameraComponent>();
            if (fromCamera && toCamera)
                camera->fovY = fromCamera->fovY + (toCamera->fovY - fromCamera->fovY) * t;
        }
        if (blend->elapsed >= blend->duration)
            blend->playing = false;
    }
    for (EntityID id : ctx.scene.GetEntities<CameraShakeComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* shake = ctx.scene.GetComponent<CameraShakeComponent>(id);
        if (!go || !shake)
            continue;
        if (!shake->runtimeInitialized) {
            shake->runtimeInitialized = true;
            shake->playing = shake->playing || shake->playOnAwake;
        }
        const bool hasAppliedShake = !ExactlyEqual(shake->appliedPositionOffset, math::Vector3::ZERO)
            || !ExactlyEqual(shake->appliedRotationOffset, math::Quaternion::Identity());
        const bool playing = shake->enabled && shake->playing && go->activeInHierarchy();
        if (!hasAppliedShake && !playing) continue;
        transforms.SynchronizeInputs();
        const math::Vector3 previousPosition = go->transform.position;
        const math::Quaternion previousRotation = go->transform.rotation;
        go->transform.position -= shake->appliedPositionOffset;
        go->transform.rotation = (go->transform.rotation * shake->appliedRotationOffset.Inverse()).Normalized();
        shake->appliedPositionOffset = math::Vector3::ZERO;
        shake->appliedRotationOffset = math::Quaternion::Identity();
        /// @note 前フレームのずらしは無効化されても上で必ず戻す (戻さないとカメラがずれたまま止まる)。進めるのは有効なときだけ。
        if (!playing) {
            transforms.MarkChanged(!ExactlyEqual(previousPosition, go->transform.position)
                || !ExactlyEqual(previousRotation, go->transform.rotation));
            continue;
        }
        shake->elapsed += ctx.dt;
        const float progress = Clamp01(shake->elapsed / std::max(shake->duration, 0.0001f));
        const float strength = std::pow(1.0f - progress, std::max(shake->falloffPower, 0.0f));
        const float phase = shake->elapsed * shake->frequency;
        shake->appliedPositionOffset = {
            HashNoise(phase, shake->seed) * shake->amplitude * strength,
            HashNoise(phase, shake->seed + 1) * shake->amplitude * strength,
            HashNoise(phase, shake->seed + 2) * shake->amplitude * strength
        };
        const math::Vector3 rotationDegrees = {
            HashNoise(phase, shake->seed + 3) * shake->rotationAmplitudeDegrees * strength,
            HashNoise(phase, shake->seed + 4) * shake->rotationAmplitudeDegrees * strength,
            HashNoise(phase, shake->seed + 5) * shake->rotationAmplitudeDegrees * strength
        };
        shake->appliedRotationOffset = math::Quaternion::FromEuler(rotationDegrees * DEG_TO_RAD);
        go->transform.position += shake->appliedPositionOffset;
        go->transform.rotation = (go->transform.rotation * shake->appliedRotationOffset).Normalized();
        transforms.MarkChanged(!ExactlyEqual(previousPosition, go->transform.position)
            || !ExactlyEqual(previousRotation, go->transform.rotation));
        if (progress >= 1.0f) {
            shake->playing = false;
            shake->elapsed = 0.0f;
        }
    }
    transforms.FlushChanges();
}

void BillboardSystem::Update(SystemContext& ctx)
{
    if (ctx.scene.GetEntities<BillboardComponent>().empty()) return;
    GameObject* camera = FindMainCamera(ctx.scene);
    if (!camera)
        return;
    WorldTransformSynchronization transforms(ctx.scene);
    for (EntityID id : ctx.scene.GetEntities<BillboardComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* billboard = ctx.scene.GetComponent<BillboardComponent>(id);
        if (!go || !billboard || !billboard->enabled || !go->activeInHierarchy())
            continue;
        transforms.SynchronizeInputs();
        if (billboard->mode == BillboardMode::MatchCamera) {
            transforms.MarkChanged(SetWorldRotation(*go, camera->transform.worldRotation));
            continue;
        }
        math::Vector3 direction = camera->transform.worldPosition - go->transform.worldPosition;
        if (billboard->reverseForward)
            direction = -direction;
        if (billboard->mode == BillboardMode::YAxisOnly)
            direction.y = 0.0f;
        if (direction.LengthSq() > 0.000001f)
            transforms.MarkChanged(SetWorldRotation(*go,
                math::Quaternion::LookRotation(direction.Normalized())));
    }
    transforms.FlushChanges();
}

void PresentationSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources)
        return;
    renderer::ResourceManager& resources = *ctx.resources;
    GameObject* mainCamera = FindMainCamera(ctx.scene);
    /// @note 2 枚を交互に使い、確保済みの容量に収まる限り中身だけ差し替える。
    /// @note 作り直しに戻る条件と、なぜ 1 枚では駄目かは DoubleBufferedMesh のヘッダーを参照。
    /// @note 容量は 2 の冪で確保する。線の頂点数は毎フレーム増減するため、ぴったり確保すると
    /// @note 1 頂点増えただけで作り直しに逆戻りする。
    const auto applyMaterial = [](GameObject& go, const std::string& path,
                                   const math::Vector4& color, const std::string& texture,
                                   int64_t sortValue, bool receiveLighting) {
        auto* material = go.GetComponent<MaterialComponent>();
        if (!material) material = &go.AddComponent<MaterialComponent>();
        material->SetPresentationDefaults(path, color, texture, sortValue, receiveLighting);
    };
    const auto inheritedSort = [](GameObject& go) {
        int64_t result = 0;
        for (GameObject* parent = go.GetParent(); parent; parent = parent->GetParent()) {
            if (const auto* group = parent->GetComponent<SortingGroupComponent>();
                group && group->enabled)
                result += static_cast<int64_t>(group->sortingLayer) * 1000 + group->orderInLayer;
        }
        return result;
    };

    for (EntityID id : ctx.scene.GetEntities<SpriteRendererComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* sprite = ctx.scene.GetComponent<SpriteRendererComponent>(id);
        if (!go || !sprite)
            continue;
        (void)PrepareRuntimeMesh(ctx.scene, id, sprite->runtimeMesh, resources);
        asset::ResolvedSprite resolved;
        resolved.texturePath = sprite->spritePath;
        if (!sprite->spritePath.empty()) {
            uint32_t sourceWidth = 0, sourceHeight = 0;
            (void)asset::StreamedTextureResolver::Engine().ResolveGpuWithSourceSize(
                resources, sprite->spritePath, sourceWidth, sourceHeight);
            resolved = asset::ResolveSpriteReference(sprite->spritePath,
                static_cast<float>(sourceWidth), static_cast<float>(sourceHeight));
            if (sprite->useSpriteNativeSize && resolved.pixelsPerUnit > 0
                && resolved.sizePixels.x > 0 && resolved.sizePixels.y > 0) {
                sprite->size = {resolved.sizePixels.x / resolved.pixelsPerUnit,
                    resolved.sizePixels.y / resolved.pixelsPerUnit};
                sprite->pivot = resolved.pivot;
            }
        }
        if (sprite->drawMode == SpriteDrawMode::Tiled && !resolved.isSpriteReference)
            resolved.uvMax = sprite->size;
        CaptureSpriteGeometryInputs(*sprite, resolved, sprite->runtimeMesh.candidateInputs);
        if (!sprite->runtimeMesh.HasMesh() || sprite->runtimeMesh.geometryInputs != sprite->runtimeMesh.candidateInputs) {
            std::vector<renderer::Vertex> vertices;
            std::vector<uint32_t> indices;
            const bool valid = BuildSpriteGeometry(*sprite, resolved, vertices, indices);
            if (!UploadRuntimeMesh(sprite->runtimeMesh, resources, vertices, indices)) continue;
            sprite->runtimeMesh.geometryValid = valid;
            sprite->runtimeMesh.geometryInputs = sprite->runtimeMesh.candidateInputs;
        }
        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!meshRenderer)
            meshRenderer = &go->AddComponent<MeshRenderer>();
        meshRenderer->mesh = sprite->enabled && sprite->runtimeMesh.geometryValid ? sprite->runtimeMesh.Current() : nullptr;
        applyMaterial(*go, sprite->materialPath, sprite->color, resolved.texturePath,
            inheritedSort(*go) + static_cast<int64_t>(sprite->sortingLayer) * 1000 + sprite->orderInLayer, sprite->receiveLighting);
    }

    for (EntityID id : ctx.scene.GetEntities<LineRendererComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* line = ctx.scene.GetComponent<LineRendererComponent>(id);
        if (!go || !line)
            continue;
        (void)PrepareRuntimeMesh(ctx.scene, id, line->runtimeMesh, resources);
        const auto* cameraPosition = mainCamera ? &mainCamera->transform.worldPosition : nullptr;
        const ControlPointReadContext readContext = ctx.playing
            ? ControlPointReadContext::RUNTIME : ControlPointReadContext::AUTHORING;
        const LineControlPointSnapshot linePoints = ResolveLineControlPoints(ctx.scene, id, *line, readContext);
        if (!linePoints.valid) {
            line->runtimeMesh.geometryValid = false;
            line->runtimeMesh.geometryInputs.clear();
            line->runtimeMesh.candidateInputs.clear();
        } else {
            const LineGeometryView geometry{*line, linePoints.points, linePoints.space};
            CaptureLineGeometryInputs(geometry, go->transform, cameraPosition, line->runtimeMesh.candidateInputs);
            if (!line->runtimeMesh.HasMesh() || line->runtimeMesh.geometryInputs != line->runtimeMesh.candidateInputs) {
                std::vector<renderer::Vertex> vertices;
                std::vector<uint32_t> indices;
                const bool valid = BuildLineGeometry(geometry, go->transform, cameraPosition, vertices, indices);
                if (!UploadRuntimeMesh(line->runtimeMesh, resources, vertices, indices)) continue;
                line->runtimeMesh.geometryValid = valid;
                line->runtimeMesh.geometryInputs = line->runtimeMesh.candidateInputs;
            }
        }
        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!meshRenderer)
            meshRenderer = &go->AddComponent<MeshRenderer>();
        meshRenderer->mesh = line->enabled && line->runtimeMesh.geometryValid ? line->runtimeMesh.Current() : nullptr;
        /// @note 線の全 RGBA は頂点側に置く。材質 tint の既定白で startColor の二重乗算を避ける。
        applyMaterial(*go, line->materialPath, {1, 1, 1, 1}, "",
            inheritedSort(*go) + static_cast<int64_t>(line->sortingLayer) * 1000 + line->orderInLayer, false);
    }

    for (EntityID id : ctx.scene.GetEntities<ProjectorComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* projector = ctx.scene.GetComponent<ProjectorComponent>(id);
        if (!go || !projector)
            continue;
        auto* decal = go->GetComponent<DecalComponent>();
        if (!decal)
            decal = &go->AddComponent<DecalComponent>();
        decal->enabled = projector->enabled;
        decal->receiverLayerMask = static_cast<fbzz::LayerMask>(
            static_cast<uint32_t>(projector->receiverLayerMask));
        decal->albedoColor[0] = projector->color.x;
        decal->albedoColor[1] = projector->color.y;
        decal->albedoColor[2] = projector->color.z;
        decal->albedoColor[3] = projector->color.w;
        asset::MaterialAsset materialAsset;
        if (!projector->materialPath.empty()
            && projector->runtimeLoadedMaterialPath != projector->materialPath
            && asset::LoadMaterialAssetFromFile(projector->materialPath, materialAsset)) {
            /// @note render_path = "decal" の .mat はシェーダーごと DecalComponent へ渡せるが、
            /// @note それ以外はメッシュ用シェーダーを指しデカールパスで描けないため、
            /// @note テクスチャだけ抜いて組み込み描画へ載せる。
            const auto authoringSnapshot = materialAsset.ReadAuthoring();
            const auto& authoring = authoringSnapshot->Data();
            if (authoring.renderPath == asset::RenderPath::Decal) {
                decal->materialPath = projector->materialPath;
            } else {
                decal->materialPath.clear();
                if (auto it = authoring.textures.find("albedo"); it != authoring.textures.end())
                    decal->albedoTexPath = it->second;
                if (auto it = authoring.textures.find("normal"); it != authoring.textures.end())
                    decal->normalTexPath = it->second;
                if (auto it = authoring.textures.find("emissive"); it != authoring.textures.end())
                    decal->emissiveTexPath = it->second;
            }
            projector->runtimeLoadedMaterialPath = projector->materialPath;
        } else if (projector->materialPath.empty() && !projector->runtimeLoadedMaterialPath.empty()) {
            /// @note 割り当てを外したら .mat も外す。残すとテクスチャを消しても材質が描き続ける。
            projector->runtimeLoadedMaterialPath.clear();
            decal->materialPath.clear();
        }
        /// @note .mat 経路では albedoColor が効かないので、投影の濃さは opacity へ回す。
        /// @note 組み込み経路は albedoColor[3] が担うため 1.0 のままにする (二重掛けを避ける)。
        decal->opacity = decal->materialPath.empty()
            ? 1.0f : std::clamp(projector->color.w, 0.0f, 1.0f);
        if (projector->shape == ProjectorShape::Perspective) {
            const float depth = (std::max)(projector->farClip - projector->nearClip, 0.001f);
            const float radius = std::tan(projector->fieldOfView * DEG_TO_RAD * 0.5f)
                * projector->farClip;
            go->transform.scale = { radius * 2.0f, radius * 2.0f, depth };
        }
    }
}

} ///< @note namespace fbzz::scene
