/// @file    ParticleCurveAsset.cpp
/// @brief   .curve / .gradient の TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/ParticleCurveAsset.hpp>

#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>
#include <Engine/Core/Logger.hpp>
#include <toml++/toml.hpp>
#include <fstream>
#include <sstream>

namespace fbzz::asset {

bool SaveParticleCurveAsset(const std::string& absPath, const ParticleCurveAsset& asset)
{
    if (!asset.hasCurve && !asset.hasGradient) return false;

    toml::table root;
    root.insert("version", 1);
    if (asset.hasCurve)    root.insert("curve", SerializeParticleCurve(asset.curve));
    if (asset.hasGradient) root.insert("gradient", SerializeParticleGradient(asset.gradient));

    std::ofstream out(absPath, std::ios::trunc);
    if (!out) {
        FBZZ_LOG_ERROR("ParticleCurveAsset: 書き出せません: %s", absPath.c_str());
        return false;
    }
    out << root;
    return static_cast<bool>(out);
}

bool LoadParticleCurveAssetFile(const std::string& absPath, ParticleCurveAsset& outAsset)
{
    std::ifstream in(absPath);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();

    toml::parse_result parsed = toml::parse(buffer.str());
    if (!parsed) {
        FBZZ_LOG_ERROR("ParticleCurveAsset: TOML が壊れています: %s (%s)",
                       absPath.c_str(), std::string(parsed.error().description()).c_str());
        return false;
    }
    const toml::table& root = parsed.table();

    ParticleCurveAsset asset;
    if (root.contains("curve")) {
        DeserializeParticleCurve(root, "curve", asset.curve);
        asset.hasCurve = true;
    }
    if (root.contains("gradient")) {
        DeserializeParticleGradient(root, "gradient", asset.gradient);
        asset.hasGradient = true;
    }
    if (!asset.hasCurve && !asset.hasGradient) {
        // 中身が無いファイルを «読めた» にすると、貼っても何も起きない曲線が
        // 静かに配られる。読めなかったものとして扱う。
        FBZZ_LOG_ERROR("ParticleCurveAsset: curve も gradient もありません: %s", absPath.c_str());
        return false;
    }
    outAsset = asset;
    return true;
}

} // namespace fbzz::asset
