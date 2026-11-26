/// @file    ClothComponent.hpp
/// @brief   格子・アセット布の設定、ボーン追従と個体専用の実行状態。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Components/Gameplay/PresentationComponents.hpp>
#include <Engine/Scene/ControlPoints/ControlPointAdapters.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <Engine/Scene/Geometry/MeshBuilder.hpp>
#include <Engine/Asset/Physics/ClothAsset.hpp>
#include <Physics/Cloth/ClothSolver.hpp>
#include <algorithm>
#include <array>
#include <cstdint>

namespace fbzz::scene {
class Scene;

/// @note target は同一 Scene の Bone または GameObject。localPosition はそのローカル座標、maxDistance はワールド [m]。
/// @see Docs/design/cloth.md ボーン追従の時間契約と固定点との優先順位。
struct ClothAttachment {
    int particle = 0; ///< 取り付ける質点の添字。現在の布トポロジーの粒子列を参照する。
    EntityID target{}; ///< 取り付け先の骨または GameObject の、同じシーン内の実行時 ID。
    math::Vector3 localPosition{}; ///< 取り付け先の Transform を基準とする拘束中心のローカル座標。
    float maxDistance = 0.0f; ///< 拘束中心から質点が離れられる最大距離（ワールド m）。0 で中心に固定する。
    std::uint32_t bindingId = 0; ///< 取り付けを並べ替えても保持する識別子。
    ObjectReference targetReference; ///< 保存したシーンから取り付け先を再解決するためのオブジェクト参照。
    std::uint64_t topologySignature = 0; ///< 取り付け時の布トポロジーの署名。異なる布の質点番号の誤適用を防ぐ。
    bool operator==(const ClothAttachment& other) const
    {
        return bindingId == other.bindingId && particle == other.particle && target == other.target
            && targetReference.guid == other.targetReference.guid
            && targetReference.id == other.targetReference.id
            && targetReference.sceneGeneration == other.targetReference.sceneGeneration
            && topologySignature == other.topologySignature
            && localPosition == other.localPosition && maxDistance == other.maxDistance;
    }
};
/// @note 質点ごとの最大移動距離 [m]。未指定は skinMaxDistance を継承し、非スキン布では拘束なし。
/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints Motion Constraints。
struct ClothMaxDistance {
    int particle = 0; ///< 移動距離を上書きする質点の添字。
    float distance = 0.1f; ///< 拘束中心からこの質点が離れられる最大距離（ワールド m）。
    bool operator==(const ClothMaxDistance&) const = default;
};
struct ClothColliderPose {
    EntityID entity{}; ///< 前回の固定更新と照合する接触コライダーのエンティティ ID。
    math::Vector3 position{}; ///< 接触の連続性を評価するために保持するコライダーのワールド位置。
    math::Quaternion rotation{}; ///< 接触の連続性を評価するために保持するコライダーのワールド回転。
};

struct ClothRuntime {
    physics::ClothSolver solver; ///< この布の実行時質点と接触を所有するソルバー。
    MeshBuilder mesh; ///< 現在の布の描画位置から頂点と添字を組み立てる CPU メッシュ。
    std::vector<math::Vector3> restLocal; ///< 布オブジェクトのローカル空間における、初期状態の質点座標列。
    std::vector<math::Vector3> previous; ///< 描画補間の起点として保持する、前の固定更新のワールド質点座標。
    std::vector<uint32_t> pins; ///< 固定済みの質点を示すソルバー内の添字列。
    std::vector<uint32_t> renderToParticle; ///< 描画頂点から対応するシミュレーション質点を引く添字表。
    std::vector<asset::ClothRenderBinding> renderBindings; ///< 描画頂点へ複数の質点の変位を反映するためのアセットの補間情報。
    std::vector<int> appliedPins; ///< ソルバーを初期化したときに用いたオーサリング側の固定点リスト。
    std::vector<ClothAttachment> appliedAttachments; ///< 前回の拘束中心を再利用できるか判定するために保持した取り付け設定。
    std::vector<ClothMaxDistance> appliedMaxDistances; ///< 前回の固定更新で適用した質点ごとの移動半径設定。
    std::vector<ClothColliderPose> colliderPoses; ///< 固定更新間の接触形状の移動を計算するために保存したコライダー姿勢。
    std::vector<math::Vector3> attachmentCenters; ///< 前回適用した取り付け先ごとの拘束中心（ワールド座標）。
    std::vector<math::Vector3> skinCenters; ///< 前回のボーン姿勢から求めた、質点番号ごとの拘束中心（ワールド座標）。
    EntityID appliedSkinTarget{}; ///< 前回の拘束中心の計算に使ったスキニング対象のエンティティ ID。
    float appliedSkinMaxDistance = 0.0f; ///< 前回適用したスキニング拘束の既定半径（m）。
    std::string appliedAsset; ///< ソルバーを初期化したときに使用した布アセット参照。
    uint64_t assetRevision = 0; ///< ソルバーの再初期化を判断するために保持した布アセットの版。
    bool appliedOverridePins = false; ///< ソルバーを初期化したときに固定点リストを差し替えたか。
    std::array<float, 9> shape{}; ///< 布の再初期化比較に用いる格子寸法、刻み数、面密度、固定辺、ワールドスケール、移動しきい値。
    math::Vector3 lastOrigin{}; ///< テレポートを判定するために保持する前回の布オブジェクトのワールド位置。
    Scene* ownerScene = nullptr; ///< 布オブジェクトの参照を解決する非所有のシーン。未設定は nullptr。
    EntityID owner{}; ///< 自分の Transform を照会するための布オブジェクトのエンティティ ID。
    bool initialized = false; ///< 現在の形状でソルバーの初期化に成功した場合に true。
    bool failed = false; ///< ソルバーや描画用変換の失敗状態。再初期化に成功すると解消する。
    std::string appliedMaterial; ///< ランタイムメッシュに最後に反映した材質アセットの参照。
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
    bool enabled = true; ///< この布をソルバーおよび描画メッシュの更新対象にする。
    std::string clothAssetPath; ///< 布トポロジーのアセット参照。空なら設定値から格子を生成する。
    bool overridePins = false; ///< pinnedParticles で形状の既定固定点を差し替える。
    std::vector<int> pinnedParticles; ///< overridePins=true ならアセット/格子の固定点をこの質点番号リストで置き換える。空なら全点が自由。
    std::vector<ClothAttachment> attachments; ///< 質点をボーンなどのオブジェクトへ追従させる取り付け設定列。
    bool useSkinning = false; ///< スキン情報を持つ布の拘束中心を現在のボーン姿勢から求める。
    EntityID skinTarget{}; ///< スキニングに用いる Animator を持つオブジェクトの参照。
    float skinMaxDistance = 0.1f; ///< スキニングで求めた姿勢から質点が離れられる既定の半径（ワールド m）。
    std::vector<ClothMaxDistance> maxDistances; ///< 明示 attachments と固定点が優先。非スキン布では Transform で移動する静止位置を中心とする。
    float width = 2.0f; ///< 手続き生成する格子布のローカル X 方向の幅（m）。
    float height = 2.0f; ///< 手続き生成する格子布のローカル Y 方向の高さ（m）。
    int segments = 16; ///< 格子布を各軸で分割する区間数。頂点は各軸 segments + 1 個。
    float surfaceDensity = 0.2f; ///< 布の面積から質点の質量を求める際に使う面密度（kg/m²）。
    bool pinTop = true; ///< 手続き生成した格子布の上辺を既定の固定点に含める。
    bool pinLeft = false; ///< 手続き生成した格子布の左辺を既定の固定点に含める。
    bool collideWithSpheres = true; ///< 球コライダーを布の接触候補に含める。
    bool collideWithCapsules = true; ///< カプセルコライダーを布の接触候補に含める。
    uint32_t collisionMask = 0xffffffffu; ///< この布と接触できるコライダーのレイヤービット集合。
    bool twoWayCoupling = false; ///< 布が受けた接触の反作用を相手側の剛体へ返す。
    float interCollisionDistance = 0.0f; ///< 0 は別 Cloth との接触なし。双方の距離が正のとき大きい方を質点間の最小距離 [m] に使う。
    bool interCollisionFaces = false; ///< 別の布との接触判定に面を含める。
    bool interContinuousCollision = false; ///< 別の布との接触判定で固定更新間の移動も考慮する。
    bool groundEnabled = false; ///< この布の接触相手に水平な地面を追加する。
    float groundHeight = 0.0f; ///< 布の地面とする水平面のワールド Y 座標。
    float teleportDistance = 5.0f; ///< 1 回の更新でこの距離（ワールド m）を超えて移動すると布を初期化し直す。
    bool receiveFlowFields = false; ///< シーンの流れの影響をこの布の質点に適用する。
    uint32_t flowFieldChannels = 0xffffffffu; ///< この布が受け取る流れのチャンネルマスク。
    std::string materialPath = "guid:732e0b24798a45baa3285d0c867a0fec"; ///< 布のランタイム描画メッシュに割り当てる材質アセットの参照。
    physics::ClothSettings settings; ///< 布ソルバーの重力、拘束、減衰などのシミュレーション設定。
    ClothRuntime runtime; ///< 個体ごとに所有する未保存の布ソルバーと準備状態。
    DoubleBufferedMesh runtimeMesh; ///< 布の描画メッシュの更新と公開を入れ替える二重バッファ。

    void ResetGpuRuntime() { runtimeMesh.Reset(); runtime.initialized = false; }

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
        if (removed < pinnedParticles.size()) {
            pinnedParticles.erase(pinnedParticles.begin() + static_cast<std::ptrdiff_t>(removed));
        }
        r.Group("Bone Attachments");
        const size_t attachmentCount = r.BeginObjectList("attachments", attachments.size());
        attachments.resize(attachmentCount);
        for (size_t i = 0; i < attachmentCount; ++i) {
            r.BeginObjectElement(i);
            auto& attachment = attachments[i];
            int bindingId = static_cast<int>((std::min)(attachment.bindingId, 0x7fffffffu));
            r.Field("bindingId", bindingId);
            attachment.bindingId = bindingId < 0 ? 0u : static_cast<std::uint32_t>(bindingId);
            r.Field("particle", attachment.particle);
            r.Field("target", attachment.target);
            r.ObjectReferenceField("targetReference", attachment.targetReference);
            ReflectControlPointSignature(r, "topologySignature", attachment.topologySignature);
            r.Field("localPosition", attachment.localPosition);
            r.Field("maxDistance", attachment.maxDistance);
            r.EndObjectElement();
        }
        const size_t removedAttachment = r.EndObjectList();
        if (removedAttachment < attachments.size()) {
            attachments.erase(attachments.begin() + static_cast<std::ptrdiff_t>(removedAttachment));
        }
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
        if (removedDistance < maxDistances.size()) {
            maxDistances.erase(maxDistances.begin() + static_cast<std::ptrdiff_t>(removedDistance));
        }
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
