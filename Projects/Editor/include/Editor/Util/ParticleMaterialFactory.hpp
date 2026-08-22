/// @file   ParticleMaterialFactory.hpp
/// @brief  テクスチャ 1 枚から Particle 用 .mat を用意する共有ファクトリ
/// @author Hasegawa Jin
/// @date   2026-08-22
///
/// WHY: ParticleEmitter の描画設定は materialPath (.mat) が単一の信頼元で、
///      テクスチャを直接持てない。ところが担当者の手元にあるのは .png であって
///      .mat ではないため、素材を貼るたびに Material を手作りする往復が要る。
///      その往復が Recipe ウィザード / Procedural 生成 / D&D の 3 か所すべてで
///      発生し、どれも「割り当てられないので何も出ない」で終わっていた。
///      変換を 1 か所へ寄せ、テクスチャを渡せば使える .mat が返るようにする。
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <string>

namespace fbzz::editor {

/// テクスチャに対応する Particle 用 .mat を用意する。
///
/// 出力先は `Assets/Materials/Particles/<Stem><Blend>.mat` 固定。
/// 命名が決定的なので、同じテクスチャ・同じブレンドなら常に同じ .mat を指し、
/// 呼ぶたびに複製が増えることはない (既存ならそのまま再利用する)。
///
/// @param projectRoot  プロジェクトルート (EditorContext::projectRoot)
/// @param texturePath  Assets 起点 / 絶対パスどちらでもよいテクスチャ参照
/// @param blend        .mat へ焼くブレンド。ParticlePass はここを Emitter より優先する
/// @param outError     失敗理由 (任意)
/// @return Assets 起点の .mat パス。失敗時は空文字列
[[nodiscard]] std::string EnsureParticleMaterial(const std::string& projectRoot,
                                                 const std::string& texturePath,
                                                 scene::ParticleBlendMode blend,
                                                 std::string* outError = nullptr);

/// パスがテクスチャ拡張子か。D&D の受け口が「.mat か .png か」を分けるのに使う。
[[nodiscard]] bool IsTextureAssetPath(const std::string& path);

/// Particle / Trail の Material 欄。
///
/// `.mat` に加えてテクスチャの D&D・ピッカー選択も受け付け、テクスチャが来たときは
/// EnsureParticleMaterial で .mat へ包んでからパスを書き込む。
/// WHY: 素材欄が .mat しか受けないと、担当者は手持ちの .png を貼れず
///      「ドロップしたのに何も起きない」で止まる。受け口の側で吸収する。
///
/// @param blendForNewMaterial テクスチャから .mat を新規生成するときのブレンド。
///                            既存 .mat を選んだ場合は使われない。
/// @return true if materialPath was changed
bool ParticleMaterialField(const char* label, std::string& materialPath,
                           scene::ParticleBlendMode blendForNewMaterial,
                           const std::string& projectRoot);

} // namespace fbzz::editor
