/// @file    DX12IblBaker.hpp
/// @brief   キャプチャ済み環境キューブをirradiance/prefilterへGPU畳み込みする。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <vector>

namespace fbzz::renderer {

class DX12Context;
class DX12PsoCache;
class DX12RenderTarget;
class DX12StateTracker;
class DX12Texture;
class DX12UploadArena;
class DX12Shader;

class DX12IblBaker final {
public:
    DX12IblBaker(DX12Context* context, DX12StateTracker* tracker,
                 DX12PsoCache* psoCache, DX12UploadArena* uploadArena);
    bool Convolve(DX12RenderTarget& environment, uint32_t irradianceSize,
                  uint32_t prefilterSize, uint32_t prefilterMips, uint32_t sampleCount,
                  std::unique_ptr<DX12Texture>& irradiance,
                  std::unique_ptr<DX12Texture>& prefilter);

private:
    struct CubeOutput;
    bool EnsureShaders();
    bool CreateOutput(uint32_t size, uint32_t mipCount, CubeOutput& output);
    bool DispatchIrradiance(DX12RenderTarget& environment, CubeOutput& output);
    bool DispatchPrefilter(DX12RenderTarget& environment, CubeOutput& output, uint32_t sampleCount);

    /// @brief 一時ビューを bindless 枠へ発行し、後で返せるよう控えておく。
    /// @note 面ごと・mip ごとの UAV は自前のリソースが持てる添字ではない (同じリソースの
    ///       別スライスを指すビュー) ので、ベイクの間だけ枠を借りる。
    uint32_t PublishTransient(D3D12_CPU_DESCRIPTOR_HANDLE source);
    /// @brief PublishTransient で借りた枠をすべて返す。Convolve の出口で必ず呼ぶ。
    void ReleaseTransients();

    DX12Context* m_context = nullptr;
    DX12StateTracker* m_tracker = nullptr;
    DX12PsoCache* m_psoCache = nullptr;
    DX12UploadArena* m_uploadArena = nullptr;
    std::unique_ptr<DX12Shader> m_irradianceShader;
    std::unique_ptr<DX12Shader> m_prefilterShader;
    /// ベイク中だけ借りている bindless 枠。Convolve の出口でまとめて返す。
    std::vector<uint32_t> m_transientBindless;

    /// 読み込み失敗を報告済みか。失敗しても毎フレーム再試行するため、
    /// これが無いと同じエラーで Console が埋まって他のログが押し出される。
    bool m_shaderFailureReported = false;
};

} // namespace fbzz::renderer
