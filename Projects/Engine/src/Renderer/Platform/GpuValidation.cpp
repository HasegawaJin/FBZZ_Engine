/// @file    GpuValidation.cpp
/// @brief   GpuValidation の実装 (DXGI デバッグインターフェース越しの生存オブジェクト集計)。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include "GpuValidation.hpp"

#if defined(FBZZ_GPU_VALIDATION)
/// @note DXGIGetDebugInterface1 は dxgi1_3.h、IDXGIDebug1 と RLO フラグは dxgidebug.h にある。
#include <dxgi1_3.h>
#include <dxgidebug.h>
#include <wrl/client.h>
#endif

namespace fbzz::renderer::gpuvalidation {

bool IsEnabled()
{
    if constexpr (!kBuiltIn) {
        return false;
    } else {
        /// @note 起動中に変わるものではないので 1 回だけ読む。
        static const bool enabled = [] {
            wchar_t value[8]{};
            const DWORD length = GetEnvironmentVariableW(L"FBZZ_GPU_VALIDATION", value, 8);
            const bool  hasOverride = (length > 0 && length < 8);
            const bool  on = hasOverride ? (value[0] != L'0') : kDefaultOn;

            if (on) {
                FBZZ_LOG_WARN("GpuValidation: D3D 検証レイヤーを有効化します%s。"
                              "API 呼び出しごとに負荷が掛かるため FPS は落ちます "
                              "(切るには FBZZ_GPU_VALIDATION=0)",
                              hasOverride ? " (FBZZ_GPU_VALIDATION=1)" : "");
            } else if (hasOverride) {
                FBZZ_LOG_INFO("GpuValidation: FBZZ_GPU_VALIDATION=0 のため検証レイヤーを立てません");
            } else {
                FBZZ_LOG_INFO("GpuValidation: この構成では検証レイヤーは既定で無効です "
                              "(立てるには FBZZ_GPU_VALIDATION=1)");
            }
            return on;
        }();
        return enabled;
    }
}

bool ShouldBreakOnError()
{
    return IsEnabled() && IsDebuggerPresent() != FALSE;
}

void ReportLiveObjects(const GUID& apiId, const char* label)
{
#if defined(FBZZ_GPU_VALIDATION)
    if (!IsEnabled())
        return;

    Microsoft::WRL::ComPtr<IDXGIDebug1> dxgiDebug;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
        FBZZ_LOG_WARN("GpuValidation: DXGIGetDebugInterface1 取得不可 (生存オブジェクトを出せません)");
        return;
    }

    /// @note DETAIL を付けると 1 件ごとに «誰が作ったか» まで出る。終了時の 1 回だけなので、
    ///       内訳が要らない SUMMARY より «どのリソースが残ったか» が分かる方を採る。
    const auto flags = static_cast<DXGI_DEBUG_RLO_FLAGS>(
        DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL);
    FBZZ_LOG_INFO("GpuValidation: 生存 COM オブジェクトを出力します (%s) — 出力先はデバッガーの出力ウィンドウ",
                  label != nullptr ? label : "?");
    dxgiDebug->ReportLiveObjects(apiId, flags);
#else
    (void)apiId;
    (void)label;
#endif
}

} // namespace fbzz::renderer::gpuvalidation
