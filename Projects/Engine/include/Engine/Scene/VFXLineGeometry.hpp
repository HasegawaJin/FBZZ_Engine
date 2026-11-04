/// @file    VFXLineGeometry.hpp
/// @brief   VFX Line の形 (雷の本流と枝・ビームの曲線) と明るさを作る純関数
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note 同じ関数を VFXLineSystem (シーン) と Inspector のプレビューが呼ぶため描画から切り離す。形が決定論的 (同じ seed なら同じ雷) なのでテストでも固定できる。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::scene {

/// @note 帯 1 本ぶんの折れ線。
struct VFXLineStrand {
    std::vector<math::Vector3> points;
    /// @note 幅の倍率 (本流 = 1)。
    float width = 1.0f;
    float brightness = 1.0f;
    /// @note 始点側・終点側の幅 (width に掛ける)。枝は先端で 0 まで細る。
    float startTaper = 1.0f;
    float endTaper = 1.0f;
};

/// @note from → to の雷を作る。strikeSeed が同じなら同じ形。jitterTime は打ち直しの間の震えの時刻。
/// @note 本流の最初と最後の点は必ず from / to と一致する (端が電極や手から離れない)。
FBZZ_ENGINE_API void GenerateLightning(const VFXLineComponent& line, const math::Vector3& from, const math::Vector3& to,
                       std::uint32_t strikeSeed, float jitterTime, std::vector<VFXLineStrand>& out);

/// @note from → to のビーム (たるみ・横ゆれ込み)。両端は動かない。
FBZZ_ENGINE_API void GenerateBeam(const VFXLineComponent& line, const math::Vector3& from, const math::Vector3& to, float time,
                  std::vector<VFXLineStrand>& out);

/// @note 出入り (fadeIn / duration / fadeOut / loop) の明るさ [0,1]。
[[nodiscard]] FBZZ_ENGINE_API float VFXLineEnvelope(const VFXLineComponent& line, float time);

/// @note 明滅を含めた明るさの倍率 [0,1]。雷は打った直後が最も明るく、打つたびに強さが揺れる。
[[nodiscard]] FBZZ_ENGINE_API float VFXLineBrightness(const VFXLineComponent& line, float time, std::uint32_t strikeIndex,
                                      float strikeClock);

/// @note プリセットの値を入れる (端点・マテリアル・seed には触らない)。
FBZZ_ENGINE_API void ApplyVFXLinePreset(VFXLineComponent& line, VFXLinePreset preset);
[[nodiscard]] FBZZ_ENGINE_API const char* VFXLinePresetName(VFXLinePreset preset);

} /// @note namespace fbzz::scene
