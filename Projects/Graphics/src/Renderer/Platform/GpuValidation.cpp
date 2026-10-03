/// @file    GpuValidation.cpp
/// @brief   GpuValidation の実装 (DXGI デバッグインターフェース越しの生存オブジェクト集計)。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12sdklayers.h>
#include <cstddef>
#include <vector>
#include <d3d12.h>
#include <atomic>
#include "GpuValidation.hpp"

#if defined(FBZZ_GPU_VALIDATION)
/// @note DXGIGetDebugInterface1 は dxgi1_3.h、IDXGIDebug1 と RLO フラグは dxgidebug.h にある。
#include <dxgi1_3.h>
#include <dxgidebug.h>
#include <wrl/client.h>
#endif

namespace fbzz::renderer::gpuvalidation {
namespace {
std::atomic<bool> g_validationActive{false};
}

bool IsExplicitlyRequested()
{
    static const bool required = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"FBZZ_GPU_VALIDATION", value, 8);
        return length == 1 && value[0] == L'1';
    }();
    return required;
}

bool IsActive()
{
    return g_validationActive.load(std::memory_order_acquire);
}

bool InitializeD3D12()
{
    /// @note 関数ローカル static は C++ の同期済み初期化で再生成・複数 context でも一度だけ実行される。
    static const bool success = [] {
        if (!IsEnabled() && !IsExplicitlyRequested()) return true;
#if defined(FBZZ_GPU_VALIDATION)
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            g_validationActive.store(true, std::memory_order_release);
            FBZZ_LOG_INFO("GpuValidation: D3D12 debug layer active; GPU-based validation disabled");
            return true;
        }
#endif
        if (IsExplicitlyRequested()) {
            FBZZ_LOG_ERROR("GpuValidation: FBZZ_GPU_VALIDATION=1 requires compiled validation support and matching SDK layers / Windows Graphics Tools");
            return false;
        }
        FBZZ_LOG_WARN("GpuValidation: default validation unavailable; rendering continues with validation inactive");
        return true;
    }();
    return success;
}

void DrainStoredMessages(ID3D12InfoQueue& queue, const char* tag)
{
    const std::uint64_t count = queue.GetNumStoredMessages();
    if (count == 0) {
        queue.ClearStoredMessages();
        return;
    }

    FBZZ_LOG_WARN("%s: 検証レイヤーのメッセージ %llu 件", tag,
                  static_cast<unsigned long long>(count));

    std::vector<std::max_align_t> buffer;
    for (std::uint64_t i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(queue.GetMessage(i, nullptr, &length)) || length == 0)
            continue;
        buffer.resize((length + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* message = static_cast<D3D12_MESSAGE*>(static_cast<void*>(buffer.data()));
        if (FAILED(queue.GetMessage(i, message, &length)))
            continue;

        FBZZ_LOG_WARN("  [%s] (id=%d) %s",
                      SeverityName(static_cast<int>(message->Severity)),
                      static_cast<int>(message->ID),
                      message->pDescription != nullptr ? message->pDescription : "(no description)");
    }
    queue.ClearStoredMessages();
}


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
    return IsActive() && IsDebuggerPresent() != FALSE;
}

void ReportLiveObjects(const GUID& apiId, const char* label)
{
#if defined(FBZZ_GPU_VALIDATION)
    if (!IsActive())
        return;

    Microsoft::WRL::ComPtr<IDXGIDebug1> dxgiDebug;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
        FBZZ_LOG_WARN("GpuValidation: DXGIGetDebugInterface1 取得不可 (生存オブジェクトを出せません)");
        return;
    }

    /// @note DETAIL を付けると 1 件ごとに «誰が作ったか» まで出る。終了時の 1 回だけなので、
    /// @note       内訳が要らない SUMMARY より «どのリソースが残ったか» が分かる方を採る。
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

} /// @note namespace fbzz::renderer::gpuvalidation
