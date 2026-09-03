/// @file    RendererFactory.cpp
/// @brief   RendererBackend → 具象バックエンドの生成を振り分ける合成ポイント。
/// @author  Hasegawa Jin
/// @date    2026-07-02
#include "Engine/Renderer/RendererFactory.hpp"

#include "Engine/Core/Logger.hpp"
#include "Engine/Renderer/BackendEntry.hpp"

namespace fbzz::renderer {

RendererBundle CreateRenderer(RendererBackend backend, void* hwnd, uint32_t width, uint32_t height)
{
    FBZZ_LOG_INFO("RendererFactory: CreateRenderer 要求 backend=%s", ToString(backend));
    switch (backend) {
    case RendererBackend::DX11:
        return CreateDX11Backend(hwnd, width, height);
    case RendererBackend::DX12:
#if FBZZ_ENABLE_DX12
        return CreateDX12Backend(hwnd, width, height);
#else
        // 設定ファイルに残った "dx12" 指定でプロセスごと落とさない。DX11 へ倒すかは呼び出し側の判断。
        FBZZ_LOG_ERROR("RendererFactory: この構成は DX12 を含まずにビルドされています "
                       "(FBZZ_ENABLE_DX12=OFF)");
        return {};
#endif
    }

    FBZZ_LOG_ERROR("RendererFactory: 未対応のバックエンドが指定されました (%d)",
                   static_cast<int>(backend));
    return {};
}

} // namespace fbzz::renderer
