/// @file    GlowMaterial.hpp
/// @brief   発光パーツ材質 (M_GlowPart) への書き込み口。アタッチしないユーティリティ
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// WHY 1 箇所に集めるか:
///   キャラクターの光る部位はすべて Assets/Materials/Character/M_GlowPart.mat を共有し、
///   «何色でどれだけ光るか» だけを GameObject 単位の override で決める。
///   プロパティ名はシェーダー側の変数名と一致していないと MaterialInstance が黙って
///   書き込みを捨てるため、綴りが 2 箇所にあると片方だけ光らなくなる。
///
/// NOTE: MaterialInstance は HLSL リフレクション上の変数名を検証する。.mat 内の別名では
///       なくシェーダー変数へ合わせること (SkinnedGlowPart.hlsl の cbuffer を参照)。
#pragma once

#include <Engine/Scene/Script.hpp>

namespace sandbox {

// SkinnedGlowPart.hlsl / SkinnedPBR.hlsl 共通の自発光パラメーター。
//
// WHY 完全修飾するか: アタッチしないユーティリティは using namespace を持たない
//     (取り込んだ側の名前解決を汚さないため)。BladeColors.hpp と同じ方針。
inline constexpr fbzz::scene::MaterialPropertyId kEmissiveColorId{ "emissiveColor" };
inline constexpr fbzz::scene::MaterialPropertyId kEmissiveScaleId{ "emissiveScale" };

} // namespace sandbox
