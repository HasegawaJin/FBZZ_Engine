// FBZZ Engine
// ScriptMaterialProxy.hpp | fbzz::scene
// Scriptから共有Material参照とGameObject単位overrideを安全に操作する
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
struct ScriptMaterialProxy;
struct UserRenderPassDesc;

// Shader property名をconstexpr hashへ変換する軽量ID。
// nameも保持し、Shader reflection検証と衝突時の照合に使う。
struct MaterialPropertyId {
    uint64_t hash = 0;
    std::string_view name;

    constexpr MaterialPropertyId() = default;
    constexpr explicit MaterialPropertyId(std::string_view propertyName)
        : hash(Hash(propertyName))
        , name(propertyName)
    {
    }

    [[nodiscard]] constexpr bool IsValid() const { return hash != 0 && !name.empty(); }

private:
    [[nodiscard]] static constexpr uint64_t Hash(std::string_view value)
    {
        uint64_t result = 14695981039346656037ull;
        for (const char c : value) {
            result ^= static_cast<uint8_t>(c);
            result *= 1099511628211ull;
        }
        return result;
    }
};

enum class MaterialBlendMode {
    Opaque,
    Alpha,
    Additive,
};

// MaterialComponentをDLL境界へ公開しないopaque runtime handle。
// slot は submesh 番号に対応する。SkinnedMeshRenderer は 1 GameObject で
// モデル全体を描くため、submesh ごとの見た目はこの slot で指定する。
class MaterialInstance {
public:
    MaterialInstance() = default;
    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] bool HasProperty(MaterialPropertyId property) const;

    bool SetFloat(MaterialPropertyId property, float value) const;
    bool SetInt(MaterialPropertyId property, int value) const;
    bool SetVector3(MaterialPropertyId property, const math::Vector3& value) const;
    bool SetVector4(MaterialPropertyId property, const math::Vector4& value) const;
    bool SetColor(MaterialPropertyId property, const math::Vector4& value) const;
    bool SetTexture(MaterialPropertyId property, const TextureRef& texture) const;

    bool TryGetFloat(MaterialPropertyId property, float& value) const;
    bool TryGetInt(MaterialPropertyId property, int& value) const;
    bool TryGetVector3(MaterialPropertyId property, math::Vector3& value) const;
    bool TryGetVector4(MaterialPropertyId property, math::Vector4& value) const;
    bool TryGetColor(MaterialPropertyId property, math::Vector4& value) const;
    bool TryGetTexture(MaterialPropertyId property, TextureRef& texture) const;

    bool ClearOverride(MaterialPropertyId property) const;
    bool ClearAllOverrides() const;
    bool SetBlendMode(MaterialBlendMode blendMode) const;
    bool SetDoubleSided(bool doubleSided) const;
    bool SetRenderQueue(int32_t renderQueue) const;

private:
    friend struct ScriptMaterialProxy;
    enum class PropertyKind { Float, Int, Vector3, Vector4, Texture };
    MaterialInstance(Script* owner, EntityRef target, uint32_t slot)
        : m_script(owner), m_target(target), m_slot(slot) {}
    // 戻り値は MaterialSlot* (DLL 境界へ型を出さないため void*)。
    [[nodiscard]] void* ResolveComponent(bool ensure) const;
    // スロットを所有する MaterialComponent。コンポーネント全体の操作に使う。
    [[nodiscard]] struct MaterialComponent* ResolveOwner(bool ensure) const;
    [[nodiscard]] bool ValidateProperty(MaterialPropertyId property, PropertyKind kind) const;
    [[nodiscard]] const std::string& ResolvePropertyName(void* component,
                                                        MaterialPropertyId property) const;
    Script* m_script = nullptr;
    EntityRef m_target;
    uint32_t m_slot = 0;
};

struct ScriptMaterialProxy {
    Script* script = nullptr;

    [[nodiscard]] MaterialInstance Instance(uint32_t slot = 0) const;
    [[nodiscard]] MaterialInstance Instance(EntityRef target, uint32_t slot = 0) const;

    // 共有.mat参照の割当だけを行う。runtime property変更はInstance()へ分離する。
    bool SetSharedMaterial(const MaterialRef& material, uint32_t slot = 0) const;
    bool SetSharedMaterial(EntityRef target,
                           const MaterialRef& material,
                           uint32_t slot = 0) const;

    // ── 共有 .mat アセットの読み取り (書き込みは提供しない) ────────────────
    // 差し替え候補の .mat に書かれている値を、実際に適用する前に参照するためのもの。
    // 例: 「被弾マテリアルの発光色を読んで、その色でヒットエフェクトを出す」。
    //
    // WHY 書き込み版が無いか: .mat は参照する全 GameObject が共有する実体で、
    //     ランタイムに書き換えると 1 体だけ光らせたい演出が全体へ波及する。
    //     さらに変更は AssetManager 上のメモリにしか残らず Play 停止でも戻らないため、
    //     エディタセッションを汚染する。オブジェクト単位の変更は Instance() を使うこと。
    [[nodiscard]] bool HasSharedProperty(const MaterialRef& material,
                                         MaterialPropertyId property) const;
    [[nodiscard]] bool TryGetSharedFloat(const MaterialRef& material,
                                         MaterialPropertyId property, float& value) const;
    [[nodiscard]] bool TryGetSharedVector3(const MaterialRef& material,
                                           MaterialPropertyId property, math::Vector3& value) const;
    [[nodiscard]] bool TryGetSharedVector4(const MaterialRef& material,
                                           MaterialPropertyId property, math::Vector4& value) const;
    [[nodiscard]] bool TryGetSharedColor(const MaterialRef& material,
                                         MaterialPropertyId property, math::Vector4& value) const;
    [[nodiscard]] bool TryGetSharedTexture(const MaterialRef& material,
                                           MaterialPropertyId property, TextureRef& texture) const;

    bool EnsureMaterial(std::string_view materialPath) const;
    bool HasParam(std::string_view param) const;
    bool SetFloat(std::string_view param, float value) const;
    bool SetInt(std::string_view param, int value) const;
    bool SetVector3(std::string_view param, const math::Vector3& value) const;
    bool SetVector4(std::string_view param, const math::Vector4& value) const;
    bool SetTexture(std::string_view slot, std::string_view texturePath) const;
    float GetFloat(std::string_view param) const;
    int GetInt(std::string_view param) const;
    math::Vector3 GetVector3(std::string_view param) const;
    math::Vector4 GetVector4(std::string_view param) const;
    // コンポーネント全体の有効/無効。
    bool SetEnabled(bool enabled) const;
    [[nodiscard]] bool IsEnabled() const;
    // submesh (スロット) 単位の表示切替。
    // WHY: SkinnedMeshRenderer が 1 GameObject = モデル全体を描くようになったため、
    //      「装備の一部だけ隠す」といった操作はスロット番号で行う。
    bool SetSlotVisible(uint32_t slot, bool visible) const;
    [[nodiscard]] bool IsSlotVisible(uint32_t slot) const;
    // 指定 submesh だけを表示する。slot < 0 で全 submesh を表示。
    bool SetOnlyVisibleSlot(int slot) const;
    bool SetBlendMode(MaterialBlendMode blendMode) const;
    bool SetDoubleSided(bool doubleSided) const;
    bool SetRenderQueue(int32_t renderQueue) const;
    // 上書きが無ければ共有 .mat の値を返す (実際に描画へ使われる値)。
    [[nodiscard]] bool    IsDoubleSided() const;
    [[nodiscard]] int32_t GetRenderQueue() const;
    // 割り当てられている共有 .mat のパス。未割り当てなら空文字列。
    [[nodiscard]] std::string GetSharedMaterialPath(uint32_t slot = 0) const;
    // MaterialComponent が持つ submesh スロット数。
    [[nodiscard]] uint32_t GetSlotCount() const;

    void QueueRenderPass(UserRenderPassDesc desc) const;
};

} // namespace fbzz::scene
