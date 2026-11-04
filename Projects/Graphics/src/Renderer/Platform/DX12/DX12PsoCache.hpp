/// @file    DX12PsoCache.hpp
/// @brief   汎用 Root Signature と遅延生成 Graphics PSO のキャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/RenderState.hpp>
#include <d3d12.h>
#include <cstdint>
#include <unordered_map>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12Shader;

class DX12PsoCache final {
public:
    /// @brief ルートシグネチャと PSO キャッシュを初期化する。
    /// @param bindlessEnabled シェーダーから ResourceDescriptorHeap を引けるか
    /// @note        (DX12Context::SupportsBindless)。true のときだけルートシグネチャへ
    /// @note        CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED を立てる。非対応機で立てると生成が失敗する。
    /// @see  Docs/design/bindless.md
    FBZZ_GRAPHICS_API bool Initialize(ID3D12Device* device, bool bindlessEnabled);
    FBZZ_GRAPHICS_API void Shutdown();
    /// @note このキャッシュを参照する GPU コマンドがすべて完了していること。
    FBZZ_GRAPHICS_API void ClearPipelines();
    ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
    ID3D12RootSignature* GetComputeRootSignature() const { return m_computeRootSignature.Get(); }
    ID3D12CommandSignature* GetIndexedInstanceOffsetCommandSignature() const {
        return m_indexedInstanceOffsetCommandSignature.Get();
    }
    /// @param reversedZ 束縛中の RT の深度が Reversed-Z か。深度比較の向き (GREATER / LESS) を決める。
    FBZZ_GRAPHICS_API ID3D12PipelineState* GetOrCreate(const DX12Shader& shader, const PipelineStateDesc& state,
                                    PrimitiveTopology topology, DXGI_FORMAT renderTargetFormat,
                                    uint32_t renderTargetCount, bool reversedZ);
    FBZZ_GRAPHICS_API ID3D12PipelineState* GetOrCreateMesh(const DX12Shader& shader, const PipelineStateDesc& state,
                                        PrimitiveTopology topology, DXGI_FORMAT renderTargetFormat,
                                        uint32_t renderTargetCount, bool reversedZ);
    FBZZ_GRAPHICS_API ID3D12PipelineState* GetOrCreateCompute(const DX12Shader& shader);

private:
    struct Key {
        /// @note Shader allocation addresses are reusable after resource reset; bytecode generation is the identity instead.
        uint64_t shaderIdentity = 0;
        RasterizerMode rasterizer{};
        BlendMode blend{};
        DepthMode depth{};
        PrimitiveTopology topology{};
        DXGI_FORMAT renderTargetFormat{};
        uint32_t renderTargetCount = 0;
        bool reversedZ = false;
        int32_t depthBias = 0;
        float depthBiasSlope = 0.0f;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        FBZZ_GRAPHICS_API size_t operator()(const Key& key) const;
    };

    FBZZ_GRAPHICS_API bool CreateRootSignature();
    FBZZ_GRAPHICS_API bool CreateComputeRootSignature();
    FBZZ_GRAPHICS_API void CreateIndexedInstanceOffsetCommandSignature();
    /// @brief 両ルートシグネチャに共通で載せるフラグ。
    /// @note bindless 非対応機では CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED を落とす。
    /// @note       非対応機で立てるとシリアライズが E_INVALIDARG で失敗し、起動できなくなる。
    FBZZ_GRAPHICS_API D3D12_ROOT_SIGNATURE_FLAGS BaseRootSignatureFlags() const;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    bool m_bindlessEnabled = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_indexedInstanceOffsetCommandSignature;
    std::unordered_map<Key, Microsoft::WRL::ComPtr<ID3D12PipelineState>, KeyHash> m_cache;
    std::unordered_map<Key, Microsoft::WRL::ComPtr<ID3D12PipelineState>, KeyHash> m_meshCache;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_computeCache;
};

} /// @note namespace fbzz::renderer
