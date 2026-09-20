/// @file    RendererFactory.cpp
/// @brief   RendererBackend → 具象バックエンドの生成を振り分ける合成ポイント。
/// @author  Hasegawa Jin
/// @date    2026-07-02
#include "Graphics/Renderer/RendererFactory.hpp"

#include "Core/Logger.hpp"
#include "Graphics/Renderer/BackendEntry.hpp"

namespace fbzz::renderer {

RendererBundle CreateRenderer(RendererBackend backend, void* hwnd, uint32_t width, uint32_t height)
{
    FBZZ_LOG_INFO("RendererFactory: CreateRenderer 要求 backend=%s", ToString(backend));
    switch (backend) {
    case RendererBackend::DX12:
#if FBZZ_ENABLE_DX12
        return CreateDX12Backend(hwnd, width, height);
#else
        /// @note 設定ファイルの指定でプロセスごと落とさない。倒す先は呼び出し側の判断。
        /// @note       DX11 撤去後、この構成に生成できるバックエンドは 1 つも無いため引数は捨てる。
        (void)hwnd;
        (void)width;
        (void)height;
        FBZZ_LOG_ERROR("RendererFactory: この構成は描画バックエンドを含まずにビルドされています "
                       "(FBZZ_ENABLE_DX12=OFF)");
        return {};
#endif
    }

    FBZZ_LOG_ERROR("RendererFactory: 未対応のバックエンドが指定されました (%d)",
                   static_cast<int>(backend));
    return {};
}

} /// @note namespace fbzz::renderer
