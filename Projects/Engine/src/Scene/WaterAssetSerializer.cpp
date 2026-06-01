// FBZZ Engine
// WaterAssetSerializer.cpp | fbzz::scene
// WaterComponent の描画パラメータを .fbzzwater へ保存・復元する実装
#include <Engine/Scene/WaterAssetSerializer.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>

namespace fbzz::scene {
namespace {

double RoundFloat(double v)
{
    constexpr double SCALE = 1000000.0;
    const double r = std::round(v * SCALE) / SCALE;
    return r == 0.0 ? 0.0 : r;
}

void NormalizeFloats(toml::node& node)
{
    if (auto* f = node.as_floating_point()) { f->get() = RoundFloat(f->get()); return; }
    if (auto* t = node.as_table())  { for (auto&& [k, c] : *t) { (void)k; NormalizeFloats(c); } return; }
    if (auto* a = node.as_array()) { for (auto& c : *a) NormalizeFloats(c); }
}

toml::array Vec2ToArr(math::Vector2 v) { toml::array a; a.push_back((double)v.x); a.push_back((double)v.y); return a; }
toml::array Vec3ToArr(math::Vector3 v) { toml::array a; a.push_back((double)v.x); a.push_back((double)v.y); a.push_back((double)v.z); return a; }

math::Vector2 ArrToVec2(const toml::array* a, math::Vector2 def)
{
    if (!a || a->size() < 2) return def;
    return { (float)(*a)[0].value_or(0.0), (float)(*a)[1].value_or(0.0) };
}
math::Vector3 ArrToVec3(const toml::array* a, math::Vector3 def)
{
    if (!a || a->size() < 3) return def;
    return { (float)(*a)[0].value_or(0.0), (float)(*a)[1].value_or(0.0), (float)(*a)[2].value_or(0.0) };
}

// WHAT: WaterComponent の視覚パラメータをすべて TOML テーブルに書き出す。
//       ジオメトリ（extentX/Z, resolutionX/Z）はシーン側で管理するため含めない。
toml::table WriteWaterData(const WaterComponent& w)
{
    toml::table t;

    // 色・透明度
    t.insert("shallowColor",   Vec3ToArr(w.shallowColor));
    t.insert("deepColor",      Vec3ToArr(w.deepColor));
    t.insert("shallowDepth",   (double)w.shallowDepth);
    t.insert("deepDepth",      (double)w.deepDepth);
    t.insert("opacity",        (double)w.opacity);

    // Fresnel 反射
    t.insert("reflectivity",   (double)w.reflectivity);
    t.insert("fresnelBias",    (double)w.fresnelBias);
    t.insert("fresnelPower",   (double)w.fresnelPower);

    // 法線マップ
    t.insert("normalMap1Path",    w.normalMap1Path);
    t.insert("normalMap2Path",    w.normalMap2Path);
    t.insert("normalMap1Tiling",  (double)w.normalMap1Tiling);
    t.insert("normalMap2Tiling",  (double)w.normalMap2Tiling);
    t.insert("normalStrength",    (double)w.normalStrength);
    t.insert("normalMap1Scroll",  Vec2ToArr(w.normalMap1Scroll));
    t.insert("normalMap2Scroll",  Vec2ToArr(w.normalMap2Scroll));

    // Gerstner 波
    t.insert("enableGerstnerWaves", w.enableGerstnerWaves);
    toml::array waveArr;
    for (const GerstnerWave& wave : w.waves) {
        toml::table wt;
        wt.insert("direction",  Vec2ToArr(wave.direction));
        wt.insert("amplitude",  (double)wave.amplitude);
        wt.insert("wavelength", (double)wave.wavelength);
        wt.insert("steepness",  (double)wave.steepness);
        waveArr.push_back(std::move(wt));
    }
    t.insert("waves", std::move(waveArr));

    // 岸辺泡
    t.insert("foamThreshold", (double)w.foamThreshold);
    t.insert("foamFade",      (double)w.foamFade);
    t.insert("foamStrength",  (double)w.foamStrength);
    t.insert("foamTexPath",   w.foamTexPath);
    t.insert("foamTiling",    (double)w.foamTiling);

    // スクリーンスペース屈折
    t.insert("refractionStrength", (double)w.refractionStrength);

    // フローマップ
    t.insert("enableFlowMap", w.enableFlowMap);
    t.insert("flowMapPath",   w.flowMapPath);
    t.insert("flowSpeed",     (double)w.flowSpeed);
    t.insert("flowTiling",    (double)w.flowTiling);

    // コースティクス
    t.insert("enableCaustics",    w.enableCaustics);
    t.insert("causticsIntensity", (double)w.causticsIntensity);
    t.insert("causticsTiling",    (double)w.causticsTiling);
    t.insert("causticsSpeed",     (double)w.causticsSpeed);
    t.insert("causticsTexPath",   w.causticsTexPath);

    // 環境マップ
    t.insert("envCubemapPath", w.envCubemapPath);

    return t;
}

// WHAT: TOML テーブルから WaterComponent の視覚パラメータを復元する。
void ReadWaterData(const toml::table& t, WaterComponent& w)
{
    w.shallowColor  = ArrToVec3(t["shallowColor"].as_array(),  w.shallowColor);
    w.deepColor     = ArrToVec3(t["deepColor"].as_array(),     w.deepColor);
    w.shallowDepth  = (float)t["shallowDepth"].value_or(0.5);
    w.deepDepth     = (float)t["deepDepth"].value_or(5.0);
    w.opacity       = (float)t["opacity"].value_or(0.85);

    w.reflectivity  = (float)t["reflectivity"].value_or(0.5);
    w.fresnelBias   = (float)t["fresnelBias"].value_or(0.02);
    w.fresnelPower  = (float)t["fresnelPower"].value_or(5.0);

    w.normalMap1Path    = t["normalMap1Path"].value_or(std::string{});
    w.normalMap2Path    = t["normalMap2Path"].value_or(std::string{});
    w.normalMap1Tiling  = (float)t["normalMap1Tiling"].value_or(4.0);
    w.normalMap2Tiling  = (float)t["normalMap2Tiling"].value_or(6.0);
    w.normalStrength    = (float)t["normalStrength"].value_or(1.0);
    w.normalMap1Scroll  = ArrToVec2(t["normalMap1Scroll"].as_array(), w.normalMap1Scroll);
    w.normalMap2Scroll  = ArrToVec2(t["normalMap2Scroll"].as_array(), w.normalMap2Scroll);

    w.enableGerstnerWaves = t["enableGerstnerWaves"].value_or(true);
    if (const auto* waveArr = t["waves"].as_array()) {
        size_t idx = 0;
        for (const auto& node : *waveArr) {
            if (idx >= w.waves.size()) break;
            if (const auto* wt = node.as_table()) {
                w.waves[idx].direction  = ArrToVec2((*wt)["direction"].as_array(), w.waves[idx].direction);
                w.waves[idx].amplitude  = (float)(*wt)["amplitude"].value_or(0.0);
                w.waves[idx].wavelength = (float)(*wt)["wavelength"].value_or(10.0);
                w.waves[idx].steepness  = (float)(*wt)["steepness"].value_or(0.5);
            }
            ++idx;
        }
    }

    w.foamThreshold = (float)t["foamThreshold"].value_or(0.3);
    w.foamFade      = (float)t["foamFade"].value_or(0.5);
    w.foamStrength  = (float)t["foamStrength"].value_or(1.0);
    w.foamTexPath   = t["foamTexPath"].value_or(std::string{});
    w.foamTiling    = (float)t["foamTiling"].value_or(8.0);

    w.refractionStrength = (float)t["refractionStrength"].value_or(0.03);

    w.enableFlowMap = t["enableFlowMap"].value_or(false);
    w.flowMapPath   = t["flowMapPath"].value_or(std::string{});
    w.flowSpeed     = (float)t["flowSpeed"].value_or(0.3);
    w.flowTiling    = (float)t["flowTiling"].value_or(1.0);

    w.enableCaustics    = t["enableCaustics"].value_or(true);
    w.causticsIntensity = (float)t["causticsIntensity"].value_or(0.4);
    w.causticsTiling    = (float)t["causticsTiling"].value_or(0.5);
    w.causticsSpeed     = (float)t["causticsSpeed"].value_or(0.15);
    w.causticsTexPath   = t["causticsTexPath"].value_or(std::string{});

    w.envCubemapPath = t["envCubemapPath"].value_or(std::string{});
}

} // namespace

bool WaterAssetSerializer::Save(const WaterComponent& component, const std::string& path)
{
    if (path.empty()) return false;

    toml::table doc;
    toml::table meta;
    meta.insert("format_version", 1);
    doc.insert("water_asset", std::move(meta));
    doc.insert("water", WriteWaterData(component));

    NormalizeFloats(doc);

    std::ostringstream ss;
    ss << doc;

    const std::string dir = util::FileSystem::GetDirectory(path);
    if (!dir.empty())
        util::FileSystem::EnsureDirectory(dir);

    if (!util::FileSystem::WriteText(path, ss.str())) {
        FBZZ_LOG_ERROR("WaterAssetSerializer: save failed: %s", path.c_str());
        return false;
    }
    return true;
}

bool WaterAssetSerializer::Load(const std::string& path, WaterComponent& component)
{
    if (path.empty()) return false;

    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        FBZZ_LOG_ERROR("WaterAssetSerializer: read failed: %s", path.c_str());
        return false;
    }

    toml::parse_result result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("WaterAssetSerializer: parse failed: %s", path.c_str());
        return false;
    }

    toml::table doc = result.table();
    const toml::table* waterTbl = doc["water"].as_table();
    if (!waterTbl) waterTbl = &doc;

    ReadWaterData(*waterTbl, component);

    // WHY: 外部アセットを読み込んだ直後は必ず GPU リソースを再構築する。
    component.meshDirty = true;
    component.foamDirty = true;
    component.texDirty  = true;
    return true;
}

} // namespace fbzz::scene
