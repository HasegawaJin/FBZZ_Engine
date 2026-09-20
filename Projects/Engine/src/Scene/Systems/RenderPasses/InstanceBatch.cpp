/// @file    InstanceBatch.cpp
/// @brief   連続する同一の描画を 1 回の Instanced Draw へ束ねる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// @see Docs/design/gpu-instancing.md
#include <Engine/Scene/Systems/RenderPasses/InstanceBatch.hpp>
#include <Engine/Renderer/DynamicBufferPool.hpp>
#include <cstring>

namespace fbzz::scene {

namespace {

/// @note インスタンスバッファの貸出プール。フレームごとに列を分ける規則は DynamicBufferPool.hpp。
/// @note 下限を 64 にするのは、束ねる価値が出るのが数十個からで、2 個の束に 16 個ぶんの枠を
///       何十本も抱えるより、少数の大きい枠を回した方が確保の本数が減るため。
renderer::DynamicStructuredBufferPool s_instancePool{ 64 };

/// @brief この束を Instanced Draw として出す価値があるか。
/// @note 2 個だと «ドロー 2 回» が «ドロー 1 回 + バッファ確保 + 128 バイト転送» に変わるだけで、
///       CPU 側の得が確実とは言えない。3 個から束ねる。
constexpr std::size_t kMinInstanceCount = 3;

} // namespace

bool IsSameInstanceBatch(const renderer::DrawCall& a, const renderer::DrawCall& b)
{
    /// @note b1 は per-object なので比較しない。instanceBuffer / instanceCount は束ね側が決める。
    if (a.vertexBuffer  != b.vertexBuffer)  return false;
    if (a.indexBuffer   != b.indexBuffer)   return false;
    if (a.shader        != b.shader)        return false;
    if (a.pipelineState != b.pipelineState) return false;
    if (a.indexCount    != b.indexCount)    return false;
    if (a.vertexCount   != b.vertexCount)   return false;
    if (a.startIndex    != b.startIndex)    return false;
    if (a.baseVertex    != b.baseVertex)    return false;
    if (a.layer         != b.layer)         return false;
    if (a.topology      != b.topology)      return false;

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
                                 bool shadow)
    : m_ctx(ctx)
    , m_baseShader(baseShader)
    , m_instancedShader(instancedShader)
    , m_shadow(shadow)
{
}

InstanceBatcher::~InstanceBatcher()
{
    Flush();
}

void InstanceBatcher::Add(const renderer::DrawCall& prototype, const PerObjectCB& object)
{
    /// @note LOD 遷移中は objectParams.x を PS が読む。per-instance に運べないので束ねない
    ///       (Docs/design/gpu-instancing.md §2.1)。
    const bool batchable = m_instancedShader.IsValid()
                        && prototype.shader == m_baseShader
                        && object.objectParams.x == 0.0f;

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
}

void InstanceBatcher::Flush()
{
    if (m_pending.empty()) return;

    /// @note 吐き出す前に空にするのは、確保に失敗した経路が SubmitOne を呼ぶため。
    ///       残したままだと Flush が再入して同じ束を二重に描く。
    std::vector<PerObjectCB> pending;
    pending.swap(m_pending);

    renderer::ResourceHandle<renderer::StructuredBufferTag> instanceBuffer;
    if (pending.size() >= kMinInstanceCount) {
        instanceBuffer = s_instancePool.Acquire(
            m_ctx.resources, pending.size(), static_cast<std::uint32_t>(sizeof(PerInstanceData)));
    }

    if (!instanceBuffer.IsValid()) {
        /// @note 束ねる価値が無い / 確保に失敗した。従来どおり 1 件ずつ出す。
        for (const PerObjectCB& object : pending) {
            renderer::DrawCall call = m_prototype;
            BindObjectConstants(call, object);
            SubmitOne(call);
        }
        return;
    }

    std::vector<PerInstanceData> instances;
    instances.reserve(pending.size());
    for (const PerObjectCB& object : pending)
        instances.push_back(PerInstanceData{ object.world, object.worldInvTranspose });
    m_ctx.resources.Update(instanceBuffer, instances.data(),
                           instances.size() * sizeof(PerInstanceData));

    renderer::DrawCall call = m_prototype;
    call.shader         = m_instancedShader;
    call.instanceBuffer = instanceBuffer;
    call.instanceCount  = static_cast<std::uint32_t>(instances.size());
    /// @note b1 は PS の objectParams (LOD ディザ) のためだけに束縛する。world / worldInvTranspose の
    ///       枠は変種 VS が読まない (読むと束の全員が同じ場所に描かれるので、絵で即座に分かる)。
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
    ++m_ctx.statsInstancedBatches;
    m_ctx.statsInstancedDrawsSaved += static_cast<int>(instances.size()) - 1;
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
}

} // namespace fbzz::scene
