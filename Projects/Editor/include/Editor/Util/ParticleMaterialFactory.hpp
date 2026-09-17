/// @file    ParticleMaterialFactory.hpp
/// @brief   テクスチャ 1 枚から Particle 用 .mat を用意する共有ファクトリ
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// ParticleEmitter の描画設定は materialPath (.mat) のみを受け付けテクスチャを直接持てない。
/// Recipe ウィザード / Procedural 生成 / D&D の 3 経路それぞれで Material を手作りしていた
/// 往復をここへ集約し、テクスチャを渡せば使える .mat を返す。
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <string>

namespace fbzz::editor {

/// テクスチャに対応する Particle 用 .mat を用意する (`Assets/Materials/Particles/` 配下に stem とブレンドから決定的な名前で生成し、複製されない)。
/// @param projectRoot プロジェクトルート (EditorContext::projectRoot)
/// @param texturePath Assets 起点 / 絶対パスどちらでもよいテクスチャ参照
/// @param blend       .mat へ焼くブレンド。既定は加算 (ParticleFallback.mat と同じ)。
/// @param outError    失敗理由 (任意)
/// @return Assets 起点の .mat パス。失敗時は空文字列
/// @note blend の既定値は .mat 側が正本のため。作成前は初期値を持たないことがあり、
///       作った後は Material の Inspector で変更する。
[[nodiscard]] std::string EnsureParticleMaterial(
    const std::string& projectRoot,
    const std::string& texturePath,
    scene::ParticleBlendMode blend = scene::ParticleBlendMode::Additive,
    std::string* outError = nullptr);

/// パスがテクスチャ拡張子か。D&D の受け口が「.mat か .png か」を分けるのに使う。
[[nodiscard]] bool IsTextureAssetPath(const std::string& path);

/// materialPath が指す .mat の [particle]。未設定・ロード失敗なら nullptr。
/// @note 見た目が .mat へ移ったため、GPU 可否や有効な機能を答えるには素材まで読む必要が
///       ある。Inspector / VFX グラフ / AI バスがそれぞれ解決を書くと答えが食い違う。
/// @note 戻り値は AssetManager のキャッシュを指す。次のロードまでの寿命しか無いので、
///       その場で読むだけにして保持しないこと。
[[nodiscard]] const asset::ParticleMaterialSettings* ResolveParticleMaterialSettings(
    const std::string& materialPath);

/// Particle / Trail の Material 欄。 `.mat` に加えてテクスチャの D&D・ピッカー選択も受け付け、
/// テクスチャが来たときは EnsureParticleMaterial で .mat へ包んでからパスを書き込む。
/// @note 素材欄が .mat しか受けないと、手持ちの .png を貼れず「ドロップしたのに何も
///       起きない」で止まる。受け口の側で吸収する。
/// @param blendForNewMaterial テクスチャから .mat を新規生成するときのブレンド。
///                            既存 .mat を選んだ場合は使われない。
/// @return true if materialPath was changed
bool ParticleMaterialField(const char* label, std::string& materialPath,
                           const std::string& projectRoot,
                           scene::ParticleBlendMode blendForNewMaterial
                               = scene::ParticleBlendMode::Additive);

} // namespace fbzz::editor
