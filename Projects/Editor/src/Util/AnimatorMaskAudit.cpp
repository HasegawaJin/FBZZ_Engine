/// @file    AnimatorMaskAudit.cpp
/// @brief   レイヤー合成の数値化と静的検証
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Editor/Util/AnimatorMaskAudit.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>

#include <algorithm>
#include <cstdio>

namespace fbzz::editor::maskaudit {

namespace {

/// Override レイヤーの実効 alpha がこの値を超えていれば「そのレイヤーが所有している」と見なす。
/// @note 1.0 ちょうどにしない: Inspector のスライダーは 0.99 のような端数を作れるため、厳密比較だと意図どおりのマスクも警告に出る。
constexpr float OWNED_THRESHOLD = 0.95f;
/// これ未満は「効いていない」。ここと OWNED_THRESHOLD の間が中途半端な帯。
constexpr float NEGLIGIBLE_THRESHOLD = 0.05f;

float MaskWeightOf(const LayerInfo& layer,
                   const std::string& bonePath,
                   const std::string& boneName)
{
    /// @note AnimatorSystem::LayerBoneWeight と同じ規則。マスクを «指定した» のに読めない間は
    ///       全身ではなく 0 として扱う。ここが食い違うと、監査だけが «効いている» と答える。
    if (!layer.mask) return layer.maskLoadFailed ? 0.0f : 1.0f;
    return asset::EvaluateAvatarMaskWeight(*layer.mask, bonePath, boneName);
}

std::string FormatPercent(float value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0f);
    return text;
}

} // namespace

std::vector<LayerInfo> CollectLayers(scene::AnimatorComponent& animator)
{
    std::vector<LayerInfo> layers;
    layers.reserve(animator.layers.size());

    for (auto& layer : animator.layers) {
        LayerInfo info;
        info.name     = layer.name;
        info.additive = layer.mode == scene::AnimationLayerMode::Additive;
        info.enabled  = layer.enabled;
        info.weight   = layer.weight;
        info.maskPath = layer.mask.path;

        /// @note AnimatorSystem はシミュレーション中しかマスクを読まない。編集中は誰も読んで
        ///       いないので、ここで同じキャッシュへ読み込む (別キャッシュにすると再生中と
        ///       編集中で違うマスクを見ることになる)。
        if (!layer.mask.path.empty()) {
            if (!layer.mask.loaded || layer.mask.loadedPath != layer.mask.path) {
                layer.mask.loadedPath = layer.mask.path;
                layer.mask.asset = asset::AvatarMaskAsset{};
                layer.mask.loaded = asset::LoadAvatarMaskAsset(
                    asset::AssetManager::ResolveAssetPath(layer.mask.path), layer.mask.asset);
                layer.mask.failed = !layer.mask.loaded;
            }
            if (layer.mask.loaded) info.mask = &layer.mask.asset;
            else                   info.maskLoadFailed = true;
        }
        layers.push_back(std::move(info));
    }
    return layers;
}

BoneContribution Evaluate(const std::vector<LayerInfo>& layers,
                          const std::string& bonePath,
                          const std::string& boneName)
{
    BoneContribution result;
    result.share.assign(layers.size(), 0.0f);
    result.additiveGain.assign(layers.size(), 0.0f);

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const LayerInfo& layer = layers[i];
        if (!layer.enabled) continue;

        const float maskWeight = MaskWeightOf(layer, bonePath, boneName);
        if (layer.additive) {
            /// @note 加算は誰の取り分も奪わない。倍率としてそのまま持つ。
            result.additiveGain[i] =
                std::clamp(layer.weight * maskWeight, 0.0f, scene::MAX_LAYER_WEIGHT);
            continue;
        }

        const float alpha = std::clamp(layer.weight * maskWeight, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;

        /// @note ApplyAnimationLayers と同じ Lerp(現在のポーズ, レイヤーのポーズ, alpha) を畳む。
        ///       先に積まれた取り分は (1 - alpha) 倍に薄まる。
        result.baseShare *= (1.0f - alpha);
        for (std::size_t j = 0; j < i; ++j)
            result.share[j] *= (1.0f - alpha);
        result.share[i] = alpha;
    }

    float best = result.baseShare;
    for (std::size_t i = 0; i < result.share.size(); ++i) {
        if (result.share[i] > best) {
            best = result.share[i];
            result.owner = static_cast<int>(i);
        }
    }
    return result;
}

int CountMaskedBones(const asset::Skeleton& skeleton, const asset::AvatarMaskAsset& mask)
{
    int count = 0;
    for (int i = 0; i < static_cast<int>(skeleton.nodes.size()); ++i) {
        const std::string path = asset::BuildSkeletonNodePath(skeleton, i);
        if (asset::EvaluateAvatarMaskWeight(
                mask, path, skeleton.nodes[static_cast<size_t>(i)].name) > 0.0f)
            ++count;
    }
    return count;
}

std::vector<float> BlendDepthRamp(float weight, int blendDepth)
{
    std::vector<float> ramp;
    const int steps = (std::max)(blendDepth, 0);
    ramp.reserve(static_cast<std::size_t>(steps) + 1);
    asset::AvatarMaskEntry entry;
    entry.weight = weight;
    entry.blendDepth = blendDepth;
    for (int depth = 0; depth <= steps; ++depth)
        ramp.push_back(std::clamp(asset::AvatarMaskRampedWeight(entry, depth), 0.0f, 1.0f));
    return ramp;
}

std::vector<Issue> Audit(const asset::Skeleton& skeleton, const std::vector<LayerInfo>& layers)
{
    std::vector<Issue> issues;
    if (skeleton.nodes.empty() || layers.empty()) return issues;

    /// @note ボーンパスは 1 度だけ組む。Evaluate は全レイヤーぶんここを参照する。
    std::vector<std::string> paths(skeleton.nodes.size());
    for (int i = 0; i < static_cast<int>(skeleton.nodes.size()); ++i)
        paths[static_cast<size_t>(i)] = asset::BuildSkeletonNodePath(skeleton, i);

    struct LayerStat {
        int   maskedBones   = 0;   ///< マスクが 0 より大きい値を返したボーン
        int   ownedBones    = 0;   ///< alpha >= OWNED_THRESHOLD
        int   partialBones  = 0;   ///< 中途半端な帯にいるボーン
        float worstPartial  = 1.0f;
        std::string worstPartialBone;
        /// マスク領域の起点 (親がマスク外) なのに 100% 取れていないボーン。
        /// @note 「上半身を乗っ取る」目的のレイヤーで乗っ取れない骨を示す。途中の骨が薄いのは設計だが起点が薄いのは事故になりやすい。
        int   weakRoots     = 0;
        float weakRootAlpha = 1.0f;
        std::string weakRootBone;
        /// Additive がベース由来のポーズに乗っているボーン。
        int   additiveOnBase = 0;
        std::string additiveOnBaseBone;
    };
    std::vector<LayerStat> stats(layers.size());

    for (int i = 0; i < static_cast<int>(skeleton.nodes.size()); ++i) {
        const auto& node = skeleton.nodes[static_cast<size_t>(i)];
        const std::string& path = paths[static_cast<size_t>(i)];
        const BoneContribution contribution = Evaluate(layers, path, node.name);

        for (std::size_t li = 0; li < layers.size(); ++li) {
            const LayerInfo& layer = layers[li];
            if (!layer.enabled) continue;
            LayerStat& stat = stats[li];
            const float maskWeight = MaskWeightOf(layer, path, node.name);
            if (maskWeight > 0.0f) ++stat.maskedBones;

            if (layer.additive) {
                /// @note 加算の基準ポーズは「その骨を誰が動かしているか」を前提に作られている。
                ///       ベースがまだ大半を占める骨に乗ると、基準ポーズとの差が二重に出る。
                if (contribution.additiveGain[li] > NEGLIGIBLE_THRESHOLD &&
                    contribution.baseShare > 0.5f) {
                    if (stat.additiveOnBase == 0) stat.additiveOnBaseBone = node.name;
                    ++stat.additiveOnBase;
                }
                continue;
            }

            const float alpha = std::clamp(layer.weight * maskWeight, 0.0f, 1.0f);
            if (alpha > NEGLIGIBLE_THRESHOLD && alpha < OWNED_THRESHOLD) {
                const int parent = node.parentIndex;
                const float parentMask = parent >= 0
                    ? MaskWeightOf(layer, paths[static_cast<size_t>(parent)],
                                   skeleton.nodes[static_cast<size_t>(parent)].name)
                    : 0.0f;
                if (parentMask <= 0.0f) {
                    ++stat.weakRoots;
                    if (alpha < stat.weakRootAlpha) {
                        stat.weakRootAlpha = alpha;
                        stat.weakRootBone = node.name;
                    }
                }
            }

            if (alpha >= OWNED_THRESHOLD) { ++stat.ownedBones; continue; }
            if (alpha <= NEGLIGIBLE_THRESHOLD) continue;
            ++stat.partialBones;
            if (alpha < stat.worstPartial) {
                stat.worstPartial = alpha;
                stat.worstPartialBone = node.name;
            }
        }
    }

    for (std::size_t li = 0; li < layers.size(); ++li) {
        const LayerInfo& layer = layers[li];
        const LayerStat& stat = stats[li];
        if (!layer.enabled) continue;

        if (layer.maskLoadFailed) {
            issues.push_back({ Severity::Warning, layer.name, {},
                "Mask を読み込めません (" + layer.maskPath +
                ")。マスク無しとして全身へ適用されます。" });
            continue;
        }

        /// @note これが最初に見るべき警告。マスクの骨名が骨格と噛み合っていないと、
        ///       レイヤーは 1 本も動かさないまま「設定はしてある」状態になる。
        if (layer.mask && stat.maskedBones == 0) {
            issues.push_back({ Severity::Warning, layer.name, {},
                "Mask がスケルトンのどのボーンにも一致しません。"
                "このレイヤーは何も動かしません (ボーン名を確認)。" });
            continue;
        }

        if (layer.weight <= 0.0f) {
            issues.push_back({ Severity::Info, layer.name, {},
                "Layer weight が 0 です。マスクの内容に関わらず何も出ません。" });
            continue;
        }

        if (layer.additive) {
            if (stat.additiveOnBase > 0) {
                issues.push_back({ Severity::Info, layer.name, stat.additiveOnBaseBone,
                    "Additive が Base 由来のボーン " + std::to_string(stat.additiveOnBase) +
                    " 本に乗ります (例: " + stat.additiveOnBaseBone +
                    ")。基準ポーズが Base と違うと二重に動きます。" });
            }
            continue;
        }

        if (stat.ownedBones == 0 && stat.partialBones > 0) {
            issues.push_back({ Severity::Warning, layer.name, stat.worstPartialBone,
                "このレイヤーが 100% 所有するボーンが 1 本もありません。"
                "最小 " + FormatPercent(stat.worstPartial) + " (" + stat.worstPartialBone +
                ")。Base が残り続けます。" });
        } else if (stat.weakRoots > 0) {
            /// @note マスク領域の起点が薄い = そのレイヤーは胴体を乗っ取れていない。
            ///       blend depth のランプは起点から始まるため、意図せずここが最弱になる。
            issues.push_back({ Severity::Warning, layer.name, stat.weakRootBone,
                "マスクの起点 " + stat.weakRootBone + " が " +
                FormatPercent(stat.weakRootAlpha) + " しか効きません (Base が " +
                FormatPercent(1.0f - stat.weakRootAlpha) +
                " 残ります)。blend depth のランプは起点の骨から始まります。" });
        } else if (stat.partialBones > 0) {
            issues.push_back({ Severity::Info, layer.name, stat.worstPartialBone,
                std::to_string(stat.partialBones) + " 本のボーンが中途半端に混ざります。"
                "最小 " + FormatPercent(stat.worstPartial) + " = " + stat.worstPartialBone +
                " (Base が " + FormatPercent(1.0f - stat.worstPartial) + " 残ります)。" });
        }
    }

    return issues;
}

} // namespace fbzz::editor::maskaudit
