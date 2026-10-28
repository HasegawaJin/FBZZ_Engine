/// @file    InstanceBatch.cpp
/// @brief   連続する同一の描画を 1 回の Instanced Draw へ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// @see Docs/design/gpu-instancing.md
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Graphics/Renderer/RenderCpuTrace.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/GpuInstanceCulling.hpp>
#include <cstring>
#include <Graphics/Renderer/IBuffer.hpp>
#include <array>
#include <algorithm>

namespace fbzz::renderer {

namespace {

/// @note インスタンスバッファの貸出プール。フレームごとに列を分ける規則は DynamicBufferPool.hpp。
/// @note 下限を 64 にするのは、束ねる価値が出るのが数十個からで、2 個の束に 16 個ぶんの枠を
/// @note 何十本も抱えるより、少数の大きい枠を回した方が確保の本数が減るため。
renderer::DynamicStructuredBufferPool s_instancePool{ 64 };
GpuInstanceCulling s_instanceCulling;
struct DeformedInstanceData {
    math::Matrix4 world;
    math::Matrix4 normal;
    uint32_t vertexIndex;
    uint32_t padding[3]{};
};
static_assert(sizeof(DeformedInstanceData) == 144);

ResourceHandle<ShaderTag> DeformedShader(RenderPassContext& ctx, ResourceHandle<ShaderTag> base)
{
    if (!ctx.settings.gpuInstancing || !ctx.renderer.GetCapabilities().bindless) return {};
    size_t index = 3;
    if (base == ctx.handles.gbufferShader) index = 0;
    if (base == ctx.handles.shadowShader) index = 1;
    if (base == ctx.handles.objectMaskShader) index = 2;
    if (index == 3) return {};
    struct Cache { uint64_t owner = 0, epoch = UINT64_MAX; std::array<ResourceHandle<ShaderTag>,3> shaders{}; bool loaded = false; };
    static Cache cache;
    if (cache.owner != ctx.resources.GetInstanceId() || cache.epoch != ctx.resources.GetResetVersion()) {
        cache = {}; cache.owner = ctx.resources.GetInstanceId(); cache.epoch = ctx.resources.GetResetVersion();
    }
    if (!cache.loaded) {
        const char* paths[] = {"Assets/Shaders/Pipeline/Deferred/GBufferDeformedInstanced.hlsl",
            "Assets/Shaders/Pipeline/Shadow/ShadowMapDeformedInstanced.hlsl",
            "Assets/Shaders/Pipeline/Mask/ObjectMaskDeformedInstanced.hlsl"};
        for (size_t i = 0; i < 3; ++i) cache.shaders[i] = ctx.resources.LoadShader(paths[i]);
        cache.loaded = true;
    }
    return ctx.resources.Get(cache.shaders[index]) ? cache.shaders[index] : ResourceHandle<ShaderTag>{};
}

/// @brief この束を Instanced Draw として出す価値があるか。
/// @note 2 個だと «ドロー 2 回» が «ドロー 1 回 + バッファ確保 + 128 バイト転送» に変わるだけで、
/// @note CPU 側の得が確実とは言えない。3 個から束ねる。
constexpr std::size_t kMinInstanceCount = 3;

} /// @note namespace

bool IsSameInstanceBatch(const renderer::DrawCall& a, const renderer::DrawCall& b)
{
    if (a.preparedSkinning != b.preparedSkinning || a.preparedSkinningRow != b.preparedSkinningRow) return false;
    /// @note b1 は per-object なので比較しない。instanceBuffer / instanceCount は束ね側が決める。
    if (a.deformationSource != b.deformationSource || a.indirectReadBuffers != b.indirectReadBuffers) return false;
    if (!a.deformationSource.IsValid() && a.vertexBuffer != b.vertexBuffer) return false;
    if (a.indexBuffer   != b.indexBuffer)   return false;
    if (a.shader        != b.shader)        return false;
    if (a.pipelineState != b.pipelineState) return false;
    if (a.indexCount    != b.indexCount)    return false;
    if (a.vertexCount   != b.vertexCount)   return false;
    if (a.startIndex    != b.startIndex)    return false;
    if (a.baseVertex    != b.baseVertex)    return false;
    if (a.layer         != b.layer)         return false;
    if (a.topology      != b.topology)      return false;
    if (a.indirectArguments != b.indirectArguments || a.indirectArgumentOffset != b.indirectArgumentOffset
        || a.indirectCommandCount != b.indirectCommandCount || a.indirectInstanceOffset != b.indirectInstanceOffset
        || a.meshGroupCountX != b.meshGroupCountX || a.meshGroupCountY != b.meshGroupCountY
        || a.meshGroupCountZ != b.meshGroupCountZ) return false;
    if (a.indirectReadTextures != b.indirectReadTextures || a.vsRawBuffers != b.vsRawBuffers
        || a.auxiliaryPsBuffers != b.auxiliaryPsBuffers) return false;

    for (std::size_t i = 0; i < a.constantBuffers.size(); ++i) {
        /// @note b1 (ObjectConstants) だけは中身が物体ごとに違って当然なので見ない。
        if (i == 1) continue;
        if (a.constantBuffers[i] != b.constantBuffers[i]) return false;
    }
    for (std::size_t i = 0; i < a.textures.size(); ++i)
        if (a.textures[i] != b.textures[i]) return false;
    for (std::size_t i = 0; i < a.vsBuffers.size(); ++i)
        if (a.vsBuffers[i] != b.vsBuffers[i]) return false;
    for (std::size_t i = 0; i < a.psBuffers.size(); ++i)
        if (a.psBuffers[i] != b.psBuffers[i]) return false;
    return true;
}

InstanceBatcher::InstanceBatcher(RenderPassContext& ctx,
                                 renderer::ResourceHandle<renderer::ShaderTag> baseShader,
                                 renderer::ResourceHandle<renderer::ShaderTag> instancedShader,
                                  bool shadow, const math::Frustum* gpuFrustum)
    : m_ctx(ctx)
    , m_baseShader(baseShader)
    , m_instancedShader(instancedShader)
    , m_shadow(shadow)
{
    m_gpuFrustum = gpuFrustum;
    m_deformedShader = DeformedShader(ctx, baseShader);
}

InstanceBatcher::~InstanceBatcher()
{
    Flush();
}

void InstanceBatcher::Add(const renderer::DrawCall& prototype, const PerObjectCB& object,
    const math::Vector4& worldSphere)
{
    const cpu_trace::SourceScope traceSource(prototype.deformationSource.IsValid()
        ? cpu_trace::Source::BATCH_DEFORMED : cpu_trace::Source::BATCH_STATIC, "InstanceBatcher::Add");
    /// @note LOD 遷移中は objectParams.x を PS が読む。per-instance に運べないので束ねない
    /// @note (Docs/design/gpu-instancing.md §2.1)。
    const auto* deformed = prototype.deformationSource.IsValid() ? m_ctx.resources.Get(prototype.vertexBuffer) : nullptr;
    const bool deformedReady = !prototype.deformationSource.IsValid()
        || (m_deformedShader.IsValid() && deformed && deformed->GetContentVersion()
            && deformed->GetBindlessSrvIndex() != INVALID_BINDLESS_INDEX);
    const bool batchable = !prototype.preparedSkinning && deformedReady && m_instancedShader.IsValid()
                        && prototype.shader == m_baseShader
                        && object.objectParams.x == 0.0f
                        && !prototype.instanceBuffer.IsValid() && prototype.instanceCount == 1
                        && !prototype.indirectArguments.IsValid() && prototype.meshGroupCountX == 0;

    if (!batchable) {
        Flush();
        renderer::DrawCall call = prototype;
        BindObjectConstants(call, object);
        SubmitOne(call);
        return;
    }

    if (!m_pending.empty() && !IsSameInstanceBatch(m_prototype, prototype))
        Flush();

    if (m_pending.empty())
        m_prototype = prototype;
    m_pending.push_back(object);
    m_deformedVertices.push_back(prototype.vertexBuffer);
    m_gpuBounds.push_back(worldSphere);
}

void InstanceBatcher::Flush()
{
    const cpu_trace::SourceScope traceSource(cpu_trace::Source::BATCH_FLUSH, "InstanceBatcher::Flush");
    if (m_pending.empty()) return;

    /// @note 吐き出す前に空にするのは、確保に失敗した経路が SubmitOne を呼ぶため。
    /// @note 残したままだと Flush が再入して同じ束を二重に描く。
    std::vector<PerObjectCB> pending;
    pending.swap(m_pending);
    std::vector<math::Vector4> bounds;
    bounds.swap(m_gpuBounds);
    std::vector<ResourceHandle<BufferTag>> vertices;
    vertices.swap(m_deformedVertices);
    const bool deformedBatch = m_prototype.deformationSource.IsValid();

    const bool gpuCulling = m_gpuFrustum && m_ctx.frustumCullingEnabled
        && (m_ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INDIRECT
            || m_ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INSTANCE_INDIRECT);
    if (gpuCulling && !deformedBatch && pending.size() >= 64) {
        std::vector<GpuCullInstance> gpuInstances;
        gpuInstances.reserve(pending.size());
        for (size_t i = 0; i < pending.size(); ++i)
            gpuInstances.push_back({pending[i].world, pending[i].worldInvTranspose, bounds[i]});
        renderer::DrawCall gpuCall = m_prototype;
        gpuCall.shader = m_instancedShader;
        BindObjectConstants(gpuCall, PerObjectCB{});
        if (s_instanceCulling.SubmitStatic(m_ctx.renderer, m_ctx.resources, *m_gpuFrustum, gpuInstances, gpuCall)) {
            /// @note SubmitStatic already enqueued the draw; only publish candidate statistics here.
            if (m_shadow) { ++m_ctx.statsShadowDrawCalls; m_ctx.statsShadowTriangleCount += DrawCallTriangleCount(gpuCall); }
            else { ++m_ctx.statsDrawCalls; m_ctx.statsVertexCount += DrawCallVertexCount(gpuCall); m_ctx.statsTriangleCount += DrawCallTriangleCount(gpuCall); }
            return;
        }
    }
    if (gpuCulling && deformedBatch && pending.size() >= 32) {
        std::vector<GpuDeformedInstance> instances;
        instances.reserve(pending.size());
        bool ready = true;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto* buffer = m_ctx.resources.Get(vertices[i]);
            if (!buffer || !buffer->GetContentVersion() || buffer->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) { ready = false; break; }
            instances.push_back({pending[i].world, pending[i].worldInvTranspose, buffer->GetBindlessSrvIndex(), {}});
        }
        DrawCall call = m_prototype;
        call.shader = m_deformedShader; call.vertexBuffer = {};
        call.indirectReadBuffers = vertices;
        if (ready) {
            BindObjectConstants(call, PerObjectCB{});
            if (s_instanceCulling.SubmitDeformed(m_ctx.renderer, m_ctx.resources, *m_gpuFrustum, instances, bounds, call)) {
                if (m_shadow) { ++m_ctx.statsShadowDrawCalls; m_ctx.statsShadowTriangleCount += DrawCallTriangleCount(call); }
                else { ++m_ctx.statsDrawCalls; m_ctx.statsVertexCount += DrawCallVertexCount(call); m_ctx.statsTriangleCount += DrawCallTriangleCount(call); }
                return;
            }
        }
    }
    /// @note Deferred candidates also include offscreen objects; failed and small GPU batches must cull on the CPU.
    if (gpuCulling) {
        size_t write = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto& sphere = bounds[i];
            if (sphere.w > 0 && !m_gpuFrustum->IntersectsSphere({sphere.x, sphere.y, sphere.z}, sphere.w)) continue;
            vertices[write] = vertices[i];
            pending[write++] = pending[i];
        }
        pending.resize(write);
        vertices.resize(write);
        if (pending.empty()) return;
    }

    renderer::ResourceHandle<renderer::StructuredBufferTag> instanceBuffer;
    const bool deformedResourcesReady = !deformedBatch || std::all_of(vertices.begin(), vertices.end(), [&](const auto handle) {
        const auto* buffer = m_ctx.resources.Get(handle);
        return buffer && buffer->GetContentVersion() && buffer->GetBindlessSrvIndex() != INVALID_BINDLESS_INDEX;
    });
    if (deformedResourcesReady && pending.size() >= kMinInstanceCount) {
        instanceBuffer = s_instancePool.Acquire(
            m_ctx.resources, pending.size(), static_cast<std::uint32_t>(deformedBatch ? sizeof(DeformedInstanceData) : sizeof(PerInstanceData)));
    }

    if (!instanceBuffer.IsValid()) {
        /// @note 束ねる価値が無い / 確保に失敗した。従来どおり 1 件ずつ出す。
        for (size_t i = 0; i < pending.size(); ++i) {
            renderer::DrawCall call = m_prototype;
            call.vertexBuffer = vertices[i];
            BindObjectConstants(call, pending[i]);
            SubmitOne(call);
        }
        return;
    }

    renderer::DrawCall call = m_prototype;
    if (deformedBatch) {
        std::vector<DeformedInstanceData> instances;
        instances.reserve(pending.size());
        for (size_t i = 0; i < pending.size(); ++i) instances.push_back({pending[i].world,
            pending[i].worldInvTranspose, m_ctx.resources.Get(vertices[i])->GetBindlessSrvIndex(), {}});
        m_ctx.resources.Update(instanceBuffer, instances.data(), instances.size() * sizeof(DeformedInstanceData));
        call.shader = m_deformedShader;
        call.vertexBuffer = {};
        call.indirectReadBuffers = std::move(vertices);
    } else {
        std::vector<PerInstanceData> instances;
        instances.reserve(pending.size());
        for (const PerObjectCB& object : pending) instances.push_back({object.world, object.worldInvTranspose});
        m_ctx.resources.Update(instanceBuffer, instances.data(), instances.size() * sizeof(PerInstanceData));
        call.shader = m_instancedShader;
    }
    call.instanceBuffer = instanceBuffer;
    call.instanceCount = static_cast<std::uint32_t>(pending.size());
    /// @note b1 は PS の objectParams (LOD ディザ) のためだけに束縛する。world / worldInvTranspose の
    /// @note 枠は変種 VS が読まない (読むと束の全員が同じ場所に描かれるので、絵で即座に分かる)。
    BindObjectConstants(call, PerObjectCB{});

    m_ctx.renderer.Submit(call, m_ctx.resources);
    if (m_shadow) {
        ++m_ctx.statsShadowDrawCalls;
        m_ctx.statsShadowTriangleCount += DrawCallTriangleCount(call);
    } else {
        ++m_ctx.statsDrawCalls;
        m_ctx.statsVertexCount   += DrawCallVertexCount(call);
        m_ctx.statsTriangleCount += DrawCallTriangleCount(call);
    }
    if (!call.indirectArguments.IsValid()) {
        ++m_ctx.statsInstancedBatches;
        m_ctx.statsInstancedDrawsSaved += static_cast<int>(pending.size()) - 1;
    }
}

void InstanceBatcher::SubmitOne(const renderer::DrawCall& call)
{
    if (m_shadow) SubmitCountedShadow(m_ctx, call);
    else          SubmitCounted(m_ctx, call);
}

void InstanceBatcher::BindObjectConstants(renderer::DrawCall& call, const PerObjectCB& object)
{
    /// @note 同じ内容の載せ直しを飛ばす。同じ物体の submesh が連続する影の列で効く。
    if (!m_hasLastObject || std::memcmp(&m_lastObject, &object, sizeof(PerObjectCB)) != 0) {
        m_ctx.resources.Update(m_ctx.handles.objectCB, &object, sizeof(PerObjectCB));
        m_lastObject    = object;
        m_hasLastObject = true;
    }
    call.constantBuffers[1] = m_ctx.handles.objectCB;
}

void ReleaseInstanceBatchCaches(renderer::ResourceManager& resources)
{
    s_instancePool.ReleaseAll(resources);
    s_instanceCulling.Release(resources);
}

} /// @note namespace fbzz::renderer
