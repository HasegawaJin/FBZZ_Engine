/// @file    GpuCullDrawQueue.hpp
/// @brief   GPU カリングと描画を記録順に実行し、必要に応じて代替描画を使う。
/// @author  Hasegawa Jin
/// @date    2026-10-19
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <span>

namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
namespace gpu_cull_detail {
/// @brief 初期化とカリングを順に試み、成功時は間接描画を送信する。
/// @param[in,out] renderer 命令を記録するレンダラー。描画スレッドを使う場合は要求を値でキューに保持する。
/// @param[in,out] resources 描画時点でハンドルを解決するマネージャー。通常の Submit と同じ期間、生存すること。
/// @param[in] compute 間接描画で使う結果を作るカリング命令。
/// @param[in] indirect カリング結果を使う間接描画。
/// @param[in] fallback カリングを記録できなかった場合に使う描画。通常のインデックス範囲が失効していれば送信を省く。
/// @param[in] setup カリングより先に実行する初期化命令。不要なら nullptr。
FBZZ_GRAPHICS_API void SubmitCullDraw(IRenderer& renderer, ResourceManager& resources,
    const ComputeCall& compute, const DrawCall& indirect, const DrawCall& fallback,
    const ComputeCall* setup = nullptr);

/// @brief 呼び出し順と各 Dispatch のバリアを保ち、終了までに各命令の記録結果を返す。
/// @param[in,out] renderer 各命令を記録するレンダラー。
/// @param[in,out] resources コマンドから参照するリソースを管理する。
/// @param[in] calls 実行順に並べた命令。呼び出しが完了するまで有効であること。
/// @param[out] receipts 各 Dispatch の記録結果 (0 または 1)。寸法不一致ではすべて 0。
/// @param[in] stopOnFailure 依存する後続 Dispatch を失敗時に止め、未記録の結果を 0 にする。
void TryDispatchSequence(IRenderer& renderer, ResourceManager& resources,
    std::span<const ComputeCall> calls, std::span<uint8_t> receipts, bool stopOnFailure = false);
} // namespace gpu_cull_detail
} // namespace fbzz::renderer
