// FBZZ Engine
// ScriptMaterialProxy.hpp | fbzz::scene
// Script から MaterialComponent と RenderPass を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::renderer {
struct ShaderDescriptor;
}

namespace fbzz::scene {

struct MaterialComponent;
class Script;
struct UserRenderPassDesc;

struct ScriptMaterialProxy {
    Script* script = nullptr;

    // Get: 自 GameObject の MaterialComponent を取得する。存在しない場合は nullptr。
    MaterialComponent* Get() const;

    // Ensure: 自 GameObject に MaterialComponent がなければ追加し、その参照を返す。
    // WHY: カスタムマテリアルは Script から動的に付け替える用途が多いため、呼び出し側の定型処理を減らす。
    MaterialComponent* Ensure() const;

    // SetShader: 任意 HLSL を MaterialComponent に割り当てる。
    // WHAT: resetParameters=true のとき ShaderDescriptor に合わせて paramData / texturePaths を再初期化する。
    bool SetShader(std::string_view shaderPath, bool resetParameters = true) const;

    // EnsureCustomMaterial: MaterialComponent を確保し、指定 shader が未設定なら割り当てる。
    // WHY: OnStart と OnUpdate のどちらから呼んでも同じカスタムマテリアル状態に収束させる。
    bool EnsureCustomMaterial(std::string_view shaderPath, bool resetParameters = true) const;

    // HasParam: 現在の shader が指定名の MaterialConstants 変数を持つか確認する。
    bool HasParam(std::string_view param) const;

    void SetFloat(std::string_view param, float v) const;
    void SetInt(std::string_view param, int v) const;
    void SetVector3(std::string_view param, const math::Vector3& v) const;
    void SetVector4(std::string_view param, const math::Vector4& v) const;
    void SetTexture(std::string_view slot, std::string_view texPath) const;

    // SetEnabled: MaterialComponent の描画有効状態を切り替える。
    bool SetEnabled(bool enabled) const;

    // SetBlendMode: 不透明 / アルファブレンド / 加算合成を切り替える。
    bool SetBlendMode(renderer::BlendMode blendMode) const;

    // SetDoubleSided: 背面カリングを無効化するかを切り替える。
    bool SetDoubleSided(bool doubleSided) const;

    // SetRenderQueue: 描画順を直接指定する。標準値は renderer::RenderQueue を使う。
    bool SetRenderQueue(int32_t renderQueue) const;

    void QueueRenderPass(UserRenderPassDesc desc) const;
    const renderer::ShaderDescriptor* GetShaderDescriptor(std::string_view path) const;
};

} // namespace fbzz::scene
