/// @file    PipelineDiagnostics.cpp
/// @brief   現在のパイプラインでは効かない設定を列挙する
/// @author  Hasegawa Jin
/// @date    2026-08-25
#include <Graphics/Renderer/PipelineDiagnostics.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {

namespace {

constexpr const char* kNeedsClusterRemedy =
    "Pipeline を Forward+ または Deferred+ にしてください";

} /// @note namespace

std::vector<InertSetting> CollectInertSettings(const RenderSettings& settings)
{
    std::vector<InertSetting> issues;

    /// @note SSAO / GTAO / SSR / 接触影 は Forward でも GBuffer プリパスを走らせて
    /// @note       効くようになったため、ここには挙げない。差はコストだけになった。

    /// @note クラスタ設定は "+" でしか読まれない。ライトそのものは全パイプラインで同じ配列から
    /// @note       供給される (Forward/Deferred は全数走査) ため、効かないのは「カリングの調整値」だけで
    /// @note       絵は変わらない。
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
    /// @note 現状、パイプラインで丸ごと無効になる VolumeOverride は無い。判定の置き場所を
    /// @note       1 か所に決めておくため関数として残しており、対象機能が増えても UI 側は変えずに済む。
    (void)settings;
    (void)typeName;
    return nullptr;
}

} /// @note namespace fbzz::renderer
