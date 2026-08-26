// FBZZ Engine
// GameplayComponentSystems.cpp | fbzz::scene
// 汎用GameObject Componentの追従、曲線移動、Camera制御、Billboard姿勢を評価する
#include <Engine/Scene/Systems/GameplayComponentSystems.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
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
#include <string>
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

void SetWorldPosition(GameObject& go, const math::Vector3& value)
{
    if (GameObject* parent = go.GetParent()) {
        const math::Vector3 delta = parent->transform.worldRotation.Inverse()
            * (value - parent->transform.worldPosition);
        go.transform.position = DivideSafe(delta, parent->transform.worldScale);
    } else {
        go.transform.position = value;
    }
}

void SetWorldRotation(GameObject& go, const math::Quaternion& value)
{
    if (GameObject* parent = go.GetParent())
        go.transform.rotation = (parent->transform.worldRotation.Inverse() * value).Normalized();
    else
        go.transform.rotation = value.Normalized();
}

void SetWorldScale(GameObject& go, const math::Vector3& value)
{
    if (GameObject* parent = go.GetParent())
        go.transform.scale = DivideSafe(value, parent->transform.worldScale);
    else
        go.transform.scale = value;
}

// この GameObject 自身がソケット名に一致するか。
//
// WHY 名前と BoneComponent の両方を見るか:
//   ソケットは「FBX から生成された骨ノード」のことも「人が手で置いた空の GameObject」の
//   こともある。前者は GameObject 名をリネームされても boneName が原本を保つため、
//   両方を見ないとどちらか一方の運用でだけ引けなくなる。
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
    // 空名は「target 自身に付ける」の意味。従来動作なのでここだけ空を許す。
    if (socketName.empty() || MatchesSocket(*root, socketName))
        return root;
    for (int index = 0; index < root->GetChildCount(); ++index) {
        if (GameObject* result = FindSocket(root->GetChild(index), socketName))
            return result;
    }
    return nullptr;
}

// target 未設定のとき、自分の祖先を根へ向かってたどりながらソケットを探す。
//
// WHY target を必須にしないか:
//   target は EntityRef、つまり「シーン内の特定 GameObject」への参照。Prefab はシーン上の
//   オブジェクトを参照できないので、target が必須である限り「武器 Prefab 自身が追従の
//   宣言を持つ」ことが原理的に成立しない。さらに JsonReflector は参照型を読み書きしない
//   (JsonReflector.hpp 冒頭) ため、Inspector 以外から target を書く手段も無い。
//   結果として「スクリプトが Play 開始時に代入する」以外の経路が塞がれ、編集中だけ追従が
//   成立しない = エディタと再生で配置が食い違う、という状態になっていた。
//   target を省略できるようにすると、宣言をシーンにも Prefab にも保存でき、編集時と実行時が
//   同じ 1 本の計算を通る。
//
// WHY シーン全体の名前検索にしないか:
//   SOCKET_Muzzle は左右の銃にそれぞれ 1 本ずつ存在する。名前がシーン内で一意でない以上、
//   全体検索では「どちらか片方」が返り、しかもどちらが返るかは GameObject の生成順に依存する。
//   祖先方向へ上がりながら探せば必ず「自分から一番近いソケット」が最初に見つかる。
//
// WHY 自分が上がってきた枝を除外するか:
//   自分の部分木にも同名のソケットがあり得る (銃側とキャラ側で同じソケット名を使う運用)。
//   自分側を先に拾うと自分自身へ追従して、その場から動かなくなる。
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

math::Vector3 CatmullRom(const SplineComponent& spline, float normalized)
{
    const int count = static_cast<int>(spline.points.size());
    if (count == 0)
        return math::Vector3::ZERO;
    if (count == 1)
        return spline.points.front();
    const int segmentCount = spline.closed ? count : count - 1;
    const float scaled = Clamp01(normalized) * static_cast<float>(segmentCount);
    const int segment = std::min(static_cast<int>(scaled), segmentCount - 1);
    const float t = scaled - static_cast<float>(segment);
    auto point = [&](int index) -> const math::Vector3& {
        if (spline.closed) {
            index = (index % count + count) % count;
            return spline.points[static_cast<size_t>(index)];
        }
        return spline.points[static_cast<size_t>(std::clamp(index, 0, count - 1))];
    };
    const math::Vector3& p0 = point(segment - 1);
    const math::Vector3& p1 = point(segment);
    const math::Vector3& p2 = point(segment + 1);
    const math::Vector3& p3 = point(segment + 2);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t
        + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2
        + (-p0 + p1 * 3.0f - p2 * 3.0f + p3) * t3) * 0.5f;
}

float EstimateLength(const SplineComponent& spline)
{
    constexpr int SAMPLES = 32;
    float length = 0.0f;
    math::Vector3 previous = CatmullRom(spline, 0.0f);
    for (int index = 1; index <= SAMPLES; ++index) {
        const math::Vector3 current = CatmullRom(spline, static_cast<float>(index) / SAMPLES);
        length += (current - previous).Length();
        previous = current;
    }
    return std::max(length, 0.0001f);
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

} // namespace

ComponentAccess ConstraintSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
ComponentAccess SplineSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
ComponentAccess CameraRigSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
ComponentAccess BillboardSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
ComponentAccess PresentationSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }

void ConstraintSystem::Update(SystemContext& ctx)
{
    FlushWorldTransforms(ctx.scene);
    for (EntityID id : ctx.scene.GetEntities<SocketAttachmentComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* attachment = ctx.scene.GetComponent<SocketAttachmentComponent>(id);
        if (!go || !attachment || !attachment->enabled || !go->activeInHierarchy())
            continue;
        GameObject* targetRoot = attachment->target.Resolve(ctx.scene);

        // 追従先が書き換わった瞬間だけを「切り替え」として拾う。呼び出し側は
        // socketName へ行き先を代入するだけでよく、開始通知を送る必要がない。
        // WHY イベントにしないか: 通知を取りこぼすと補間が始まらないまま行き先だけ
        //      変わり、銃が瞬間移動する。差分検出なら取りこぼしようがない。
        if (attachment->socketName != attachment->appliedSocketName) {
            attachment->blendFromSocketName = attachment->appliedSocketName;
            attachment->appliedSocketName   = attachment->socketName;
            // 初回 (補間元が無い) と編集中はスナップする。編集中に補間を進めると、
            // Play していないのに Inspector の値が毎フレーム変わって見える。
            attachment->blendRemaining =
                (attachment->blendFromSocketName.empty() || !ctx.simulating)
                    ? 0.0f
                    : std::max(attachment->blendDuration, 0.0f);
        }

        // target が指定されていればその部分木から、省略されていれば自分の祖先から探す。
        // どちらの経路でも「見つかったソケットのワールド姿勢に合わせる」以降は同一。
        const auto resolveSocket = [&](const std::string& name) -> GameObject* {
            return targetRoot ? FindSocket(targetRoot, name)
                              : FindSocketInAncestors(*go, name);
        };

        GameObject* socket = resolveSocket(attachment->socketName);
        if (!socket) {
            // WHY 報告するか: ここで黙って抜けると症状は「追従先を切り替えたのに動かない」
            //     だけになり、名前の綴り違い・ソケットが階層の別枝にある・Target の指定漏れ
            //     のどれなのかが画面から区別できない。どの経路で探したかまで残す。
            // WHY 1 度だけか: 解決は毎フレーム試みるので、そのまま出すと Console が
            //     同じ 1 行で埋まり、他のログを押し出す。
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
        const math::Quaternion offsetRotation =
            math::Quaternion::FromEuler(attachment->rotationOffsetDegrees * DEG_TO_RAD);

        // ソケット 1 つぶんの「合わせたいワールド姿勢」。オフセットまで畳んだ形で返す。
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

        // 切り替え中は旧ソケットと新ソケットの「その瞬間の」姿勢を混ぜる。
        // 両方ともアニメーションで動き続けるので、キャラが歩いていても置き去りにならない。
        if (attachment->blendRemaining > 0.0f) {
            attachment->blendRemaining =
                std::max(0.0f, attachment->blendRemaining - std::max(ctx.dt, 0.0f));
            GameObject* from = resolveSocket(attachment->blendFromSocketName);
            if (from && attachment->blendDuration > 0.0f) {
                const float linear =
                    Clamp01(1.0f - attachment->blendRemaining / attachment->blendDuration);
                // smoothstep。等速で移すと出だしと着地が硬く、手に「置いた」感が出ない。
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
                // 旧ソケットが消えた / 補間時間が 0。追いかけようがないので打ち切る。
                attachment->blendRemaining = 0.0f;
            }
        }

        // 自分側の合わせ点。「この子ソケットが相手ソケットに重なる」ように原点をずらす。
        // 相対姿勢は自分のローカル空間で測るため、自分自身の現在姿勢には依存しない。
        if (!attachment->localSocketName.empty()) {
            GameObject* localSocket = FindSocket(go, attachment->localSocketName);
            if (localSocket && localSocket != go) {
                const math::Quaternion selfInverse = go->transform.worldRotation.Inverse();
                const math::Quaternion localRotation =
                    (selfInverse * localSocket->transform.worldRotation).Normalized();
                const math::Vector3 localPosition = selfInverse
                    * (localSocket->transform.worldPosition - go->transform.worldPosition);
                desiredRotation = (desiredRotation * localRotation.Inverse()).Normalized();
                desiredPosition = desiredPosition - desiredRotation * localPosition;
            }
        }

        if (attachment->followPosition)
            SetWorldPosition(*go, desiredPosition);
        if (attachment->followRotation)
            SetWorldRotation(*go, desiredRotation);
        if (attachment->followScale)
            SetWorldScale(*go, desiredScale);
    }
    FlushWorldTransforms(ctx.scene);
    for (EntityID id : ctx.scene.GetEntities<TransformConstraintComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* constraint = ctx.scene.GetComponent<TransformConstraintComponent>(id);
        GameObject* target = constraint ? constraint->target.Resolve(ctx.scene) : nullptr;
        if (!go || !constraint || !constraint->enabled || !target || !go->activeInHierarchy())
            continue;
        const float weight = Clamp01(constraint->weight);
        const math::Quaternion rotationOffset = constraint->maintainOffset
            ? math::Quaternion::FromEuler(constraint->rotationOffsetDegrees * DEG_TO_RAD)
            : math::Quaternion::Identity();
        const math::Vector3 positionOffset = constraint->maintainOffset
            ? constraint->positionOffset : math::Vector3::ZERO;
        const math::Vector3 scaleMultiplier = constraint->maintainOffset
            ? constraint->scaleMultiplier : math::Vector3::ONE;
        if (constraint->mode == TransformConstraintMode::Parent
            || constraint->mode == TransformConstraintMode::Position) {
            const math::Vector3 desired = target->transform.worldPosition
                + target->transform.worldRotation * positionOffset;
            SetWorldPosition(*go, math::Vector3::Lerp(go->transform.worldPosition, desired, weight));
        }
        if (constraint->mode == TransformConstraintMode::Parent
            || constraint->mode == TransformConstraintMode::Rotation) {
            const math::Quaternion desired = target->transform.worldRotation * rotationOffset;
            SetWorldRotation(*go, math::Quaternion::Slerp(go->transform.worldRotation, desired, weight));
        }
        if (constraint->mode == TransformConstraintMode::Parent
            || constraint->mode == TransformConstraintMode::Scale) {
            const math::Vector3 desired = Multiply(target->transform.worldScale, scaleMultiplier);
            SetWorldScale(*go, math::Vector3::Lerp(go->transform.worldScale, desired, weight));
        }
        if (constraint->mode == TransformConstraintMode::Aim
            || constraint->mode == TransformConstraintMode::LookAt) {
            const math::Vector3 direction = target->transform.worldPosition - go->transform.worldPosition;
            if (direction.LengthSq() > 0.000001f) {
                const math::Vector3 aimAxis = constraint->aimAxis.LengthSq() > 0.000001f
                    ? constraint->aimAxis.Normalized() : math::Vector3::FORWARD;
                const math::Quaternion axisCorrection =
                    math::Quaternion::LookRotation(aimAxis, constraint->upAxis).Inverse();
                math::Quaternion desired =
                    math::Quaternion::LookRotation(direction.Normalized(), constraint->upAxis)
                    * axisCorrection;
                desired *= rotationOffset;
                SetWorldRotation(*go, math::Quaternion::Slerp(go->transform.worldRotation, desired, weight));
            }
        }
    }
    FlushWorldTransforms(ctx.scene);
}

void SplineSystem::Update(SystemContext& ctx)
{
    for (EntityID id : ctx.scene.GetEntities<SplineFollowerComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* follower = ctx.scene.GetComponent<SplineFollowerComponent>(id);
        GameObject* splineObject = follower ? follower->spline.Resolve(ctx.scene) : nullptr;
        auto* spline = splineObject ? splineObject->GetComponent<SplineComponent>() : nullptr;
        if (!go || !follower || !follower->enabled || !spline
            || !spline->enabled || spline->points.size() < 2)
            continue;
        if (ctx.simulating && follower->playing) {
            const float direction = follower->reverse ? -1.0f : 1.0f;
            follower->normalizedPosition += direction * follower->speed * ctx.dt / EstimateLength(*spline);
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
        const math::Vector3 localPosition = CatmullRom(*spline, follower->normalizedPosition);
        const math::Vector3 worldPosition = splineObject->transform.worldPosition
            + splineObject->transform.worldRotation * Multiply(localPosition, splineObject->transform.worldScale);
        SetWorldPosition(*go, worldPosition);
        if (follower->orientToPath) {
            const float next = std::min(follower->normalizedPosition + 0.001f, 1.0f);
            const math::Vector3 tangent =
                splineObject->transform.worldRotation * (CatmullRom(*spline, next) - localPosition);
            if (tangent.LengthSq() > 0.000001f)
                SetWorldRotation(*go, math::Quaternion::LookRotation(tangent.Normalized()));
        }
    }
    FlushWorldTransforms(ctx.scene);
}

void CameraRigSystem::Update(SystemContext& ctx)
{
    FlushWorldTransforms(ctx.scene);
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
        if (!go || !follow || !follow->enabled || !target)
            continue;
        const math::Vector3 targetPosition = target->transform.worldPosition;
        const math::Quaternion targetRotation = target->transform.worldRotation;
        const math::Vector3 offset = follow->useTargetRotation
            ? targetRotation * follow->offset : follow->offset;
        const math::Vector3 desired = targetPosition + offset;
        SetWorldPosition(*go, math::Vector3::Lerp(go->transform.worldPosition, desired,
            SmoothFactor(follow->positionDamping, ctx.dt)));
        if (follow->lookAtTarget) {
            const math::Vector3 direction = targetPosition - desired;
            if (direction.LengthSq() > 0.000001f)
                SetWorldRotation(*go, math::Quaternion::Slerp(go->transform.worldRotation,
                    math::Quaternion::LookRotation(direction.Normalized()),
                    SmoothFactor(follow->rotationDamping, ctx.dt)));
        }
    }
    for (EntityID id : ctx.scene.GetEntities<CameraBlendComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* blend = ctx.scene.GetComponent<CameraBlendComponent>(id);
        GameObject* from = blend ? blend->fromCamera.Resolve(ctx.scene) : nullptr;
        GameObject* to = blend ? blend->toCamera.Resolve(ctx.scene) : nullptr;
        if (!go || !blend || !blend->enabled || !blend->playing || !from || !to)
            continue;
        blend->elapsed += ctx.dt;
        float t = blend->curve == CameraBlendCurve::Cut ? 1.0f
            : Clamp01(blend->elapsed / std::max(blend->duration, 0.0001f));
        if (blend->curve == CameraBlendCurve::EaseInOut)
            t = t * t * (3.0f - 2.0f * t);
        SetWorldPosition(*go, math::Vector3::Lerp(from->transform.worldPosition, to->transform.worldPosition, t));
        SetWorldRotation(*go, math::Quaternion::Slerp(from->transform.worldRotation, to->transform.worldRotation, t));
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
        go->transform.position -= shake->appliedPositionOffset;
        go->transform.rotation = (go->transform.rotation * shake->appliedRotationOffset.Inverse()).Normalized();
        shake->appliedPositionOffset = math::Vector3::ZERO;
        shake->appliedRotationOffset = math::Quaternion::Identity();
        if (!shake->enabled || !shake->playing)
            continue;
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
        if (progress >= 1.0f) {
            shake->playing = false;
            shake->elapsed = 0.0f;
        }
    }
    FlushWorldTransforms(ctx.scene);
}

void BillboardSystem::Update(SystemContext& ctx)
{
    GameObject* camera = FindMainCamera(ctx.scene);
    if (!camera)
        return;
    for (EntityID id : ctx.scene.GetEntities<BillboardComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* billboard = ctx.scene.GetComponent<BillboardComponent>(id);
        if (!go || !billboard || !billboard->enabled || !go->activeInHierarchy())
            continue;
        if (billboard->mode == BillboardMode::MatchCamera) {
            SetWorldRotation(*go, camera->transform.worldRotation);
            continue;
        }
        math::Vector3 direction = camera->transform.worldPosition - go->transform.worldPosition;
        if (billboard->reverseForward)
            direction = -direction;
        if (billboard->mode == BillboardMode::YAxisOnly)
            direction.y = 0.0f;
        if (direction.LengthSq() > 0.000001f)
            SetWorldRotation(*go, math::Quaternion::LookRotation(direction.Normalized()));
    }
    FlushWorldTransforms(ctx.scene);
}

void PresentationSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources)
        return;
    renderer::ResourceManager& resources = *ctx.resources;
    GameObject* mainCamera = FindMainCamera(ctx.scene);
    // 2 枚を交互に使い、確保済みの容量に収まる限り中身だけ差し替える。
    // 作り直しに戻る条件と、なぜ 1 枚では駄目かは DoubleBufferedMesh のヘッダーを参照。
    //
    // WHY 容量を 2 の冪で取るか:
    //   線の頂点数は BuildPath が逆極へ届いた時点で打ち切られるぶん毎フレーム増減する。
    //   ぴったり確保すると 1 頂点増えただけで «毎フレーム作り直し» へ逆戻りする。
    const auto uploadMesh = [&](DoubleBufferedMesh& target,
                                std::vector<renderer::Vertex> vertices,
                                std::vector<uint32_t> indices) {
        target.current ^= 1u;
        std::shared_ptr<renderer::Mesh>& slot = target.slots[target.current];
        if (!slot) slot = std::make_shared<renderer::Mesh>();
        renderer::Mesh& mesh = *slot;

        mesh.cpuVertices = std::move(vertices);
        mesh.cpuIndices  = std::move(indices);
        mesh.vertexCount = static_cast<uint32_t>(mesh.cpuVertices.size());
        mesh.indexCount  = static_cast<uint32_t>(mesh.cpuIndices.size());
        mesh.ComputeBounds();

        const auto capacityFor = [](uint32_t needed) {
            uint32_t capacity = 256;
            while (capacity < needed) capacity *= 2;
            return capacity;
        };

        if (!mesh.vertexBuffer.IsValid() || mesh.vertexCapacity < mesh.vertexCount) {
            if (mesh.vertexBuffer.IsValid()) resources.Release(mesh.vertexBuffer);
            mesh.vertexCapacity = capacityFor(mesh.vertexCount);
            mesh.vertexBuffer   = resources.CreateVertexBuffer(
                nullptr, static_cast<size_t>(mesh.vertexCapacity) * sizeof(renderer::Vertex),
                sizeof(renderer::Vertex));
        }
        if (!mesh.indexBuffer.IsValid() || mesh.indexCapacity < mesh.indexCount) {
            if (mesh.indexBuffer.IsValid()) resources.Release(mesh.indexBuffer);
            mesh.indexCapacity = capacityFor(mesh.indexCount);
            mesh.indexBuffer   = resources.CreateIndexBuffer(nullptr, mesh.indexCapacity);
        }

        if (mesh.vertexCount > 0)
            resources.Update(mesh.vertexBuffer, mesh.cpuVertices.data(),
                             static_cast<size_t>(mesh.vertexCount) * sizeof(renderer::Vertex));
        if (mesh.indexCount > 0)
            resources.Update(mesh.indexBuffer, mesh.cpuIndices.data(),
                             static_cast<size_t>(mesh.indexCount) * sizeof(uint32_t));
    };
    const auto applyMaterial = [](GameObject& go, const std::string& path,
                                  const math::Vector4& color, const std::string& texture,
                                  int sortValue) {
        auto* material = go.GetComponent<MaterialComponent>();
        if (!material)
            material = &go.AddComponent<MaterialComponent>();
        material->enabled = true;
        material->materialPath = path;
        material->paramOverrides["albedo"] = { color.x, color.y, color.z, color.w };
        if (!texture.empty())
            material->textureOverrides["albedo"] = texture;
        // WHY 合成方法だけ上書きしないか:
        //   以前はここで毎フレーム ALPHA_BLEND を焼き付けていた。結果、.mat に
        //   blend_mode = "Additive" と書いても通らず、線とスプライトだけ «見た目の正本が
        //   .mat ではない» という状態になっていた。パーティクルで同じ壊れ方を潰したのと
        //   同じ理由 (ParticleMaterialSettings.hpp のヘッダー) で、合成は .mat へ返す。
        //   ここで false へ倒しもしないのは、ScriptMaterialProxy::SetBlendMode で
        //   明示的に指定した側を毎フレーム剥がさないため。
        //
        // WHY 両面と描画キューは上書きし続けるか:
        //   どちらも «.mat には決めようがない» 値である。帯のメッシュは毎フレーム
        //   カメラ向きから組み直すため巻き順が裏返りうるので、片面にすると見る角度で
        //   消える。描画キューは sortingLayer / orderInLayer をキューへ写す仕組みそのもので、
        //   .mat に書かせると同じ .mat を共有する線が全部同じ順序になる。
        material->hasDoubleSidedOverride = true;
        material->doubleSidedOverride = true;
        material->hasRenderQueueOverride = true;
        material->renderQueueOverride = 3000 + sortValue;
    };
    const auto inheritedSort = [](GameObject& go) {
        int result = 0;
        for (GameObject* parent = go.GetParent(); parent; parent = parent->GetParent()) {
            if (const auto* group = parent->GetComponent<SortingGroupComponent>();
                group && group->enabled)
                result += group->sortingLayer * 1000 + group->orderInLayer;
        }
        return result;
    };

    for (EntityID id : ctx.scene.GetEntities<SpriteRendererComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* sprite = ctx.scene.GetComponent<SpriteRendererComponent>(id);
        if (!go || !sprite)
            continue;
        std::string texturePath = sprite->spritePath;
        math::Vector2 uvMin = math::Vector2::ZERO;
        math::Vector2 uvMax = math::Vector2::ONE;
        if (!sprite->spritePath.empty()) {
            const auto textureHandle = resources.LoadTexture(sprite->spritePath);
            const renderer::ITexture* texture = resources.Get(textureHandle);
            const asset::ResolvedSprite resolved = asset::ResolveSpriteReference(
                sprite->spritePath,
                texture ? static_cast<float>(texture->GetWidth())  : 0.0f,
                texture ? static_cast<float>(texture->GetHeight()) : 0.0f);
            texturePath = resolved.texturePath;

            if (resolved.resolved) {
                uvMin = resolved.uvMin;
                uvMax = resolved.uvMax;
            } else if (!resolved.isSpriteReference && sprite->drawMode == SpriteDrawMode::Tiled) {
                uvMax = sprite->size;
            }

            // 素材が持つ寸法と基準点をそのまま採る。1 単位 = pixelsPerUnit ピクセル。
            if (sprite->useSpriteNativeSize && resolved.pixelsPerUnit > 0.0f
                && resolved.sizePixels.x > 0.0f && resolved.sizePixels.y > 0.0f) {
                sprite->size = { resolved.sizePixels.x / resolved.pixelsPerUnit,
                                 resolved.sizePixels.y / resolved.pixelsPerUnit };
                sprite->pivot = resolved.pivot;
            }
        } else if (sprite->drawMode == SpriteDrawMode::Tiled) {
            uvMax = sprite->size;
        }
        std::size_t signature = std::hash<std::string>{}(sprite->spritePath);
        signature ^= std::hash<float>{}(sprite->size.x) + (std::hash<float>{}(sprite->size.y) << 1);
        signature ^= std::hash<float>{}(sprite->pivot.x) + (std::hash<float>{}(sprite->pivot.y) << 1);
        signature ^= static_cast<std::size_t>(sprite->flipX) << 5;
        signature ^= static_cast<std::size_t>(sprite->flipY) << 6;
        signature ^= std::hash<float>{}(uvMin.x + uvMin.y * 7.0f + uvMax.x * 31.0f + uvMax.y * 127.0f);
        if (!sprite->runtimeMesh.HasMesh() || sprite->runtimeMesh.signature != signature) {
            const float left = -sprite->pivot.x * sprite->size.x;
            const float top = (1.0f - sprite->pivot.y) * sprite->size.y;
            const float right = left + sprite->size.x;
            const float bottom = top - sprite->size.y;
            const float u0 = sprite->flipX ? uvMax.x : uvMin.x;
            const float u1 = sprite->flipX ? uvMin.x : uvMax.x;
            const float v0 = sprite->flipY ? uvMax.y : uvMin.y;
            const float v1 = sprite->flipY ? uvMin.y : uvMax.y;
            const math::Vector3 normal = math::Vector3::FORWARD;
            const math::Vector3 tangent = math::Vector3::RIGHT;
            uploadMesh(sprite->runtimeMesh, {
                {{left, top, 0.0f}, normal, tangent, {u0, v0}},
                {{right, top, 0.0f}, normal, tangent, {u1, v0}},
                {{right, bottom, 0.0f}, normal, tangent, {u1, v1}},
                {{left, bottom, 0.0f}, normal, tangent, {u0, v1}}
            }, {0, 1, 2, 0, 2, 3});
            sprite->runtimeMesh.signature = signature;
        }
        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!meshRenderer)
            meshRenderer = &go->AddComponent<MeshRenderer>();
        meshRenderer->mesh = sprite->runtimeMesh.Current();
        meshRenderer->enabled = sprite->enabled;
        applyMaterial(*go, sprite->materialPath, sprite->color, texturePath,
            inheritedSort(*go) + sprite->sortingLayer * 1000 + sprite->orderInLayer);
    }

    for (EntityID id : ctx.scene.GetEntities<LineRendererComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* line = ctx.scene.GetComponent<LineRendererComponent>(id);
        if (!go || !line)
            continue;
        std::size_t signature = line->points.size();
        for (const math::Vector3& point : line->points)
            signature ^= std::hash<float>{}(point.x + point.y * 31.0f + point.z * 997.0f);
        signature ^= std::hash<float>{}(line->startWidth) ^ (std::hash<float>{}(line->endWidth) << 1);
        signature ^= static_cast<std::size_t>(line->loop) << 4;
        signature ^= static_cast<std::size_t>(line->shape) << 6;
        signature ^= static_cast<std::size_t>(line->radialSegments) << 8;
        if (line->space == LineSpace::World)
            signature ^= std::hash<float>{}(go->transform.worldPosition.x
                + go->transform.worldPosition.y * 31.0f + go->transform.worldPosition.z * 997.0f);
        // WHY 筒ではカメラを鍵に混ぜないか: 形が視点に依存しないため、混ぜるとカメラが
        //     動いた «だけ» で毎フレーム焼き直すことになる。板は向きを作り直すので要る。
        if (line->shape == LineShape::Ribbon && line->billboard && mainCamera)
            signature ^= std::hash<float>{}(mainCamera->transform.worldPosition.x
                + mainCamera->transform.worldPosition.y * 31.0f
                + mainCamera->transform.worldPosition.z * 997.0f);
        if ((!line->runtimeMesh.HasMesh() || line->runtimeMesh.signature != signature)
            && line->points.size() >= 2 && line->shape == LineShape::Tube) {
            // ── 筒 ────────────────────────────────────────────────────────────
            // 点ごとに円環を 1 枚置き、隣の環と繋いで押し出す。
            //
            // WHY 平行移動フレームで組むか (毎回 UP から作り直さないか):
            //   環の基準ベクトルを毎回 UP との外積で作ると、線が真上を向いた区間で
            //   基準が反転し、そこだけ筒が 180 度ねじれる。前の環の基準を «軸へ
            //   直交するよう倒し直す» だけにすれば、経路が曲がってもねじれが増えない。
            std::vector<renderer::Vertex> vertices;
            std::vector<uint32_t> indices;
            const int  radial = std::clamp(line->radialSegments, 3, 32);
            const size_t ringCount = line->points.size();
            const size_t spanCount = line->loop ? ringCount : ringCount - 1;

            // 経路をローカルへ落とす。板側と同じ規則。
            std::vector<math::Vector3> path;
            path.reserve(ringCount);
            for (const math::Vector3& point : line->points) {
                path.push_back(line->space == LineSpace::World
                    ? DivideSafe(go->transform.worldRotation.Inverse()
                        * (point - go->transform.worldPosition), go->transform.worldScale)
                    : point);
            }

            // 最初の基準。軸が真上に近いときだけ前方へ倒す (外積が縮退するため)。
            math::Vector3 firstAxis =
                (path.size() > 1 ? (path[1] - path[0]) : math::Vector3::FORWARD)
                    .NormalizedOr(math::Vector3::FORWARD);
            math::Vector3 reference = std::fabs(firstAxis.y) > 0.9f
                ? math::Vector3::FORWARD : math::Vector3::UP;
            math::Vector3 normalRef =
                math::Vector3::Cross(firstAxis, reference).NormalizedOr(math::Vector3::RIGHT);

            for (size_t ring = 0; ring < ringCount; ++ring) {
                // 環の軸は前後の区間の平均。折れ点で筒が角張らない。
                const math::Vector3 back = ring > 0 ? (path[ring] - path[ring - 1])
                                                    : math::Vector3::ZERO;
                const math::Vector3 forward = ring + 1 < ringCount ? (path[ring + 1] - path[ring])
                                                                   : math::Vector3::ZERO;
                const math::Vector3 axis = (back + forward).NormalizedOr(firstAxis);

                // 前の基準を新しい軸へ直交させる (平行移動フレーム)。
                normalRef = (normalRef - axis * math::Vector3::Dot(normalRef, axis))
                                .NormalizedOr(normalRef);
                const math::Vector3 binormal =
                    math::Vector3::Cross(axis, normalRef).NormalizedOr(math::Vector3::UP);

                const float t = ringCount > 1
                    ? static_cast<float>(ring) / static_cast<float>(ringCount - 1) : 0.0f;
                const float w = (line->startWidth + (line->endWidth - line->startWidth) * t) * 0.5f;

                for (int step = 0; step <= radial; ++step) {
                    // 継ぎ目のため最後の 1 本を重ねる (uv が 1 で閉じる)。
                    const float angle = static_cast<float>(step) / static_cast<float>(radial)
                                      * 6.28318530718f;
                    const math::Vector3 outward =
                        normalRef * std::cos(angle) + binormal * std::sin(angle);
                    vertices.push_back({ path[ring] + outward * w, outward, axis,
                                         { t, static_cast<float>(step) / static_cast<float>(radial) } });
                }
            }

            const uint32_t stride = static_cast<uint32_t>(radial) + 1u;
            for (size_t span = 0; span < spanCount; ++span) {
                const uint32_t a = static_cast<uint32_t>(span) * stride;
                const uint32_t b = static_cast<uint32_t>((span + 1) % ringCount) * stride;
                for (int step = 0; step < radial; ++step) {
                    const uint32_t i0 = a + static_cast<uint32_t>(step);
                    const uint32_t i1 = a + static_cast<uint32_t>(step) + 1u;
                    const uint32_t j0 = b + static_cast<uint32_t>(step);
                    const uint32_t j1 = b + static_cast<uint32_t>(step) + 1u;
                    indices.insert(indices.end(), { i0, j0, j1, i0, j1, i1 });
                }
            }
            uploadMesh(line->runtimeMesh, std::move(vertices), std::move(indices));
            line->runtimeMesh.signature = signature;
        } else if ((!line->runtimeMesh.HasMesh() || line->runtimeMesh.signature != signature)
            && line->points.size() >= 2) {
            std::vector<renderer::Vertex> vertices;
            std::vector<uint32_t> indices;
            const size_t segmentCount = line->loop ? line->points.size() : line->points.size() - 1;
            for (size_t index = 0; index < segmentCount; ++index) {
                math::Vector3 a = line->points[index];
                math::Vector3 b = line->points[(index + 1) % line->points.size()];
                if (line->space == LineSpace::World) {
                    a = DivideSafe(go->transform.worldRotation.Inverse()
                        * (a - go->transform.worldPosition), go->transform.worldScale);
                    b = DivideSafe(go->transform.worldRotation.Inverse()
                        * (b - go->transform.worldPosition), go->transform.worldScale);
                }
                // 同じ点が 2 つ並んだ区間は面積 0 で描くものが無い。方向も作れないので飛ばす。
                // 頂点は区間ごとに独立しているため、抜けても残りの帯は繋がったままになる。
                if ((b - a).LengthSq() < 0.000001f) continue;
                const math::Vector3 direction = (b - a).Normalized();
                math::Vector3 viewDirection = math::Vector3::FORWARD;
                if (line->billboard && mainCamera) {
                    const math::Vector3 cameraLocal = DivideSafe(go->transform.worldRotation.Inverse()
                        * (mainCamera->transform.worldPosition - go->transform.worldPosition),
                        go->transform.worldScale);
                    // カメラが区間の中点に重なると向きが決まらない。板の面は side 側で
                    // 作り直されるので、既定の前方を入れておけば絵は崩れない。
                    viewDirection = (cameraLocal - (a + b) * 0.5f)
                        .NormalizedOr(math::Vector3::FORWARD);
                }
                math::Vector3 side = math::Vector3::Cross(direction, viewDirection);
                if (side.LengthSq() < 0.000001f)
                    side = math::Vector3::UP;
                side = side.Normalized();
                const float t0 = static_cast<float>(index) / static_cast<float>(segmentCount);
                const float t1 = static_cast<float>(index + 1) / static_cast<float>(segmentCount);
                const float w0 = (line->startWidth + (line->endWidth - line->startWidth) * t0) * 0.5f;
                const float w1 = (line->startWidth + (line->endWidth - line->startWidth) * t1) * 0.5f;
                const uint32_t base = static_cast<uint32_t>(vertices.size());
                vertices.push_back({a - side * w0, math::Vector3::FORWARD, direction, {t0, 0.0f}});
                vertices.push_back({a + side * w0, math::Vector3::FORWARD, direction, {t0, 1.0f}});
                vertices.push_back({b + side * w1, math::Vector3::FORWARD, direction, {t1, 1.0f}});
                vertices.push_back({b - side * w1, math::Vector3::FORWARD, direction, {t1, 0.0f}});
                indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            }
            uploadMesh(line->runtimeMesh, std::move(vertices), std::move(indices));
            line->runtimeMesh.signature = signature;
        }
        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!meshRenderer)
            meshRenderer = &go->AddComponent<MeshRenderer>();
        meshRenderer->mesh = line->runtimeMesh.Current();
        meshRenderer->enabled = line->enabled && line->points.size() >= 2;
        applyMaterial(*go, line->materialPath, line->startColor, "",
            inheritedSort(*go) + line->sortingLayer * 1000 + line->orderInLayer);
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
            // WHY 用途で分岐するか: render_path = "decal" の .mat はシェーダーごと
            //     DecalComponent へ渡せる。それ以外はメッシュ用シェーダーを指しており
            //     デカールパスでは描けないので、従来どおりテクスチャだけ抜いて
            //     組み込み描画へ載せる (既存の Projector 設定を壊さないため)。
            if (materialAsset.renderPath == asset::RenderPath::Decal) {
                decal->materialPath = projector->materialPath;
            } else {
                decal->materialPath.clear();
                if (auto it = materialAsset.textures.find("albedo"); it != materialAsset.textures.end())
                    decal->albedoTexPath = it->second;
                if (auto it = materialAsset.textures.find("normal"); it != materialAsset.textures.end())
                    decal->normalTexPath = it->second;
                if (auto it = materialAsset.textures.find("emissive"); it != materialAsset.textures.end())
                    decal->emissiveTexPath = it->second;
            }
            projector->runtimeLoadedMaterialPath = projector->materialPath;
        } else if (projector->materialPath.empty() && !projector->runtimeLoadedMaterialPath.empty()) {
            // 割り当てを外したら .mat も外す。残すとテクスチャを消しても材質が描き続ける。
            projector->runtimeLoadedMaterialPath.clear();
            decal->materialPath.clear();
        }
        // .mat 経路では albedoColor が効かないので、投影の濃さは opacity へ回す。
        // 組み込み経路は albedoColor[3] が担うため 1.0 のままにする (二重掛けを避ける)。
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

} // namespace fbzz::scene
