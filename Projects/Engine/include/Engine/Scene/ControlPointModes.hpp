/// @file    ControlPointModes.hpp
/// @brief   領域別アダプターで共有する ControlPoint の制作時入力元とランタイムポリシー。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>

namespace fbzz::scene {

struct IReflector;

/// @brief ControlPoint を使うコンポーネントの位置の入力元。
enum class ControlPointSourceMode : std::uint8_t {
    INLINE = 0,
    POINT_OBJECTS = 1,
};

/// @brief 現在の参照先を追うか、数値ベイクを保持するかを決めるランタイムポリシー。
enum class ControlPointRuntimeMode : std::uint8_t {
    LIVE = 0,
    BAKED_STATIC = 1,
};

/// @brief 制作時の検証または検証済みランタイムデータの利用を選ぶ処理コンテキスト。
enum class ControlPointReadContext : std::uint8_t {
    AUTHORING,
    RUNTIME,
};

/// @brief 未対応の保存値を保持して入力元モードを反射する。
FBZZ_ENGINE_API void ReflectControlPointSourceMode(IReflector& reflector, const char* fieldName,
    ControlPointSourceMode& mode);

/// @brief 未対応の保存値を保持してランタイムモードを反射する。
FBZZ_ENGINE_API void ReflectControlPointRuntimeMode(IReflector& reflector, const char* fieldName,
    ControlPointRuntimeMode& mode);

/// @brief 64 ビットの正規化入力署名を対応する文字列コーデックで反射する。
FBZZ_ENGINE_API void ReflectControlPointSignature(IReflector& reflector, const char* fieldName,
    std::uint64_t& signature);

/// @brief 非負のアダプター版を対応する整数コーデックで反射する。
FBZZ_ENGINE_API void ReflectControlPointVersion(IReflector& reflector, const char* fieldName,
    std::uint32_t& version);

}
