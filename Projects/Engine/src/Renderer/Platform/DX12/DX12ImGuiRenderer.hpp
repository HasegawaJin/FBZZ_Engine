/// @file    DX12ImGuiRenderer.hpp
/// @brief   Dear ImGui の DirectX 12 バックエンド橋渡し。
/// @author  Hasegawa Jin
/// @date    2026-07-15
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
    // CacheKey::kind — ハンドルの種別。回収時にどちらの Get で生存を問うかを決める。
    static constexpr uint8_t CACHE_KIND_RENDER_TARGET = 0;
    static constexpr uint8_t CACHE_KIND_TEXTURE       = 1;
    // 生存判定を回す間隔 [フレーム]。毎フレーム全件を舐めるほど枯渇は速くない。
    static constexpr uint64_t SWEEP_INTERVAL_FRAMES = 30;

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

    // 回収済みディスクリプタ。GPU が読み終わるまで再利用へ回せない。
    struct RetiredDescriptor {
        uint32_t index = 0;
        uint64_t frame = 0;  // 回収した ImGui フレーム番号
    };

    void* CacheDescriptor(const CacheKey& key, D3D12_CPU_DESCRIPTOR_HANDLE source);
    void SweepReleasedDescriptors();

    DX12Context* m_context = nullptr;
    bool m_imguiInitialized = false;
    uint32_t m_nextDescriptor = 0;
    std::vector<uint32_t> m_freeDescriptors;
    std::unordered_map<CacheKey, uint32_t, CacheKeyHash> m_textureCache;
    std::vector<RetiredDescriptor> m_retiredDescriptors;
    uint64_t m_frameCounter = 0;
    bool m_reportedHeapExhaustion = false;
};

} // namespace fbzz::renderer
