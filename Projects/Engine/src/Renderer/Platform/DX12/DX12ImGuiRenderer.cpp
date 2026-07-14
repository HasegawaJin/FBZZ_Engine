// FBZZ Engine
// DX12ImGuiRenderer.cpp | fbzz::renderer
// imgui_impl_win32 / imgui_impl_dx12 の初期化と描画記録
#include "DX12ImGuiRenderer.hpp"

#include "DX12Context.hpp"
#include "DX12RenderTarget.hpp"
#include "DX12Texture.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <bit>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

namespace fbzz::renderer {

namespace {

// Phase 1 ではフォント 1 枚だけを使う。Phase 3 でフリーリストアロケーターへ置き換える。
void AllocateSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    auto* renderer = static_cast<DX12ImGuiRenderer*>(info->UserData);
    uint32_t index = 0;
    renderer->AllocateDescriptor(index, *cpu, *gpu);
}

void FreeSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    static_cast<DX12ImGuiRenderer*>(info->UserData)->FreeDescriptor(cpu);
}

} // namespace

size_t DX12ImGuiRenderer::CacheKeyHash::operator()(const CacheKey& key) const
{
    size_t hash = static_cast<size_t>(key.id);
    hash ^= static_cast<size_t>(key.generation) << 1;
    hash ^= static_cast<size_t>(key.slot) << 17;
    hash ^= static_cast<size_t>(key.kind) << 25;
    return hash;
}

DX12ImGuiRenderer::~DX12ImGuiRenderer()
{
    ImGuiShutdown();
}

bool DX12ImGuiRenderer::Init(DX12Context* context)
{
    m_context = context;
    return m_context != nullptr;
}

void DX12ImGuiRenderer::ImGuiInit(void* hwnd)
{
    if (m_imguiInitialized || !m_context)
        return;
    FBZZ_LOG_INFO("DX12ImGuiRenderer::ImGuiInit: 開始");
    const bool win32Initialized = ImGui_ImplWin32_Init(hwnd);
    FBZZ_LOG_INFO("DX12ImGuiRenderer: ImGui_ImplWin32_Init = %d", win32Initialized ? 1 : 0);
    ImGui_ImplDX12_InitInfo info{};
    info.Device = m_context->GetDevice();
    info.CommandQueue = m_context->GetCommandQueue();
    info.NumFramesInFlight = DX12Context::FRAME_COUNT;
    info.RTVFormat = DX12Context::BACK_BUFFER_FORMAT;
    info.DSVFormat = DX12Context::DEPTH_FORMAT;
    info.UserData = this;
    info.SrvDescriptorHeap = m_context->GetImGuiSrvHeap();
    info.SrvDescriptorAllocFn = AllocateSrv;
    info.SrvDescriptorFreeFn = FreeSrv;
    const bool dx12Initialized = ImGui_ImplDX12_Init(&info);
    FBZZ_LOG_INFO("DX12ImGuiRenderer: ImGui_ImplDX12_Init = %d", dx12Initialized ? 1 : 0);
    m_imguiInitialized = win32Initialized && dx12Initialized;
    if (!m_imguiInitialized) {
        FBZZ_LOG_ERROR("DX12ImGuiRenderer: ImGui バックエンド初期化失敗 (win32=%d dx12=%d)",
                       win32Initialized ? 1 : 0, dx12Initialized ? 1 : 0);
        if (dx12Initialized) ImGui_ImplDX12_Shutdown();
        if (win32Initialized) ImGui_ImplWin32_Shutdown();
    } else {
        FBZZ_LOG_INFO("DX12ImGuiRenderer::ImGuiInit: 完了");
    }
}

void DX12ImGuiRenderer::ImGuiShutdown()
{
    if (!m_imguiInitialized)
        return;
    // WHY: 直前フレームのコマンドリストが GPU 実行中のままバックエンドを破棄すると、
    //      フォントテクスチャやマルチビューポートのスワップチェーンが final-release され、
    //      OBJECT_DELETED_WHILE_STILL_IN_USE (EXECUTION ERROR #921) → Debug Layer の
    //      BREAK で終了時クラッシュになる。破棄前に必ず GPU 完了を待つ。
    //      (DX11 はランタイムが参照を保持するため不要だった DX12 特有の同期点)
    if (m_context)
        m_context->Flush();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    m_imguiInitialized = false;
    m_textureCache.clear();
    m_freeDescriptors.clear();
    m_nextDescriptor = 0;
    m_reportedHeapExhaustion = false;
}

void DX12ImGuiRenderer::ImGuiNewFrame()
{
    if (!m_imguiInitialized) return;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
}

void DX12ImGuiRenderer::ImGuiRenderDrawData()
{
    if (!m_imguiInitialized || !m_context->IsFrameOpen())
        return;
    ID3D12DescriptorHeap* heaps[] = {m_context->GetImGuiSrvHeap()};
    m_context->GetCommandList()->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_context->GetCommandList());
}

bool DX12ImGuiRenderer::AllocateDescriptor(
    uint32_t& index, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
{
    if (!m_freeDescriptors.empty()) {
        index = m_freeDescriptors.back();
        m_freeDescriptors.pop_back();
    } else {
        if (m_nextDescriptor >= m_context->GetImGuiDescriptorCapacity()) {
            if (!m_reportedHeapExhaustion) {
                FBZZ_LOG_ERROR("DX12ImGuiRenderer: 永続SRVヒープが枯渇しました (%u)",
                               m_context->GetImGuiDescriptorCapacity());
                m_reportedHeapExhaustion = true;
            }
            cpu = {};
            gpu = {};
            return false;
        }
        index = m_nextDescriptor++;
    }
    cpu = m_context->GetImGuiSrvCpu(index);
    gpu = m_context->GetImGuiSrvGpu(index);
    return true;
}

void DX12ImGuiRenderer::FreeDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    if (!m_context || cpu.ptr < m_context->GetImGuiSrvCpu().ptr) return;
    const SIZE_T delta = cpu.ptr - m_context->GetImGuiSrvCpu().ptr;
    const uint32_t increment = m_context->GetSrvDescriptorIncrement();
    if (increment == 0 || delta % increment != 0) return;
    const uint32_t index = static_cast<uint32_t>(delta / increment);
    if (index < m_nextDescriptor) m_freeDescriptors.push_back(index);
}

void* DX12ImGuiRenderer::CacheDescriptor(const CacheKey& key, D3D12_CPU_DESCRIPTOR_HANDLE source)
{
    if (!source.ptr || !m_context) return nullptr;
    uint32_t index = 0;
    if (const auto found = m_textureCache.find(key); found != m_textureCache.end()) {
        index = found->second;
    } else {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if (!AllocateDescriptor(index, cpu, gpu)) return nullptr;
        m_textureCache.emplace(key, index);
    }
    const auto destination = m_context->GetImGuiSrvCpu(index);
    m_context->GetDevice()->CopyDescriptorsSimple(
        1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return std::bit_cast<void*>(m_context->GetImGuiSrvGpu(index).ptr);
}

void* DX12ImGuiRenderer::GetImTextureID(
    ResourceHandle<RenderTargetTag> handle, ResourceManager& resources, int slot)
{
    auto* targetBase = resources.Get(handle);
    if (!targetBase || slot < 0) return nullptr;
    auto* target = static_cast<DX12RenderTarget*>(targetBase);
    if (static_cast<uint32_t>(slot) >= target->GetColorCount()) return nullptr;
    if (m_context->IsFrameOpen())
        target->TransitionColorForRead(m_context->GetCommandList(), static_cast<uint32_t>(slot));
    return CacheDescriptor({handle.id, handle.gen, static_cast<uint16_t>(slot), 0},
                           target->GetColorSrv(static_cast<uint32_t>(slot)));
}

void* DX12ImGuiRenderer::GetImTextureID(
    ResourceHandle<TextureTag> handle, ResourceManager& resources)
{
    auto* textureBase = resources.Get(handle);
    if (!textureBase) return nullptr;
    auto* texture = static_cast<DX12Texture*>(textureBase);
    if (m_context->IsFrameOpen()) texture->TransitionForPixelRead(m_context->GetCommandList());
    return CacheDescriptor({handle.id, handle.gen, 0, 1}, texture->GetSrvCpu());
}

} // namespace fbzz::renderer
