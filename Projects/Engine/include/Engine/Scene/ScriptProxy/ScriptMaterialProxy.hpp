/// @file    ScriptMaterialProxy.hpp
/// @brief   Scriptから共有Material参照とGameObject単位overrideを安全に操作する。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector2.hpp>
#include <Math/Matrix4.hpp>
#include <span>
#include <vector>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
struct ScriptMaterialProxy;
struct UserRenderPassDesc;

/// @brief Shader property 名を constexpr hash へ変換する軽量 ID。
/// @note name も保持し、Shader reflection 検証と衝突時の照合に使う。
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

/// @brief MaterialComponent を DLL 境界へ公開しない opaque runtime handle。
/// @note slot は submesh 番号に対応する。SkinnedMeshRenderer は 1 GameObject で
/// @note モデル全体を描くため、submesh ごとの見た目はこの slot で指定する。
class MaterialInstance {
public:
    MaterialInstance() = default;
    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] bool HasProperty(MaterialPropertyId property) const;

    bool SetFloat(MaterialPropertyId property, float value) const;
    bool SetInt(MaterialPropertyId property, int value) const;
    bool SetUInt(MaterialPropertyId property, uint32_t value) const;
    bool SetBool(MaterialPropertyId property, bool value) const;
    bool SetVector2(MaterialPropertyId property, const math::Vector2& value) const;
    bool SetMatrix(MaterialPropertyId property, const math::Matrix4& value) const;
    /// @note 配列・任意サイズの行列・整数ベクトル用。要素順は配列順、行列は行優先でパディング不要。
    /// @return Reflection 未取得・型/要素数/範囲不一致は false。既存値は変更しない。
    bool SetValues(MaterialPropertyId property, std::span<const double> values) const;
    bool SetVector3(MaterialPropertyId property, const math::Vector3& value) const;
    bool SetVector4(MaterialPropertyId property, const math::Vector4& value) const;
    bool SetColor(MaterialPropertyId property, const math::Vector4& value) const;
    bool SetTexture(MaterialPropertyId property, const TextureRef& texture) const;

    bool TryGetFloat(MaterialPropertyId property, float& value) const;
    bool TryGetInt(MaterialPropertyId property, int& value) const;
    bool TryGetUInt(MaterialPropertyId property, uint32_t& value) const;
    bool TryGetBool(MaterialPropertyId property, bool& value) const;
    bool TryGetVector2(MaterialPropertyId property, math::Vector2& value) const;
    bool TryGetMatrix(MaterialPropertyId property, math::Matrix4& value) const;
    /// @return 未設定・型/要素数不一致は false。出力は未変更。
    bool TryGetValues(MaterialPropertyId property, std::vector<double>& values) const;
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
    enum class PropertyKind { FLOAT, INT, VECTOR3, VECTOR4, TEXTURE, UINT, BOOL, VECTOR2, MATRIX };
    MaterialInstance(Script* owner, EntityRef target, uint32_t slot)
        : m_script(owner), m_target(target), m_slot(slot) {}
    /// @return MaterialSlot* (DLL 境界へ型を出さないため void*)。
    [[nodiscard]] void* ResolveComponent(bool ensure) const;
    /// @return スロットを所有する MaterialComponent。コンポーネント全体の操作に使う。
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

    /// @brief 共有 .mat 参照の割当だけを行う。runtime property 変更は Instance() へ分離する。
    bool SetSharedMaterial(const MaterialRef& material, uint32_t slot = 0) const;
    bool SetSharedMaterial(EntityRef target,
                           const MaterialRef& material,
                           uint32_t slot = 0) const;

    /// @name 共有 .mat アセットの読み取り (書き込みは提供しない)
    /// @note 差し替え候補の .mat に書かれている値を、実際に適用する前に参照するためのもの。
    /// @note 例: 「被弾マテリアルの発光色を読んで、その色でヒットエフェクトを出す」。
    /// @note .mat は参照する全 GameObject が共有する実体。ランタイムに書き換えると
    /// @note 1 体だけ光らせたい演出が全体へ波及し、変更は AssetManager 上のメモリにしか
    /// @note 残らず Play 停止でも戻らないためエディタセッションを汚染する。オブジェクト
    /// @note 単位の変更は Instance() を使うこと。
    ///@{
    [[nodiscard]] bool HasSharedProperty(const MaterialRef& material,
                                         MaterialPropertyId property) const;
    /// @note 配列・行列・整数値を保存時の要素順で読む。未存在は false、出力は未変更。
    [[nodiscard]] bool TryGetSharedValues(const MaterialRef& material,
                                          MaterialPropertyId property, std::vector<double>& values) const;
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
    ///@}

    bool EnsureMaterial(std::string_view materialPath) const;
    bool HasParam(std::string_view param) const;
    bool SetFloat(std::string_view param, float value) const;
    bool SetInt(std::string_view param, int value) const;
    bool SetUInt(std::string_view param, uint32_t value) const;
    bool SetBool(std::string_view param, bool value) const;
    bool SetVector2(std::string_view param, const math::Vector2& value) const;
    bool SetMatrix(std::string_view param, const math::Matrix4& value) const;
    bool SetValues(std::string_view param, std::span<const double> values) const;
    bool SetVector3(std::string_view param, const math::Vector3& value) const;
    bool SetVector4(std::string_view param, const math::Vector4& value) const;
    bool SetTexture(std::string_view slot, std::string_view texturePath) const;
    float GetFloat(std::string_view param) const;
    int GetInt(std::string_view param) const;
    math::Vector3 GetVector3(std::string_view param) const;
    math::Vector4 GetVector4(std::string_view param) const;
    /// @brief コンポーネント全体の有効/無効。
    bool SetEnabled(bool enabled) const;
    [[nodiscard]] bool IsEnabled() const;
    /// @brief submesh (スロット) 単位の表示切替。
    /// @note SkinnedMeshRenderer は 1 GameObject でモデル全体を描くため、
    /// @note 「装備の一部だけ隠す」といった操作はスロット番号で行う。
    bool SetSlotVisible(uint32_t slot, bool visible) const;
    [[nodiscard]] bool IsSlotVisible(uint32_t slot) const;
    /// @brief 指定 submesh だけを表示する。slot < 0 で全 submesh を表示。
    bool SetOnlyVisibleSlot(int slot) const;
    bool SetBlendMode(MaterialBlendMode blendMode) const;
    bool SetDoubleSided(bool doubleSided) const;
    bool SetRenderQueue(int32_t renderQueue) const;
    /// @note 上書きが無ければ共有 .mat の値を返す (実際に描画へ使われる値)。
    [[nodiscard]] bool    IsDoubleSided() const;
    [[nodiscard]] int32_t GetRenderQueue() const;
    /// @return 割り当てられている共有 .mat のパス。未割り当てなら空文字列。
    [[nodiscard]] std::string GetSharedMaterialPath(uint32_t slot = 0) const;
    /// @return 別 GameObject のスロットに割り当てられている共有 .mat のパス。
    /// @note 部位ごとに GameObject が分かれたキャラクターで、親のスクリプトが
    /// @note 「今どの材質で描かれているか」を見て差し替え先を決める用途。
    [[nodiscard]] std::string GetSharedMaterialPath(EntityRef target, uint32_t slot) const;
    /// @return MaterialComponent が持つ submesh スロット数。
    [[nodiscard]] uint32_t GetSlotCount() const;

    void QueueRenderPass(UserRenderPassDesc desc) const;
};

} // namespace fbzz::scene
