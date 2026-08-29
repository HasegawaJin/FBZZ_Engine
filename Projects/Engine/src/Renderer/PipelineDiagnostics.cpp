/// @file    PipelineDiagnostics.cpp
/// @brief   現在のパイプラインでは効かない設定を列挙する
/// @author  Hasegawa Jin
/// @date    2026-08-25
#include <Engine/Renderer/PipelineDiagnostics.hpp>
#include <Engine/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {

namespace {

constexpr const char* kNeedsClusterRemedy =
    "Pipeline を Forward+ または Deferred+ にしてください";

} // namespace

std::vector<InertSetting> CollectInertSettings(const RenderSettings& settings)
{
    std::vector<InertSetting> issues;

    // NOTE: SSAO / GTAO / SSR / 接触影 は Forward でも GBuffer プリパスを走らせて
    //       効くようになったため、ここには挙げない。差はコストだけになった。

    // クラスタ設定は "+" でしか読まれない。
    // NOTE: ライトそのものは全パイプラインで同じ配列から供給される (Forward / Deferred は
    //       全数走査)。効かないのは「カリングの調整値」だけで、絵は変わらない。
    if (settings.clustered.enabled && !settings.UsesClusteredLighting()) {
        issues.push_back({
            "Clustered Lights",
            "Forward / Deferred はライトを全数走査するため、クラスタの調整値は使われません "
            "(絵は変わりません。負荷だけが違います)",
            kNeedsClusterRemedy });
    }
    if (settings.clustered.debugHeatmap && !settings.UsesClusteredLighting()) {
        issues.push_back({
            "Clustered Debug Heatmap",
            "クラスタが無いため可視化するものがありません",
            kNeedsClusterRemedy });
    }

    return issues;
}

const char* DescribeInertOverride(const RenderSettings& settings, std::string_view typeName)
{
    // 現状、パイプラインで丸ごと無効になる VolumeOverride は無い。
    // WHY 関数を残すか: 判定の置き場所を 1 か所に決めておくことに意味がある。
    //     ここが空でなくなるような機能を足したとき、UI 側は何も変えずに済む。
    (void)settings;
    (void)typeName;
    return nullptr;
}

} // namespace fbzz::renderer
