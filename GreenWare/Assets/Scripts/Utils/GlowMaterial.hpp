/// @file    GlowMaterial.hpp
/// @brief   発光パーツ材質 (M_GlowPart) への書き込み口。アタッチしないユーティリティ
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note キャラクターの光る部位はすべて Assets/Materials/Character/M_GlowPart.mat を共有し、
///       «何色でどれだけ光るか» だけを GameObject 単位の override で決める。プロパティ名は
///       シェーダー側の変数名 (`.mat` 内の別名ではなく SkinnedGlowPart.hlsl の cbuffer) と
///       一致しないと MaterialInstance が黙って書き込みを捨てる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace sandbox {

/// SkinnedGlowPart.hlsl / SkinnedPBR.hlsl 共通の自発光パラメーター。
///
/// @note 完全修飾する。アタッチしないユーティリティは using namespace を持たない
///       (取り込んだ側の名前解決を汚さないため)。BladeColors.hpp と同じ方針。
inline constexpr fbzz::scene::MaterialPropertyId kEmissiveColorId{ "emissiveColor" };
inline constexpr fbzz::scene::MaterialPropertyId kEmissiveScaleId{ "emissiveScale" };

/// 分割された描画メッシュの発光スロットを保持する。共有材質は変更しない。
class BossGlowMaterials {
public:
    void Reset() { m_slots.clear(); m_collected = false; }
    void Apply(fbzz::scene::GameObject* root, const fbzz::scene::ScriptMaterialProxy& proxy,
               const fbzz::math::Vector4& color, float strength, float seconds,
               const char* filter = "", const char* meshFilter = "", float breathDepth = 0.12f)
    {
        if (!root) return;
        if (!m_collected) { Collect(*root, proxy); m_collected = true; }
        for (std::size_t i = 0; i < m_slots.size(); ++i) {
            const auto& slot = m_slots[i];
            if (slot.path.find(filter) == std::string::npos ||
                slot.meshName.find(meshFilter) == std::string::npos) continue;
            /// @note 撃破時の差し替え後は消失シェーダーが色と強度を所有する。
            if (proxy.GetSharedMaterialPath(slot.target, slot.index) != slot.path) continue;
            const auto instance = proxy.Instance(slot.target, slot.index);
            if (!instance.IsValid()) continue;
            const float breath = 1.0f - breathDepth + breathDepth *
                std::sin(seconds * 2.8f - static_cast<float>(i) * 0.45f);
            instance.SetVector3(kEmissiveColorId, {color.x, color.y, color.z});
            instance.SetFloat(kEmissiveScaleId, strength * breath);
        }
    }
private:
    struct Slot { fbzz::scene::EntityRef target; uint32_t index; std::string path; std::string meshName; };
    std::vector<Slot> m_slots;
    bool m_collected = false;
    void Collect(fbzz::scene::GameObject& object, const fbzz::scene::ScriptMaterialProxy& proxy)
    {
        if (const auto* mesh = object.GetComponent<fbzz::scene::SkinnedMeshRenderer>()) {
            const fbzz::scene::EntityRef ref{object.GetID()};
            const uint32_t count = mesh->SubmeshCount() > 0 ? static_cast<uint32_t>(mesh->SubmeshCount()) : 1u;
            for (uint32_t i = 0; i < count; ++i) {
                const auto path = proxy.GetSharedMaterialPath(ref, i);
                if (path.find("Glow") != std::string::npos || path.find("Emission") != std::string::npos)
                    m_slots.push_back({ref, i, path, object.name});
            }
        }
        for (int i = 0; i < object.GetChildCount(); ++i)
            if (auto* child = object.GetChild(i)) Collect(*child, proxy);
    }
};

/// 装甲の基準色を保存し、熱・冷却・被弾をインスタンスへ重ねる。
class BossArmorMaterials {
public:
    void Reset() { m_slots.clear(); m_collected = false; m_heat = 0.0f; m_cooling = 0.0f; }
    void Apply(fbzz::scene::GameObject* root, const fbzz::scene::ScriptMaterialProxy& proxy,
               float heat, bool cooling, float hit, float dt)
    {
        if (!root) return;
        if (!m_collected) { Collect(*root, proxy); m_collected = true; }
        const float blend = 1.0f - std::exp(-9.0f * std::max(dt, 0.0f));
        m_heat += (std::clamp(heat, 0.0f, 1.0f) - m_heat) * blend;
        m_cooling += ((cooling ? 1.0f : 0.0f) - m_cooling) * blend;
        hit = std::clamp(hit, 0.0f, 1.0f);
        for (const auto& slot : m_slots) {
            /// @note 消失材質への切り替え後は死亡演出へ所有権を渡す。
            if (proxy.GetSharedMaterialPath(slot.target, slot.index) != slot.path) continue;
            const auto instance = proxy.Instance(slot.target, slot.index);
            if (!instance.IsValid()) continue;
            auto color = slot.color;
            const float hot = m_heat * (1.0f - m_cooling);
            color.x *= 1.0f - 0.18f * m_cooling;
            color.y *= 1.0f - 0.20f * hot;
            color.z *= 1.0f - 0.38f * hot;
            color.x += (1.0f - color.x) * hit * 0.38f;
            color.y += (1.0f - color.y) * hit * 0.38f;
            color.z += (1.0f - color.z) * hit * 0.38f;
            instance.SetColor(kAlbedo, color);
            if (slot.hasRoughness)
                instance.SetFloat(kRoughness, std::clamp(slot.roughness *
                    (1.0f - 0.35f * hot + 0.25f * m_cooling - 0.15f * hit), 0.08f, 1.0f));
        }
    }
private:
    inline static constexpr fbzz::scene::MaterialPropertyId kAlbedo{"albedo"};
    inline static constexpr fbzz::scene::MaterialPropertyId kRoughness{"roughness"};
    struct Slot {
        fbzz::scene::EntityRef target;
        uint32_t index;
        std::string path;
        fbzz::math::Vector4 color;
        float roughness;
        bool hasRoughness;
    };
    std::vector<Slot> m_slots;
    bool m_collected = false;
    float m_heat = 0.0f;
    float m_cooling = 0.0f;
    void Collect(fbzz::scene::GameObject& object, const fbzz::scene::ScriptMaterialProxy& proxy)
    {
        if (const auto* mesh = object.GetComponent<fbzz::scene::SkinnedMeshRenderer>()) {
            const fbzz::scene::EntityRef ref{object.GetID()};
            const uint32_t count = mesh->SubmeshCount() > 0 ? static_cast<uint32_t>(mesh->SubmeshCount()) : 1u;
            for (uint32_t i = 0; i < count; ++i) {
                const auto path = proxy.GetSharedMaterialPath(ref, i);
                if (path.find("Glow") != std::string::npos || path.find("Emission") != std::string::npos) continue;
                const auto instance = proxy.Instance(ref, i);
                if (!instance.IsValid() || instance.HasProperty(fbzz::scene::MaterialPropertyId{"dissolveAmount"})) continue;
                fbzz::math::Vector4 color;
                if (!instance.TryGetColor(kAlbedo, color)) continue;
                float roughness = 0.5f;
                const bool hasRoughness = instance.TryGetFloat(kRoughness, roughness);
                m_slots.push_back({ref, i, path, color, roughness, hasRoughness});
            }
        }
        for (int i = 0; i < object.GetChildCount(); ++i)
            if (auto* child = object.GetChild(i)) Collect(*child, proxy);
    }
};

} // namespace sandbox
