/// @file    GraphicsCapabilities.hpp
/// @brief   描画バックエンドに依存しない GPU の機能対応。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once

namespace fbzz::renderer {

/// @note 実機の対応だけを表し、シェーダー・資源・描画パスの準備成功は含まない。
/// @note 未初期化・終了済み・能力不明では全機能を非対応として返す。
struct GraphicsCapabilities {
    bool bindless = false;
    bool inlineRayQuery = false;
    bool rayTracingPipeline = false;
};

}
