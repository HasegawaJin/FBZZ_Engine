// FBZZ Engine
// PostProcessAsset.hpp | fbzz::asset
// .fzpp ポストプロセスプロファイルアセットの TOML 入出力 API
// WHY: ProjectSettings はプロジェクト全体の設定の一部として PostProcess を
//      平坦な [render] セクションに保存する。.fzpp は PP 設定だけを独立した
//      ファイルで管理し、シーン間・ゲームプレイ中の差し替えを可能にする。
//      スクリプトからは ScriptPostProcessProxy::LoadProfile() で読み込める。
#pragma once
#include <Engine/Renderer/RenderSettings.hpp>
#include <string_view>

namespace fbzz::asset {

// .fzpp から PostProcessSettings を読み込む。破損・未存在時は false を返す。
[[nodiscard]] bool LoadPostProcessAssetFromFile(std::string_view path,
                                                renderer::PostProcessSettings& out);

// PostProcessSettings を .fzpp として保存する。ディレクトリが存在しない場合は失敗。
[[nodiscard]] bool SavePostProcessAssetToFile(std::string_view path,
                                              const renderer::PostProcessSettings& settings);

} // namespace fbzz::asset
