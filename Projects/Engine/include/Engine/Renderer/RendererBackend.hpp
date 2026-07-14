// FBZZ Engine
// RendererBackend.hpp | fbzz::renderer
// 描画バックエンド種別の軽量列挙。RendererFactory / ProjectSettings / Application が共有する。
// WHY: enum を RendererFactory.hpp から切り出すことで、ProjectSettings や Application が
//      IRenderer 一式 (重いインターフェース) を include せずにバックエンド種別だけを参照できる。
#pragma once

#include <cstdint>
#include <string_view>

namespace fbzz::renderer {

enum class RendererBackend : uint8_t {
    DX11,
    DX12,
};

// 設定ファイル用の小文字トークン ("dx11" / "dx12") との相互変換。
inline const char* ToString(RendererBackend backend)
{
    switch (backend) {
    case RendererBackend::DX12: return "dx12";
    case RendererBackend::DX11: return "dx11";
    }
    return "dx11";
}

inline RendererBackend BackendFromString(std::string_view text)
{
    // WHY: DX12を全体既定とし、DX11は明示指定された場合だけ互換経路として選ぶ。
    return text == "dx11" ? RendererBackend::DX11 : RendererBackend::DX12;
}

} // namespace fbzz::renderer
