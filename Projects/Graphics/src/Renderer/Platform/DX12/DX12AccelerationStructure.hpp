/// @file    DX12AccelerationStructure.hpp
/// @brief   Immutable DXR storage and DIRECT-queue build recording.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Renderer/AccelerationStructure.hpp>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12Context;
class DX12StateTracker;
class DX12UploadArena;
class ResourceManager;

class DX12AccelerationStructure final : public IAccelerationStructure {
public:
    ~DX12AccelerationStructure() override;
    bool Init(DX12Context& context, DX12StateTracker& tracker,
              const AccelerationStructureDesc& desc, ResourceManager& resources);
    bool Build(ID3D12GraphicsCommandList4& commands, DX12UploadArena& arena,
               ResourceManager& resources);
    AccelerationStructureKind GetKind() const override { return m_desc.kind; }
    size_t GetSize() const override { return m_size; }
    bool IsBuilt() const override { return m_built; }
    uint32_t GetBindlessIndex() const override;
    ID3D12Resource* GetResource() const { return m_result.Get(); }

private:
    bool ResolveGeometry(ResourceManager& resources, DX12UploadArena* arena,
                         std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometry) const;
    bool ResolveInstances(ResourceManager& resources,
                          std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances,
                          bool requireBuilt) const;
    AccelerationStructureDesc m_desc;
    DX12Context* m_context = nullptr;
    DX12StateTracker* m_tracker = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_result;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scratch;
    /// @note A TLAS owns references to its BLAS storage through the last TLAS use fence.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_bottomLevels;
    uint32_t m_bindlessIndex = UINT32_MAX;
    size_t m_size = 0;
    bool m_built = false;
};

}
