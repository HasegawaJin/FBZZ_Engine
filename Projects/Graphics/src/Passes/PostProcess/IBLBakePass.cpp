/// @file    IBLBakePass.cpp
/// @brief   BRDF 積分 LUT をスタートアップ時に一度だけ Compute Shader で焼く。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note LUT は NdotV × roughness の全組み合わせで積分した定数テーブルで、シーンや設定が
/// @note 変わっても値は変わらない。毎フレーム計算するのは無駄なため初回フレームのみ実行し
/// @note 以降はスキップする。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>

namespace fbzz::renderer {

void ExecuteIBLBakeBrdfLutPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.iblBrdfBakeShader.IsValid() || !h.iblBrdfLut.IsValid())
        return;

    /// @note 同じ LUT リソースに焼き済みの場合だけスキップする。bool だけだとデバイス
    /// @note リセット後に再生成された未初期化テクスチャも焼き済み扱いになってしまう。
    static renderer::ResourceHandle<renderer::TextureTag> sBakedTarget;
    if (sBakedTarget == h.iblBrdfLut) return;

    /// @note 512x512 の BRDF LUT を 8x8 スレッドグループで Dispatch。
    /// @note UAV_BRDF_LUT (u0) の RG に scale/bias、BA に補助値を書き込む。
    renderer::ComputeCall bakeCS;
    bakeCS.shader       = h.iblBrdfBakeShader;
    /// @note u0: UAV_BRDF_LUT (起動時 1 回のみ。UAV_OUTPUT スロットを時分割で再利用)
    bakeCS.uavOutputs[0] = h.iblBrdfLut;
    /// @note 64 グループ × 64 グループ = 512×512 スレッド
    bakeCS.dispatchX    = (512 + 7) / 8;
    bakeCS.dispatchY    = (512 + 7) / 8;
    bakeCS.dispatchZ    = 1;
    r.Dispatch(bakeCS, resources);

    sBakedTarget = h.iblBrdfLut;
}


void IBLBrdfBakePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteIBLBakeBrdfLutPass(ctx);
}
} /// @note namespace fbzz::renderer
