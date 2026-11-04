/// @file    ScriptGameplayProxy.hpp
/// @brief   汎用 Component の native 操作をまとめ、検証済み managed dispatcher から再利用する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Engine/Api.hpp>

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

    FBZZ_ENGINE_API bool SetSprite(std::string_view assetPath) const;
    FBZZ_ENGINE_API bool SetSpriteColor(float r, float g, float b, float a) const;

    /// @name Line Renderer
    /// @note managed facade は実 row の寿命と scope を dispatcher で検証し、native 操作と row の query を組み合わせる。
    /// @note PresentationSystem が全入力 snapshot を比較するため、呼び出し側で cache を無効化しない。2 点未満では描画しない。
    /// @note Point queries read the runtime-selected values during Play and current authoring values during Edit.
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API bool HasLineRenderer() const;
    /// @return 未存在または無効な入力なら 0。POINT_OBJECTS の点数も返す。
    [[nodiscard]] FBZZ_ENGINE_API size_t GetLinePointCount() const;
    /// @return 未存在・範囲外なら false と出力未変更。座標は現在の LineSpace。
    [[nodiscard]] FBZZ_ENGINE_API bool TryGetLinePoint(size_t index, math::Vector3& point) const;
    FBZZ_ENGINE_API bool SetLineEnabled(bool enabled) const;
    FBZZ_ENGINE_API bool SetLineLoop(bool loop) const;
    FBZZ_ENGINE_API bool SetLineBillboard(bool billboard) const;
    /// @return 未存在・定義外 enum なら false と row 未変更。
    FBZZ_ENGINE_API bool SetLineSpace(LineSpace space) const;
    FBZZ_ENGINE_API bool SetLineShape(LineShape shape) const;
    /// @return 未存在・3..32 の範囲外なら false と row 未変更。
    FBZZ_ENGINE_API bool SetLineRadialSegments(int segments) const;
    FBZZ_ENGINE_API bool SetLineSorting(int layer, int order) const;
    /// @brief 2 点だけの線を張る。
    /// @param worldSpace true なら引数と幅を world の m として扱う。false は local の m。
    FBZZ_ENGINE_API bool SetLine(const math::Vector3& start,
                 const math::Vector3& end,
                 bool worldSpace = true) const;
    /// @brief Numeric point update. Empty or one-point input hides the line; larger inputs replace the selected source.
    /// @note Writes select INLINE+LIVE and retain inactive target bindings and bake metadata.
    FBZZ_ENGINE_API bool SetLinePoints(std::span<const math::Vector3> points, bool worldSpace = true) const;
    /// @brief 始点側と終点側の色 (RGBA)。異極を結ぶ線を赤→青にする、といった用途。
    FBZZ_ENGINE_API bool SetLineColors(const math::Vector4& startColor, const math::Vector4& endColor) const;
    FBZZ_ENGINE_API bool SetLineWidth(float startWidth, float endWidth) const;
    FBZZ_ENGINE_API bool SetLineMaterial(std::string_view materialPath) const;
    /// @}

    FBZZ_ENGINE_API bool PlaySpline(bool restart = false) const;
    FBZZ_ENGINE_API bool PauseSpline() const;
    FBZZ_ENGINE_API bool SetSplinePosition(float normalizedPosition) const;
    FBZZ_ENGINE_API bool SetSliderValue(float value) const;
    [[nodiscard]] FBZZ_ENGINE_API float GetSliderValue() const;
    FBZZ_ENGINE_API bool SetToggle(bool value) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetToggle() const;
    FBZZ_ENGINE_API bool SetInputText(std::string_view text) const;
    [[nodiscard]] FBZZ_ENGINE_API std::string_view GetInputText() const;
    FBZZ_ENGINE_API bool StartCameraShake(float amplitude, float duration, int seed = 1) const;
    FBZZ_ENGINE_API bool SetVirtualCameraPriority(int priority) const;
    FBZZ_ENGINE_API bool SetAudioMixerSend(std::string_view busName, float level) const;
};

} /// @note namespace fbzz::scene
