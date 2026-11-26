/// @file    SkinningComputePass.cpp
/// @brief   Execute independent skeletal outputs with shared palette uploads and mesh dispatches.
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "SkinningWorkTrace.hpp"
#include "OwnedSkinningBatch.hpp"
#include <Core/Logger/Logger.hpp>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Effects/Animation/BakedBoneReplay.hpp>
#include <Graphics/Renderer/Resources/Buffers/DynamicBufferPool.hpp>
#include <Graphics/Renderer/Resources/Buffers/IBuffer.hpp>
#include <Graphics/Renderer/Resources/Buffers/IStructuredBuffer.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <algorithm>
#include <map>
#include <tuple>
#include <vector>
namespace fbzz::renderer {
namespace {
thread_local skinning_trace_detail::RequestCounters* s_skinningWorkTrace=nullptr;
struct SkinningRow { uint32_t boneOffset, outputIndex, boneCount, padding; };
static_assert(sizeof(SkinningRow) == 16);
DynamicStructuredBufferPool s_palettePool;
/// @note Forward the original factory unchanged; counts are observed at its real Create call, not inferred from pool sizes.
struct SkinningRowBufferFactory {
    static constexpr size_t kMinCapacity=detail::DynamicBufferFactory<StructuredBufferTag>::kMinCapacity;
    static ResourceHandle<StructuredBufferTag> Create(ResourceManager& resources,size_t capacity,uint32_t stride,Where where)
    {
        auto* trace=s_skinningWorkTrace;
        if(trace)++trace->backendRowPoolCreateAttempts;
        const auto handle=detail::DynamicBufferFactory<StructuredBufferTag>::Create(resources,capacity,stride,where);
        if(trace && handle.IsValid())++trace->backendRowPoolCreateSuccesses;
        return handle;
    }
};
DynamicBufferPool<StructuredBufferTag,SkinningRowBufferFactory> s_rowPool;

/// @brief スキニング要求の頂点範囲とボーンパレットを実行直前に検証する。
/// @param[in] input スキニングに使う頂点・出力バッファとパレットの範囲。
/// @param[in,out] resources バッファの照会と、必要なら出力 UAV の取得に使う資源管理元。
/// @param[in] matrices GPU パレットを直接使わない要求のための CPU 側行列。
/// @return 入力を読み取れ、出力へ指定の頂点数を書き込める場合は true。
bool ValidRequest(const RenderSkinningInput& input, ResourceManager& resources,
    std::span<const math::Matrix4> matrices)
{
    const auto* source = resources.Get(input.sourceVertices);
    const auto* output = resources.Get(input.outputVertices);
    if (!source || !output || !input.vertexCount) {
        return false;
    }

    // 入力と出力の頂点形式は異なるため、各形式で要求頂点数が収まることを確認する。
    const bool validSource = source->GetStride() == sizeof(SkinnedVertex)
        && source->GetElementCount() >= input.vertexCount
        && source->GetSize() / sizeof(SkinnedVertex) >= input.vertexCount;
    if (!validSource) {
        return false;
    }
    const bool validOutput = output->GetStride() == sizeof(Vertex)
        && output->GetSize() / sizeof(Vertex) >= input.vertexCount;
    if (!validOutput || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) {
        return false;
    }

    // GPU パレットを直接指す場合と、CPU 行列配列を転送する場合で参照範囲を確認する。
    if (input.gpuBonePalette) {
        const auto* palette = resources.Get(input.bonePalette);
        return palette && input.boneCount && palette->GetStride() == sizeof(math::Matrix4)
            && input.boneOffset <= palette->GetElementCount()
            && input.boneCount <= palette->GetElementCount() - input.boneOffset
            && palette->GetSize() / sizeof(math::Matrix4) >= palette->GetElementCount();
    }
    if (input.boneCount) {
        return input.boneOffset <= matrices.size() && input.boneCount <= matrices.size() - input.boneOffset;
    }
    const auto* palette = resources.Get(input.bonePalette);
    return palette && palette->GetStride() == sizeof(math::Matrix4) && palette->GetElementCount()
        && palette->GetSize() / sizeof(math::Matrix4) >= palette->GetElementCount();
}

void DispatchOne(const RenderSkinningInput& input, RenderPassContext& ctx)
{
    auto* trace=s_skinningWorkTrace;
    auto palette = input.bonePalette;
    if (input.boneCount && !input.gpuBonePalette) {
        palette = s_palettePool.Acquire(ctx.resources, input.boneCount, sizeof(math::Matrix4));
        if (!palette.IsValid()) {
            return;
        }
        ctx.resources.Update(palette, ctx.skinningMatrices.data() + input.boneOffset,
            input.boneCount * sizeof(math::Matrix4));
        if(trace){++trace->bonePaletteUploadCalls;trace->bonePaletteUploadBytes+=input.boneCount*sizeof(math::Matrix4);}
        ctx.statsSkinningPaletteUploadBytes += input.boneCount * sizeof(math::Matrix4);
        ++ctx.statsSkinningPaletteUploads;
    }
    const uint32_t constants[4] = {input.vertexCount, input.gpuBonePalette ? input.boneOffset : 0u, 0, 0};
    ctx.resources.Update(ctx.handles.skinningCB, constants, sizeof(constants));
    ComputeCall call;
    call.shader = ctx.handles.skinningComputeCS;
    call.constantBuffers[cb::SKIN_COMPUTE] = ctx.handles.skinningCB;
    call.srvBuffers[srv::SKIN_SRC_VERTICES] = input.sourceVertices;
    call.srvBuffers[srv::SKIN_BONES] = palette;
    call.uavVertexBuffer = input.outputVertices;
    call.dispatchX = (input.vertexCount - 1u) / 64u + 1u;
    if(trace)++trace->backendDispatchAttempts;
    if (ctx.renderer.TryDispatch(call, ctx.resources)) {
        if(trace){++trace->backendDispatchSuccesses;trace->backendDispatchedVertexCount+=input.vertexCount;}
        ++ctx.statsSkinningDispatchCount;
        ctx.statsSkinningVertexCount += input.vertexCount;
    }
}
}

skinning_trace_detail::RequestCounters* skinning_trace_detail::ExchangeRequestCounters(RequestCounters* counters)
{
    auto* previous=s_skinningWorkTrace;
    s_skinningWorkTrace=counters;
    return previous;
}

ResourceHandle<ShaderTag> ResolveSkinningBatchShader(RenderPassContext& ctx)
{
    if (!ctx.renderer.GetCapabilities().bindless) {
        return {};
    }
    struct Cache { uint64_t owner = 0, epoch = UINT64_MAX; ResourceHandle<ShaderTag> shader; };
    static Cache cache;
    if (cache.owner != ctx.resources.GetInstanceId() || cache.epoch != ctx.resources.GetResetVersion()) {
        cache = {}; cache.owner = ctx.resources.GetInstanceId(); cache.epoch = ctx.resources.GetResetVersion();
        cache.shader = ctx.resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningBatch.cs.hlsl");
    }
    return ctx.resources.Get(cache.shader) ? cache.shader : ResourceHandle<ShaderTag>{};
}

static void ExecuteSkinningRequestsControl(RenderPassContext& ctx)
{
    if (ctx.skinningExecuted) {
        return;
    }
    ctx.skinningExecuted = true;
    if (!ctx.resources.Get(ctx.handles.skinningComputeCS) || !ctx.resources.Get(ctx.handles.skinningCB)
        || ctx.skinningRequests.empty()) {
        return;
    }
    auto* trace=s_skinningWorkTrace;
    const auto shader = ResolveSkinningBatchShader(ctx);
    ResourceHandle<StructuredBufferTag> palette;
    if (shader.IsValid() && !ctx.skinningMatrices.empty()) {
        palette = s_palettePool.Acquire(ctx.resources, ctx.skinningMatrices.size(), sizeof(math::Matrix4));
        if (palette.IsValid()) {
            ctx.resources.Update(palette, ctx.skinningMatrices.data(), ctx.skinningMatrices.size() * sizeof(math::Matrix4));
            if(trace){++trace->bonePaletteUploadCalls;trace->bonePaletteUploadBytes+=ctx.skinningMatrices.size()*sizeof(math::Matrix4);}
            ctx.statsSkinningPaletteUploadBytes += ctx.skinningMatrices.size() * sizeof(math::Matrix4);
            ++ctx.statsSkinningPaletteUploads;
        }
    }
    std::map<std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>,std::vector<const RenderSkinningInput*>> groups;
    ctx.renderer.BeginComputeBatch();
    for (const auto& input : ctx.skinningRequests) {
        if (!ValidRequest(input, ctx.resources, ctx.skinningMatrices)) {
            continue;
        }
        const auto inputPalette = input.gpuBonePalette ? input.bonePalette : palette;
        if (!shader.IsValid() || !inputPalette.IsValid() || !input.boneCount || input.vertexCount > 65535u * 64u) {
            DispatchOne(input, ctx); continue;
        }
        groups[{input.sourceVertices.id, input.sourceVertices.gen, input.vertexCount, inputPalette.id, inputPalette.gen}].push_back(&input);
    }
    for (const auto& [key, group] : groups) {
        // Separate chunks respect D3D12's 65535-group limit in Y and preserve individual output ownership.
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch
        for (size_t start = 0; start < group.size(); start += 65535u) {
            const size_t count = (std::min)(group.size() - start, size_t{65535});
            std::vector<SkinningRow> rows;
            rows.reserve(count);
            ComputeCall call;
            call.shader = shader;
            call.constantBuffers[cb::SKIN_COMPUTE] = ctx.handles.skinningCB;
            call.srvBuffers[srv::SKIN_SRC_VERTICES] = group[start]->sourceVertices;
            call.srvBuffers[srv::SKIN_BONES] = group[start]->gpuBonePalette ? group[start]->bonePalette : palette;
            for (size_t i = start; i < start + count; ++i) {
                const auto& input = *group[i];
                rows.push_back({input.boneOffset, ctx.resources.Get(input.outputVertices)->GetBindlessUavIndex(), input.boneCount, 0});
                call.indirectWriteVertexBuffers.push_back(input.outputVertices);
            }
            const uint64_t rowCreatesBefore=trace?trace->backendRowPoolCreateAttempts:0;
            call.srvBuffers[srv::SKIN_ROWS] = s_rowPool.Acquire(ctx.resources, rows.size(), sizeof(SkinningRow));
            if(trace && call.srvBuffers[srv::SKIN_ROWS].IsValid() && trace->backendRowPoolCreateAttempts==rowCreatesBefore) {
                ++trace->backendRowPoolReuses;
            }
            bool dispatched = false;
            if (call.srvBuffers[srv::SKIN_ROWS].IsValid()) {
                ctx.resources.Update(call.srvBuffers[srv::SKIN_ROWS], rows.data(), rows.size() * sizeof(SkinningRow));
                const uint32_t constants[4] = {group[start]->vertexCount, static_cast<uint32_t>(count), 0, 0};
                ctx.resources.Update(ctx.handles.skinningCB, constants, sizeof(constants));
                call.dispatchX = (group[start]->vertexCount - 1u) / 64u + 1u;
                call.dispatchY = static_cast<uint32_t>(count);
                // One synchronization per mesh group confirms PSO/descriptor allocation before selecting the fallback.
                if(trace)++trace->backendDispatchAttempts;
                dispatched = ctx.renderer.TryDispatch(call, ctx.resources);
            }
            if (dispatched) {
                if(trace){++trace->backendDispatchSuccesses;trace->backendDispatchedVertexCount+=static_cast<uint64_t>(count)*group[start]->vertexCount;}
                ++ctx.statsSkinningDispatchCount;
                for (size_t i = start; i < start + count; ++i) {
                    ctx.statsSkinningVertexCount += group[i]->vertexCount;
                }
            }
            else {
                for (size_t i = start; i < start + count; ++i) {
                    DispatchOne(*group[i], ctx);
                }
            }
        }
    }
    ctx.renderer.EndComputeBatch();
}
namespace {
/// @note CPU cache state is shared by the registry, a bounded worker task, and its serial release; GPU ownership stays in ResourceManager.
struct OwnedSkinningCache {
    /// @note The caller keeps this facade alive until ReleaseSkinningRequestCaches returns, including its serial worker release operation.
    IRenderer* facade = nullptr;
    uint64_t owner = 0, epoch = 0;
    bool transient = false;
    DynamicStructuredBufferPool palettePool;
    DynamicBufferPool<StructuredBufferTag, SkinningRowBufferFactory> rowPool;
};
std::mutex s_ownedCacheRegistryMutex;
std::array<std::shared_ptr<OwnedSkinningCache>, 16> s_ownedCacheRegistry;

std::shared_ptr<OwnedSkinningCache> AcquireOwnedSkinningCache(IRenderer& facade, ResourceManager& resources)
{
    std::lock_guard lock(s_ownedCacheRegistryMutex);
    std::shared_ptr<OwnedSkinningCache>* empty = nullptr;
    bool differentFacade = false;
    for (auto& entry : s_ownedCacheRegistry) {
        if (entry && entry->owner == resources.GetInstanceId()) {
            if (entry->epoch == resources.GetResetVersion() && entry->facade == &facade) {
                return entry;
            }
            if (entry->epoch != resources.GetResetVersion()) {
                entry.reset();
            }
            else {
                differentFacade = true;
            }
        }
        if (!entry && !empty) {
            empty = &entry;
        }
    }
    auto cache = std::make_shared<OwnedSkinningCache>();
    cache->facade = &facade;
    cache->owner = resources.GetInstanceId();
    cache->epoch = resources.GetResetVersion();
    cache->transient = differentFacade || empty == nullptr;
    if (!cache->transient) {
        *empty = cache;
    }
    return cache;
}

void ReleaseOwnedSkinningCache(ResourceManager& resources)
{
    std::shared_ptr<OwnedSkinningCache> cache;
    {
        std::lock_guard lock(s_ownedCacheRegistryMutex);
        for (auto& entry : s_ownedCacheRegistry) {
            if (entry && entry->owner == resources.GetInstanceId()) {
                cache = std::move(entry);
                break;
            }
        }
    }
    if (!cache || cache->epoch != resources.GetResetVersion()) {
        return;
    }
    // Never hold the registry mutex across a pump; release executes after every preceding owned task on the same facade.
    cache->facade->ExecuteResourceOperation([cache, &resources] {
        if (cache->owner != resources.GetInstanceId() || cache->epoch != resources.GetResetVersion()) {
            return;
        }
        cache->palettePool.ReleaseAll(resources);
        cache->rowPool.ReleaseAll(resources);
    });
}

struct OwnedSkinningWork {
    std::vector<RenderSkinningInput> requests;
    std::vector<math::Matrix4> matrices;
    ResourceHandle<ShaderTag> computeShader, batchShader;
    ResourceHandle<ConstantBufferTag> constants;
    std::shared_ptr<OwnedSkinningCache> cache;
    RenderSkinningBatchReceipt receipt;
    uint64_t vertices = 0, paletteBytes = 0;
    uint32_t dispatches = 0, paletteUploads = 0;
    bool traceEnabled = false;
    skinning_trace_detail::RequestCounters trace;
};

bool ConfirmOwnedSkinningWrite(OwnedSkinningWork& work, size_t index, ResourceManager& resources)
{
    auto& receipt = work.receipt.writes[index];
    const auto* output = resources.Get(receipt.output);
    receipt.resultingVersion = output ? output->GetContentVersion() : 0;
    receipt.recorded = receipt.resultingVersion > receipt.previousVersion;
    return receipt.recorded;
}

void LogOwnedSkinningSuccess(const OwnedSkinningWork& work)
{
    static std::atomic<bool> logged{false};
    // This witness requires a true native dispatch result and a confirmed DIRECT recording-version advance, never GPU fence completion.
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        FBZZ_LOG_INFO("OwnedSkinningBatch path=owned selected=1 work=1 dispatch=1 resourceFrame=%llu", static_cast<unsigned long long>(work.receipt.resourceFrame));
    }
}

void DispatchOwnedSkinningOne(size_t index, OwnedSkinningWork& work, IRenderer& facade, ResourceManager& resources)
{
    const auto& input = work.requests[index];
    auto palette = input.bonePalette;
    if (input.boneCount && !input.gpuBonePalette) {
        palette = work.cache->palettePool.Acquire(resources, input.boneCount, sizeof(math::Matrix4));
        if (!palette.IsValid()) {
            return;
        }
        resources.Update(palette, work.matrices.data() + input.boneOffset, input.boneCount * sizeof(math::Matrix4));
        const uint64_t bytes = input.boneCount * sizeof(math::Matrix4);
        work.paletteBytes += bytes; ++work.paletteUploads;
        if (work.traceEnabled) { ++work.trace.bonePaletteUploadCalls; work.trace.bonePaletteUploadBytes += bytes; }
    }
    const uint32_t constants[4] = {input.vertexCount, input.gpuBonePalette ? input.boneOffset : 0u, 0, 0};
    resources.Update(work.constants, constants, sizeof(constants));
    ComputeCall call;
    call.shader = work.computeShader;
    call.constantBuffers[cb::SKIN_COMPUTE] = work.constants;
    call.srvBuffers[srv::SKIN_SRC_VERTICES] = input.sourceVertices;
    call.srvBuffers[srv::SKIN_BONES] = palette;
    call.uavVertexBuffer = input.outputVertices;
    call.dispatchX = (input.vertexCount - 1u) / 64u + 1u;
    if (work.traceEnabled) {
        ++work.trace.backendDispatchAttempts;
    }
    if (!facade.TryDispatch(call, resources) || !ConfirmOwnedSkinningWrite(work, index, resources)) {
        return;
    }
    LogOwnedSkinningSuccess(work);
    ++work.dispatches; work.vertices += input.vertexCount;
    if (work.traceEnabled) { ++work.trace.backendDispatchSuccesses; work.trace.backendDispatchedVertexCount += input.vertexCount; }
}

void RunOwnedSkinningWork(OwnedSkinningWork& work, IRenderer& facade, ResourceManager& resources)
{
    if (work.receipt.resourceInstance != resources.GetInstanceId() || work.receipt.resourceEpoch != resources.GetResetVersion()
        || work.receipt.resourceFrame != resources.FrameStamp() || !resources.Get(work.computeShader) || !resources.Get(work.constants)) {
        return;
    }
    // A full bounded registry uses uncached native work, retiring its pools after the batch closes instead of changing geometry/fallback behavior.
    struct TransientPoolScope {
        OwnedSkinningWork& work; ResourceManager& resources;
        ~TransientPoolScope()
        {
            if (!work.cache->transient || work.cache->epoch != resources.GetResetVersion()) {
                return;
            }
            work.cache->palettePool.ReleaseAll(resources); work.cache->rowPool.ReleaseAll(resources);
        }
    } transientPools{work, resources};
    skinning_trace_detail::RequestCounterScope traceScope(work.traceEnabled ? &work.trace : nullptr);
    work.receipt.writes.reserve(work.requests.size());
    for (const auto& input : work.requests) {
        const auto* output = resources.Get(input.outputVertices);
        work.receipt.writes.push_back({input.outputVertices, output ? output->GetContentVersion() : 0, 0, false});
    }
    const auto shader = resources.Get(work.batchShader) ? work.batchShader : ResourceHandle<ShaderTag>{};
    ResourceHandle<StructuredBufferTag> palette;
    if (shader.IsValid() && !work.matrices.empty()) {
        palette = work.cache->palettePool.Acquire(resources, work.matrices.size(), sizeof(math::Matrix4));
        if (palette.IsValid()) {
            const uint64_t bytes = work.matrices.size() * sizeof(math::Matrix4);
            resources.Update(palette, work.matrices.data(), bytes);
            work.paletteBytes += bytes; ++work.paletteUploads;
            if (work.traceEnabled) { ++work.trace.bonePaletteUploadCalls; work.trace.bonePaletteUploadBytes += bytes; }
        }
    }
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, std::vector<size_t>> groups;
    facade.BeginComputeBatch();
    // Close native UAV barriers even when validation/allocation/dispatch rejects every row.
    struct EndBatchScope { IRenderer& facade; ~EndBatchScope() { facade.EndComputeBatch(); } } endBatch{facade};
    for (size_t index = 0; index < work.requests.size(); ++index) {
        const auto& input = work.requests[index];
        if (!ValidRequest(input, resources, work.matrices)) {
            continue;
        }
        const auto inputPalette = input.gpuBonePalette ? input.bonePalette : palette;
        if (!shader.IsValid() || !inputPalette.IsValid() || !input.boneCount || input.vertexCount > 65535u * 64u) {
            DispatchOwnedSkinningOne(index, work, facade, resources); continue;
        }
        groups[{input.sourceVertices.id, input.sourceVertices.gen, input.vertexCount, inputPalette.id, inputPalette.gen}].push_back(index);
    }
    // Only owned values survive between groups; clear row and output lists while retaining this operation's capacity.
    std::vector<SkinningRow> rows;
    ComputeCall call;
    call.shader = shader; call.constantBuffers[cb::SKIN_COMPUTE] = work.constants;
    for (const auto& [key, group] : groups) {
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch Dispatch limits.
        for (size_t start = 0; start < group.size(); start += 65535u) {
            const size_t count = (std::min)(group.size() - start, size_t{65535});
            const auto& first = work.requests[group[start]];
            rows.clear();
            rows.reserve(count);
            call.indirectWriteVertexBuffers.clear();
            call.indirectWriteVertexBuffers.reserve(count);
            call.srvBuffers[srv::SKIN_ROWS] = {};
            call.srvBuffers[srv::SKIN_SRC_VERTICES] = first.sourceVertices;
            call.srvBuffers[srv::SKIN_BONES] = first.gpuBonePalette ? first.bonePalette : palette;
            bool rowsReady = true;
            for (size_t offset = start; offset < start + count; ++offset) {
                const auto& input = work.requests[group[offset]];
                const auto* output = resources.Get(input.outputVertices);
                if (!output || output->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) { rowsReady = false; break; }
                rows.push_back({input.boneOffset, output->GetBindlessUavIndex(), input.boneCount, 0});
                call.indirectWriteVertexBuffers.push_back(input.outputVertices);
            }
            const uint64_t createsBefore = work.trace.backendRowPoolCreateAttempts;
            if (rowsReady) {
                call.srvBuffers[srv::SKIN_ROWS] = work.cache->rowPool.Acquire(resources, rows.size(), sizeof(SkinningRow));
            }
            if (work.traceEnabled && call.srvBuffers[srv::SKIN_ROWS].IsValid() && work.trace.backendRowPoolCreateAttempts == createsBefore) {
                ++work.trace.backendRowPoolReuses;
            }
            bool dispatched = false;
            if (call.srvBuffers[srv::SKIN_ROWS].IsValid()) {
                resources.Update(call.srvBuffers[srv::SKIN_ROWS], rows.data(), rows.size() * sizeof(SkinningRow));
                const uint32_t constants[4] = {first.vertexCount, static_cast<uint32_t>(count), 0, 0};
                resources.Update(work.constants, constants, sizeof(constants));
                call.dispatchX = (first.vertexCount - 1u) / 64u + 1u; call.dispatchY = static_cast<uint32_t>(count);
                if (work.traceEnabled) {
                    ++work.trace.backendDispatchAttempts;
                }
                dispatched = facade.TryDispatch(call, resources);
            }
            if (!dispatched) {
                for (size_t offset = start; offset < start + count; ++offset) {
                    DispatchOwnedSkinningOne(group[offset], work, facade, resources);
                }
                continue;
            }
            bool allWritten = true;
            for (size_t offset = start; offset < start + count; ++offset) {
                allWritten &= ConfirmOwnedSkinningWrite(work, group[offset], resources);
            }
            if (!allWritten) {
                continue;
            }
            LogOwnedSkinningSuccess(work);
            const uint64_t vertices = static_cast<uint64_t>(count) * first.vertexCount;
            ++work.dispatches; work.vertices += vertices;
            if (work.traceEnabled) { ++work.trace.backendDispatchSuccesses; work.trace.backendDispatchedVertexCount += vertices; }
        }
    }
}

void AddOwnedSkinningCounters(skinning_trace_detail::RequestCounters& destination, const skinning_trace_detail::RequestCounters& source)
{
    destination.bonePaletteUploadCalls += source.bonePaletteUploadCalls;
    destination.bonePaletteUploadBytes += source.bonePaletteUploadBytes;
    destination.backendRowPoolCreateAttempts += source.backendRowPoolCreateAttempts;
    destination.backendRowPoolCreateSuccesses += source.backendRowPoolCreateSuccesses;
    destination.backendRowPoolReuses += source.backendRowPoolReuses;
    destination.backendDispatchAttempts += source.backendDispatchAttempts;
    destination.backendDispatchSuccesses += source.backendDispatchSuccesses;
    destination.backendDispatchedVertexCount += source.backendDispatchedVertexCount;
}
} // namespace

void ExecuteSkinningRequests(RenderPassContext& ctx)
{
    const bool owned = skinning_batch_detail::Enabled();
    // Explicit comparison runs emit one path witness before warmup; an unset default adds no logging/allocation/clock/queue operation.
    if (!owned && std::getenv("FBZZ_OWNED_SKINNING_BATCH")) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            FBZZ_LOG_INFO("OwnedSkinningBatch path=control selected=0 work=0 resourceFrame=%llu", static_cast<unsigned long long>(ctx.resources.FrameStamp()));
        }
    }
    if (!owned) { ExecuteSkinningRequestsControl(ctx); return; }
    if (ctx.skinningExecuted) {
        return;
    }
    ctx.skinningExecuted = true;
    ctx.skinningBatchReceipt = {};
    ctx.skinningBatchReceipt.ownedPath = true;
    ctx.skinningBatchReceipt.resourceInstance = ctx.resources.GetInstanceId();
    ctx.skinningBatchReceipt.resourceEpoch = ctx.resources.GetResetVersion();
    ctx.skinningBatchReceipt.resourceFrame = ctx.resources.FrameStamp();
    if (!ctx.resources.Get(ctx.handles.skinningComputeCS) || !ctx.resources.Get(ctx.handles.skinningCB) || ctx.skinningRequests.empty()) {
        return;
    }
    // The shared CPU owner holds every input and result through synchronous packet destruction; it owns no native resource or host reference.
    auto work = std::make_shared<OwnedSkinningWork>();
    work->requests = ctx.skinningRequests;
    work->matrices = ctx.skinningMatrices;
    work->computeShader = ctx.handles.skinningComputeCS;
    work->constants = ctx.handles.skinningCB;
    work->receipt = ctx.skinningBatchReceipt;
    work->traceEnabled = s_skinningWorkTrace != nullptr;
    // AssetPathService/load callbacks stay on the producer; the worker receives only resolved typed handles.
    work->batchShader = ResolveSkinningBatchShader(ctx);
    if (work->receipt.resourceInstance != ctx.resources.GetInstanceId() || work->receipt.resourceEpoch != ctx.resources.GetResetVersion()) {
        return;
    }
    work->cache = AcquireOwnedSkinningCache(ctx.renderer, ctx.resources);
    auto& facade = ctx.renderer;
    auto& resources = ctx.resources;
    // Host/facade lifetime is borrowed only for this bounded synchronous operation; no ctx, element, matrix span or producer trace pointer is captured.
    facade.ExecuteResourceOperation([work, &facade, &resources] { RunOwnedSkinningWork(*work, facade, resources); });
    ctx.skinningBatchReceipt = std::move(work->receipt);
    ctx.statsSkinningDispatchCount += work->dispatches;
    ctx.statsSkinningVertexCount += work->vertices;
    ctx.statsSkinningPaletteUploads += work->paletteUploads;
    ctx.statsSkinningPaletteUploadBytes += work->paletteBytes;
    if (work->traceEnabled && s_skinningWorkTrace) {
        AddOwnedSkinningCounters(*s_skinningWorkTrace, work->trace);
    }
}

void SkinningComputePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSkinningRequests(ctx);
}
void ReleaseSkinningRequestCaches(ResourceManager& resources)
{
    ReleaseBakedBoneReplayCaches(resources);
    s_palettePool.ReleaseAll(resources);
    s_rowPool.ReleaseAll(resources);
    ReleaseOwnedSkinningCache(resources);
}
}
