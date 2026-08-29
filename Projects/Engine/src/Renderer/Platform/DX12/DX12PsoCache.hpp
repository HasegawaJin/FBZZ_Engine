/// @file    DX12PsoCache.hpp
/// @brief   汎用 Root Signature と遅延生成 Graphics PSO のキャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Engine/Renderer/RenderState.hpp>
#include <d3d12.h>
#include <unordered_map>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12Shader;

class DX12PsoCache final {
public:
    bool Initialize(ID3D12Device* device);
    void Shutdown();
    ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
    ID3D12RootSignature* GetComputeRootSignature() const { return m_computeRootSignature.Get(); }
    ID3D12PipelineState* GetOrCreate(const DX12Shader& shader, const PipelineStateDesc& state,
                                    PrimitiveTopology topology, DXGI_FORMAT renderTargetFormat,
                                    uint32_t renderTargetCount);
    ID3D12PipelineState* GetOrCreateCompute(const DX12Shader& shader);

private:
    struct Key {
        const DX12Shader* shader = nullptr;
        RasterizerMode rasterizer{};
        BlendMode blend{};
        DepthMode depth{};
        PrimitiveTopology topology{};
        DXGI_FORMAT renderTargetFormat{};
        uint32_t renderTargetCount = 0;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const;
    };

    bool CreateRootSignature();
    bool CreateComputeRootSignature();
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;
    std::unordered_map<Key, Microsoft::WRL::ComPtr<ID3D12PipelineState>, KeyHash> m_cache;
    std::unordered_map<const DX12Shader*, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_computeCache;
};

} // namespace fbzz::renderer
