/// @file    ScriptGameplayProxy.hpp
/// @brief   汎用 Component の native 操作をまとめ、検証済み managed dispatcher から再利用する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstddef>
#include <span>
#include <string_view>

namespace fbzz::scene {

class Script;
enum class LineSpace : int;
enum class LineShape : int;

struct ScriptGameplayProxy {
    explicit ScriptGameplayProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    bool SetSprite(std::string_view assetPath) const;
    bool SetSpriteColor(float r, float g, float b, float a) const;

    /// @name Line Renderer
    /// @note managed facade は実 row の寿命と scope を dispatcher で検証し、native 操作と row の query を組み合わせる。
    /// @note PresentationSystem が全入力 snapshot を比較するため、呼び出し側で cache を無効化しない。2 点未満では描画しない。
    /// @note Point queries read the runtime-selected values during Play and current authoring values during Edit.
    ///@{
    [[nodiscard]] bool HasLineRenderer() const;
    /// @return 未存在または無効な入力なら 0。POINT_OBJECTS の点数も返す。
    [[nodiscard]] size_t GetLinePointCount() const;
    /// @return 未存在・範囲外なら false と出力未変更。座標は現在の LineSpace。
    [[nodiscard]] bool TryGetLinePoint(size_t index, math::Vector3& point) const;
    bool SetLineEnabled(bool enabled) const;
    bool SetLineLoop(bool loop) const;
    bool SetLineBillboard(bool billboard) const;
    /// @return 未存在・定義外 enum なら false と row 未変更。
    bool SetLineSpace(LineSpace space) const;
    bool SetLineShape(LineShape shape) const;
    /// @return 未存在・3..32 の範囲外なら false と row 未変更。
    bool SetLineRadialSegments(int segments) const;
    bool SetLineSorting(int layer, int order) const;
    /// @brief 2 点だけの線を張る。
    /// @param worldSpace true なら引数と幅を world の m として扱う。false は local の m。
    bool SetLine(const math::Vector3& start,
                 const math::Vector3& end,
                 bool worldSpace = true) const;
    /// @brief Numeric point update. Empty or one-point input hides the line; larger inputs replace the selected source.
    /// @note Writes select INLINE+LIVE and retain inactive target bindings and bake metadata.
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

} /// @note namespace fbzz::scene
