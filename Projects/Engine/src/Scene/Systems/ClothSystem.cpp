/// @file    ClothSystem.cpp
/// @brief   格子布の初期化、固定点と接触の同期、描画補間。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RuntimeMeshSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {
using math::Vector3;
Vector3 ToWorld(const Transform& t, const Vector3& p)
{
    return t.worldPosition + t.worldRotation * Vector3{p.x * t.worldScale.x, p.y * t.worldScale.y, p.z * t.worldScale.z};
}
Vector3 ToLocal(const Transform& t, const Vector3& p)
{
    const Vector3 v = t.worldRotation.Inverse() * (p - t.worldPosition);
    return {v.x / t.worldScale.x, v.y / t.worldScale.y, v.z / t.worldScale.z};
}
void Fail(ClothComponent& cloth)
{
    if (!cloth.runtime.failed) FBZZ_LOG_ERROR("Cloth: invalid settings, topology, attachment target or non-finite simulation; check Cloth fields and Transform scale.");
    cloth.runtime.failed = true;
}
bool EnsureCloth(ClothComponent& cloth, const Transform& t, bool force, const asset::ClothAsset** outSource = nullptr)
{
    auto& state = cloth.runtime;
    const auto& q = t.worldRotation;
    const float poseValues[]{t.worldPosition.x, t.worldPosition.y, t.worldPosition.z, q.x, q.y, q.z, q.w};
    for (float v : poseValues) if (!std::isfinite(v)) { Fail(cloth); return false; }
    if (std::abs(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w - 1.0f) > 0.01f) { Fail(cloth); return false; }
    const std::array<float, 9> shape{cloth.width, cloth.height, static_cast<float>(cloth.segments),
        cloth.surfaceDensity, static_cast<float>((cloth.pinTop ? 1 : 0) | (cloth.pinLeft ? 2 : 0)),
        t.worldScale.x, t.worldScale.y, t.worldScale.z, cloth.teleportDistance};
    for (float v : shape) if (!std::isfinite(v)) { Fail(cloth); return false; }
    if (cloth.width < 0.01f || cloth.height < 0.01f || cloth.width > 100.0f || cloth.height > 100.0f
        || cloth.segments < 2 || cloth.segments > 64 || cloth.surfaceDensity < 0.001f
        || cloth.surfaceDensity > 100.0f || t.worldScale.x <= 0.0001f || t.worldScale.y <= 0.0001f
        || t.worldScale.z <= 0.0001f || cloth.teleportDistance <= 0.0f) { Fail(cloth); return false; }
    if (!state.solver.SetSettings(cloth.settings)) { Fail(cloth); return false; }
    const asset::ClothAsset* source = nullptr;
    if (!cloth.clothAssetPath.empty()) {
        source = asset::AssetManager::Get(asset::AssetManager::Load<asset::ClothAsset>(cloth.clothAssetPath));
        if (!source) { state.initialized = false; Fail(cloth); return false; }
    }
    const uint64_t revision = source ? source->revision : 0;
    if (outSource) *outSource = source;
    const bool authoringChanged = state.appliedAsset != cloth.clothAssetPath || state.assetRevision != revision
        || state.appliedOverridePins != cloth.overridePins || state.appliedPins != cloth.pinnedParticles;
    const bool teleported = (t.worldPosition - state.lastOrigin).LengthSq() > cloth.teleportDistance * cloth.teleportDistance;
    if (state.initialized && state.shape == shape && !force && !teleported && !authoringChanged) return true;
    const int row = cloth.segments + 1;
    state.mesh.Clear();
    state.restLocal.clear();
    state.pins.clear();
    std::vector<Vector3> positions;
    state.renderToParticle.clear();
    std::vector<uint32_t> indices;
    if (source) {
        state.mesh.Vertices() = source->vertices;
        state.mesh.Indices() = source->indices;
        state.restLocal = source->particles;
        state.renderToParticle = source->renderToParticle;
        state.pins = source->pins;
        for (const auto& local : state.restLocal) positions.push_back(ToWorld(t, local));
        for (uint32_t index : source->indices) indices.push_back(source->renderToParticle[index]);
    } else {
        for (int y = 0; y < row; ++y) {
            for (int x = 0; x < row; ++x) {
                const float u = static_cast<float>(x) / cloth.segments;
                const float v = static_cast<float>(y) / cloth.segments;
                const Vector3 local{(u - 0.5f) * cloth.width, -v * cloth.height, 0.0f};
                state.restLocal.push_back(local);
                positions.push_back(ToWorld(t, local));
                state.mesh.AddVertex(local, Vector3::FORWARD, {u, v});
                if ((cloth.pinTop && y == 0) || (cloth.pinLeft && x == 0))
                    state.pins.push_back(static_cast<uint32_t>(y * row + x));
            }
        }
        for (int y = 0; y < cloth.segments; ++y) {
            for (int x = 0; x < cloth.segments; ++x) {
                const uint32_t a = static_cast<uint32_t>(y * row + x), b = a + 1, c = a + row, d = c + 1;
                state.mesh.AddTriangle(a, c, b);
                state.mesh.AddTriangle(b, c, d);
            }
        }
            indices = state.mesh.Indices();
        for (uint32_t i = 0; i < positions.size(); ++i) state.renderToParticle.push_back(i);
    }
    if (cloth.overridePins) {
        state.pins.clear();
        for (int pin : cloth.pinnedParticles) {
            if (pin < 0 || static_cast<size_t>(pin) >= positions.size()) {
                state.initialized = false; Fail(cloth); return false;
            }
            state.pins.push_back(static_cast<uint32_t>(pin));
        }
        std::sort(state.pins.begin(), state.pins.end());
        state.pins.erase(std::unique(state.pins.begin(), state.pins.end()), state.pins.end());
    }
    std::vector<float> masses(positions.size(), 0.0f);
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        const float mass = Vector3::Cross(positions[b] - positions[a], positions[c] - positions[a]).Length() * cloth.surfaceDensity / 6.0f;
        masses[a] += mass; masses[b] += mass; masses[c] += mass;
    }
    for (float& mass : masses) mass = mass > 0.0f ? 1.0f / mass : 0.0f;
    for (uint32_t pin : state.pins) masses[pin] = 0.0f;
    if (!state.solver.Initialize(positions, indices, masses)) { state.initialized = false; Fail(cloth); return false; }
    state.previous = positions;
    state.appliedAttachments.clear();
    state.attachmentCenters.clear();
    state.skinCenters.clear();
    state.shape = shape;
    state.appliedAsset = cloth.clothAssetPath;
    state.assetRevision = revision;
    state.appliedOverridePins = cloth.overridePins;
    state.appliedPins = cloth.pinnedParticles;
    state.lastOrigin = t.worldPosition;
    state.initialized = true;
    state.failed = false;
    return true;
}

/// @note Physics は LateUpdate より先。Bone の前回確定姿勢に当フレームの PrePhysics Transform 更新を反映した座標を読む。
/// @see Docs/design/cloth.md ボーン追従の時間契約。
bool MotionConstraints(Scene& scene, const ClothComponent& cloth, std::vector<physics::ClothMotionConstraint>& result,
                       std::vector<Vector3>& skinCenters, const asset::ClothAsset* source)
{
    if (cloth.attachments.empty() && !cloth.useSkinning) return true;
    const auto& state = cloth.runtime;
    const bool reuse = state.appliedAttachments == cloth.attachments
        && state.attachmentCenters.size() == cloth.attachments.size();
    std::vector<unsigned char> used(state.restLocal.size(), 0);
    for (size_t i = 0; i < cloth.attachments.size(); ++i) {
        const auto& attachment = cloth.attachments[i];
        if (attachment.particle < 0 || static_cast<size_t>(attachment.particle) >= used.size()
            || !std::isfinite(attachment.maxDistance) || attachment.maxDistance < 0.0f) return false;
        const uint32_t particle = static_cast<uint32_t>(attachment.particle);
        if (used[particle]) return false;
        used[particle] = 1;
        const auto* target = scene.GetGameObject(attachment.target);
        if (!target || !target->activeInHierarchy()) return false;
        const auto& t = target->transform;
        const auto& q = t.worldRotation;
        const float values[]{q.x, q.y, q.z, q.w, t.worldScale.x, t.worldScale.y, t.worldScale.z};
        for (float value : values) if (!std::isfinite(value)) return false;
        if (std::abs(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w - 1.0f) > 0.01f
            || t.worldScale.x <= 0.0001f || t.worldScale.y <= 0.0001f || t.worldScale.z <= 0.0001f) return false;
        const Vector3 center = ToWorld(t, attachment.localPosition);
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) return false;
        const Vector3 previous = reuse ? state.attachmentCenters[i]
            : (attachment.maxDistance == 0.0f ? state.solver.Positions()[particle] : center);
        result.push_back({particle, previous, center, attachment.maxDistance});
    }
    if (cloth.useSkinning) {
        if (!source || source->skinWeights.size() != state.restLocal.size()
            || !std::isfinite(cloth.skinMaxDistance) || cloth.skinMaxDistance < 0.0f) return false;
        const auto* owner = scene.GetGameObject(cloth.skinTarget);
        const auto* renderer = scene.GetComponent<SkinnedMeshRenderer>(cloth.skinTarget);
        if (!owner || !owner->activeInHierarchy() || !renderer || !renderer->model || !renderer->model->skeleton) return false;
        const auto& skeleton = *renderer->model->skeleton;
        std::vector<math::Matrix4> boneWorld;
        for (const auto& sourceBone : source->skinBones) {
            int node = -1;
            bool found = false;
            for (const auto& bone : skeleton.bones) {
                if (bone.name != sourceBone.name) continue;
                if (found) return false;
                found = true;
                node = bone.nodeIndex;
            }
            if (!found || node < 0 || static_cast<size_t>(node) >= renderer->nodeEntities.size()) return false;
            const auto* bone = scene.GetGameObject(renderer->nodeEntities[node]);
            if (!bone || !bone->activeInHierarchy()) return false;
            const auto& t = bone->transform;
            const auto& q = t.worldRotation;
            const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
            if (!std::isfinite(norm) || std::abs(norm - 1.0f) > 0.01f
                || t.worldScale.x <= 0.0001f || t.worldScale.y <= 0.0001f || t.worldScale.z <= 0.0001f) return false;
            boneWorld.push_back(math::Matrix4::TRS(t.worldPosition, t.worldRotation, t.worldScale));
        }
        if (!asset::EvaluateClothSkinning(*source, boneWorld, skinCenters)) return false;
        std::vector<bool> pinned(state.restLocal.size(), false);
        for (uint32_t pin : state.pins) pinned[pin] = true;
        const bool reuseSkin = state.appliedSkinTarget == cloth.skinTarget
            && state.appliedSkinMaxDistance == cloth.skinMaxDistance && state.skinCenters.size() == skinCenters.size();
        for (uint32_t particle = 0; particle < skinCenters.size(); ++particle) {
            if (used[particle]) continue;
            const float radius = pinned[particle] ? 0.0f : cloth.skinMaxDistance;
            /// @note 明示 attachment から戻る固定点も、古いスキン中心ではなく実際の位置から補間する。
            const Vector3 previous = radius == 0.0f ? state.solver.Positions()[particle]
                : (reuseSkin ? state.skinCenters[particle] : skinCenters[particle]);
            result.push_back({particle, previous, skinCenters[particle], radius});
        }
    }
    return true;
}

std::vector<physics::ClothContact> Contacts(Scene& scene, const ClothComponent& cloth, EntityID owner)
{
    std::vector<physics::ClothContact> result;
    if (cloth.groundEnabled) {
        physics::ClothContact contact;
        contact.type = physics::ClothContactType::PLANE;
        contact.offset = cloth.groundHeight;
        result.push_back(contact);
    }
    if (cloth.collideWithSpheres) {
        for (EntityID id : scene.GetEntities<SphereColliderComponent>()) {
            auto* go = scene.GetGameObject(id);
            const auto* col = scene.GetComponent<SphereColliderComponent>(id);
            if (id == owner || !go || !go->activeInHierarchy() || !col->enabled || col->isTrigger) continue;
            physics::ClothContact contact;
            contact.a = ColliderWorldCenter(*go, *col);
            const Vector3 scale = go->transform.worldScale;
            contact.radius = col->radius * std::max({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
            result.push_back(contact);
        }
    }
    if (cloth.collideWithCapsules) {
        for (EntityID id : scene.GetEntities<CapsuleColliderComponent>()) {
            auto* go = scene.GetGameObject(id);
            const auto* col = scene.GetComponent<CapsuleColliderComponent>(id);
            if (id == owner || !go || !go->activeInHierarchy() || !col->enabled || col->isTrigger) continue;
            physics::ClothContact contact;
            contact.type = physics::ClothContactType::CAPSULE;
            const Vector3 scale = go->transform.worldScale;
            const Vector3 center = ColliderWorldCenter(*go, *col);
            const Vector3 axis = go->transform.worldRotation * Vector3{0, col->halfHeight * std::abs(scale.y), 0};
            contact.a = center - axis;
            contact.b = center + axis;
            contact.radius = col->radius * std::max(std::abs(scale.x), std::abs(scale.z));
            result.push_back(contact);
        }
    }
    return result;
}

bool Upload(ClothComponent& cloth, renderer::ResourceManager& resources)
{
    auto& target = cloth.runtimeMesh;
    target.current ^= 1u;
    auto& slot = target.slots[target.current];
    if (!slot) slot = std::make_unique<renderer::Mesh>();
    auto& mesh = *slot;
    const auto& builder = cloth.runtime.mesh;
    const bool topologyChanged = mesh.cpuIndices != builder.Indices();
    mesh.cpuVertices = builder.Vertices();
    mesh.cpuIndices = builder.Indices();
    mesh.vertexCount = builder.VertexCount();
    mesh.indexCount = builder.IndexCount();
    mesh.ComputeBounds();
    if (!mesh.vertexBuffer.IsValid() || mesh.vertexCapacity < mesh.vertexCount) {
        if (mesh.vertexBuffer.IsValid()) resources.Release(mesh.vertexBuffer);
        mesh.vertexCapacity = mesh.vertexCount;
        mesh.vertexBuffer = resources.CreateVertexBuffer(nullptr, mesh.vertexCapacity * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    }
    bool uploadIndices = topologyChanged;
    if (!mesh.indexBuffer.IsValid() || mesh.indexCapacity < mesh.indexCount) {
        if (mesh.indexBuffer.IsValid()) resources.Release(mesh.indexBuffer);
        mesh.indexCapacity = mesh.indexCount;
        mesh.indexBuffer = resources.CreateIndexBuffer(nullptr, mesh.indexCapacity);
        uploadIndices = true;
    }
    if (!mesh.vertexBuffer.IsValid() || !mesh.indexBuffer.IsValid()) return false;
    resources.Update(mesh.vertexBuffer, mesh.cpuVertices.data(), mesh.cpuVertices.size() * sizeof(renderer::Vertex));
    if (uploadIndices) resources.Update(mesh.indexBuffer, mesh.cpuIndices.data(), mesh.cpuIndices.size() * sizeof(uint32_t));
    return true;
}
}

ComponentAccess ClothSystem::GetAccess() const
{
    return ComponentAccess{}.Reads<Transform, SphereColliderComponent, CapsuleColliderComponent, SkinnedMeshRenderer>().Writes<ClothComponent>();
}
OrderingHints ClothSystem::GetOrder() const { return OrderingHints{}.After<PhysicsSystem>(); }
void ClothSystem::Update(SystemContext& ctx)
{
    if (!ctx.simulating) return;
    for (EntityID id : ctx.scene.GetEntities<ClothComponent>()) {
        auto* go = ctx.scene.GetGameObject(id);
        auto* cloth = ctx.scene.GetComponent<ClothComponent>(id);
        if (!go || !go->activeInHierarchy() || !cloth->enabled) continue;
        const asset::ClothAsset* source = nullptr;
        if (!EnsureCloth(*cloth, go->transform, false, &source)) continue;
        auto& state = cloth->runtime;
        std::vector<physics::ClothMotionConstraint> motion;
        std::vector<Vector3> skinCenters;
        if (!MotionConstraints(ctx.scene, *cloth, motion, skinCenters, source)) { Fail(*cloth); continue; }
        state.previous = state.solver.Positions();
        for (uint32_t pin : state.pins)
            if (!state.solver.SetPinTarget(pin, ToWorld(go->transform, state.restLocal[pin]))) Fail(*cloth);
        const auto contacts = Contacts(ctx.scene, *cloth, id);
        if (!state.solver.Step(ctx.fixedDt, contacts, motion)) Fail(*cloth);
        else {
            state.failed = false;
            state.appliedAttachments = cloth->attachments;
            state.attachmentCenters.clear();
            for (size_t i = 0; i < cloth->attachments.size(); ++i) state.attachmentCenters.push_back(motion[i].center);
            state.skinCenters = std::move(skinCenters);
            state.appliedSkinTarget = cloth->skinTarget;
            state.appliedSkinMaxDistance = cloth->skinMaxDistance;
        }
        state.lastOrigin = go->transform.worldPosition;
    }
}

ComponentAccess ClothRenderSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }
OrderingHints ClothRenderSystem::GetOrder() const
{
    return OrderingHints{}.After<TransformLateUpdate>().After<RuntimeMeshSystem>();
}
void ClothRenderSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;
    for (EntityID id : ctx.scene.GetEntities<ClothComponent>()) {
        auto* go = ctx.scene.GetGameObject(id);
        auto* cloth = ctx.scene.GetComponent<ClothComponent>(id);
        if (!go || !cloth) continue;
        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!go->activeInHierarchy() || !cloth->enabled) {
            if (meshRenderer && meshRenderer->mesh == cloth->runtimeMesh.Current()) meshRenderer->enabled = false;
            cloth->runtime.initialized = false;
            continue;
        }
        if (!EnsureCloth(*cloth, go->transform, !ctx.playing && !ctx.simulating)) {
            if (meshRenderer && meshRenderer->mesh == cloth->runtimeMesh.Current()) meshRenderer->enabled = false;
            continue;
        }
        auto& state = cloth->runtime;
        state.ownerScene = &ctx.scene;
        state.owner = id;
        const auto& positions = state.solver.Positions();
        const float alpha = ctx.simulating ? std::clamp(ctx.interpolationAlpha, 0.0f, 1.0f) : 1.0f;
        for (size_t i = 0; i < state.renderToParticle.size(); ++i) {
            const uint32_t particle = state.renderToParticle[i];
            state.mesh.Vertices()[i].position = ToLocal(go->transform, Vector3::Lerp(state.previous[particle], positions[particle], alpha));
        }
        state.mesh.RecalculateNormals();
        state.mesh.RecalculateTangents();
        if (!Upload(*cloth, *ctx.resources)) { Fail(*cloth); continue; }
        if (!meshRenderer) meshRenderer = &go->AddComponent<MeshRenderer>();
        meshRenderer->mesh = cloth->runtimeMesh.Current();
        meshRenderer->enabled = true;
        /// @note 空の参照なら同じ GameObject の既存 Material を使用する。
        if (!cloth->materialPath.empty() && state.appliedMaterial != cloth->materialPath) {
            auto* material = go->GetComponent<MaterialComponent>();
            if (!material) material = &go->AddComponent<MaterialComponent>();
            material->materialPath = cloth->materialPath;
            state.appliedMaterial = cloth->materialPath;
        }
    }
}
}
