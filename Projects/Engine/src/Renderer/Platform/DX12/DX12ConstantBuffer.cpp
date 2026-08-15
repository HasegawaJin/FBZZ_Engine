// FBZZ Engine
// DX12ConstantBuffer.cpp | fbzz::renderer
// Update ごとに Upload Arena の新しい CBV スライスへコピーする
#include "DX12ConstantBuffer.hpp"

#include "DX12UploadArena.hpp"
#include <Engine/Core/Logger.hpp>
#include <cstring>

namespace fbzz::renderer {

bool DX12ConstantBuffer::Init(DX12UploadArena* arena, size_t sizeBytes)
{
    if (!arena || sizeBytes == 0)
        return false;
    m_arena = arena;
    m_size = (sizeBytes + 255) & ~size_t(255);
    m_cpuData.resize(m_size, 0);
    return true;
}

void DX12ConstantBuffer::Update(const void* data, size_t sizeBytes)
{
    if (!data || sizeBytes > m_size) {
        FBZZ_LOG_ERROR("DX12ConstantBuffer: 無効な更新サイズです (%zu / %zu)", sizeBytes, m_size);
        return;
    }
    std::memcpy(m_cpuData.data(), data, sizeBytes);
    // 256 バイト境界へ切り上げた余白だけをゼロで埋める。
    // WHY: 以前は毎 Update で全域を memset していたが、直後に sizeBytes 分を上書きするので
    //      ゼロにする意味があるのは末尾の余白だけ。ShadowConstants (464B → 512B) のように
    //      実体が大きい CB ほど、無駄なゼロ埋めがそのまま記録コストになっていた。
    if (sizeBytes < m_size)
        std::memset(m_cpuData.data() + sizeBytes, 0, m_size - sizeBytes);
    m_hasData = true;
    m_dirty = true;
}

D3D12_GPU_VIRTUAL_ADDRESS DX12ConstantBuffer::PrepareForSubmit()
{
    if (!m_arena || !m_hasData) return 0;

    // 内容が変わっておらず、かつ同じフレーム (= アリーナが巻き戻っていない) なら、
    // 前回配られたスライスがそのまま生きている。確保も転送もせずアドレスだけ返す。
    const uint64_t epoch = m_arena->GetEpoch();
    if (!m_dirty && m_cachedGpuAddress != 0 && m_cachedArenaEpoch == epoch)
        return m_cachedGpuAddress;

    const auto allocation = m_arena->Allocate(
        m_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!allocation) return 0;
    // WHY: UploadArenaはフレーム開始時に再利用されるため、過去フレームのGPUアドレスを
    //      保持できない。Submit直前にCPU shadowから転送して記録済みDrawの値を安定させる。
    std::memcpy(allocation.cpu, m_cpuData.data(), m_size);

    m_cachedGpuAddress = allocation.gpu;
    m_cachedArenaEpoch = epoch;
    m_dirty = false;
    return allocation.gpu;
}

} // namespace fbzz::renderer
