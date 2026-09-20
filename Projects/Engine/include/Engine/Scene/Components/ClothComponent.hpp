/// @file    ClothComponent.hpp
/// @brief   格子・アセット布の設定、ボーン追従と個体専用の実行状態。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Physics/Cloth/ClothSolver.hpp>
#include <array>

namespace fbzz::scene {
class Scene;

/// @note target は同一 Scene の Bone または GameObject。localPosition はそのローカル座標、maxDistance はワールド [m]。
/// @see Docs/design/cloth.md ボーン追従の時間契約と固定点との優先順位。
struct ClothAttachment {
    int particle = 0;
    EntityID target{};
    math::Vector3 localPosition{};
    float maxDistance = 0.0f;
    bool operator==(const ClothAttachment&) const = default;
};
/// @note 質点ごとの最大移動距離 [m]。未指定は skinMaxDistance を継承し、非スキン布では拘束なし。
/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints Motion Constraints。
struct ClothMaxDistance {
    int particle = 0;
    float distance = 0.1f;
    bool operator==(const ClothMaxDistance&) const = default;
};
struct ClothColliderPose {
    EntityID entity{};
    math::Vector3 position{};
    math::Quaternion rotation{};
};

struct ClothRuntime {
    physics::ClothSolver solver;
    MeshBuilder mesh;
    std::vector<math::Vector3> restLocal;
    std::vector<math::Vector3> previous;
    std::vector<uint32_t> pins;
    std::vector<uint32_t> renderToParticle;
    std::vector<asset::ClothRenderBinding> renderBindings;
    std::vector<int> appliedPins;
    std::vector<ClothAttachment> appliedAttachments;
    std::vector<ClothMaxDistance> appliedMaxDistances;
    std::vector<ClothColliderPose> colliderPoses;
    std::vector<math::Vector3> attachmentCenters;
    std::vector<math::Vector3> skinCenters;
    EntityID appliedSkinTarget{};
    float appliedSkinMaxDistance = 0.0f;
    std::string appliedAsset;
    uint64_t assetRevision = 0;
    bool appliedOverridePins = false;
    std::array<float, 9> shape{};
    math::Vector3 lastOrigin{};
    Scene* ownerScene = nullptr;
    EntityID owner{};
    bool initialized = false;
    bool failed = false;
    std::string appliedMaterial;
    ClothRuntime() = default;
    /// @note 複製先は質点・Scene 参照を継承せず、自分の Transform で初期化する。
    ClothRuntime(const ClothRuntime&) {}
    ClothRuntime& operator=(const ClothRuntime&) { *this = ClothRuntime{}; return *this; }
    ClothRuntime(ClothRuntime&&) noexcept = default;
    ClothRuntime& operator=(ClothRuntime&&) noexcept = default;
};

/// @note アセット未指定ならローカル XY 平面の格子布。幅 [m]、高さ [m]、面密度 [kg/m²]。
/// @see Docs/design/cloth.md
struct ClothComponent {
    bool enabled = true;
    std::string clothAssetPath;
    bool overridePins = false;
    /// @note overridePins=true ならアセット/格子の固定点をこの質点番号リストで置き換える。空なら全点が自由。
    std::vector<int> pinnedParticles;
    std::vector<ClothAttachment> attachments;
    bool useSkinning = false;
    EntityID skinTarget{};
    float skinMaxDistance = 0.1f;
    /// @note 明示 attachments と固定点が優先。非スキン布では Transform で移動する静止位置を中心とする。
    std::vector<ClothMaxDistance> maxDistances;
    float width = 2.0f;
    float height = 2.0f;
    int segments = 16;
    float surfaceDensity = 0.2f;
    bool pinTop = true;
    bool pinLeft = false;
    bool collideWithSpheres = true;
    bool collideWithCapsules = true;
    uint32_t collisionMask = 0xffffffffu;
    bool twoWayCoupling = false;
    /// @note 0 は別 Cloth との接触なし。双方の距離が正のとき大きい方を質点間の最小距離 [m] に使う。
    float interCollisionDistance = 0.0f;
    bool interCollisionFaces = false;
    bool interContinuousCollision = false;
    bool groundEnabled = false;
    float groundHeight = 0.0f;
    float teleportDistance = 5.0f;
    bool receiveFlowFields = false;
    uint32_t flowFieldChannels = 0xffffffffu;
    std::string materialPath = "guid:732e0b24798a45baa3285d0c867a0fec";
    physics::ClothSettings settings;
    ClothRuntime runtime;
    DoubleBufferedMesh runtimeMesh;

    const char* GetTypeName() const { return "Cloth"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Group("Cloth Asset");
        r.Field("clothAssetPath", clothAssetPath);
        r.Field("overridePins", overridePins);
        const size_t pinCount = r.BeginObjectList("pinnedParticles", pinnedParticles.size());
        pinnedParticles.resize(pinCount);
        for (size_t i = 0; i < pinCount; ++i) {
            r.BeginObjectElement(i);
            r.Field("particle", pinnedParticles[i]);
            r.EndObjectElement();
        }
        const size_t removed = r.EndObjectList();
        if (removed < pinnedParticles.size()) pinnedParticles.erase(pinnedParticles.begin() + static_cast<std::ptrdiff_t>(removed));
        r.Group("Bone Attachments");
        const size_t attachmentCount = r.BeginObjectList("attachments", attachments.size());
        attachments.resize(attachmentCount);
        for (size_t i = 0; i < attachmentCount; ++i) {
            r.BeginObjectElement(i);
            auto& attachment = attachments[i];
            r.Field("particle", attachment.particle);
            r.Field("target", attachment.target);
            r.Field("localPosition", attachment.localPosition);
            r.Field("maxDistance", attachment.maxDistance);
            r.EndObjectElement();
        }
        const size_t removedAttachment = r.EndObjectList();
        if (removedAttachment < attachments.size())
            attachments.erase(attachments.begin() + static_cast<std::ptrdiff_t>(removedAttachment));
        r.Group("Skin Weights");
        r.Field("useSkinning", useSkinning);
        r.Field("skinTarget", skinTarget);
        r.Field("skinMaxDistance", skinMaxDistance);
        const size_t distanceCount = r.BeginObjectList("maxDistances", maxDistances.size());
        maxDistances.resize(distanceCount);
        for (size_t i = 0; i < distanceCount; ++i) {
            r.BeginObjectElement(i);
            r.Field("particle", maxDistances[i].particle);
            r.Field("distance", maxDistances[i].distance);
            r.EndObjectElement();
        }
        const size_t removedDistance = r.EndObjectList();
        if (removedDistance < maxDistances.size())
            maxDistances.erase(maxDistances.begin() + static_cast<std::ptrdiff_t>(removedDistance));
        r.Group("Grid");
        r.FloatRange("width", width, 0.01f, 100.0f);
        r.FloatRange("height", height, 0.01f, 100.0f);
        r.IntRange("segments", segments, 2, 64);
        r.FloatRange("surfaceDensity", surfaceDensity, 0.001f, 100.0f);
        r.Field("pinTop", pinTop);
        r.Field("pinLeft", pinLeft);
        r.Field("materialPath", materialPath);
        r.Group("Simulation");
        r.Field("gravity", settings.gravity);
        r.Field("windVelocity", settings.windVelocity);
        r.Field("stretchCompliance", settings.stretchCompliance);
        r.Field("bendCompliance", settings.bendCompliance);
        r.Field("dihedralBending", settings.dihedralBending);
        r.Field("damping", settings.damping);
        r.Field("airDensity", settings.airDensity);
        r.Field("dragCoefficient", settings.dragCoefficient);
        r.IntRange("substeps", settings.substeps, 1, 64);
        r.IntRange("iterations", settings.iterations, 1, 32);
        r.Field("teleportDistance", teleportDistance);
        r.Group("Flow Fields");
        r.Field("receiveFlowFields", receiveFlowFields);
        int flowChannelsValue = static_cast<int>(flowFieldChannels);
        r.Field("flowFieldChannels", flowChannelsValue);
        flowFieldChannels = static_cast<uint32_t>(flowChannelsValue);
        r.Group("Collision");
        r.Field("collideWithSpheres", collideWithSpheres);
        r.Field("collideWithCapsules", collideWithCapsules);
        int collisionMaskValue = static_cast<int>(collisionMask);
        r.Field("collisionMask", collisionMaskValue);
        collisionMask = static_cast<uint32_t>(collisionMaskValue);
        r.Field("twoWayCoupling", twoWayCoupling);
        r.Field("interCollisionDistance", interCollisionDistance);
        r.Field("interCollisionFaces", interCollisionFaces);
        r.Field("interContinuousCollision", interContinuousCollision);
        r.Field("groundEnabled", groundEnabled);
        r.Field("groundHeight", groundHeight);
        r.Field("thickness", settings.thickness);
        r.Field("selfCollisionDistance", settings.selfCollisionDistance);
        r.FloatRange("selfCollisionStiffness", settings.selfCollisionStiffness, 0.0f, 1.0f);
        r.Field("selfCollisionFaces", settings.selfCollisionFaces);
        r.Field("continuousCollision", settings.continuousCollision);
        r.FloatRange("friction", settings.friction, 0.0f, 1.0f);
    }
};
}
