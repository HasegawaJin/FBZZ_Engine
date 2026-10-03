/// @file    ShaderCapabilities.hpp
/// @brief   シェーダーの .meta から能力宣言を解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <string_view>

namespace fbzz::asset {

/// @note GUID は索引の実体へ解決し、表示ヒントへの復旧は行わない。未存在・未宣言・不正な宣言は未知の能力を返す。
/// @note .meta の更新時刻を見て再読込する。GPU で使用中の版は ResourceManager の成功した reload でのみ変わる。
[[nodiscard]] renderer::ShaderCapabilities ResolveShaderCapabilities(std::string_view shaderReference);

} /// @note namespace fbzz::asset
