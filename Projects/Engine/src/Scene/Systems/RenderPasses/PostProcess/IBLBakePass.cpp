// FBZZ Engine
// IBLBakePass.cpp | fbzz::scene
// BRDF 積分 LUT をスタートアップ時に一度だけ Compute Shader で焼く。
// WHY: LUT は NdotV × roughness の全組み合わせで積分した定数テーブルで、
//      シーンや設定が変わっても値は変わらない。毎フレーム計算するのは無駄なため、
//      初回フレームのみ実行し、以降はスキップする。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteIBLBakeBrdfLutPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.iblBrdfBakeShader.IsValid() || !h.iblBrdfLut.IsValid())
        return;

    // 同じ LUT リソースに焼き済みの場合だけスキップする。
    // WHY: bool だけではデバイスリセット後に再生成された未初期化テクスチャも焼き済み扱いになる。
    static renderer::ResourceHandle<renderer::TextureTag> sBakedTarget;
    if (sBakedTarget == h.iblBrdfLut) return;

    // 512x512 の BRDF LUT を 8x8 スレッドグループで Dispatch
    // WHAT: UAV_BRDF_LUT (u0) の RG に scale/bias、BA に補助値を書き込む
    renderer::ComputeCall bakeCS;
    bakeCS.shader       = h.iblBrdfBakeShader;
    bakeCS.uavOutputs[0] = h.iblBrdfLut;   // u0: UAV_BRDF_LUT (起動時 1 回のみ。UAV_OUTPUT スロットを時分割で再利用)
    bakeCS.dispatchX    = (512 + 7) / 8;   // 64 グループ × 64 グループ = 512×512 スレッド
    bakeCS.dispatchY    = (512 + 7) / 8;
    bakeCS.dispatchZ    = 1;
    r.Dispatch(bakeCS, resources);

    sBakedTarget = h.iblBrdfLut;
}

} // namespace fbzz::scene
