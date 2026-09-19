/// @file    ParticleCurveAsset.hpp
/// @brief   .curve / .gradient — 名前を付けて使い回す時間曲線と色グラデーション。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note «炎の呼吸» «煙の減衰» のような形は複数のエミッターで共有され、インラインのままだと
///       1 本直すたび全部を手で合わせることになる。ただしエミッター側は既定でインライン保持の
///       ままとし (共有ハンドルだと 1 本触ると全員が変わってしまう)、共有したい形だけを
///       明示的にアセット化する。
#pragma once
#include <Engine/Scene/ParticleCurve.hpp>
#include <string>

namespace fbzz::asset {

/// .curve と .gradient は同じ型で読む。どちらが入っているかは hasCurve / hasGradient で分かる。
/// @note 「この形」を保存する操作は 1 つで拡張子は中身の違いでしかないため型は 1 つにした。
///       型を 2 つに割ると AssetStore もインポーターも 2 組になり、得るものが無い。
struct ParticleCurveAsset {
    scene::ParticleCurve    curve;
    scene::ParticleGradient gradient;
    bool hasCurve = false;
    bool hasGradient = false;
};

/// TOML として書き出す。curve と gradient は既存の
/// SerializeParticleCurve / SerializeParticleGradient と同じ形なので、
/// .particle から切り出した値をそのまま貼れる。
[[nodiscard]] bool SaveParticleCurveAsset(const std::string& absPath, const ParticleCurveAsset& asset);
[[nodiscard]] bool LoadParticleCurveAssetFile(const std::string& absPath, ParticleCurveAsset& outAsset);

} // namespace fbzz::asset
