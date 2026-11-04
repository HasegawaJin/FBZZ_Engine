/// @file    ComputedSkinningOutput.hpp
/// @brief   捕捉済み姿勢を必要とする後段 consumer へ所有付き出力を解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Effects/PreparedSkinning.hpp>

namespace fbzz::renderer {
struct DrawCall;
struct RenderMeshItem;

/// @note 元 snapshot の ticket だけを実行し、現在の Scene や Animator から姿勢を取得しない。
/// @note 失敗時は出力未変更。呼出し元はその描画を提出しない。
/// @pre 出力 handle は借用参照。呼出し元は GPU 要求へ所有を引き継ぐまで ticket を保持する。
[[nodiscard]] FBZZ_GRAPHICS_API bool ResolvePreparedSkinningOutput(const std::shared_ptr<PreparedSkinningTicket>& ticket,
    uint32_t row, ResourceHandle<BufferTag>& output, uint64_t* contentVersion = nullptr);

/// @note ticket が無ければ call は未変更。native 出力の寿命を DrawCall と描画 frame に引き継ぐ。
[[nodiscard]] FBZZ_GRAPHICS_API bool BindPreparedSkinningOutput(const RenderMeshItem& item, DrawCall& call);
}
