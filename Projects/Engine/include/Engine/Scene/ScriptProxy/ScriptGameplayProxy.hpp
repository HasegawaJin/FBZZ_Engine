/// @file    ScriptGameplayProxy.hpp
/// @brief   新しい汎用ComponentをScript DLL境界から基本型だけで操作するProxy。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <span>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptGameplayProxy {
    explicit ScriptGameplayProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    bool SetSprite(std::string_view assetPath) const;
    bool SetSpriteColor(float r, float g, float b, float a) const;

    /// @name Line Renderer
    /// 2 点以上を結ぶ帯状の線。動いている 2 体の間にラインを張り続けるような
    /// 「毎フレーム両端が変わる」用途を想定している。
    /// @note LineRendererComponent は Engine 実装型なので、スクリプトから直接触ると
    ///       DLL 境界を越えた実装依存になる。この Proxy が唯一の口。
    /// @note 描画メッシュは points から毎フレーム自動で再構築される。呼び出し側が
    ///       キャッシュの無効化を気にする必要はない。点が 2 個未満のときは描画されない。
    ///@{
    [[nodiscard]] bool HasLineRenderer() const;
    bool SetLineEnabled(bool enabled) const;
    /// @brief 2 点だけの線を張る。
    /// @param worldSpace true なら引数はワールド座標として扱われ、ライン自身の
    ///        GameObject の Transform に関係なく指定した位置に出る。
    bool SetLine(const math::Vector3& start,
                 const math::Vector3& end,
                 bool worldSpace = true) const;
    /// @brief 3 点以上の折れ線。空や 1 点を渡した場合は線が消えるだけで、エラーにはならない。
    bool SetLinePoints(std::span<const math::Vector3> points, bool worldSpace = true) const;
    /// @brief 始点側と終点側の色 (RGBA)。異極を結ぶ線を赤→青にする、といった用途。
    bool SetLineColors(const math::Vector4& startColor, const math::Vector4& endColor) const;
    bool SetLineWidth(float startWidth, float endWidth) const;
    bool SetLineMaterial(std::string_view materialPath) const;
    ///@}

    bool PlaySpline(bool restart = false) const;
    bool PauseSpline() const;
    bool SetSplinePosition(float normalizedPosition) const;
    bool SetSliderValue(float value) const;
    [[nodiscard]] float GetSliderValue() const;
    bool SetToggle(bool value) const;
    [[nodiscard]] bool GetToggle() const;
    bool SetInputText(std::string_view text) const;
    [[nodiscard]] std::string_view GetInputText() const;
    bool StartCameraShake(float amplitude, float duration, int seed = 1) const;
    bool SetVirtualCameraPriority(int priority) const;
    bool SetAudioMixerSend(std::string_view busName, float level) const;
};

} // namespace fbzz::scene
