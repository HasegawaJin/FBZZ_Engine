// FBZZ Engine
// VFXGraphSystem.cpp | fbzz::scene
// .vfx DAGの読込、ランタイムGameObject生成、時間に沿ったComponent有効化
#include <Engine/Scene/Systems/VFXGraphSystem.hpp>
#include <Engine/Asset/VFXParameterRuntime.hpp>

#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleForceField.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/Components/VFXScreenEffect.hpp>
#include <Engine/Scene/Components/WindZoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/ParticleSimulationSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <any>
#include <cmath>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {
namespace {

void SetNodeActive(Scene& scene, VFXRuntimeNodeState& state, bool active)
{
    GameObject* gameObject = scene.GetGameObject(state.entity);
    if (gameObject == nullptr) return;
    if (active) {
        gameObject->SetActive(true);
        if (auto* screen = gameObject->GetComponent<VFXScreenEffect>()) {
            screen->enabled = true;
            screen->weight = 0.0f; // 立ち上がりはエンベロープ更新に任せる
        }
        if (auto* shake = gameObject->GetComponent<VFXCameraShake>()) {
            shake->enabled = true;
            shake->weight = 0.0f;
            shake->elapsed = 0.0f;
        }
        if (auto* scale = gameObject->GetComponent<VFXTimeScale>()) {
            scale->enabled = true;
            scale->weight = 0.0f;
        }
        if (auto* emitter = gameObject->GetComponent<ParticleEmitter>()) emitter->ResetPlayback();
        if (auto* graph = gameObject->GetComponent<VFXGraphComponent>()) graph->Restart();
        if (auto* animator = gameObject->GetComponent<AnimatorComponent>()) {
            animator->stateTime = 0.0f;
            animator->previousEventTime = 0.0f;
            animator->previousEventClipName.clear();
            animator->firedEvents.clear();
        }
        if (auto* audio = gameObject->GetComponent<AudioSourceComponent>()) {
            audio->m_played = false;
            audio->m_pendingPlay = true;
            audio->m_pendingStop = false;
        }
    } else {
        if (auto* emitter = gameObject->GetComponent<ParticleEmitter>()) {
            emitter->playing = false;
            emitter->particles.clear();
            emitter->gpuClearPending = true;
        }
        if (auto* audio = gameObject->GetComponent<AudioSourceComponent>()) {
            audio->m_pendingPlay = false;
            audio->m_pendingStop = true;
        }
        // Scene::View は非アクティブGameObjectを除外しないため、画面演出は明示的に殺す。
        // これを怠るとノード終了後もフラッシュや揺れや減速が残り続ける。
        if (auto* screen = gameObject->GetComponent<VFXScreenEffect>()) {
            screen->enabled = false;
            screen->weight = 0.0f;
        }
        if (auto* shake = gameObject->GetComponent<VFXCameraShake>()) {
            shake->enabled = false;
            shake->weight = 0.0f;
        }
        if (auto* scale = gameObject->GetComponent<VFXTimeScale>()) {
            scale->enabled = false;
            scale->weight = 0.0f;
        }
        gameObject->SetActive(false);
    }
    state.active = active;
}

void DestroyRuntimeNodes(Scene& scene, GameObject& owner, VFXGraphComponent& component)
{
    for (const auto& state : component.runtimeNodes) {
        if (!scene.IsValid(state.entity)) continue;
        if (!state.poolKey.empty()) {
            if (GameObject* pooled = scene.GetGameObject(state.entity)) {
                pooled->SetActive(false);
                pooled->SetParent(owner);
                component.animatedMeshPool.push_back({ state.poolKey, state.entity });
                continue;
            }
        }
        scene.DestroyGameObject(state.entity);
    }
    // 実体を先に消してからグループを消す。逆順だと子を巻き込む破棄と二重解放が競合しうる。
    for (const auto& pivot : component.runtimePivots)
        if (scene.IsValid(pivot.entity)) scene.DestroyGameObject(pivot.entity);
    component.runtimePivots.clear();
    component.runtimeNodes.clear();
    component.runtimeLinks.clear();
    component.initialized = false;
    component.loadedGraphPath.clear();
    component.graphDuration = 0.0f;
    component.hasEventLinks = false;
    component.runtimeGraph.reset();
}

void ApplyTransform(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    gameObject.transform.position = node.localPosition;
    gameObject.transform.rotation = math::Quaternion::FromEuler(node.localRotationDegrees * DEG_TO_RAD);
    gameObject.transform.scale = node.localScale;
}

GameObject* FindVFXSocket(GameObject& root, std::string_view boneName)
{
    if (const auto* bone = root.GetComponent<BoneComponent>(); bone != nullptr && bone->boneName == boneName)
        return &root;
    for (int index = 0; index < root.GetChildCount(); ++index)
        if (GameObject* child = root.GetChild(index))
            if (GameObject* found = FindVFXSocket(*child, boneName)) return found;
    return nullptr;
}

void AddParticle(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    // authoring設定を丸ごと複製し、Graphの時間制御だけを上書きする。
    // WHY: モジュール追加時にランタイム生成コードへ転記を足す運用は保存漏れを再発させるため。
    ParticleEmitter emitter = node.particle;
    emitter.duration = node.duration;
    emitter.loop = false;
    emitter.startDelay = 0.0f;
    emitter.enabled = true;
    emitter.playing = true;
    emitter.ResetPlayback();
    gameObject.AddComponent<ParticleEmitter>(std::move(emitter));
    if (!node.particle.meshParticlePath.empty()) {
        MeshRenderer renderer;
        renderer.meshPath = node.particle.meshParticlePath;
        const auto model = asset::AssetManager::LoadModel(node.particle.meshParticlePath);
        if (model && !model->meshes.empty()) renderer.mesh = model->meshes.front().get();
        gameObject.AddComponent<MeshRenderer>(std::move(renderer));
        MeshTrailComponent meshParticles;
        meshParticles.materialPath = node.particle.materialPath;
        meshParticles.duration = 0.01f;
        gameObject.AddComponent<MeshTrailComponent>(std::move(meshParticles));
    }
}

void AddTrail(GameObject& gameObject, const asset::VFXGraphNode& node, bool meshTrail)
{
    if (meshTrail) {
        if (!node.trail.meshPath.empty()) {
            MeshRenderer renderer;
            renderer.meshPath = node.trail.meshPath;
            const auto model = asset::AssetManager::LoadModel(node.trail.meshPath);
            if (model && !model->meshes.empty()) renderer.mesh = model->meshes.front().get();
            gameObject.AddComponent<MeshRenderer>(std::move(renderer));
        }
        MeshTrailComponent trail;
        trail.materialPath = node.trail.materialPath;
        trail.colorStart = node.trail.colorStart;
        trail.colorEnd = node.trail.colorEnd;
        trail.duration = node.trail.lifetime;
        gameObject.AddComponent<MeshTrailComponent>(std::move(trail));
        return;
    }
    TrailComponent trail;
    trail.materialPath = node.trail.materialPath;
    trail.colorStart = node.trail.colorStart;
    trail.colorEnd = node.trail.colorEnd;
    trail.duration = node.trail.lifetime;
    trail.widthStart = node.trail.widthStart;
    trail.widthEnd = node.trail.widthEnd;
    trail.beamMode = node.trail.beamMode;
    trail.beamStart = node.trail.beamStart;
    trail.beamEnd = node.trail.beamEnd;
    gameObject.AddComponent<TrailComponent>(std::move(trail));
}

void AddLight(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    LightComponent light;
    light.type = LightComponent::Type::Point;
    light.color = node.light.color;
    light.intensity = node.light.intensity;
    light.range = node.light.range;
    light.castShadows = false;
    gameObject.AddComponent<LightComponent>(light);
}

void AddAudio(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    AudioSourceComponent audio;
    audio.clipPath = node.audio.clipPath;
    audio.volume = node.audio.volume;
    audio.pitch = node.audio.pitch;
    audio.spatialBlend = node.audio.spatialBlend;
    audio.loop = node.audio.loop;
    audio.playOnAwake = true;
    gameObject.AddComponent<AudioSourceComponent>(std::move(audio));
}

void AddDecal(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    DecalComponent decal;
    decal.albedoTexPath = node.decal.albedoPath;
    decal.normalTexPath = node.decal.normalPath;
    decal.emissiveTexPath = node.decal.emissivePath;
    decal.albedoColor[0] = node.decal.color.x;
    decal.albedoColor[1] = node.decal.color.y;
    decal.albedoColor[2] = node.decal.color.z;
    decal.albedoColor[3] = node.decal.color.w;
    decal.normalStrength = node.decal.normalStrength;
    decal.emissiveScale = node.decal.emissiveScale;
    decal.lifetime = node.duration;
    decal.fadeTime = node.decal.fadeTime;
    decal.angleFadeStrength = node.decal.angleFadeStrength;
    decal.angleFadeDegrees = node.decal.angleFadeDegrees;
    gameObject.AddComponent<DecalComponent>(std::move(decal));
}

void AddForceField(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    ParticleForceField field;
    // Asset側はScene層のenumへ依存しないようintで持つため、ここで範囲をクランプして変換する。
    const int typeValue = std::clamp(node.forceField.fieldType, 0,
                                     static_cast<int>(ParticleForceFieldType::Drag));
    field.fieldType = static_cast<ParticleForceFieldType>(typeValue);
    field.strength = node.forceField.strength;
    field.radius = node.forceField.radius;
    field.falloffPower = node.forceField.falloffPower;
    field.direction = node.forceField.direction;
    field.noiseFrequency = node.forceField.noiseFrequency;
    field.noiseSpeed = node.forceField.noiseSpeed;
    field.enabled = true;
    gameObject.AddComponent<ParticleForceField>(std::move(field));
}

void AddMesh(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    MeshRenderer renderer;
    // メッシュ未指定でも「何も出ない」を避け、球シェルとして成立させる。
    renderer.meshPath = node.mesh.meshPath.empty() ? "primitive:sphere" : node.mesh.meshPath;
    // "primitive:*" はRenderSystem側が組み込み形状として解決するため、モデル読み込みは行わない。
    if (!renderer.meshPath.starts_with("primitive:")) {
        const auto model = asset::AssetManager::LoadModel(renderer.meshPath);
        if (model && !model->meshes.empty()) renderer.mesh = model->meshes.front().get();
    }
    gameObject.AddComponent<MeshRenderer>(std::move(renderer));
    MaterialComponent material;
    // 未割当なら VFX 用フォールバックへ。共通の Fallback.mat (不透明マゼンタ) に落ちると
    // 「壊れている」表示になってエフェクトとして使い物にならないため、ここで先回りする。
    material.materialPath = node.mesh.materialPath.empty()
        ? asset::VFX_MESH_FALLBACK_MATERIAL : node.mesh.materialPath;
    gameObject.AddComponent<MaterialComponent>(std::move(material));
}

std::string AnimatedMeshPoolKey(const asset::VFXGraphNode& node)
{
    return node.animatedMesh.modelPath + "|" + node.animatedMesh.controllerPath + "|"
        + node.animatedMesh.materialPath + "|"
        + std::to_string(node.animatedMesh.meshIndex);
}

void ConfigureAnimatedMesh(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    SkinnedMeshRenderer* renderer = gameObject.GetComponent<SkinnedMeshRenderer>();
    if (renderer == nullptr) renderer = &gameObject.AddComponent<SkinnedMeshRenderer>();
    renderer->enabled = true;
    renderer->modelPath = node.animatedMesh.modelPath;
    renderer->model = asset::AssetManager::LoadModel(renderer->modelPath);
    renderer->morphWeights.clear();
    renderer->appliedMorphWeights.clear();

    MaterialComponent* material = gameObject.GetComponent<MaterialComponent>();
    if (material == nullptr) material = &gameObject.AddComponent<MaterialComponent>();
    material->materialPath = node.animatedMesh.materialPath.empty()
        ? asset::VFX_MESH_FALLBACK_MATERIAL : node.animatedMesh.materialPath;
    material->paramOverrides.clear();
    // SkinnedMeshRenderer は常にモデル全体を描くようになったため、
    // 「この submesh だけを出す」指定はマテリアルスロットの可視フラグで表現する。
    if (renderer->model)
        material->ResizeSlots(renderer->model->meshes.size());
    material->SetOnlyVisibleSlot(node.animatedMesh.meshIndex);

    AnimatorComponent* animator = gameObject.GetComponent<AnimatorComponent>();
    if (animator == nullptr) animator = &gameObject.AddComponent<AnimatorComponent>();
    animator->enabled = true;
    animator->controllerPath = node.animatedMesh.controllerPath;
    asset::AnimatorControllerAsset controller;
    if (asset::LoadAnimatorControllerAsset(animator->controllerPath, controller)) {
        asset::ApplyAnimatorControllerAsset(controller, *animator);
        animator->loadedControllerPath = animator->controllerPath;
    } else {
        animator->loadedControllerPath.clear();
    }
    animator->currentStateName = node.animatedMesh.initialState;
    if (!node.animatedMesh.initialState.empty())
        for (auto& state : animator->states)
            if (state.name == node.animatedMesh.initialState)
                state.loop = node.animatedMesh.loop;
    animator->stateTime = 0.0f;
    animator->speed = node.animatedMesh.speed;
    animator->playing = !node.animatedMesh.syncToGraphTime;
    // VFX の決定論的 Preview は Transform を動かしてはいけないため、
    // 抽出オフ時は「ポーズはそのまま (Keep)」を選ぶ。
    // WHY: Strip にするとルート成分だけが消え、エフェクト用メッシュの見た目が変わる。
    animator->rootMotion.mode = node.animatedMesh.applyRootMotion
        ? RootMotionMode::ApplyToTransform
        : RootMotionMode::None;
    animator->rootMotion.poseMode = RootMotionPoseMode::Keep;
    animator->previousEventTime = 0.0f;
    animator->previousEventClipName.clear();
    animator->firedEvents.clear();
    animator->blendToState.clear();
    animator->blendToTime = 0.0f;
    animator->blendWeight = 0.0f;
    animator->rootMotionDeltaPosition = math::Vector3::ZERO;
    animator->rootMotionDeltaRotation = math::Quaternion::Identity();
}

void AddScreenEffect(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    VFXScreenEffect effect;
    effect.flashColor = node.screenEffect.flashColor;
    effect.flashIntensity = node.screenEffect.flashIntensity;
    effect.bloomBoost = node.screenEffect.bloomBoost;
    effect.chromaticAberration = node.screenEffect.chromaticAberration;
    effect.lensDistortion = node.screenEffect.lensDistortion;
    effect.vignette = node.screenEffect.vignette;
    effect.weight = 0.0f; // エンベロープはVFXGraphSystemが毎フレーム更新する
    gameObject.AddComponent<VFXScreenEffect>(std::move(effect));
}

void AddCameraShake(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    VFXCameraShake shake;
    shake.amplitude = node.cameraShake.amplitude;
    shake.rotationAmplitude = node.cameraShake.rotationAmplitude;
    shake.frequency = node.cameraShake.frequency;
    shake.radius = node.cameraShake.radius;
    shake.weight = 0.0f;
    gameObject.AddComponent<VFXCameraShake>(std::move(shake));
}

void AddTimeScale(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    VFXTimeScale scale;
    scale.timeScale = node.timeScale.timeScale;
    scale.weight = 0.0f;
    gameObject.AddComponent<VFXTimeScale>(std::move(scale));
}

void AddWind(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    WindZoneComponent wind;
    wind.direction = node.wind.direction;
    wind.strength = node.wind.strength;
    wind.turbulence = node.wind.turbulence;
    wind.pulseFrequency = node.wind.pulseFrequency;
    wind.enabled = true;
    gameObject.AddComponent<WindZoneComponent>(std::move(wind));
}

void AddSubGraph(GameObject& gameObject, const asset::VFXGraphNode& node, int nestingDepth)
{
    VFXGraphComponent graph;
    graph.graphPath = node.subGraph.graphPath;
    graph.playOnAwake = false;
    graph.playing = false;
    graph.nestingDepth = nestingDepth;
    gameObject.AddComponent<VFXGraphComponent>(std::move(graph));
}

VFXRuntimeNodeState* FindRuntimeNode(VFXGraphComponent& component, int nodeId)
{
    for (auto& node : component.runtimeNodes)
        if (node.nodeId == nodeId) return &node;
    return nullptr;
}

void ResetRuntimeSchedule(Scene& scene, VFXGraphComponent& component)
{
    for (auto& node : component.runtimeNodes) {
        if (node.active) SetNodeActive(scene, node, false);
        node.startTime = node.scheduledStartTime;
        node.endTime = node.scheduledEndTime;
    }
    for (auto& link : component.runtimeLinks) {
        link.fired = false;
        if (link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnCollision)
            || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnDeath)
            || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnAnimationEvent)
            || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnTrigger)) {
            if (auto* target = FindRuntimeNode(component, link.toNode)) {
                target->startTime = (std::numeric_limits<float>::max)();
                target->endTime = target->startTime;
            }
        }
    }
}

void ApplyPlaybackScale(Scene& scene, VFXRuntimeNodeState& state,
                        float scale, bool editorPreview)
{
    GameObject* gameObject = scene.GetGameObject(state.entity);
    if (gameObject == nullptr) return;
    if (auto* emitter = gameObject->GetComponent<ParticleEmitter>()) {
        emitter->editorTimeScale = scale;
        emitter->editorTimeScaleFrame = Time::frameCount;
    }
    if (auto* graph = gameObject->GetComponent<VFXGraphComponent>()) {
        graph->speed = scale;
        if (editorPreview) graph->editorPreviewFrame = Time::frameCount;
    }
    if (auto* animator = gameObject->GetComponent<AnimatorComponent>())
        if (animator->playing) animator->speed = state.basePlaybackSpeed * scale;
}

// ノードの生存時間に沿って値を動かす「エンベロープ」。
// WHY: Mesh の膨張やフラッシュの減衰は静的な設定では表現できず、かといってノードごとに
//      専用システムを増やすとグラフの再生制御が分散する。時間駆動の更新はここへ集約する。
void ApplyNodeEnvelope(Scene& scene, const VFXRuntimeNodeState& state,
                       const asset::VFXGraphNode& node, float localTime)
{
    const bool timeDriven = node.type == asset::VFXNodeType::Mesh
        || (node.type == asset::VFXNodeType::AnimatedMesh
            && node.animatedMesh.syncToGraphTime)
        || node.type == asset::VFXNodeType::ScreenEffect
        || node.type == asset::VFXNodeType::CameraShake
        || node.type == asset::VFXNodeType::TimeScale
        // Light / Decal はカーブを使うときだけ時間駆動になる。
        // 常に対象にすると、カーブ未使用のノードでも毎フレーム書き戻しが走り、
        // Inspector やスクリプトからの直接編集を上書きしてしまう。
        || (node.type == asset::VFXNodeType::Light
            && (node.light.useIntensityCurve || node.light.useColorGradient))
        || (node.type == asset::VFXNodeType::Decal && node.decal.useFadeCurve);
    if (!timeDriven) return;
    GameObject* gameObject = scene.GetGameObject(state.entity);
    if (gameObject == nullptr) return;

    const float span = state.endTime > state.startTime
        ? state.endTime - state.startTime : (std::max)(node.duration, 0.0001f);
    const float normalized = std::clamp(localTime / span, 0.0f, 1.0f);

    if (node.type == asset::VFXNodeType::AnimatedMesh) {
        if (auto* animator = gameObject->GetComponent<AnimatorComponent>()) {
            // Timeline時刻から毎フレーム直接求め、フレームレートやPause/Step順序に依存させない。
            float clipDuration = span;
            const asset::AnimationClip* selectedClip = nullptr;
            const auto state = std::find_if(animator->states.begin(), animator->states.end(),
                [&](const AnimationState& candidate) {
                    return candidate.name == node.animatedMesh.initialState;
                });
            if (state != animator->states.end()
                && state->mode == AnimationStateMode::Clip) {
                if (!state->clipName.empty())
                    for (const auto& clip : animator->clips)
                        if (clip.name == state->clipName) { selectedClip = &clip; break; }
                if (selectedClip == nullptr && state->clipIndex >= 0
                    && state->clipIndex < static_cast<int>(animator->clips.size()))
                    selectedClip = &animator->clips[static_cast<std::size_t>(state->clipIndex)];
            }
            if (selectedClip != nullptr)
                clipDuration = (std::max)(
                    static_cast<float>(selectedClip->GetDurationSeconds()), 0.0001f);
            const float startOffset = std::clamp(node.animatedMesh.startNormalizedTime,
                0.0f, 1.0f) * clipDuration;
            const float animationTime = startOffset
                + (std::max)(localTime, 0.0f) * node.animatedMesh.speed;
            animator->playing = false;
            animator->speed = node.animatedMesh.speed;
            if (!node.animatedMesh.initialState.empty())
                animator->currentStateName = node.animatedMesh.initialState;
            animator->stateTime = animationTime;
        }
        return;
    }

    if (node.type == asset::VFXNodeType::Mesh) {
        // scaleEasePower < 1 で最初に一気に広がり、その後ゆっくり — 衝撃波らしい減速になる。
        const float eased = std::pow(normalized, (std::max)(node.mesh.scaleEasePower, 0.01f));
        const float factor = node.mesh.scaleStart
            + (node.mesh.scaleEnd - node.mesh.scaleStart) * eased;
        gameObject->transform.scale = node.localScale * factor;
        if (auto* material = gameObject->GetComponent<MaterialComponent>()) {
            // albedo はどのマテリアルにも存在する共通パラメーター。ここへ書くことで
            // どんな .mat を割り当てても必ず色とフェードが効く。
            const auto lerp = [normalized](float from, float to) {
                return from + (to - from) * normalized;
            };
            material->paramOverrides["albedo"] = {
                lerp(node.mesh.colorStart.x, node.mesh.colorEnd.x),
                lerp(node.mesh.colorStart.y, node.mesh.colorEnd.y),
                lerp(node.mesh.colorStart.z, node.mesh.colorEnd.z),
                lerp(node.mesh.colorStart.w, node.mesh.colorEnd.w),
            };
            if (!node.mesh.animatedParam.empty()) {
                material->paramOverrides[node.mesh.animatedParam] =
                    { lerp(node.mesh.paramStart, node.mesh.paramEnd) };
            }
        }
        return;
    }

    // Light: 閃光の減衰。定数の intensity だけでは「一瞬強く光ってすっと消える」が作れない。
    // 評価は normalized (= localTime / span) のみに依存するので、Timeline scrub と
    // vfx.preview が同じ時刻で必ず同じ絵になる (決定論の前提を崩さない)。
    if (node.type == asset::VFXNodeType::Light) {
        if (auto* light = gameObject->GetComponent<LightComponent>()) {
            if (node.light.useIntensityCurve) {
                light->intensity = node.light.intensity
                    * (std::max)(node.light.intensityCurve.Evaluate(normalized), 0.0f);
            }
            if (node.light.useColorGradient) {
                const math::Vector4 sampled = node.light.colorGradient.Evaluate(normalized);
                // LightComponent の color は Vector3。gradient の alpha は明るさ倍率として使う
                // (色と強さを 1 本のグラデーションで作れるようにするため)。
                light->color = { sampled.x * sampled.w, sampled.y * sampled.w, sampled.z * sampled.w };
            }
        }
        return;
    }

    // Decal: 焼け跡がじわっと消えるなど、線形フェードでは出せない減り方を作る。
    if (node.type == asset::VFXNodeType::Decal) {
        if (auto* decal = gameObject->GetComponent<DecalComponent>()) {
            const float fade = std::clamp(node.decal.fadeCurve.Evaluate(normalized), 0.0f, 1.0f);
            decal->albedoColor[3] = node.decal.color.w * fade;
            decal->emissiveScale = node.decal.emissiveScale * fade;
        }
        return;
    }

    // 立ち上がり -> ピーク維持 -> 減衰。合計が生存時間を超える場合は比率で押し込む。
    const auto envelope = [span, localTime](float fadeIn, float fadeOut) {
        fadeIn = (std::max)(fadeIn, 0.0f);
        fadeOut = (std::max)(fadeOut, 0.0f);
        if (fadeIn + fadeOut > span && fadeIn + fadeOut > 0.0f) {
            const float scale = span / (fadeIn + fadeOut);
            fadeIn *= scale;
            fadeOut *= scale;
        }
        float weight = 1.0f;
        if (fadeIn > 0.0f && localTime < fadeIn) weight = localTime / fadeIn;
        else if (fadeOut > 0.0f && localTime > span - fadeOut) weight = (span - localTime) / fadeOut;
        return std::clamp(weight, 0.0f, 1.0f);
    };

    if (node.type == asset::VFXNodeType::ScreenEffect) {
        if (auto* screen = gameObject->GetComponent<VFXScreenEffect>()) {
            screen->enabled = true;
            screen->weight = envelope(node.screenEffect.fadeInTime, node.screenEffect.fadeOutTime);
        }
        return;
    }
    if (node.type == asset::VFXNodeType::CameraShake) {
        if (auto* shake = gameObject->GetComponent<VFXCameraShake>()) {
            shake->enabled = true;
            shake->elapsed = localTime;
            // 揺れは頭から立ち上がって減衰するのが自然なので、fade は使わず
            // (1-t)^falloffPower で単調に減らす。
            const float remaining = std::clamp(1.0f - normalized, 0.0f, 1.0f);
            shake->weight = std::pow(remaining, (std::max)(node.cameraShake.falloffPower, 0.05f));
        }
        return;
    }
    if (node.type == asset::VFXNodeType::TimeScale) {
        if (auto* scale = gameObject->GetComponent<VFXTimeScale>()) {
            scale->enabled = true;
            scale->weight = envelope(node.timeScale.blendInTime, node.timeScale.blendOutTime);
        }
    }
}

// アクティブな VFXTimeScale を集計して Time::timeScale へ反映する。
// WHY: 複数のヒットストップが重なることは普通にある。最も遅い要求を採用し、
//      1つも無い状態では必ず 1.0 へ戻す (書き込みが残って世界が止まる事故を防ぐ)。
void ApplyVFXTimeScale(Scene& scene)
{
    float slowest = 1.0f;
    bool any = false;
    for (auto [tf, request] : scene.View<Transform, VFXTimeScale>()) {
        if (!request.enabled) continue;
        const float weight = std::clamp(request.weight, 0.0f, 1.0f);
        if (weight <= 0.0f) continue;
        any = true;
        // weight で 1.0 から目標倍率へ補間する。
        const float value = 1.0f + (request.timeScale - 1.0f) * weight;
        slowest = (std::min)(slowest, value);
    }
    // VFX が書いた間だけ責任を持ち、要求が消えた最初のフレームで等速へ戻す。
    // WHY: 常時 1.0 を書き戻すと、デバッグ用のスロー再生など外部の timeScale 設定を
    //      毎フレーム踏み潰してしまう。自分が触ったときだけ後始末する。
    static bool s_ownsTimeScale = false;
    if (any) {
        Time::timeScale = (std::max)(slowest, 0.0f);
        s_ownsTimeScale = true;
    } else if (s_ownsTimeScale) {
        Time::timeScale = 1.0f;
        s_ownsTimeScale = false;
    }
}

void ConsumeRuntimeEvents(Scene& scene, VFXGraphComponent& component)
{
    for (auto& link : component.runtimeLinks) {
        if (link.fired) continue;
        const bool collisionEvent = link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnCollision);
        const bool deathEvent = link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnDeath);
        const bool animationEvent = link.trigger
            == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnAnimationEvent);
        const bool externalTrigger = link.trigger
            == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnTrigger);
        if (!collisionEvent && !deathEvent && !animationEvent && !externalTrigger) continue;
        VFXRuntimeNodeState* source = FindRuntimeNode(component, link.fromNode);
        VFXRuntimeNodeState* target = FindRuntimeNode(component, link.toNode);
        if (source == nullptr || target == nullptr) continue;
        GameObject* sourceObject = scene.GetGameObject(source->entity);
        bool fired = false;
        if (collisionEvent || deathEvent) {
            const ParticleEmitter* emitter = sourceObject != nullptr
                ? sourceObject->GetComponent<ParticleEmitter>() : nullptr;
            fired = emitter != nullptr
                && (collisionEvent ? emitter->collisionCountThisFrame
                                   : emitter->deathCountThisFrame) > 0;
        } else if (animationEvent) {
            const AnimatorComponent* animator = sourceObject != nullptr
                ? sourceObject->GetComponent<AnimatorComponent>() : nullptr;
            if (animator != nullptr)
                fired = std::any_of(animator->firedEvents.begin(), animator->firedEvents.end(),
                    [&](const FiredAnimationEvent& event) {
                        return link.eventName.empty() || event.name == link.eventName;
                    });
        } else {
            fired = std::find(component.pendingTriggers.begin(), component.pendingTriggers.end(),
                link.eventName) != component.pendingTriggers.end();
        }
        if (!fired) continue;

        if (target->active) SetNodeActive(scene, *target, false);
        const float duration = target->scheduledEndTime - target->scheduledStartTime;
        target->startTime = component.playTime + link.delay;
        target->endTime = target->startTime + duration;
        component.graphDuration = (std::max)(component.graphDuration, target->endTime);
        link.fired = true;
    }
    component.pendingTriggers.clear();
}

// ScriptのFBZZ_FIELDを読み取り専用で走査し、Attribute Bindingの汎用ゲーム属性面にする。
class VFXAttributeReflector final : public IReflector {
public:
    explicit VFXAttributeReflector(std::string_view field) : m_field(field) {}
    void Field(const char* name, float& value) override { Capture(name, value); }
    void Field(const char* name, int& value) override { Capture(name, value); }
    void Field(const char* name, bool& value) override { Capture(name, value); }
    void Field(const char* name, math::Vector2& value) override { Capture(name, value); }
    void Field(const char* name, math::Vector3& value) override { Capture(name, value); }
    void Field(const char* name, math::Vector4& value) override { Capture(name, value); }
    void Field(const char* name, std::string& value) override { Capture(name, value); }
    void Field(const char* name, math::Quaternion& value) override { Capture(name, value); }
    [[nodiscard]] const std::any& Value() const { return m_value; }
private:
    template<typename T> void Capture(const char* name, const T& value)
    {
        if (m_value.has_value() || name == nullptr) return;
        if (m_field == PersistentKey(name)) {
            m_value = value;
            return;
        }
    }
    std::string_view m_field;
    std::any m_value;
};

bool CoerceVFXAttribute(const std::any& input, reflection::PropertyType target, std::any& output)
{
    if (target == reflection::PropertyType::Float) {
        if (const auto* value = std::any_cast<float>(&input)) output = *value;
        else if (const auto* value = std::any_cast<int>(&input)) output = static_cast<float>(*value);
        else if (const auto* value = std::any_cast<bool>(&input)) output = *value ? 1.0f : 0.0f;
    } else if (target == reflection::PropertyType::Int) {
        if (const auto* value = std::any_cast<int>(&input)) output = *value;
        else if (const auto* value = std::any_cast<float>(&input)) output = static_cast<int>(*value);
        else if (const auto* value = std::any_cast<bool>(&input)) output = *value ? 1 : 0;
    } else if (target == reflection::PropertyType::Bool) {
        if (const auto* value = std::any_cast<bool>(&input)) output = *value;
        else if (const auto* value = std::any_cast<float>(&input)) output = *value != 0.0f;
        else if (const auto* value = std::any_cast<int>(&input)) output = *value != 0;
    } else if (target == reflection::PropertyType::Vector3) {
        if (const auto* value = std::any_cast<math::Vector3>(&input)) output = *value;
        else if (const auto* value = std::any_cast<math::Vector4>(&input)) output = math::Vector3{ value->x, value->y, value->z };
    } else if (target == reflection::PropertyType::Color) {
        if (const auto* value = std::any_cast<math::Vector4>(&input)) output = *value;
        else if (const auto* value = std::any_cast<math::Vector3>(&input)) output = math::Vector4{ value->x, value->y, value->z, 1.0f };
    } else if (target == reflection::PropertyType::String || target == reflection::PropertyType::AssetRef) {
        if (const auto* value = std::any_cast<std::string>(&input)) output = *value;
    } else {
        output = input;
    }
    return output.has_value();
}

bool ResolveVFXAttribute(Scene& scene, GameObject& owner, std::string path,
                         reflection::PropertyType target, std::any& output)
{
    GameObject* source = &owner;
    if (path.starts_with("self.")) path.erase(0, 5);
    else if (path.starts_with("parent.")) { source = owner.GetParent(); path.erase(0, 7); }
    else if (path.starts_with("guid:")) {
        const std::size_t separator = path.find('.');
        if (separator == std::string::npos) return false;
        source = scene.FindByGuid(path.substr(5, separator - 5));
        path.erase(0, separator + 1);
    }
    if (source == nullptr) return false;

    std::any raw;
    if (path == "transform.position") raw = source->transform.position;
    else if (path == "transform.worldPosition") raw = source->transform.worldPosition;
    else if (path == "transform.scale") raw = source->transform.scale;
    else if (path == "physics.velocity" || path == "physics.speed"
             || path == "physics.angularVelocity") {
        const auto* body = source->GetComponent<RigidBodyComponent>();
        if (body == nullptr || body->rigidBody == nullptr) return false;
        const math::Vector3 value = path == "physics.angularVelocity"
            ? body->rigidBody->GetAngularVelocity() : body->rigidBody->GetVelocity();
        raw = path == "physics.speed" ? std::any{ value.Length() } : std::any{ value };
    } else if (path.starts_with("animator.")) {
        const auto* animator = source->GetComponent<AnimatorComponent>();
        if (animator == nullptr) return false;
        const std::string name = path.substr(9);
        const auto parameter = std::find_if(animator->parameters.begin(), animator->parameters.end(),
            [&](const AnimatorParameter& value) { return value.name == name; });
        if (parameter == animator->parameters.end()) return false;
        if (parameter->type == ParamType::Float) raw = parameter->floatValue;
        else if (parameter->type == ParamType::Int) raw = parameter->intValue;
        else raw = parameter->boolValue;
    } else if (path.starts_with("script.")) {
        const std::size_t typeEnd = path.find('.', 7);
        if (typeEnd == std::string::npos) return false;
        const std::string typeName = path.substr(7, typeEnd - 7);
        const std::string fieldName = path.substr(typeEnd + 1);
        auto* scripts = source->GetComponent<ScriptComponent>();
        if (scripts == nullptr) return false;
        for (auto& entry : scripts->scripts) {
            if (entry.script == nullptr || typeName != entry.script->GetTypeName()) continue;
            VFXAttributeReflector reflector(fieldName);
            entry.script->Reflect(reflector);
            raw = reflector.Value();
            break;
        }
    }
    return raw.has_value() && CoerceVFXAttribute(raw, target, output);
}

void ApplyRuntimeNodeSettings(Scene& scene, const VFXRuntimeNodeState& state,
                              const asset::VFXGraphNode& node, std::string_view schemaPath)
{
    GameObject* object = scene.GetGameObject(state.entity);
    if (object == nullptr) return;
    if (schemaPath == "localPosition" || schemaPath == "localRotationDegrees" || schemaPath == "localScale")
        ApplyTransform(*object, node);
    if (node.type == asset::VFXNodeType::Particle && schemaPath.starts_with("particle.")) {
        if (auto* emitter = object->GetComponent<ParticleEmitter>()) {
            reflection::ResolvedProperty sourceProperty;
            reflection::ResolvedProperty targetProperty;
            const std::string_view leaf = schemaPath.substr(9);
            if (reflection::ResolveProperty(asset::GetParticleEmitterSchema(), &node.particle, leaf, sourceProperty)
                && reflection::ResolveProperty(asset::GetParticleEmitterSchema(), emitter, leaf, targetProperty)
                && sourceProperty.property != nullptr && targetProperty.property != nullptr)
                targetProperty.property->set(targetProperty.owner, sourceProperty.property->get(sourceProperty.constOwner));
        }
    } else if (node.type == asset::VFXNodeType::Light) {
        if (auto* light = object->GetComponent<LightComponent>()) {
            light->color = node.light.color; light->intensity = node.light.intensity; light->range = node.light.range;
        }
    } else if (node.type == asset::VFXNodeType::Audio) {
        if (auto* audio = object->GetComponent<AudioSourceComponent>()) {
            audio->clipPath = node.audio.clipPath; audio->volume = node.audio.volume; audio->pitch = node.audio.pitch;
            audio->spatialBlend = node.audio.spatialBlend; audio->loop = node.audio.loop;
        }
    } else if (node.type == asset::VFXNodeType::Trail) {
        if (auto* trail = object->GetComponent<TrailComponent>()) {
            trail->materialPath = node.trail.materialPath;
            trail->colorStart = node.trail.colorStart; trail->colorEnd = node.trail.colorEnd;
            trail->duration = node.trail.lifetime; trail->widthStart = node.trail.widthStart; trail->widthEnd = node.trail.widthEnd;
            trail->beamMode = node.trail.beamMode; trail->beamStart = node.trail.beamStart; trail->beamEnd = node.trail.beamEnd;
        }
    } else if (node.type == asset::VFXNodeType::MeshTrail) {
        if (auto* trail = object->GetComponent<MeshTrailComponent>()) {
            trail->materialPath = node.trail.materialPath;
            trail->colorStart = node.trail.colorStart; trail->colorEnd = node.trail.colorEnd; trail->duration = node.trail.lifetime;
        }
    } else if (node.type == asset::VFXNodeType::Decal) {
        if (auto* decal = object->GetComponent<DecalComponent>()) {
            decal->albedoTexPath = node.decal.albedoPath; decal->normalTexPath = node.decal.normalPath;
            decal->emissiveTexPath = node.decal.emissivePath;
            decal->albedoColor[0] = node.decal.color.x; decal->albedoColor[1] = node.decal.color.y;
            decal->albedoColor[2] = node.decal.color.z; decal->albedoColor[3] = node.decal.color.w;
            decal->normalStrength = node.decal.normalStrength; decal->emissiveScale = node.decal.emissiveScale;
            decal->fadeTime = node.decal.fadeTime;
        }
    } else if (node.type == asset::VFXNodeType::SubGraph) {
        if (auto* graph = object->GetComponent<VFXGraphComponent>(); graph && graph->graphPath != node.subGraph.graphPath) {
            graph->graphPath = node.subGraph.graphPath; graph->reloadRequested = true;
        }
    } else if (node.type == asset::VFXNodeType::ForceField) {
        if (auto* field = object->GetComponent<ParticleForceField>()) {
            const int typeValue = std::clamp(node.forceField.fieldType, 0,
                                             static_cast<int>(ParticleForceFieldType::Drag));
            field->fieldType = static_cast<ParticleForceFieldType>(typeValue);
            field->strength = node.forceField.strength; field->radius = node.forceField.radius;
            field->falloffPower = node.forceField.falloffPower; field->direction = node.forceField.direction;
            field->noiseFrequency = node.forceField.noiseFrequency; field->noiseSpeed = node.forceField.noiseSpeed;
        }
    } else if (node.type == asset::VFXNodeType::Mesh) {
        // スケールと色は毎フレームのエンベロープが上書きするため、参照先だけ追従させる。
        if (auto* renderer = object->GetComponent<MeshRenderer>()) {
            const std::string wanted = node.mesh.meshPath.empty() ? "primitive:sphere" : node.mesh.meshPath;
            if (renderer->meshPath != wanted) {
                renderer->meshPath = wanted;
                renderer->mesh = nullptr;
                if (!wanted.starts_with("primitive:")) {
                    const auto model = asset::AssetManager::LoadModel(wanted);
                    if (model && !model->meshes.empty()) renderer->mesh = model->meshes.front().get();
                }
            }
        }
        if (auto* material = object->GetComponent<MaterialComponent>()) {
            const std::string wanted = node.mesh.materialPath.empty()
                ? asset::VFX_MESH_FALLBACK_MATERIAL : node.mesh.materialPath;
            if (material->materialPath != wanted) { material->materialPath = wanted; material->materialAsset = {}; }
        }
    } else if (node.type == asset::VFXNodeType::AnimatedMesh) {
        const bool resourceChanged = schemaPath == "__live__"
            || schemaPath == "animatedMesh.modelPath"
            || schemaPath == "animatedMesh.controllerPath"
            || schemaPath == "animatedMesh.materialPath"
            || schemaPath == "animatedMesh.meshIndex";
        if (resourceChanged) {
            ConfigureAnimatedMesh(*object, node);
        } else if (auto* animator = object->GetComponent<AnimatorComponent>()) {
            animator->speed = node.animatedMesh.speed;
            animator->rootMotion.mode = node.animatedMesh.applyRootMotion
                ? RootMotionMode::ApplyToTransform
                : RootMotionMode::None;
            if (!node.animatedMesh.initialState.empty())
                animator->currentStateName = node.animatedMesh.initialState;
        }
    } else if (node.type == asset::VFXNodeType::ScreenEffect) {
        // weight はエンベロープが持つ。ここでは強さだけを差し替える。
        if (auto* screen = object->GetComponent<VFXScreenEffect>()) {
            screen->flashColor = node.screenEffect.flashColor;
            screen->flashIntensity = node.screenEffect.flashIntensity;
            screen->bloomBoost = node.screenEffect.bloomBoost;
            screen->chromaticAberration = node.screenEffect.chromaticAberration;
            screen->lensDistortion = node.screenEffect.lensDistortion;
            screen->vignette = node.screenEffect.vignette;
        }
    } else if (node.type == asset::VFXNodeType::CameraShake) {
        if (auto* shake = object->GetComponent<VFXCameraShake>()) {
            shake->amplitude = node.cameraShake.amplitude;
            shake->rotationAmplitude = node.cameraShake.rotationAmplitude;
            shake->frequency = node.cameraShake.frequency;
            shake->radius = node.cameraShake.radius;
        }
    } else if (node.type == asset::VFXNodeType::TimeScale) {
        if (auto* scale = object->GetComponent<VFXTimeScale>()) scale->timeScale = node.timeScale.timeScale;
    } else if (node.type == asset::VFXNodeType::Wind) {
        if (auto* wind = object->GetComponent<WindZoneComponent>()) {
            wind->direction = node.wind.direction; wind->strength = node.wind.strength;
            wind->turbulence = node.wind.turbulence; wind->pulseFrequency = node.wind.pulseFrequency;
        }
    }
}

// 親として使われているノードの Transform 編集を、対応するグループ GameObject へ写す。
// WHY: グループはノードの実体とは別 GameObject なので、実体だけ動かすと
//      「親ノードを動かしたのに子だけ取り残される」ことになる。ライブ編集で
//      Transform を触ったときは両方へ同じ値を入れる必要がある。
void SyncPivotTransform(Scene& scene, const VFXGraphComponent& component,
                        const asset::VFXGraphNode& node)
{
    for (const auto& pivot : component.runtimePivots) {
        if (pivot.nodeId != node.id) continue;
        if (GameObject* object = scene.GetGameObject(pivot.entity)) ApplyTransform(*object, node);
        return;
    }
}

// 生成済みノードへ authoring 値を丸ごと反映する (ライブ編集の本体)。
// WHY: 値をいじるたびにグラフを作り直すと、走っている粒子・寿命・再生位置が毎回消えて
//      「揺らぎを見ながら強さを詰める」作業ができない。ここでは GameObject を壊さず
//      設定だけ差し替えるため、画面は途切れずに変化だけが乗る。
void ApplyAuthoredNodeSettingsLive(Scene& scene, const VFXRuntimeNodeState& state,
                                   const asset::VFXGraphNode& node)
{
    GameObject* object = scene.GetGameObject(state.entity);
    if (object == nullptr) return;
    ApplyTransform(*object, node);

    if (node.type == asset::VFXNodeType::Particle) {
        if (auto* emitter = object->GetComponent<ParticleEmitter>()) {
            // オーサリング項目の一覧はアセットcodecが単一の信頼元。ここを介して写すことで、
            // ランタイム状態 (particles / GPUハンドル / playTime 等) には一切触れずに済み、
            // モジュールを増やしてもこの関数を書き換えなくてよい。
            const float keepDuration = emitter->duration;
            const bool keepLoop = emitter->loop;
            asset::DeserializeParticleEmitterSettings(
                asset::SerializeParticleEmitterSettings(node.particle), *emitter);
            // グラフ側の時間制御は AddParticle と同じ規則を維持する。
            emitter->duration = node.duration > 0.0f ? node.duration : keepDuration;
            emitter->loop = keepLoop;
            emitter->startDelay = 0.0f;
            emitter->enabled = true;
            // テクスチャ/マテリアルは差分検出で再ロードされるため、ここでは触らない。
        }
        return;
    }
    // Particle 以外は ApplyRuntimeNodeSettings が全型ぶんの一括コピーを持つ。
    // WHY: 公開パラメーターのバインド適用と同じ関数を通すことで、
    //      「ライブ編集では反映されるがバインドでは反映されない」型ごとの差が生まれない。
    //      schemaPath は型ごとの分岐にしか使われないため、代表パスを渡せば全項目が反映される。
    //      Mesh のスケール・色は毎フレームのエンベロープが上書きするので、ここでは触らない。
    ApplyRuntimeNodeSettings(scene, state, node, "__live__");
}

// ノードの追加・削除・型変更・配線変更は生成物の構造そのものが変わるため作り直しが要る。
// 逆に「値だけ」の変更はここで false を返し、走っている絵を止めずに反映できる。
bool VFXGraphTopologyChanged(const asset::VFXGraphAsset& before, const asset::VFXGraphAsset& after)
{
    if (before.nodes.size() != after.nodes.size()) return true;
    if (before.links.size() != after.links.size()) return true;
    for (std::size_t index = 0; index < after.nodes.size(); ++index) {
        if (before.nodes[index].id != after.nodes[index].id) return true;
        if (before.nodes[index].type != after.nodes[index].type) return true;
        // 有効状態は生成GameObjectの有無を変えるため、通常の値差し替えではなく再構築する。
        if (before.nodes[index].enabled != after.nodes[index].enabled) return true;
        // attachBone / parentNodeId は親子付けを決めるため、変わったら生成し直す。
        // parentNodeId はグループ GameObject の有無まで変えるので、値反映では追従できない。
        if (before.nodes[index].attachBone != after.nodes[index].attachBone) return true;
        if (before.nodes[index].parentNodeId != after.nodes[index].parentNodeId) return true;
        // SubGraph の参照先が変わると子グラフごと差し替えになる。
        if (after.nodes[index].type == asset::VFXNodeType::SubGraph
            && before.nodes[index].subGraph.graphPath != after.nodes[index].subGraph.graphPath)
            return true;
    }
    for (std::size_t index = 0; index < after.links.size(); ++index) {
        if (before.links[index].fromNode != after.links[index].fromNode) return true;
        if (before.links[index].toNode != after.links[index].toNode) return true;
        if (before.links[index].trigger != after.links[index].trigger) return true;
    }
    return false;
}

// 構造変更を今すぐ作り直してよい「絵の切れ目」か。
// WHY: 再生中に作り直すと生成物が入れ替わって見た目が飛ぶ。停止中・再生終了後・
//      ループ先頭でだけ消化すれば、繋ぎ目が見えないうえ再構築の回数も減る。
bool AtGraphCycleBoundary(const VFXGraphComponent& component)
{
    if (!component.playing || component.stopped) return true;
    if (component.graphDuration <= 0.0f) return true;
    return component.playTime >= component.graphDuration;
}

// 未保存グラフの差分を、可能なら作り直さずに反映する。
// 戻り値: true=今すぐ作り直しが必要 (呼び出し側が reload する)
bool SyncAuthoringGraphLive(Scene& scene, VFXGraphComponent& component)
{
    if (component.authoringGraph == nullptr) return false;
    // 適用待ちの構造変更は、切れ目に達した時点で消化する。
    if (component.pendingStructuralRebuild && AtGraphCycleBoundary(component)) {
        component.pendingStructuralRebuild = false;
        return true;
    }
    if (component.authoringRevision == component.appliedAuthoringRevision) return false;
    if (!component.initialized || component.runtimeGraph == nullptr) return true;

    const asset::VFXGraphAsset& next = *component.authoringGraph;
    if (VFXGraphTopologyChanged(*component.runtimeGraph, next)) {
        if (AtGraphCycleBoundary(component)) return true;
        // 再生中は作り直しを次の切れ目まで持ち越す。ただし既存ノードの値変更は
        // ここで続けて反映する — 待っている間だけ操作が効かなくなるのを避けるため。
        component.pendingStructuralRebuild = true;
    }

    // スケジュールが破綻する編集途中の状態は、直す前の絵を保ったまま黙って見送る。
    // WHY: 版数を消費しないため、直した瞬間に自動で反映される。
    std::vector<float> startTimes;
    float duration = 0.0f;
    if (!asset::BuildVFXGraphSchedule(next, startTimes, duration, nullptr)) return false;

    component.resolvedParameters.clear();
    for (const auto& definition : next.parameters) {
        const auto* source = asset::ResolveVFXParamSource(next, component, definition);
        if (source != nullptr) component.resolvedParameters.push_back({ definition.name, *source });
    }

    // エディタが「このノードだけ触った」と申告していれば、そのノードだけ設定を写す。
    // 申告が無い (一括編集・Undo 等) ときだけ全ノードを対象にする。
    const int targetNode = component.authoringDirtyNodeId;
    for (std::size_t index = 0; index < next.nodes.size(); ++index) {
        const asset::VFXGraphNode& source = next.nodes[index];
        auto* state = FindRuntimeNode(component, source.id);
        if (state == nullptr) continue;
        // 時刻は全ノード分を更新する。ここは値のコピーだけで生成物には触れないため軽い。
        // 既に発火待ちのノードを取りこぼさないよう、待機中は「予定時刻」だけ更新する。
        const float newStart = startTimes[index];
        const float newEnd = newStart + source.duration;
        const bool waiting = state->startTime > duration;
        state->scheduledStartTime = newStart;
        state->scheduledEndTime = newEnd;
        if (!waiting) {
            state->startTime = newStart;
            state->endTime = newEnd;
        }
        if (!state->entity.IsValid()) continue;
        if (targetNode > 0 && source.id != targetNode) continue;
        // バインド解決とコンポーネントへの書き戻しは対象ノードだけで行う。
        asset::VFXGraphNode node = source;
        const float normalizedTime = duration > 0.0f
            ? std::clamp(newStart / duration, 0.0f, 1.0f) : 0.0f;
        asset::ApplyVFXBindings(next, component, node, normalizedTime);
        ApplyAuthoredNodeSettingsLive(scene, *state, node);
        // このノードが誰かの親なら、グループ側の Transform も同じ値へ揃える。
        SyncPivotTransform(scene, component, node);
    }
    component.graphDuration = duration;
    for (std::size_t index = 0; index < next.links.size()
                                && index < component.runtimeLinks.size(); ++index)
        component.runtimeLinks[index].delay = next.links[index].delay;
    // グラフ本体は複製せず、エディタが持つ実体をそのまま共有する。
    component.runtimeGraph = component.authoringGraph;
    component.appliedAuthoringRevision = component.authoringRevision;
    return false;
}

void ApplyDynamicVFXBindings(Scene& scene, GameObject& owner, VFXGraphComponent& component)
{
    if (component.runtimeGraph == nullptr) return;
    const asset::VFXGraphAsset& graph = *component.runtimeGraph;
    const float normalizedTime = component.graphDuration > 0.0f
        ? std::clamp(component.playTime / component.graphDuration, 0.0f, 1.0f) : 0.0f;
    for (const auto& binding : graph.bindings) {
        const auto definition = asset::FindVFXParameter(graph, binding.paramName);
        const auto source = definition != nullptr ? asset::ResolveVFXParamSource(graph, component, *definition) : nullptr;
        if (source == nullptr || std::holds_alternative<asset::VFXConstant>(source->source)
            || std::holds_alternative<asset::VFXRandomRange>(source->source)) continue;
        const auto authoring = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto& node) { return node.id == binding.nodeId; });
        const auto runtime = std::find_if(component.runtimeNodes.begin(), component.runtimeNodes.end(),
            [&](const auto& node) { return node.nodeId == binding.nodeId; });
        if (authoring == graph.nodes.end() || runtime == component.runtimeNodes.end()) continue;
        asset::VFXGraphNode node = *authoring;
        reflection::ResolvedProperty property;
        if (!reflection::ResolveProperty(asset::GetVFXNodeSchema(), &node, binding.schemaPath, property)
            || property.property == nullptr) continue;
        std::any value;
        bool resolved = false;
        if (const auto* attribute = std::get_if<asset::VFXAttributeRef>(&source->source))
            resolved = ResolveVFXAttribute(scene, owner, attribute->path, property.property->type, value);
        else
            resolved = asset::EvaluateVFXParamValue(graph, *source, *property.property, normalizedTime,
                node.particle.randomSeed, definition->name, value);
        if (!resolved || !property.property->set(property.owner, value)) continue;
        ApplyRuntimeNodeSettings(scene, *runtime, node, binding.schemaPath);
        SyncPivotTransform(scene, component, node);
    }
}

bool InitializeGraph(Scene& scene, GameObject& owner, VFXGraphComponent& component)
{
    constexpr int MAX_NESTING_DEPTH = 8;
    if (component.nestingDepth > MAX_NESTING_DEPTH) {
        FBZZ_LOG_WARN("VFX Graph nesting exceeded %d: %s", MAX_NESTING_DEPTH, component.graphPath.c_str());
        return false;
    }
    asset::VFXGraphAsset graph;
    std::string error;
    if (component.authoringGraph != nullptr) {
        // エディタの未保存グラフを最優先する。保存しないと反映されない状態を作らない。
        graph = *component.authoringGraph;
        component.appliedAuthoringRevision = component.authoringRevision;
    } else if (!asset::LoadVFXGraphAsset(component.graphPath, graph, &error)) {
        FBZZ_LOG_WARN("VFX Graph load failed: %s (%s)", component.graphPath.c_str(), error.c_str());
        return false;
    }
    std::vector<float> startTimes;
    if (!asset::BuildVFXGraphSchedule(graph, startTimes, component.graphDuration, &error)) {
        FBZZ_LOG_WARN("VFX Graph schedule failed: %s (%s)", component.graphPath.c_str(), error.c_str());
        return false;
    }
    component.resolvedParameters.clear();
    for (const auto& definition : graph.parameters) {
        const auto* source = asset::ResolveVFXParamSource(graph, component, definition);
        if (source != nullptr) component.resolvedParameters.push_back({ definition.name, *source });
    }
    component.runtimeNodes.clear();
    component.runtimeLinks.clear();
    std::unordered_set<int> eventTargets;
    std::unordered_set<int> eventSources;
    for (const auto& link : graph.links) {
        const auto source = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&link](const asset::VFXGraphNode& node) { return node.id == link.fromNode; });
        const bool disabledEventSource = source != graph.nodes.end() && !source->enabled
            && (link.trigger == asset::VFXLinkTrigger::OnCollision
                || link.trigger == asset::VFXLinkTrigger::OnDeath
                || link.trigger == asset::VFXLinkTrigger::OnAnimationEvent
                || link.trigger == asset::VFXLinkTrigger::OnTrigger);
        // 無効ノードは衝突/死亡イベントを発生できない。待機リンクを残すと非loop graphが
        // 永久停止しないため、時間スケジュールへのフォールバックとしてruntime eventだけ除外する。
        if (disabledEventSource) continue;
        component.runtimeLinks.push_back({ link.fromNode, link.toNode,
            static_cast<std::uint8_t>(link.trigger), link.delay, false, link.eventName });
        if (link.trigger == asset::VFXLinkTrigger::OnCollision
            || link.trigger == asset::VFXLinkTrigger::OnDeath
            || link.trigger == asset::VFXLinkTrigger::OnAnimationEvent
            || link.trigger == asset::VFXLinkTrigger::OnTrigger) {
            eventTargets.insert(link.toNode);
            eventSources.insert(link.fromNode);
        }
    }
    component.hasEventLinks = !eventTargets.empty();

    // ── Transform 親子のグループを先に作る ──
    // parentNodeId は link とは独立した木で、子ノードが配列上で親より前に来ることもある。
    // そのため実体生成より前に、親として使われているノードぶんだけグループを作っておく。
    // 実体ではなくグループを親にする理由は VFXGraphComponent::runtimePivots のコメントを参照。
    component.runtimePivots.clear();
    std::unordered_map<int, EntityID> pivotEntities;
    {
        // 親として参照されているノードだけを対象にする (全ノードに作ると Hierarchy が倍になる)。
        std::unordered_set<int> parentIds;
        for (const auto& node : graph.nodes)
            if (node.parentNodeId != -1) parentIds.insert(node.parentNodeId);
        // Entry / Delay は実体を持たない時間ノードなので、グループも作らず owner 直下へ縮退する
        // (CollectVFXGraphWarnings が PARENT_HAS_NO_TRANSFORM として事前に警告する)。
        const auto isPivotable = [&](int nodeId) {
            const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                [&](const asset::VFXGraphNode& candidate) { return candidate.id == nodeId; });
            return it != graph.nodes.end() && !asset::VFXNodeHasNoInstance(it->type);
        };
        // 親から順に作る必要があるため、未解決の親を持つものは次の周回へ回す。
        // ValidateVFXGraphAsset が循環を弾いているので、必ず有限回で収束する。
        std::vector<const asset::VFXGraphNode*> pending;
        pending.reserve(parentIds.size());
        for (const auto& node : graph.nodes)
            if (parentIds.contains(node.id) && isPivotable(node.id)) pending.push_back(&node);
        // 終了条件は「1 周して 1 つも作れなかった」。反復回数で上限を置くと、
        // pending が毎周縮むぶん上限も縮み、深い親子鎖の末端を取りこぼす。
        while (!pending.empty()) {
            std::vector<const asset::VFXGraphNode*> next;
            for (const asset::VFXGraphNode* node : pending) {
                const bool needsParent = node->parentNodeId != -1 && isPivotable(node->parentNodeId);
                if (needsParent && !pivotEntities.contains(node->parentNodeId)) {
                    next.push_back(node);
                    continue;
                }
                GameObject& pivot = scene.CreateGameObject(node->name);
                pivot.runtimeGenerated = true;
                if (needsParent) {
                    if (GameObject* parent = scene.GetGameObject(pivotEntities[node->parentNodeId]))
                        pivot.SetParent(*parent);
                    else
                        pivot.SetParent(owner);
                } else {
                    pivot.SetParent(owner);
                }
                ApplyTransform(pivot, *node);
                pivotEntities[node->id] = pivot.GetID();
                component.runtimePivots.push_back({ node->id, pivot.GetID() });
            }
            if (next.size() == pending.size()) break; // 前進しない = 想定外の構造。無限ループを避ける
            pending.swap(next);
        }
    }

    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        asset::VFXGraphNode node = graph.nodes[index];
        // GPUシミュレーションは粒子死/衝突カウンタをCPUへreadbackしない。
        // イベント源だけCPUへ縮退させ、グラフの発火意味論と決定論を黙って失わないようにする。
        if (node.type == asset::VFXNodeType::Particle && eventSources.contains(node.id))
            node.particle.simulationMode = ParticleSimulationMode::Cpu;
        const float normalizedTime = component.graphDuration > 0.0f
            ? std::clamp(startTimes[index] / component.graphDuration, 0.0f, 1.0f) : 0.0f;
        asset::ApplyVFXBindings(graph, component, node, normalizedTime);
        if (!node.enabled || asset::VFXNodeHasNoInstance(node.type)) {
            // Reroute は時間も消費しないので、終了時刻は開始時刻と同じ
            // (BuildVFXGraphSchedule と必ず同じ扱いにすること)。
            const float nodeDuration =
                asset::VFXNodeIsPassthrough(node.type) ? 0.0f : node.duration;
            component.runtimeNodes.push_back({
                .nodeId = node.id,
                .entity = EntityID::INVALID,
                .startTime = startTimes[index],
                .endTime = startTimes[index] + nodeDuration,
                .scheduledStartTime = startTimes[index],
                .scheduledEndTime = startTimes[index] + nodeDuration,
            });
            continue;
        }
        // 表示名はノード名そのもの。以前は owner GUID と Node ID を名前へ埋めていたが、
        // 実体が UUID 込みの長大な名前で Hierarchy に並ぶため、置いたエフェクトの中身を
        // 目で追えなかった。一意性は GameObject::instanceId が既に持っており、
        // 「保存しない」も runtimeGenerated フラグが担うので、名前は表示に専念させる。
        GameObject* generatedPointer = nullptr;
        std::string poolKey;
        if (node.type == asset::VFXNodeType::AnimatedMesh) {
            poolKey = AnimatedMeshPoolKey(node);
            const auto pooled = std::find_if(component.animatedMeshPool.begin(),
                component.animatedMeshPool.end(), [&](const VFXAnimatedMeshPoolEntry& entry) {
                    return entry.key == poolKey && scene.IsValid(entry.entity);
                });
            if (pooled != component.animatedMeshPool.end()) {
                generatedPointer = scene.GetGameObject(pooled->entity);
                component.animatedMeshPool.erase(pooled);
            }
        }
        if (generatedPointer == nullptr)
            generatedPointer = &scene.CreateGameObject(node.name);
        GameObject& generated = *generatedPointer;
        generated.name = node.name;
        generated.runtimeGenerated = true;
        // 親の決定は 3 段階。socket 指定が最優先で、次に Transform 親、無ければ owner 直下。
        // WHY: attachBone と parentNodeId はどちらも「誰に付くか」を決めるため、両方効かせると
        //      合成順で結果が変わる。socket を正と決め、併用は警告で知らせる。
        GameObject* attachTarget = &owner;
        if (!node.attachBone.empty()) {
            GameObject* socket = FindVFXSocket(owner, node.attachBone);
            // Preview ActorではVFX rootがActorの子になるため、owner配下だけでなく階層rootも探索する。
            if (socket == nullptr) {
                GameObject* hierarchyRoot = &owner;
                while (hierarchyRoot->GetParent() != nullptr)
                    hierarchyRoot = hierarchyRoot->GetParent();
                if (hierarchyRoot != &owner)
                    socket = FindVFXSocket(*hierarchyRoot, node.attachBone);
            }
            if (socket != nullptr) attachTarget = socket;
            else FBZZ_LOG_WARN("VFX socket not found: %s", node.attachBone.c_str());
        } else if (node.parentNodeId != -1) {
            const auto pivot = pivotEntities.find(node.parentNodeId);
            if (pivot != pivotEntities.end())
                if (GameObject* group = scene.GetGameObject(pivot->second)) attachTarget = group;
        }
        generated.SetParent(*attachTarget);
        ApplyTransform(generated, node);
        switch (node.type) {
        case asset::VFXNodeType::Particle:
            AddParticle(generated, node);
            // SubEmitter の名前引きを、この VFX インスタンス配下だけに閉じる。
            // WHY: 生成物の名前をノード名そのものへ短くしたことで、同じ .vfx を複数配置すると
            //      scene.Find("Smoke") が別インスタンスのノードを掴みうる。探索の起点を
            //      owner に固定して防ぐ。同時に、以前は UUID 込みの名前と一致しようがなく
            //      グラフ内の SubEmitter 参照が黙って無反応だった問題も解消する。
            if (auto* emitter = generated.GetComponent<ParticleEmitter>())
                emitter->subEmitterScopeRoot = owner.GetID();
            break;
        case asset::VFXNodeType::Trail: AddTrail(generated, node, false); break;
        case asset::VFXNodeType::MeshTrail: AddTrail(generated, node, true); break;
        case asset::VFXNodeType::Light: AddLight(generated, node); break;
        case asset::VFXNodeType::Audio: AddAudio(generated, node); break;
        case asset::VFXNodeType::Decal: AddDecal(generated, node); break;
        case asset::VFXNodeType::ForceField: AddForceField(generated, node); break;
        case asset::VFXNodeType::Mesh: AddMesh(generated, node); break;
        case asset::VFXNodeType::AnimatedMesh: ConfigureAnimatedMesh(generated, node); break;
        case asset::VFXNodeType::ScreenEffect: AddScreenEffect(generated, node); break;
        case asset::VFXNodeType::CameraShake: AddCameraShake(generated, node); break;
        case asset::VFXNodeType::TimeScale: AddTimeScale(generated, node); break;
        case asset::VFXNodeType::Wind: AddWind(generated, node); break;
        case asset::VFXNodeType::SubGraph: {
            AddSubGraph(generated, node, component.nestingDepth + 1);
            if (auto* child = generated.GetComponent<VFXGraphComponent>()) {
                for (const auto& forward : graph.subGraphForwards) {
                    if (forward.nodeId != node.id) continue;
                    const auto parameter = std::find_if(component.resolvedParameters.begin(),
                        component.resolvedParameters.end(), [&](const asset::VFXParamOverride& value) {
                            return value.paramName == forward.parentParam;
                        });
                    if (parameter != component.resolvedParameters.end())
                        child->parameterOverrides.push_back({ forward.childParam, parameter->value });
                }
            }
            break;
        }
        default: break;
        }
        generated.SetActive(false);
        const bool waitsForEvent = eventTargets.contains(node.id);
        const float runtimeStart = waitsForEvent
            ? (std::numeric_limits<float>::max)() : startTimes[index];
        component.runtimeNodes.push_back({
            .nodeId = node.id,
            .entity = generated.GetID(),
            .startTime = runtimeStart,
            .endTime = waitsForEvent ? runtimeStart : startTimes[index] + node.duration,
            .scheduledStartTime = startTimes[index],
            .scheduledEndTime = startTimes[index] + node.duration,
            .poolKey = poolKey,
            .basePlaybackSpeed = node.type == asset::VFXNodeType::AnimatedMesh
                ? node.animatedMesh.speed : 1.0f,
        });
    }
    component.loadedGraphPath = component.graphPath;
    component.initialized = true;
    component.reloadRequested = false;
    component.playTime = 0.0f;
    component.stopped = false;
    component.playing = component.playing || component.playOnAwake;
    // ライブ編集中はエディタが持つ実体をそのまま共有し、複製を1回省く。
    if (component.authoringGraph != nullptr) component.runtimeGraph = component.authoringGraph;
    else component.runtimeGraph = std::make_shared<const asset::VFXGraphAsset>(std::move(graph));
    return true;
}

} // namespace

ComponentAccess VFXGraphSystem::GetAccess() const
{
    // Scene構造を変更し複数Componentを生成するため、型単位の並列化対象から外す。
    return ComponentAccess{}.Unrestricted();
}

OrderingHints VFXGraphSystem::GetOrder() const
{
    // AudioSystemより先に音源を有効化する。Particleは後段LateUpdateなので同フレームに評価される。
    return OrderingHints{}.Before<AudioSystem>().Before<AnimatorSystem>();
}

void VFXGraphSystem::Update(SystemContext& ctx)
{
    // ヒットストップはゲーム実行時だけ。エディタプレビューで効かせるとエディタ全体が遅くなる。
    if (ctx.simulating) ApplyVFXTimeScale(ctx.scene);

    // GetEntities は Scene 内部ストレージを指す span を返すため、ループ内で GameObject を
    // 生成/破棄すると無効化される。イテレーション前に vector へコピーしてスナップショットを取る。
    const std::span<const EntityID> ownerView = ctx.scene.GetEntities<VFXGraphComponent>();
    const std::vector<EntityID> owners(ownerView.begin(), ownerView.end());
    for (const EntityID id : owners) {
        GameObject* owner = ctx.scene.GetGameObject(id);
        VFXGraphComponent* component = ctx.scene.GetComponent<VFXGraphComponent>(id);
        if (owner == nullptr || component == nullptr) continue;

        const bool editorPreview = component->editorPreviewFrame > 0
            && component->editorPreviewFrame + 2 >= Time::frameCount;
        if (!ctx.simulating && !editorPreview) {
            // Editorプレビューが途切れたら一時GameObjectを破棄し、次回PlayをplayOnAwakeから初期化する。
            if (component->initialized) DestroyRuntimeNodes(ctx.scene, *owner, *component);
            continue;
        }
        if (!component->enabled || component->graphPath.empty()) {
            DestroyRuntimeNodes(ctx.scene, *owner, *component);
            continue;
        }
        // 未保存グラフの差分をまず「作り直さずに」当てにいく。値だけの変更なら
        // 走っている粒子も再生位置も保ったまま見た目が変わる。
        // 構造が変わったときだけ true が返り、下の破棄・再構築へ落ちる。
        const bool authoringNeedsRebuild = SyncAuthoringGraphLive(ctx.scene, *component);
        // 構造が変わった作り直しでも、エディタプレビュー中は再生位置を引き継いで
        // 編集のたびに頭出しへ戻らないようにする (ゲーム実行時の reload 意味論は変えない)。
        const bool resumeAfterRebuild = editorPreview && authoringNeedsRebuild
            && !component->reloadRequested;
        const float resumeTime = component->playTime;
        const bool resumePlaying = component->playing;
        if (component->reloadRequested || authoringNeedsRebuild
            || component->loadedGraphPath != component->graphPath) {
            DestroyRuntimeNodes(ctx.scene, *owner, *component);
        }
        if (!component->initialized && !InitializeGraph(ctx.scene, *owner, *component)) continue;
        if (resumeAfterRebuild) {
            component->playTime = (std::min)(resumeTime, (std::max)(component->graphDuration, 0.0f));
            component->playing = resumePlaying;
        }
        if (component->restartRequested) {
            ResetRuntimeSchedule(ctx.scene, *component);
            component->restartRequested = false;
            component->playTime = 0.0f;
        }
        if (component->editorScrubTime >= 0.0f) {
            const float targetTime = std::clamp(component->editorScrubTime, 0.0f,
                                                (std::max)(component->graphDuration, 0.0f));
            ResetRuntimeSchedule(ctx.scene, *component);
            component->playTime = targetTime;
            component->editorScrubTime = -1.0f;
            for (auto& node : component->runtimeNodes) {
                const bool shouldBeActive = targetTime >= node.startTime
                    && (node.endTime <= node.startTime || targetTime < node.endTime);
                if (shouldBeActive) {
                    SetNodeActive(ctx.scene, node, true);
                    if (GameObject* object = ctx.scene.GetGameObject(node.entity)) {
                        if (auto* emitter = object->GetComponent<ParticleEmitter>())
                            emitter->editorScrubTime = (std::max)(targetTime - node.startTime, 0.0f);
                        if (auto* graph = object->GetComponent<VFXGraphComponent>()) {
                            graph->editorScrubTime = (std::max)(targetTime - node.startTime, 0.0f);
                            graph->Pause();
                            if (editorPreview) graph->editorPreviewFrame = Time::frameCount;
                        }
                    }
                }
            }
        }
        // Curve/Gradient/Attribute/Signalは固定defaultと異なり時刻・ゲーム状態へ追従するため毎フレーム再評価する。
        ApplyDynamicVFXBindings(ctx.scene, *owner, *component);
        if (!component->playing) {
            for (auto& node : component->runtimeNodes) {
                if (component->stopped && node.active) SetNodeActive(ctx.scene, node, false);
                else if (node.active) ApplyPlaybackScale(ctx.scene, node, 0.0f, editorPreview);
            }
            continue;
        }

        component->playTime += ctx.dt * (std::max)(component->speed, 0.0f);
        if (component->syncParentAnimator) {
            GameObject* parent = owner;
            while (parent != nullptr) {
                if (auto* animator = parent->GetComponent<AnimatorComponent>()) {
                    const float animationTime = component->playTime * animator->speed;
                    animator->playing = false;
                    animator->stateTime = animationTime;
                    break;
                }
                parent = parent->GetParent();
            }
        }
        if (component->loop && component->graphDuration > 0.0f
            && component->playTime >= component->graphDuration) {
            component->playTime = std::fmod(component->playTime, component->graphDuration);
            ResetRuntimeSchedule(ctx.scene, *component);
            // ループ先頭が最も繋ぎ目の見えない切れ目。ここで適用待ちの構造変更を消化する。
            // WHY: この直後に playTime は 0 付近へ戻るため、AtGraphCycleBoundary では
            //      ループの折り返しを検出できない。折り返した瞬間に予約しておく。
            if (component->pendingStructuralRebuild) {
                component->pendingStructuralRebuild = false;
                component->reloadRequested = true;
            }
        }
        ConsumeRuntimeEvents(ctx.scene, *component);
        for (auto& node : component->runtimeNodes) {
            const bool shouldBeActive = component->playTime >= node.startTime
                && (node.endTime <= node.startTime || component->playTime < node.endTime);
            if (node.active != shouldBeActive) SetNodeActive(ctx.scene, node, shouldBeActive);
            if (!node.active) continue;
            ApplyPlaybackScale(ctx.scene, node, component->speed, editorPreview);
            // 時間駆動のノード (Mesh / ScreenEffect) だけ、オーサリング値を引いて更新する。
            // ノード数はたかだか数十のため線形探索で足りる。
            if (!component->runtimeGraph) continue;
            const auto& authoredNodes = component->runtimeGraph->nodes;
            const auto authored = std::find_if(authoredNodes.begin(), authoredNodes.end(),
                [&node](const asset::VFXGraphNode& candidate) { return candidate.id == node.nodeId; });
            if (authored != authoredNodes.end())
                ApplyNodeEnvelope(ctx.scene, node, *authored, component->playTime - node.startTime);
        }
        const bool awaitingEvent = std::any_of(component->runtimeLinks.begin(), component->runtimeLinks.end(),
            [](const VFXRuntimeLinkState& link) {
                return !link.fired
                    && (link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnCollision)
                        || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnDeath)
                        || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnAnimationEvent)
                        || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnTrigger));
            });
        if (!component->loop && component->graphDuration > 0.0f
            && component->playTime >= component->graphDuration && !awaitingEvent) {
            component->playing = false;
        }
    }
}

} // namespace fbzz::scene
