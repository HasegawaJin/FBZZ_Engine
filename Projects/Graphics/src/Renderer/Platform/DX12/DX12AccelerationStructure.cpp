/// @file    DX12AccelerationStructure.cpp
/// @brief   Validate static DXR inputs, preserve AS state and defer GPU storage retirement.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include "DX12AccelerationStructure.hpp"
#include "DX12Buffer.hpp"
#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include "DX12UploadArena.hpp"
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/HResult.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace fbzz::renderer {
namespace {

bool ValidRayTransform(const math::Matrix4& transform)
{
    for (const auto& row : transform.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    if (transform.m[3][0] != 0.0f || transform.m[3][1] != 0.0f
        || transform.m[3][2] != 0.0f || transform.m[3][3] != 1.0f)
        return false;
    const auto& m = transform.m;
    const double determinant = static_cast<double>(m[0][0]) * (static_cast<double>(m[1][1]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][1])
        - static_cast<double>(m[0][1]) * (static_cast<double>(m[1][0]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][0])
        + static_cast<double>(m[0][2]) * (static_cast<double>(m[1][0]) * m[2][1] - static_cast<double>(m[1][1]) * m[2][0]);
    return determinant != 0.0 && std::isfinite(determinant);
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS BuildInputs(
    AccelerationStructureKind kind, uint32_t count)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = kind == AccelerationStructureKind::BOTTOM_LEVEL
        ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL
        : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = count;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    return inputs;
}

bool CreateRayStorage(ID3D12Device& device, uint64_t bytes, D3D12_RESOURCE_STATES state,
                      Microsoft::WRL::ComPtr<ID3D12Resource>& storage)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    FBZZ_HR_CHECK(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, nullptr, IID_PPV_ARGS(&storage)));
    return true;
}

}

DX12AccelerationStructure::~DX12AccelerationStructure()
{
    if (m_tracker) m_tracker->Remove(m_result.Get());
    if (m_tracker) m_tracker->Remove(m_scratch.Get());
    if (m_context) {
        m_context->FreeBindlessSlot(m_bindlessIndex);
        m_context->DeferRelease(m_result);
        m_context->DeferRelease(m_scratch);
        for (auto& bottomLevel : m_bottomLevels)
            m_context->DeferRelease(bottomLevel);
    }
}

bool DX12AccelerationStructure::ResolveGeometry(ResourceManager& resources, DX12UploadArena* arena,
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometry) const
{
    geometry.reserve(m_desc.geometries.size());
    uint64_t primitiveCount = 0;
    for (const auto& source : m_desc.geometries) {
        auto* vertices = static_cast<DX12Buffer*>(resources.Get(source.vertices));
        auto* indices = static_cast<DX12Buffer*>(resources.Get(source.indices));
        if (!vertices || vertices->GetKind() != DX12Buffer::Kind::Vertex || source.vertexCount == 0)
            return false;
        const uint32_t stride = vertices->GetStride();
        if (stride < 12 || stride % 4 != 0 || source.positionOffset % 4 != 0
            || source.positionOffset > stride - 12)
            return false;
        const uint64_t availableVertices = vertices->GetDataSize() / stride;
        if (source.vertexCount > availableVertices || source.firstVertex > availableVertices - source.vertexCount)
            return false;
        if (source.indices.IsValid()) {
            if (!indices || indices->GetKind() != DX12Buffer::Kind::Index
                || source.indexCount == 0 || source.indexCount % 3 != 0
                || !indices->ValidateIndexRange(source.firstIndex, source.indexCount, source.vertexCount))
                return false;
        } else if (source.indexCount != 0 || source.firstIndex != 0 || source.vertexCount % 3 != 0) {
            return false;
        }
        primitiveCount += (indices ? source.indexCount : source.vertexCount) / 3;
        /// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#geometry-limits DXR per-BLAS primitive limit
        if (primitiveCount > (1u << 29)) return false;
        const auto vertexAddress = arena ? vertices->GetVertexView(*arena).BufferLocation
                                         : vertices->GetResource()->GetGPUVirtualAddress();
        const auto indexAddress = indices ? (arena ? indices->GetIndexView(*arena).BufferLocation
                                                   : indices->GetResource()->GetGPUVirtualAddress()) : 0;
        if (!vertexAddress || (indices && !indexAddress)) return false;
        D3D12_RAYTRACING_GEOMETRY_DESC target{};
        target.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        target.Flags = source.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        target.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        target.Triangles.VertexCount = source.vertexCount;
        target.Triangles.VertexBuffer = { vertexAddress + static_cast<uint64_t>(source.firstVertex) * stride + source.positionOffset, stride };
        target.Triangles.IndexFormat = indices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
        target.Triangles.IndexCount = source.indexCount;
        target.Triangles.IndexBuffer = indices ? indexAddress + static_cast<uint64_t>(source.firstIndex) * sizeof(uint32_t) : 0;
        geometry.push_back(target);
    }
    return true;
}

bool DX12AccelerationStructure::ResolveInstances(ResourceManager& resources,
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances, bool requireBuilt) const
{
    instances.reserve(m_desc.instances.size());
    for (const auto& source : m_desc.instances) {
        auto* bottomLevel = static_cast<DX12AccelerationStructure*>(resources.Get(source.bottomLevel));
        if (!bottomLevel || bottomLevel->GetKind() != AccelerationStructureKind::BOTTOM_LEVEL
            || (requireBuilt && !bottomLevel->IsBuilt()) || source.instanceId > 0xFFFFFF
            || !ValidRayTransform(source.transform))
            return false;
        D3D12_RAYTRACING_INSTANCE_DESC target{};
        /// @note Math already stores the column-vector affine transform as three row-major float4 rows.
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_instance_desc Transform layout
        std::memcpy(target.Transform, source.transform.m, sizeof(target.Transform));
        target.InstanceID = source.instanceId;
        target.InstanceMask = source.mask;
        target.Flags = (source.doubleSided ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE : 0)
            | (source.frontCounterClockwise ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE : 0);
        target.AccelerationStructure = bottomLevel->GetResource()->GetGPUVirtualAddress();
        instances.push_back(target);
    }
    return true;
}

bool DX12AccelerationStructure::Init(DX12Context& context, DX12StateTracker& tracker,
    const AccelerationStructureDesc& desc, ResourceManager& resources)
{
    if (!context.GetDevice() || !context.SupportsInlineRaytracing() || !context.SupportsBindless())
        return false;
    const bool bottomLevel = desc.kind == AccelerationStructureKind::BOTTOM_LEVEL;
    if ((!bottomLevel && desc.kind != AccelerationStructureKind::TOP_LEVEL)
        || (bottomLevel ? (desc.geometries.empty() || !desc.instances.empty())
                        : (desc.instances.empty() || !desc.geometries.empty()))
        || desc.geometries.size() > (1u << 24) || desc.instances.size() > (1u << 24))
        return false;
    m_desc = desc;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    if (bottomLevel ? !ResolveGeometry(resources, nullptr, geometry)
                    : !ResolveInstances(resources, instances, false))
        return false;
    auto inputs = BuildInputs(desc.kind, static_cast<uint32_t>(bottomLevel ? geometry.size() : instances.size()));
    if (bottomLevel) inputs.pGeometryDescs = geometry.data();
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    FBZZ_HR_CHECK(context.GetDevice()->QueryInterface(IID_PPV_ARGS(&device)));
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    if (sizes.ResultDataMaxSizeInBytes == 0 || sizes.ScratchDataSizeInBytes == 0
        || sizes.ResultDataMaxSizeInBytes > (std::numeric_limits<size_t>::max)() - sizes.ScratchDataSizeInBytes)
        return false;
    m_context = &context;
    m_tracker = &tracker;
    m_size = static_cast<size_t>(sizes.ResultDataMaxSizeInBytes + sizes.ScratchDataSizeInBytes);
    if (!CreateRayStorage(*device.Get(), sizes.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, m_result)
        || !CreateRayStorage(*device.Get(), sizes.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_COMMON, m_scratch))
        return false;
    tracker.Register(m_result.Get(), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    tracker.Register(m_scratch.Get(), D3D12_RESOURCE_STATE_COMMON);
    if (!bottomLevel) {
        m_bindlessIndex = context.AllocateBindlessSlot();
        if (m_bindlessIndex == UINT32_MAX) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.RaytracingAccelerationStructure.Location = m_result->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &view, context.GetBindlessCpu(m_bindlessIndex));
    }
    return true;
}

bool DX12AccelerationStructure::Build(ID3D12GraphicsCommandList4& commands,
    DX12UploadArena& arena, ResourceManager& resources)
{
    if (m_built || !m_result || !m_scratch) return false;
    const bool bottomLevel = m_desc.kind == AccelerationStructureKind::BOTTOM_LEVEL;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    if (bottomLevel ? !ResolveGeometry(resources, &arena, geometry)
                    : !ResolveInstances(resources, instances, true))
        return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = BuildInputs(m_desc.kind, static_cast<uint32_t>(bottomLevel ? geometry.size() : instances.size()));
    if (bottomLevel) {
        build.Inputs.pGeometryDescs = geometry.data();
        for (const auto& source : m_desc.geometries) {
            auto* vertices = static_cast<DX12Buffer*>(resources.Get(source.vertices));
            if (vertices->IsGpuWritable())
                m_tracker->QueueTransition(vertices->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    } else {
        const auto upload = arena.Allocate(instances.size() * sizeof(instances[0]),
            D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
        if (!upload) return false;
        std::memcpy(upload.cpu, instances.data(), upload.size);
        build.Inputs.InstanceDescs = upload.gpu;
        for (const auto& source : m_desc.instances)
            m_bottomLevels.emplace_back(static_cast<DX12AccelerationStructure*>(resources.Get(source.bottomLevel))->GetResource());
    }
    build.DestAccelerationStructureData = m_result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    m_tracker->QueueTransition(m_scratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_tracker->FlushBarriers(&commands);
    commands.BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    /// @note AS remains in AS state; UAV synchronization makes the build visible to TLAS construction and trace.
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#synchronizing-acceleration-structure-memory-writesreads AS synchronization
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_result.Get();
    commands.ResourceBarrier(1, &barrier);
    m_built = true;
    return true;
}

uint32_t DX12AccelerationStructure::GetBindlessIndex() const
{
    return m_built ? m_bindlessIndex : UINT32_MAX;
}

}
