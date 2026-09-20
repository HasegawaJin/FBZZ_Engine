/// @file    GeometryRoute.hpp
/// @brief   材質スロットから描画経路判定用の値を抽出する Engine 側の境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Engine/Renderer/GeometryRoute.hpp>
#include <string_view>

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::scene {

struct MaterialSlot;

/// @note 既存の Scene 描画パス向け互換名。Renderer からこのヘッダーを参照しない。
using renderer::GeometryRoute;

/// @param shaderPath アセット参照またはパス。空文字は既定の Fallback を指す。
/// @note 未知のシェーダーは非対応。GUID の解決は Engine 側で完結させる。
[[nodiscard]] bool IsGBufferEquivalentShader(std::string_view shaderPath);

/// @note EnsureMaterialAsset 後に呼ぶ。ロードや GPU 材質の更新は行わない。
/// @note 未解決・失効済みのアセットは GBuffer 非対応。インスタンスの上書きを優先する。
/// @note 戻り値は独立した値であり、後続の材質編集・破棄では変化しない。再抽出で反映する。
[[nodiscard]] renderer::GeometryMaterialInput ExtractGeometryMaterial(const MaterialSlot& slot);

/// @note 呼び出し側で共有アセットを解決済みの場合の入口。nullptr は未解決として扱う。
[[nodiscard]] renderer::GeometryMaterialInput ExtractGeometryMaterial(
    const MaterialSlot& slot, const asset::MaterialAsset* material);

/// @note フレーム入力へ移行中の描画パス用。抽出後は Renderer の規則に委ねる。
[[nodiscard]] GeometryRoute ResolveGeometryRoute(const MaterialSlot& slot, bool gbufferPipeline);

} /// @note namespace fbzz::scene
