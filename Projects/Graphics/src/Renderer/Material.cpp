/// @file    Material.cpp
/// @brief   Material の GPU パラメーター初期化と転送。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Graphics/Renderer/Material.hpp"
#include "Graphics/Renderer/ResourceManager.hpp"
#include "Graphics/Renderer/IConstantBuffer.hpp"
#include "MaterialTransferPayload.hpp"
#include <cstring>
#include <utility>

namespace fbzz::renderer {

namespace {

/// @note Active() を使う理由: Material は Model/MaterialComponent/各パスのキャッシュへ埋め込まれて
/// @note       畳まれるため、破棄地点へ ResourceManager& を渡す経路が無い。ResourceManager より後に
/// @note       消える Material では Active() が空になり、そのときは解放先が無いので何もしない。
void ReleaseParamsBuffer(ResourceHandle<ConstantBufferTag>& buffer)
{
    if (!buffer.IsValid()) return;
    if (ResourceManager* resources = ResourceManager::Active())
        resources->Release(buffer);
    buffer = {};
}

} /// @note namespace

namespace material_transfer_detail {
namespace {
thread_local MaterialTransferPayloadScope* s_currentMaterialTransfer = nullptr;

uint64_t PayloadHash(std::span<const uint8_t> bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t value : bytes) hash = (hash ^ value) * 1099511628211ull;
    return hash;
}

/// @note Hash, comparison, allocation and entry publication are one reentry-protected interval.
class MaterialTransferBusyScope final {
public:
    explicit MaterialTransferBusyScope(bool& busy) : m_busy(busy) { m_busy = true; }
    ~MaterialTransferBusyScope() { m_busy = false; }
private:
    bool& m_busy;
};
} /// @note namespace

MaterialTransferPayloadScope::MaterialTransferPayloadScope(ResourceManager& resources) noexcept
    : m_previous(s_currentMaterialTransfer), m_resources(&resources), m_facade(&resources.m_renderer)
    , m_resourceInstance(resources.GetInstanceId())
{
    if (m_previous) m_depth = m_previous->m_depth < MAX_DEPTH
        ? static_cast<uint8_t>(m_previous->m_depth + 1) : static_cast<uint8_t>(MAX_DEPTH + 1);
    m_eligible = m_depth <= MAX_DEPTH && IsRegisteredOwnedTransferProducer(*m_facade);
    s_currentMaterialTransfer = this;
}

MaterialTransferPayloadScope::~MaterialTransferPayloadScope()
{
    s_currentMaterialTransfer = m_previous;
}

std::shared_ptr<const OwnedConstantBufferData> MaterialTransferPayloadScope::CopyOrReuse(
    ResourceManager& resources, std::span<const uint8_t> bytes)
{
    auto* scope = s_currentMaterialTransfer;
    if (!scope || !scope->m_eligible || scope->m_busy || scope->m_resources != &resources
        || scope->m_resourceInstance != resources.GetInstanceId()
        || !resources.IsMaterialTransferProducer(scope->m_facade)
        || bytes.empty() || bytes.size() > MAX_PAYLOAD_BYTES) return {};
    const MaterialTransferBusyScope busy(scope->m_busy);
    const uint64_t hash = PayloadHash(bytes);
    for (size_t i = 0; i < scope->m_count; ++i) {
        const auto& entry = scope->m_entries[i];
        const auto stored = entry.payload->Bytes();
        if (entry.hash == hash && stored.size() == bytes.size()
            && std::memcmp(stored.data(), bytes.data(), bytes.size()) == 0) return entry.payload;
    }
    if (scope->m_count == MAX_ENTRIES || bytes.size() > MAX_SCOPE_BYTES - scope->m_ownedBytes) return {};
    auto payload = std::make_shared<const OwnedConstantBufferData>(bytes);
    /// @note Hash the owned copy itself; no raw input or entry reference leaves the helper.
    scope->m_entries[scope->m_count] = {PayloadHash(payload->Bytes()), payload};
    ++scope->m_count;
    scope->m_ownedBytes += payload->Bytes().size();
    return payload;
}

} /// @note namespace material_transfer_detail

SurfaceMaterialData Material::ResolveRaySurface(const std::array<uint8_t, 96>& canonicalParams,
    bool standardPbr, uint32_t declaredTextureMask, bool unresolvedTexture, bool advancedLobes,
    ResourceManager& resources) const
{
    auto params = canonicalParams;
    uint32_t mask = 0;
    std::memcpy(&mask, params.data() + 80, sizeof(mask));
    mask |= declaredTextureMask;
    std::array<RayTextureBinding, 5> bindings{};
    for (size_t i = 0; i < textures.size(); ++i) {
        if (!textures[i]) continue;
        if (i >= bindings.size()) { unresolvedTexture = true; continue; }
        mask |= 1u << i;
        const auto* texture = resources.Get(textures[i]);
        if (!texture || !texture->IsRayMaterialTexture()
            || texture->GetBindlessIndex() == INVALID_BINDLESS_INDEX) continue;
        bindings[i] = {textures[i], texture->GetContentVersion()};
    }
    std::memcpy(params.data() + 80, &mask, sizeof(mask));
    return ResolveTexturedSurfaceMaterial(params, standardPbr, bindings, unresolvedTexture, advancedLobes, dielectric);
}

Material::~Material()
{
    ReleaseParamsBuffer(paramsBuffer);
}

Material::Material(Material&& other) noexcept
    : shader(other.shader)
    , textures(std::move(other.textures))
    , paramsBuffer(other.paramsBuffer)
    , paramData(std::move(other.paramData))
    , shaderPath(std::move(other.shaderPath))
    , dielectric(other.dielectric)
{
    other.paramsBuffer = {};
}

Material& Material::operator=(Material&& other) noexcept
{
    if (this == &other) return *this;
    ReleaseParamsBuffer(paramsBuffer);
    shader       = other.shader;
    textures     = std::move(other.textures);
    paramsBuffer = other.paramsBuffer;
    paramData    = std::move(other.paramData);
    shaderPath   = std::move(other.shaderPath);
    dielectric   = other.dielectric;
    other.paramsBuffer = {};
    return *this;
}

Material Material::CloneWithoutGpuResources() const
{
    Material clone;
    clone.shader     = shader;
    clone.textures   = textures;
    clone.paramData  = paramData;
    clone.shaderPath = shaderPath;
    clone.dielectric = dielectric;
    return clone;
}

void Material::Init(ResourceManager& resources, uint32_t cbufferSize, Where where)
{
    /// @note 呼び出し側が CPU 配列を先に組み直すため、容量は GPU 実体で判定する。
    /// @note       DX12 は 256 byte 単位で確保するので、必要量以上なら再利用できる。
    if (paramData.size() != cbufferSize)
        paramData.assign(cbufferSize, 0u);
    const auto* buffer = resources.Get(paramsBuffer);
    if (buffer && cbufferSize > 0 && buffer->GetSize() >= cbufferSize) return;
    if (paramsBuffer.IsValid()) {
        /// @note ハンドルの上書きだけでは旧 ConstantBuffer が ResourceManager に残るため、
        /// @note       シェーダー変更でレイアウトが変わる前に明示的に解放する。
        resources.Release(paramsBuffer);
        paramsBuffer = {};
    }
    if (cbufferSize > 0)
        paramsBuffer = resources.CreateConstantBuffer(cbufferSize, where);
}

void Material::Upload(ResourceManager& resources, const ShaderDescriptor& desc, Where where)
{
    Init(resources, desc.cbufferSize, where);
    if (!paramsBuffer.IsValid()) return;
    if (paramData.size() != desc.cbufferSize) return;

    /// @note textureMask を textures の有効性から計算し paramData に書き込む。
    /// @note       エディターでテクスチャを差し替えても自動反映されるよう毎フレーム再計算する。
    if (desc.textureMaskOffset != UINT32_MAX
        && desc.textureMaskOffset + 4 <= static_cast<uint32_t>(paramData.size()))
    {
        uint32_t mask = 0;
        for (size_t i = 0; i < textures.size() && i < 8; ++i)
            if (textures[i].IsValid()) mask |= (1u << i);
        std::memcpy(paramData.data() + desc.textureMaskOffset, &mask, sizeof(uint32_t));
    }

    /// @note bindless の添字を MaterialConstants へ毎フレーム書き込む。添字はテクスチャの生存に
    /// @note       紐づき、差し替え・再読み込み・ホットリロードで変わるため、焼き込むと前のテクスチャが
    /// @note       貼られたままになる。未割り当てを INVALID で埋めるのは、0 がヒープ先頭の有効な
    /// @note       ディスクリプタで「差していない」と区別できないため。シェーダーは添字の有効判定で分岐する。
    for (const auto& bind : desc.textures) {
        if (bind.constantOffset == UINT32_MAX
            || bind.constantOffset + sizeof(uint32_t) > paramData.size())
            continue;
        uint32_t index = INVALID_BINDLESS_INDEX;
        if (bind.slot < textures.size()) {
            if (const ITexture* texture = resources.Get(textures[bind.slot]))
                index = texture->GetBindlessIndex();
        }
        std::memcpy(paramData.data() + bind.constantOffset, &index, sizeof(uint32_t));
    }

    if (auto payload = material_transfer_detail::MaterialTransferPayloadScope::CopyOrReuse(resources, paramData))
        resources.UpdateOwnedMaterialBytes(paramsBuffer, std::move(payload));
    else
        resources.Update(paramsBuffer, paramData.data(),
                         static_cast<uint32_t>(paramData.size()));
}

} /// @note namespace fbzz::renderer
