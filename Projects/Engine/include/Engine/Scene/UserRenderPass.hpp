/// @file    UserRenderPass.hpp
/// @brief   Scene が保持する描画要求と実行文脈の境界。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <Graphics/Pipeline/RenderPassInjectionPoint.hpp>
#include <Graphics/Renderer/RenderGraphAccess.hpp>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::scene {

struct RenderPassContext;
using renderer::UserRenderPassInjectionPoint;

/// @brief Scene から追加する描画パスの宣言。
/// @note callback は描画記録スレッドでフレーム内に実行し、Scene の構造を変更しない。
struct UserRenderPassDesc {
    std::string name;
    UserRenderPassInjectionPoint injectionPoint = UserRenderPassInjectionPoint::AfterTransparent;
    std::vector<renderer::RenderResourceAccess> accesses;
    std::function<void(RenderPassContext&)> execute;
    bool allowCulling = true;
};

} /// @note namespace fbzz::scene
