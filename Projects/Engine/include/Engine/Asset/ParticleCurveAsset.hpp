/// @file    ParticleCurveAsset.hpp
/// @brief   .curve / .gradient — 名前を付けて使い回す時間曲線と色グラデーション。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY 曲線をアセットにするか:
///   «炎の呼吸» «煙の減衰» のような形は、同じ演出の中で 10 個以上のエミッターが共有する。
///   インラインのままだと 1 本直すたびに全部を手で合わせることになり、実際には合わない。
///
/// WHY それでもエミッター側はインライン保持のままか:
///   曲線をハンドルにすると «1 本触ったら共有している全員が変わる» が既定になる。
///   粒子の曲線は per-emitter に微調整するほうが普通で、Unity が ParticleSystem の
///   カーブをアセット化しなかったのも同じ理由。ここは «読み込む / 名前を付けて保存する»
///   という明示操作にとどめ、共有したい形だけをアセットにする。
#pragma once
#include <Engine/Scene/ParticleCurve.hpp>
#include <string>

namespace fbzz::asset {

/// .curve と .gradient は同じ型で読む。どちらが入っているかは hasCurve / hasGradient で分かる。
/// WHY 1 つの型か: 「この形」を保存したいという操作は 1 つで、拡張子は中身の違いでしかない。
///      型を 2 つに割ると AssetStore もインポーターも 2 組になり、得るものが無い。
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
