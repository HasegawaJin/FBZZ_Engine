// FBZZ Engine
// DX12ImGuiRenderer.hpp | fbzz::renderer
// Dear ImGui の DirectX 12 バックエンド橋渡し
#pragma once

#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <d3d12.h>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class DX12Context;

// Phase 1 のフォント描画用 SRV を所有コンテキストの可視ヒープへ割り当てる。
class DX12ImGuiRenderer final : public IImGuiRenderer {
public:
    ~DX12ImGuiRenderer() override;
    bool Init(DX12Context* context);
    void ImGuiInit(void* hwnd) override;
    void ImGuiShutdown() override;
    void ImGuiNewFrame() override;
    void ImGuiRenderDrawData() override;
    void ImGuiRenderPlatformWindows() override;
    void* GetImTextureID(ResourceHandle<RenderTargetTag>, ResourceManager&, int = 0) override;
    void* GetImTextureID(ResourceHandle<TextureTag>, ResourceManager&) override;

    // imgui_impl_dx12 のC形式コールバックから呼ぶディスクリプタ割り当て境界。
    bool AllocateDescriptor(uint32_t& index, D3D12_CPU_DESCRIPTOR_HANDLE& cpu,
                            D3D12_GPU_DESCRIPTOR_HANDLE& gpu);
    void FreeDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu);

private:
    struct CacheKey {
        uint32_t id = 0;
        uint32_t generation = 0;
        uint16_t slot = 0;
        uint8_t kind = 0;
        bool operator==(const CacheKey&) const = default;
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& key) const;
    };

    void* CacheDescriptor(const CacheKey& key, D3D12_CPU_DESCRIPTOR_HANDLE source);

    DX12Context* m_context = nullptr;
    bool m_imguiInitialized = false;
    uint32_t m_nextDescriptor = 0;
    std::vector<uint32_t> m_freeDescriptors;
    std::unordered_map<CacheKey, uint32_t, CacheKeyHash> m_textureCache;
    bool m_reportedHeapExhaustion = false;
};

} // namespace fbzz::renderer
