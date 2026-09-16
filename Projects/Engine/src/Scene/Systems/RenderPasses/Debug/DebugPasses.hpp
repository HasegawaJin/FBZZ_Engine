/// @file    DebugPasses.hpp
/// @brief   デバッグ描画 IRenderPass の宣言。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

#include <vector>

namespace fbzz::scene {

struct RenderPassContext;

// スクリプトの Gizmo と DebugDraw 要求を HDR へ重ねる。
// WHY クラスにしないか: OnDrawGizmos の実行はスクリプトの寿命に触るので、
//     表示トグルではなく «常に» 走る。他のデバッグ描画とは性質が違う。
void ExecuteScriptDebugDrawPass(RenderPassContext& ctx);

// デバッグ描画パスは «表示が切られていればグラフに載せない»。
// WHY IsEnabled に置くか: Execute 冒頭で早期 return しても、パス自体はグラフに載ったまま
//     HDR の read-write 連鎖を伸ばし続ける。表示を切ったのに依存だけが残る状態になり、
//     デッドパスカリングも効かない。判定は 1 か所 (IsEnabled) に置く。

class DebugCollidersPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class AnimatorDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class GridDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class LightRangeDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

// パーティクル力場の影響体積とエミッターの発生形状をワイヤーで可視化する。
// WHY: どちらも「粒子の動きからしか推測できない見えない体積」で、VFX の調整で
//      最も当て推量になりやすい部分だった (力場の半径・向き、Sphere/Cone/Box の発生範囲)。
class VFXGizmoDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class TerrainCollisionDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class NavMeshDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class DecalDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ConstraintDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

// ラグドールの剛体・関節の可動域・接触点を重ねて描く。
// WHY: 「どの関節が力負けしたか」「足が床に触れているか」は数だけでは位置が分からず、
//      可動域とサーボの詰めが当て推量になる。飽和した関節を色で分けるのが要点。
class RagdollDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;

private:
    std::vector<RagdollDebugLine> m_lines;
};

} // namespace fbzz::scene
