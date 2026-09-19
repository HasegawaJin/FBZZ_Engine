/// @file    DebugPasses.hpp
/// @brief   デバッグ描画 IRenderPass の宣言。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

struct RenderPassContext;
class GameObject;

/// @brief 描き先 1 枚へ ReadWrite で線を重ねるデバッグパスの共通部。
/// @param target グラフ上の描き先。深度なしの線はトーンマップ後の LDR 終端 (UpscaleSrc / Output)、
///               シーン深度で遮蔽させたいものは "HDR"。
/// @note 表示の入り切りは IsEnabled に置く。Execute の早期 return ではグラフに依存が残る。
/// @see RenderSystem.cpp のデバッグパス登録
class DebugOverlayPass : public IRenderPass {
public:
    explicit DebugOverlayPass(std::string_view target) : m_target(target) {}
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;

protected:
    std::string m_target;
};

/// @brief 選択判定。選択の子孫も選択扱い (エディターが selectedObjects へ子孫を展開する)。
[[nodiscard]] bool IsSelectedForDebug(const GameObject& go, const RenderPassContext& ctx);

/// @brief axis に直交する単位ベクトルを 1 本返す。
/// @param axis 正規化済みであること。
[[nodiscard]] math::Vector3 DebugPerpendicular(const math::Vector3& axis);

/// @brief axis に直交する円。dashed なら破線。
/// @param axis 正規化済みであること。
void DrawDebugCircle(renderer::IRenderer& r, const math::Vector3& center, const math::Vector3& axis,
                     float radius, const math::Vector4& color, bool dashed = false);

/// @brief 直交 3 円の球。DebugDraw::Sphere と違い破線を選べる。
void DrawDebugSphere(renderer::IRenderer& r, const math::Vector3& center, float radius,
                     const math::Vector4& color, bool dashed = false);

/// @brief axis 周りの回転の向きを 3/4 周の弧と矢じりで示す。
/// @param sign 正なら v = Cross(axis, u) の側へ回る (SampleFlow の Vortex と同じ向き)。0 なら描かない。
void DrawDebugSwirl(renderer::IRenderer& r, const math::Vector3& center, const math::Vector3& axis,
                    float radius, float sign, const math::Vector4& color);

/// @brief スクリプトの OnDrawGizmos / OnDrawGizmosSelected と debug.Draw* を記録する。
/// @note グラフ実行前に 1 ビュー 1 回だけ呼ぶ。深度ありと深度なしを別パスで描き分けるため。
/// @note duration の減算は Scene::TickScriptDebugDrawCommands が別途フレーム 1 回で行う。
void CaptureScriptGizmos(RenderPassContext& ctx, renderer::DebugDrawCapture& out);

/// @brief CaptureScriptGizmos の結果のうち 1 層を描き先へ流す。
class ScriptGizmoPass final : public DebugOverlayPass {
public:
    ScriptGizmoPass(std::string_view target, std::string_view name,
                    renderer::DebugDrawLayer layer, const renderer::DebugDrawCapture& capture)
        : DebugOverlayPass(target), m_name(name), m_layer(layer), m_capture(capture) {}
    std::string_view Name() const override { return m_name; }
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    std::string                        m_name;
    renderer::DebugDrawLayer           m_layer;
    const renderer::DebugDrawCapture&  m_capture;
};

class DebugCollidersPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class AnimatorDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief グリッドは深度テストありの線なので、シーン深度を持つ HDR へ固定で描く。
class GridDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class LightRangeDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief エミッターの発生形状と初速。
/// @note 流れの場は FlowFieldDebugPass へ分けた。同じスイッチだと粒子の形を見たいだけで場の球が画面を埋める。
class VFXGizmoDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief FlowField の効く範囲 (球 / Baked の箱)・減衰の目安・流れの向き。
/// @note 選択中は濃く、それ以外は薄く描く。channels を絞った場は破線。
class FlowFieldDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief 格子点で SampleFlow を評価した実効流速の矢印。
/// @note 範囲は選択中の FlowField を囲む箱、無ければカメラ前方。channels は全ビットで引く。
class FlowSampleDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    std::vector<math::Vector3> m_positions;
    std::vector<math::Vector3> m_velocities;
};

/// @brief 全 WaterComponent の水流・渦・浮力の届く深さ。
/// @note 範囲は浮力 (MakeWaterFluidDesc) と同じ «回転を無視した軸平行の矩形» で描く。
class WaterFlowDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief VolumeComponent (重力・渦・爆風・時間・磁場) のトリガー形状と効果の向き・残り時間。
/// @note トリガーが無い・判定できない形状の Volume は赤で知らせる (物理側では黙って効かない)。
class PhysicsVolumeDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class TerrainCollisionDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief NavMesh の面は深度テストありの塗りなので、シーン深度を持つ HDR へ固定で描く。
class NavMeshDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class DecalDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ConstraintDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief ラグドールの剛体・可動域・接触点。飽和した関節を色で分ける。
class RagdollDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    std::vector<RagdollDebugLine> m_lines;
};

/// @brief 選択中 RigidBody の速度矢印と重心。
class RigidBodyDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief IKSolver のチェーン・ターゲット・ポール。
class IKDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief SpringBone のチェーン (シミュレーション済みの尾端) とコライダー。
class SpringBoneDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief SocketAttachment / TransformConstraint から追従先への線。
class AttachmentDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @brief 選択中の線状エフェクト (VFXLine / VFXBeam / Trail / MeshTrail / LineRenderer) の経路。
class VFXPathDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    std::vector<math::Vector3> m_points;
};

/// @brief Terrain の範囲箱と LODGroup の判定球。
class BoundsDebugPass final : public DebugOverlayPass {
public:
    using DebugOverlayPass::DebugOverlayPass;
    std::string_view Name() const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
