// FBZZ Engine
// MaterialComponent.hpp | fbzz::scene
// GameObject に割り当てるマテリアル設定と、シェーダー反映用パラメータを保持する。
#pragma once

#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::renderer {
class Material;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MaterialComponent {
    std::shared_ptr<renderer::Material> material;
    bool        enabled    = true;
    std::string shaderPath;

    // ── 描画状態 ──────────────────────────────────────────────────────────────
    // WHY: Unity/Unreal と同様にブレンドモード・カリング・描画優先度をマテリアルが持つ。
    //      RenderSystem はこれらをもとに PipelineState を動的に選択する。

    // アルファブレンド方式。ALPHA_BLEND / ADDITIVE は半透明パスで後から描画される。
    renderer::BlendMode blendMode   = renderer::BlendMode::OPAQUE_BLEND;

    // true にすると背面カリングを無効化し、両面描画になる (草・布・薄い板など)。
    bool                doubleSided = false;

    // 描画キュー。値が小さいほど先に描画される。
    // WHAT: Unity と同じ基準で 2000=Geometry、2450=AlphaTest、3000=Transparent、4000=Overlay。
    // WHY: 同一 RenderLayer 内で水面 → VFX → UI のような順序制御に使う。
    int32_t             renderQueue = renderer::RenderQueue::GEOMETRY;

    // CB_MATERIAL と同じサイズ・同じレイアウトの生バイト列。
    // WHY: シェーダーごとの定数バッファ差分を ShaderDescriptor に閉じ、RenderSystem はそのまま GPU へ転送できる。
    std::vector<uint8_t> paramData;

    // マテリアルテクスチャパス。slot 番号を index として保持する。
    // texturePaths[0]=albedo, [1]=normal, [2]=metallicRough, [3]=emissive, [4]=ao
    // WHAT: 未使用 slot は空文字にして、保存形式と Inspector 表示を単純に保つ。
    std::vector<std::string> texturePaths;

    // 型安全なアクセサ。offset は ShaderDescriptor が決める CB 内バイト位置。
    template<typename T>
    T GetParam(uint32_t offset) const
    {
        T v{};
        if (offset + sizeof(T) <= paramData.size())
            std::memcpy(&v, paramData.data() + offset, sizeof(T));
        return v;
    }

    // 型安全なアクセサ。offset は ShaderDescriptor が決める CB 内バイト位置。
    template<typename T>
    void SetParam(uint32_t offset, const T& v)
    {
        if (offset + sizeof(T) <= paramData.size())
            std::memcpy(paramData.data() + offset, &v, sizeof(T));
    }

    // 名前引きアクセサ (ShaderDescriptor 明示版)
    template<typename T>
    T GetParam(std::string_view name, const renderer::ShaderDescriptor& desc) const
    {
        if (const auto* v = desc.FindVar(name))
            return GetParam<T>(v->offset);
        return T{};
    }

    template<typename T>
    bool SetParam(std::string_view name, const T& val, const renderer::ShaderDescriptor& desc)
    {
        if (const auto* v = desc.FindVar(name)) {
            SetParam<T>(v->offset, val);
            return true;
        }
        return false;
    }

    // Descriptor に合わせて paramData と texturePaths を初期化する。
    // WHY: シェーダー切り替え時に古いレイアウトのデータを残すと、GPU 側の解釈がずれるため。
    void InitFromDescriptor(const renderer::ShaderDescriptor& desc)
    {
        paramData.assign(desc.cbufferSize, 0u);
        const uint32_t slotCount = desc.textures.empty() ? 0u
            : desc.textures.back().slot + 1u;
        texturePaths.resize((std::max)(slotCount, 5u));

        // WHY: 新しいシェーダーへ切り替えた直後に全パラメータが 0 のままだと、
        //      albedo/rimColor/rimIntensity まで 0 になり、正しいシェーダーでも真っ黒に見える。
        //      Descriptor 名で汎用初期値を書き込むことで、Inspector や Script から調整する前でも
        //      マテリアルの意図が確認できる状態にする。
        auto setFloat = [&](std::string_view name, float value) {
            const auto* v = desc.FindVar(name);
            if (!v || v->varType != renderer::ShaderVarType::Float || v->columns != 1) return;
            if (v->offset + sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
            std::memcpy(paramData.data() + v->offset, &value, sizeof(float));
        };
        auto setFloat3 = [&](std::string_view name, const float value[3]) {
            const auto* v = desc.FindVar(name);
            if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 3) return;
            if (v->offset + 3u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
            std::memcpy(paramData.data() + v->offset, value, 3u * sizeof(float));
        };
        auto setFloat4 = [&](std::string_view name, const float value[4]) {
            const auto* v = desc.FindVar(name);
            if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 4) return;
            if (v->offset + 4u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
            std::memcpy(paramData.data() + v->offset, value, 4u * sizeof(float));
        };

        const float defaultAlbedo[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const float defaultColor[3]  = { 1.0f, 1.0f, 1.0f };
        setFloat4("albedo", defaultAlbedo);
        setFloat("metallic", 0.0f);
        setFloat("roughness", 0.65f);
        setFloat("normalStrength", 1.0f);
        setFloat("occlusionStrength", 1.0f);
        setFloat3("emissiveColor", defaultColor);
        setFloat("emissiveScale", 0.0f);
        setFloat("alphaCutoff", 0.5f);
        setFloat("rimPower", 3.0f);
        setFloat("rimIntensity", 1.0f);
        setFloat3("rimColor", defaultColor);
    }

    const char* GetTypeName() const { return "Material"; }

    // シリアライズ用。paramData は SceneSerializer が Descriptor と合わせて直接扱う。
    void Reflect(IReflector& r)
    {
        r.Field("enabled",    enabled);
        r.Field("shaderPath", shaderPath);
    }
};

} // namespace fbzz::scene
