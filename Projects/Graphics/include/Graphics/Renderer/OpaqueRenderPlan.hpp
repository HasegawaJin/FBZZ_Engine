/// @file    OpaqueRenderPlan.hpp
/// @brief   不透明の描画方式と画面空間入力を資源の可用性から解決する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once

#include <cstdint>

namespace fbzz::renderer {

struct RenderSettings;

enum class OpaqueRenderPath : uint8_t {
    FORWARD,
    FORWARD_DEPTH_NORMAL,
    DEFERRED,
};

/// @note 資源の確保は呼び出し側で済ませる。解決中に GPU や Scene を参照しない。
struct OpaqueRenderAvailability {
    bool depthNormal = false;
    bool deferredLighting = false;
    bool depthCopy = false;
};

struct OpaqueRenderPlan {
    OpaqueRenderPath path = OpaqueRenderPath::FORWARD;

    [[nodiscard]] bool UsesDeferredLighting() const
    {
        return path == OpaqueRenderPath::DEFERRED;
    }

    [[nodiscard]] bool HasScreenSpaceInputs() const
    {
        return path != OpaqueRenderPath::FORWARD;
    }
};

/// @return Deferred の必須資源が欠けた場合は Forward。本描画前に必要なプリパスも同時に決める。
/// @note ライト供給の LINEAR / CLUSTERED は直交するため、この解決に含めない。
/// @see Docs/design/graphics-library.md
[[nodiscard]] OpaqueRenderPlan ResolveOpaqueRenderPlan(
    const RenderSettings& settings, const OpaqueRenderAvailability& availability);

} /// @note namespace fbzz::renderer
