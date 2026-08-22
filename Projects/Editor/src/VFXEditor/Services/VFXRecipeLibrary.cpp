// FBZZ Engine
// VFXRecipeLibrary.cpp | fbzz::editor
// 層構成 (recipe) の表と、そこからのグラフ生成
#include <Editor/VFXEditor/Services/VFXRecipeLibrary.hpp>

#include <Editor/Util/ParticleMaterialFactory.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::editor {
namespace {

using asset::VFXNodeType;

constexpr int kAdditive = 0;
constexpr int kAlpha = 1;
constexpr int kPremultiplied = 2;
constexpr int kSortNone = 0;
constexpr int kSortBackToFront = 1;
constexpr int kShapePoint = 0;
constexpr int kShapeSphere = 1;
constexpr int kShapeCone = 2;

// ── 層の表 ───────────────────────────────────────────────────────────────────
// 数値は「置いた瞬間にそれらしく見える」ことを優先した出発点で、詰めるのは
// 公開パラメーターから行う想定。ブレンド・描画順・寿命の関係だけは崩さないこと
// (全部加算にすると白飽和し、renderPriority を揃えると前後が編集順で変わる)。

constexpr VFXRecipeLayer kFireLayers[] = {
    { "Smoke", VFXNodeType::Particle, "body", 0.0f, 4.0f, 240, 0, 14.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.35f, 0.9f, 2.4f, 2.2f, 0.6f, 1.1f,
      0.0f, 0, true, true, false,
      "背景を隠す body。最奥に置く。これが無いと炎が輪郭の無い光の玉になる" },
    { "Outer Flame", VFXNodeType::Particle, "body", 0.0f, 4.0f, 180, 0, 26.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.25f, 0.7f, 0.25f, 0.9f, 0.5f, 2.4f,
      1.4f, 20, true, false, false,
      "炎の外側。Alpha で形を作る層" },
    { "Core Flame", VFXNodeType::Particle, "core", 0.0f, 4.0f, 90, 0, 18.0f,
      kAdditive, kSortNone, kShapeSphere, 0.16f, 0.42f, 0.10f, 0.55f, 0.35f, 2.8f,
      3.4f, 40, true, false, false,
      "発光する芯。Additive はここだけに限り、renderPriority を body より大きくする" },
    { "Embers", VFXNodeType::Particle, "sparks", 0.15f, 4.0f, 70, 0, 12.0f,
      kAdditive, kSortNone, kShapeSphere, 0.3f, 0.07f, 0.01f, 1.8f, 0.7f, 0.9f,
      3.0f, 50, false, false, false,
      "火の粉。上昇する小さな点で炎の高さと空気の流れを見せる" },
    { "Heat Haze", VFXNodeType::Particle, "core", 0.0f, 4.0f, 24, 0, 6.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.3f, 0.6f, 1.6f, 1.0f, 0.2f, 1.6f,
      0.0f, 100, true, false, true,
      "陽炎。シーンカラーを読むため必ず最後 (先に描くと未描画の炎を歪めることになる)" },
};

constexpr VFXRecipeLayer kExplosionLayers[] = {
    { "Core Flash", VFXNodeType::Particle, "core", 0.0f, 0.18f, 8, 2, 0.0f,
      kAdditive, kSortNone, kShapePoint, 0.0f, 4.2f, 0.6f, 0.14f, 0.0f, 0.0f,
      5.0f, 60, true, false, false,
      "閃光。極短命で最前面。粒子数が少ないので強くしてよい" },
    { "Fireball", VFXNodeType::Particle, "core", 0.0f, 1.2f, 720, 150, 0.0f,
      kAdditive, kSortNone, kShapeSphere, 0.6f, 1.5f, 3.2f, 0.9f, 6.5f, -1.2f,
      2.0f, 50, true, false, false,
      "火球。膨張しながら減衰する本体" },
    { "Sparks", VFXNodeType::Particle, "sparks", 0.02f, 1.8f, 520, 200, 0.0f,
      kAdditive, kSortNone, kShapeSphere, 0.3f, 0.15f, 0.02f, 1.4f, 14.0f, -16.0f,
      3.5f, 70, false, false, false,
      "火花。重力で落ちる軌跡が速度感を作る" },
    { "Smoke Column", VFXNodeType::Particle, "body", 0.1f, 3.2f, 620, 40, 30.0f,
      kPremultiplied, kSortBackToFront, kShapeSphere, 0.9f, 1.4f, 5.0f, 2.6f, 1.6f, 0.55f,
      0.0f, 0, true, true, false,
      "煙柱。最奥。火球より手前に来ると爆発の明るさが濁る" },
    { "Shockwave Haze", VFXNodeType::Particle, "core", 0.01f, 0.55f, 6, 2, 0.0f,
      kAlpha, kSortBackToFront, kShapePoint, 0.0f, 1.2f, 10.0f, 0.45f, 0.0f, 0.0f,
      0.0f, 100, true, false, true,
      "衝撃波の歪み。distortion はシーンカラーを読むので最後に描く" },
};

constexpr VFXRecipeLayer kImpactLayers[] = {
    { "Hit Flash", VFXNodeType::Particle, "core", 0.0f, 0.16f, 6, 2, 0.0f,
      kAdditive, kSortNone, kShapePoint, 0.0f, 1.6f, 0.2f, 0.12f, 0.0f, 0.0f,
      4.0f, 60, true, false, false,
      "着弾の芯。ここのタイミングが打撃の硬さを決める" },
    { "Sparks", VFXNodeType::Particle, "sparks", 0.0f, 0.9f, 220, 90, 0.0f,
      kAdditive, kSortNone, kShapeCone, 0.2f, 0.10f, 0.01f, 0.7f, 9.0f, -14.0f,
      3.2f, 70, false, false, false,
      "飛散する火花。円錐で法線方向へ散らす" },
    { "Dust", VFXNodeType::Particle, "body", 0.02f, 1.4f, 160, 30, 0.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.3f, 0.5f, 1.5f, 1.1f, 1.4f, 0.3f,
      0.0f, 0, true, false, false,
      "土煙。潰した形で地面に沿わせる" },
    { "Decal", VFXNodeType::Decal, "", 0.03f, 4.0f, 0, 0, 0.0f,
      kAlpha, kSortNone, kShapePoint, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0, false, false, false,
      "着弾痕。残るものが 1 つあると当たった位置が読める" },
};

constexpr VFXRecipeLayer kSmokeLayers[] = {
    { "Body", VFXNodeType::Particle, "body", 0.0f, 6.0f, 260, 0, 12.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.5f, 1.0f, 3.4f, 4.0f, 0.5f, 0.45f,
      0.0f, 0, true, true, false,
      "煙の本体。回転あり・等方サイズ" },
    { "Drift", VFXNodeType::Particle, "body", 0.6f, 6.0f, 140, 0, 6.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.9f, 1.8f, 4.6f, 5.0f, 0.7f, 0.25f,
      0.0f, 10, true, false, false,
      "上層のたなびき。横へ伸ばし、回転を止める" },
    { "Updraft", VFXNodeType::ForceField, "", 0.0f, 6.0f, 0, 0, 0.0f,
      0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, false, false, false,
      "弱い上昇風。層をまとめて動かすと 1 つの塊として読める" },
};

constexpr VFXRecipeLayer kBeamLayers[] = {
    { "Core Beam", VFXNodeType::Trail, "core", 0.0f, 1.2f, 0, 0, 0.0f,
      kAdditive, kSortNone, kShapePoint, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 50, false, false, false,
      "芯のビーム。Trail ノードの beamMode で始点/終点を持つ帯として引く" },
    { "Outer Glow", VFXNodeType::Trail, "body", 0.0f, 1.2f, 0, 0, 0.0f,
      kAlpha, kSortBackToFront, kShapePoint, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 20, false, false, false,
      "外側の広がり。芯より奥へ、太く薄く" },
    { "Motes", VFXNodeType::Particle, "sparks", 0.0f, 1.2f, 90, 0, 40.0f,
      kAdditive, kSortNone, kShapeSphere, 0.15f, 0.06f, 0.01f, 0.6f, 1.2f, 0.4f,
      2.6f, 60, false, false, false,
      "沿って流れる粒。ビームが 1 枚の板に見えるのを防ぐ" },
};

constexpr VFXRecipeLayer kAuraLayers[] = {
    { "Ground Glow", VFXNodeType::Particle, "core", 0.0f, 5.0f, 40, 0, 8.0f,
      kAdditive, kSortNone, kShapeSphere, 0.8f, 1.2f, 1.6f, 1.4f, 0.2f, 0.1f,
      2.2f, 40, true, false, false,
      "足元の発光。存在の中心を示す" },
    { "Rising Motes", VFXNodeType::Particle, "sparks", 0.0f, 5.0f, 120, 0, 18.0f,
      kAdditive, kSortNone, kShapeSphere, 1.0f, 0.07f, 0.01f, 2.4f, 0.5f, 0.9f,
      2.8f, 50, false, false, false,
      "立ち上る粒。上向きの重力でゆっくり昇らせる" },
    { "Veil", VFXNodeType::Particle, "body", 0.0f, 5.0f, 80, 0, 6.0f,
      kAlpha, kSortBackToFront, kShapeSphere, 0.9f, 1.4f, 2.2f, 2.6f, 0.3f, 0.35f,
      0.0f, 0, true, true, false,
      "薄い body。加算だけだと輪郭が消えるので後ろへ 1 枚敷く" },
};

constexpr VFXRecipe kRecipes[] = {
    { "Fire", "継続的に燃える炎",
      "煙(Alpha,6way,最奥) / 外炎(Alpha,body) / 芯炎(Additive,発光) / 火の粉(Additive,上昇) "
      "/ 陽炎(distortion,最前) / 点光源(intensityCurveでゆらぎ)",
      kFireLayers, true, true, false },
    { "Explosion", "一発の爆発",
      "閃光(Additive,極短命) / 火球(Additive,膨張) / 火花(Additive,重力落下) "
      "/ 煙柱(Premultiplied,6way,長寿命) / 衝撃波の歪み(distortion) / 点光源(減衰カーブ)",
      kExplosionLayers, false, true, true },
    { "Impact", "着弾・被弾のヒット",
      "ヒットフラッシュ(Additive,極短命) / 火花(Additive,円錐) / 土煙(Alpha,潰した形) "
      "/ デカール / カメラシェイク",
      kImpactLayers, false, true, true },
    { "Smoke", "たなびく煙",
      "本体(Alpha,6way,回転あり等方) / 上層のたなびき(Alpha,横伸ばし,回転なし) / 弱い上昇風",
      kSmokeLayers, true, false, false },
    { "Beam", "持続するビーム・レーザー",
      "芯ビーム(Trail,beamMode,Additive) / 外側の広がり(Trail,Alpha,奥) / 沿って流れる粒",
      kBeamLayers, true, true, false },
    { "Aura", "足元から立ち上るオーラ",
      "足元の発光(Additive) / 立ち上る粒(Additive,上向き重力) / 薄い body(Alpha,6way)",
      kAuraLayers, true, true, false },
};

constexpr VFXAssetRoleInfo kRoles[] = {
    { "core", "glow",
      "発光する芯 (Additive)。これが無いと光っているエフェクトが作れない",
      "body 素材を Additive で小さく使う" },
    { "body", "smoke",
      "背景を隠す body (Alpha)。全部加算だと白飽和して輪郭の無い光の玉になる",
      "glow 素材を Alpha + 低 emissive で使い、sortMode を BackToFront にする" },
    { "sparks", "spark",
      "火の粉・破片。小さな点の集合で動きの速さを見せる",
      "glow 素材を極小サイズ (0.05 以下) で使う" },
    { "animated", "flipbook",
      "コマアニメ素材。爆発の膨張や炎の揺らぎを 1 レイヤーで表現できる",
      "静止素材の noiseStrength と sizeCurve で揺らぎを代用する" },
};

} // namespace

std::span<const VFXAssetRoleInfo> GetVFXAssetRoles() { return kRoles; }
std::span<const VFXRecipe> GetVFXRecipes() { return kRecipes; }

const VFXRecipe* FindVFXRecipe(std::string_view name)
{
    for (const VFXRecipe& recipe : kRecipes)
        if (name == recipe.name) return &recipe;
    return nullptr;
}

std::vector<std::string> CollectRecipeRoles(const VFXRecipe& recipe)
{
    std::vector<std::string> roles;
    for (const VFXRecipeLayer& layer : recipe.layerSpecs) {
        if (layer.assetRole == nullptr || layer.assetRole[0] == '\0') continue;
        const std::string role = layer.assetRole;
        if (std::find(roles.begin(), roles.end(), role) == roles.end()) roles.push_back(role);
    }
    return roles;
}

asset::VFXGraphAsset BuildGraphFromRecipe(const VFXRecipe& recipe,
                                          const VFXRecipeBuildOptions& options)
{
    const float scale = (std::max)(options.scale, 0.05f);
    // 粒子数は面積ではなく体積に効くが、線形に増やすと L で budget が跳ねる。
    // 見た目の密度が保たれる範囲として scale^1.5 に抑える。
    const float countScale = std::pow(scale, 1.5f);

    asset::VFXGraphAsset graph;
    graph.name = options.graphName.empty() ? recipe.name : options.graphName;
    graph.description = recipe.summary;
    graph.tags.push_back(recipe.name);
    graph.requiredRoles = CollectRecipeRoles(recipe);

    asset::VFXGraphNode entry;
    entry.id = 1;
    entry.type = VFXNodeType::Entry;
    entry.name = "Entry";
    entry.duration = 0.0f;
    entry.editorX = 40.0f;
    entry.editorY = 120.0f;
    graph.nodes.push_back(std::move(entry));

    int nextId = 1;
    float layoutY = -60.0f;
    const auto textureFor = [&options](const char* role) -> std::string {
        if (role == nullptr || role[0] == '\0') return {};
        const auto it = options.roleTextures.find(role);
        return it == options.roleTextures.end() ? std::string{} : it->second;
    };
    // ロールのテクスチャを、その層のブレンドで描ける .mat へ変換する。
    // WHY: ParticleEmitter / Trail は materialPath しか持てない。ここでテクスチャを
    //      そのまま入れると .mat として解決できず、1x1 白テクスチャで描かれてしまう
    //      (「Recipe から作ると必ず白い四角になる」の原因はこれだった)。
    const auto materialFor = [&options, &textureFor](const char* role, int blendMode) -> std::string {
        const std::string texture = textureFor(role);
        if (texture.empty() || options.projectRoot.empty()) return {};
        return EnsureParticleMaterial(options.projectRoot, texture,
                                      static_cast<scene::ParticleBlendMode>(blendMode));
    };

    for (const VFXRecipeLayer& layer : recipe.layerSpecs) {
        asset::VFXGraphNode node;
        node.id = ++nextId;
        node.type = layer.nodeType;
        node.name = layer.name;
        node.duration = layer.duration;
        node.editorX = 300.0f;
        node.editorY = layoutY;
        layoutY += 190.0f;

        if (layer.nodeType == VFXNodeType::Particle) {
            scene::ParticleEmitter& emitter = node.particle;
            // テンプレートと同じ理由で、複数 Emitter が 1 つの演出 Bounds を作るため
            // Emitter 単位のカリングと距離 LOD は切っておく。
            emitter.cullingEnabled = false;
            emitter.lodEnabled = false;
            emitter.duration = layer.duration;
            emitter.loop = options.loop;
            emitter.maxParticles =
                (std::max)(static_cast<int>(static_cast<float>(layer.particleCount) * countScale), 1);
            emitter.shape = static_cast<scene::ParticleEmitterShape>(layer.shape);
            emitter.sphereRadius = layer.shapeRadius * scale;
            emitter.coneRadius = layer.shapeRadius * scale;
            emitter.sizeStart = layer.sizeStart * scale;
            emitter.sizeEnd = layer.sizeEnd * scale;
            emitter.lifetime = layer.lifetime;
            emitter.lifetimeRandom = layer.lifetime * 0.3f;
            emitter.velocitySpread = layer.velocitySpread * scale;
            emitter.gravity = { 0.0f, layer.gravityY * scale, 0.0f };
            emitter.emitVelocity = { 0.0f, 0.0f, 0.0f };
            emitter.materialPath = materialFor(layer.assetRole, layer.blendMode);
            emitter.blendMode = static_cast<scene::ParticleBlendMode>(layer.blendMode);
            emitter.sortMode = static_cast<scene::ParticleSortMode>(layer.sortMode);
            emitter.emissiveScale = layer.emissiveScale;
            emitter.renderPriority = layer.renderPriority;
            emitter.softParticles = layer.softParticles;
            emitter.sixWayLighting = layer.sixWayLighting;
            emitter.distortion = layer.distortion;
            if (layer.distortion) emitter.distortionStrength = 0.07f;
            // 同じ色の粒が数百枚重なると塗りつぶしにしか見えないので、
            // 発光層以外は必ず粒ごとの色温度差を入れておく。
            emitter.colorVariation = layer.emissiveScale > 3.0f ? 0.10f : 0.18f;
            if (layer.burstCount > 0) {
                emitter.emitRate = 0.0f;
                const int count =
                    (std::max)(static_cast<int>(static_cast<float>(layer.burstCount) * countScale), 1);
                emitter.bursts.push_back({ 0.0f, count, 1, 0.02f, 1.0f });
            } else {
                emitter.emitRate = layer.emitRate * countScale;
            }
        } else if (layer.nodeType == VFXNodeType::Trail) {
            node.trail.materialPath = materialFor(layer.assetRole, layer.blendMode);
            node.trail.beamMode = true;
            node.trail.beamStart = { 0.0f, 0.0f, 0.0f };
            node.trail.beamEnd = { 0.0f, 0.0f, 12.0f * scale };
            node.trail.widthStart = (layer.renderPriority >= 50 ? 0.25f : 0.6f) * scale;
            node.trail.widthEnd = node.trail.widthStart * 0.4f;
            node.trail.lifetime = layer.duration;
        } else if (layer.nodeType == VFXNodeType::Decal) {
            node.decal.albedoPath = textureFor(layer.assetRole);
            node.decal.fadeTime = 0.8f;
            node.localRotationDegrees = { -90.0f, 0.0f, 0.0f };
            node.localScale = { 2.0f * scale, 2.0f * scale, 1.0f };
        } else if (layer.nodeType == VFXNodeType::ForceField) {
            node.forceField.fieldType = 0;  // Wind
            node.forceField.direction = { 0.0f, 1.0f, 0.0f };
            node.forceField.strength = 2.0f * scale;
            node.forceField.radius = 6.0f * scale;
        }

        graph.nodes.push_back(std::move(node));
        asset::VFXGraphLink link;
        link.fromNode = 1;
        link.toNode = nextId;
        link.trigger = asset::VFXLinkTrigger::OnStart;
        link.delay = layer.startDelay;
        graph.links.push_back(link);
    }

    // 点光源。エフェクトが周囲を照らさないと、絵の中で浮いたシールのように見える。
    if (options.includeLight && recipe.wantsLight) {
        asset::VFXGraphNode light;
        light.id = ++nextId;
        light.type = VFXNodeType::Light;
        light.name = "Light";
        light.duration = recipe.loopByDefault ? 4.0f : 0.45f;
        light.editorX = 620.0f;
        light.editorY = -60.0f;
        light.localPosition = { 0.0f, 0.6f * scale, 0.0f };
        light.light.intensity = 12.0f * scale;
        light.light.range = 10.0f * scale;
        // 定数の明るさで点灯し続けると「電灯が点いた」ようにしか見えない。減衰が本体。
        light.light.useIntensityCurve = true;
        light.light.intensityCurve.keys[0] = { 0.0f, 1.0f };
        light.light.intensityCurve.keys[1] = { 0.10f, 0.85f };
        light.light.intensityCurve.keys[2] = { 0.40f, 0.30f };
        light.light.intensityCurve.keys[3] = { 1.00f, 0.0f };
        light.light.intensityCurve.keyCount = 4;
        graph.nodes.push_back(std::move(light));
        graph.links.push_back({ 1, nextId, asset::VFXLinkTrigger::OnStart, 0.0f, {} });
    }

    if (options.includeCameraShake && recipe.wantsCameraShake) {
        asset::VFXGraphNode shake;
        shake.id = ++nextId;
        shake.type = VFXNodeType::CameraShake;
        shake.name = "Camera Shake";
        shake.duration = 0.4f;
        shake.editorX = 620.0f;
        shake.editorY = 130.0f;
        shake.cameraShake.amplitude = 0.12f * scale;
        shake.cameraShake.rotationAmplitude = 1.2f * scale;
        graph.nodes.push_back(std::move(shake));
        graph.links.push_back({ 1, nextId, asset::VFXLinkTrigger::OnStart, 0.0f, {} });
    }

    // 骨格に「触るべきつまみ」を最初から生やしておく。公開パラメーターの無いグラフは
    // 結局 raw field を触ることになり、AI からもインスタンスからも制御できない。
    const auto addFloatParameter = [&graph](const char* name, float value, float minimum,
                                            float maximum, int nodeId, const char* path) {
        asset::VFXParamDefinition parameter;
        parameter.name = name;
        parameter.type = asset::VFXParamType::Float;
        parameter.defaultValue.source = asset::VFXConstant{ value };
        parameter.minimum = minimum;
        parameter.maximum = maximum;
        parameter.hasRange = true;
        graph.parameters.push_back(std::move(parameter));
        graph.bindings.push_back({ name, nodeId, path });
    };
    // 芯の強さ (発光層のうち最も手前のもの) と、body の量 (最奥の層)。
    int coreNodeId = -1;
    int bodyNodeId = -1;
    int corePriority = -1;
    for (const auto& node : graph.nodes) {
        if (node.type != VFXNodeType::Particle) continue;
        if (node.particle.emissiveScale > 1.5f && node.particle.renderPriority > corePriority) {
            corePriority = node.particle.renderPriority;
            coreNodeId = node.id;
        }
        if (bodyNodeId < 0 && node.particle.blendMode != scene::ParticleBlendMode::Additive)
            bodyNodeId = node.id;
    }
    if (coreNodeId > 0) addFloatParameter("Intensity", 3.0f, 0.0f, 8.0f, coreNodeId,
                                          "particle.emissiveScale");
    if (bodyNodeId > 0) {
        const asset::VFXGraphNode* body = nullptr;
        for (const auto& node : graph.nodes) if (node.id == bodyNodeId) body = &node;
        if (body != nullptr && body->particle.emitRate > 0.0f)
            addFloatParameter("Smokiness", body->particle.emitRate, 0.0f,
                              body->particle.emitRate * 3.0f, bodyNodeId, "particle.emitRate");
        else if (body != nullptr)
            addFloatParameter("Scale", body->particle.sizeEnd, 0.0f,
                              body->particle.sizeEnd * 3.0f, bodyNodeId, "particle.sizeEnd");
    }

    // budget は実使用量へ合わせる。既定の 100000 のままだと「上限を見ても何も判らない」。
    const asset::VFXGraphBudgetStats usage = asset::CalculateVFXGraphBudget(graph);
    graph.maxParticles = (std::max)(usage.particles, 1);
    graph.maxLights = (std::max)(usage.lights, 1);
    graph.maxAudioVoices = (std::max)(usage.audioVoices, 1);
    return graph;
}

} // namespace fbzz::editor
