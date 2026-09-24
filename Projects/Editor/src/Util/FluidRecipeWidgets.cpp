/// @file    FluidRecipeWidgets.cpp
/// @brief   .fluid レシピの編集欄 (ImGui)。Fluid Editor の Properties と Outliner の追加メニューが使う
/// @author  Hasegawa Jin
/// @date    2026-09-12

#include <Editor/Util/FluidRecipeWidgets.hpp>

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Fluid/FluidOperatorEval.hpp>
#include <Fluid/FluidRecipe.hpp>
#include <Engine/Asset/FluidSourceMaskLoader.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fbzz::editor::fluidui {
namespace {

using fluid::FluidKind;
using fluid::FluidShading;

constexpr ImVec4 kErrorColor{ 1.0f, 0.40f, 0.30f, 1.0f };
constexpr ImVec4 kWarnColor{ 1.0f, 0.75f, 0.30f, 1.0f };
/// @brief VolumeFlipbookBaker.cpp の kMaxFluidResolution。超えた値は黙ってここへ落とされるので UI で断っておく。
constexpr int kMaxCpuVolumeResolution = 96;

void Tooltip(const char* text)
{
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

std::filesystem::file_time_type FileStamp(const std::string& path)
{
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(util::FileSystem::PathFromUtf8(path), error);
    return error ? std::filesystem::file_time_type{} : stamp;
}

[[nodiscard]] int FrameCount(const fluid::FluidRecipe& recipe)
{
    return (std::max)(recipe.output.columns, 1) * (std::max)(recipe.output.rows, 1);
}

[[nodiscard]] FluidShading EffectiveShading(const fluid::FluidRecipe& recipe)
{
    if (recipe.kind == FluidKind::Liquid) return FluidShading::Liquid;
    return recipe.render.shading == FluidShading::Liquid ? FluidShading::Smoke : recipe.render.shading;
}

/// @name 色のグラデーション

math::Vector3 EvaluateRamp(const fluid::FluidColorRamp& ramp, float t)
{
    const auto& stops = ramp.stops;
    if (t <= stops[0].position) return stops[0].color;
    for (int i = 1; i < fluid::kFluidRampStops; ++i) {
        const fluid::FluidColorStop& a = stops[static_cast<std::size_t>(i) - 1];
        const fluid::FluidColorStop& b = stops[static_cast<std::size_t>(i)];
        if (t <= b.position) {
            const float span = b.position - a.position;
            const float u = span > 1.0e-6f ? (t - a.position) / span : 1.0f;
            return math::Vector3::Lerp(a.color, b.color, u);
        }
    }
    return stops[fluid::kFluidRampStops - 1].color;
}

float LinearToSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float SrgbToLinear(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

/// @brief Albedo Ramp はリニアで持つが、Lit Color / Liquid Color は sRGB。見た目で比べられるよう画面では sRGB に直す。
/// @brief 発光のグラデーションは HDR なので 1 で切るだけにする (白飛びする芯も «白» として読める)。
ImVec4 RampDisplayColor(const fluid::FluidColorRamp& ramp, float t, bool perceptual)
{
    const math::Vector3 color = EvaluateRamp(ramp, t);
    if (perceptual) return { LinearToSrgb(color.x), LinearToSrgb(color.y), LinearToSrgb(color.z), 1.0f };
    return { (std::min)(color.x, 1.0f), (std::min)(color.y, 1.0f), (std::min)(color.z, 1.0f), 1.0f };
}

/// @name テクスチャ発生源のマスク (表示用)

/// @brief 16×16 の粗さで絵柄の見当だけ付ける。256² を毎フレーム描くとパネルが重くなる。
constexpr int kMaskPreviewCells = 16;
static_assert(fluid::kFluidSourceMaskSize % kMaskPreviewCells == 0);

struct SourceMaskPreview {
    std::string path;
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point checkedAt{};
    bool checked = false;
    bool loaded = false;
    std::string error;
    std::array<float, static_cast<std::size_t>(kMaskPreviewCells) * kMaskPreviewCells> cells{};
};

/// @brief 発生源の上限ぶんあれば足りる。溢れたら古いものから捨てる。
std::vector<SourceMaskPreview> s_maskPreviews;

void RebuildMaskPreview(SourceMaskPreview& preview)
{
    fluid::FluidSourceMask mask;
    preview.error.clear();
    preview.loaded = asset::LoadFluidSourceMask(preview.path, mask, &preview.error) && mask.IsValid();
    preview.cells.fill(0.0f);
    if (!preview.loaded) return;
    constexpr int kBlock = fluid::kFluidSourceMaskSize / kMaskPreviewCells;
    for (int y = 0; y < fluid::kFluidSourceMaskSize; ++y) {
        for (int x = 0; x < fluid::kFluidSourceMaskSize; ++x) {
            preview.cells[static_cast<std::size_t>((y / kBlock) * kMaskPreviewCells + x / kBlock)] +=
                mask.values[static_cast<std::size_t>(y) * fluid::kFluidSourceMaskSize + x];
        }
    }
    const float inverse = 1.0f / static_cast<float>(kBlock * kBlock);
    for (float& cell : preview.cells) cell *= inverse;
}

/// @brief 画像を描き直して保存したときにも追従させたいが、ディスクを見るのは 1 秒に 1 回まで。
/// @brief 返したポインタは次にこの関数を呼ぶまでしか使わないこと (追加で要素が動く)。
const SourceMaskPreview* FindSourceMaskPreview(const std::string& path)
{
    if (path.empty()) return nullptr;
    auto it = std::find_if(s_maskPreviews.begin(), s_maskPreviews.end(),
                           [&path](const SourceMaskPreview& preview) { return preview.path == path; });
    if (it == s_maskPreviews.end()) {
        if (s_maskPreviews.size() >= static_cast<std::size_t>(fluid::kMaxFluidSources))
            s_maskPreviews.erase(s_maskPreviews.begin());
        SourceMaskPreview fresh;
        fresh.path = path;
        s_maskPreviews.push_back(std::move(fresh));
        it = s_maskPreviews.end() - 1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!it->checked || now - it->checkedAt >= std::chrono::seconds(1)) {
        /// @note Sprite 参照は «::sprite::» を落とした元画像を見る。付けたまま渡すと存在しない
        /// @note ファイルの時刻 (既定値) で固まり、絵を描き直しても縮小画がそのままになる。
        std::string imagePath;
        std::string spriteToken;
        const bool isSprite = asset::ParseSpriteReference(path, imagePath, spriteToken);
        const std::string imageFile = asset::AssetManager::ResolveAssetPath(imagePath);
        std::filesystem::file_time_type stamp = FileStamp(imageFile);
        /// @note 切り直し (.meta) でも矩形が変わる。新しいほうを代表にする。
        if (isSprite) stamp = (std::max)(stamp, FileStamp(imageFile + ".meta"));
        if (!it->checked || stamp != it->stamp) {
            it->stamp = stamp;
            RebuildMaskPreview(*it);
        }
        it->checked = true;
        it->checkedAt = now;
    }
    return &*it;
}

/// @name シミュレーション

bool EditGasSettings(fluid::FluidRecipe& recipe)
{
    fluid::FluidGasSettings& gas = recipe.gas;
    bool changed = false;
    bool autoGrid = gas.resolution <= 0;
    if (ImGui::Checkbox("Auto Grid", &autoGrid)) {
        gas.resolution = autoGrid ? 0 : fluid::ResolveGasResolution(recipe);
        changed = true;
    }
    Tooltip("格子をコマの解像度に合わせます (上限 256)。\n"
            "格子がコマより粗いと、大きく焼いても輪郭は格子の粗さのままぼやけます。");
    if (autoGrid) {
        ImGui::SameLine();
        ImGui::TextDisabled("= %d", fluid::ResolveGasResolution(recipe));
    } else {
        changed |= ImGui::SliderInt("Grid Resolution", &gas.resolution, 32, 512);
        Tooltip("解く格子の 1 辺。倍にすると焼き時間はおよそ 8 倍になります。\n"
                "256 を超える細かさは Look の Detail で出す方が速く、見た目もほぼ同じです。");
    }
    changed |= ImGui::DragFloat("Buoyancy", &gas.buoyancy, 0.01f, -10.0f, 20.0f);
    Tooltip("温度による上昇。負にすると冷気のように沈みます。");
    changed |= ImGui::DragFloat("Weight", &gas.weight, 0.01f, 0.0f, 10.0f);
    Tooltip("密度による下降。砂煙や重い煙で上げます。");
    changed |= ImGui::DragFloat("Vorticity", &gas.vorticity, 0.01f, 0.0f, 4.0f);
    Tooltip("渦度保存。数値計算で消えてしまう細かい巻き込みを戻します。上げすぎると泡立って見えます。");
    changed |= ImGui::DragFloat("Turbulence", &gas.turbulence, 0.01f, 0.0f, 10.0f);
    changed |= ImGui::DragFloat("Turbulence Scale", &gas.turbulenceScale, 0.05f, 0.1f, 32.0f);
    changed |= ImGui::DragFloat("Density Dissipation", &gas.densityDissipation, 0.01f, 0.0f, 10.0f);
    Tooltip("1 秒あたりの薄れ方。蒸気は大きく、インクは小さく。");
    changed |= ImGui::DragFloat("Cooling", &gas.temperatureDissipation, 0.01f, 0.0f, 20.0f);
    Tooltip("1 秒あたりの冷め方。炎の長さはここで決まります (大きいほど短い炎)。");
    changed |= ImGui::DragFloat("Velocity Damping", &gas.velocityDamping, 0.01f, 0.0f, 10.0f);
    changed |= widgets::DragVec3("Wind", gas.wind, 0.01f);
    changed |= ImGui::Checkbox("Floor", &gas.floor);
    Tooltip("下端を床として閉じます。砂煙・煙だまりのように地面を這う流れになります。");
    changed |= ImGui::SliderInt("Pressure Iterations", &gas.pressureIterations, 8, 200);
    Tooltip("圧力の反復回数。少ないと煙が «縮んだり膨らんだり» します。");
    changed |= ImGui::Checkbox("Sharp Advection", &gas.sharpAdvection);
    Tooltip("MacCormack 移流。煙の輪郭がコマを追ってもぼやけません。");
    changed |= ImGui::DragFloat("Detail Period", &gas.detailPeriod, 0.01f, 0.05f, 10.0f, "%.2f s");
    Tooltip("細部のノイズを流れに乗せて運ぶ周期。\n"
            "長いほど流れに引き伸ばされて筋っぽく、短いほど入れ替わりの揺らぎが見えます。");
    if (ImGui::TreeNode("Combustion")) {
        ImGui::TextDisabled("Source の Fuel が無ければ何も起きません。爆風は Expansion が作ります。");
        changed |= ImGui::DragFloat("Ignition Temperature", &gas.ignitionTemperature, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat("Burn Rate", &gas.burnRate, 0.05f, 0.0f, 50.0f);
        changed |= ImGui::DragFloat("Heat", &gas.burnHeat, 0.05f, 0.0f, 20.0f);
        changed |= ImGui::DragFloat("Smoke", &gas.burnSmoke, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat("Expansion", &gas.burnExpansion, 0.05f, 0.0f, 20.0f);
        ImGui::TreePop();
    }
    return changed;
}

bool EditLiquidSettings(fluid::FluidLiquidSettings& liquid)
{
    bool changed = false;
    changed |= ImGui::DragInt("Max Particles", &liquid.maxParticles, 10.0f, 100, 20000);
    changed |= ImGui::DragFloat("Particle Radius", &liquid.particleRadius, 0.0005f, 0.004f, 0.05f, "%.4f");
    Tooltip("液面の細かさ。小さいほど細い糸や小さな雫が出ますが、同じ量に多くの粒子が要ります。");
    changed |= ImGui::DragFloat("Gravity", &liquid.gravity, 0.05f, -20.0f, 40.0f);
    changed |= ImGui::SliderFloat("Viscosity", &liquid.viscosity, 0.0f, 1.0f);
    Tooltip("とろみ。水 0.02 / 血 0.15 / 溶岩 0.5 前後。");
    changed |= ImGui::SliderFloat("Cohesion", &liquid.cohesion, 0.0f, 1.0f);
    Tooltip("まとまり。0 で飛沫が霧状に散り、1 で雫になって固まります。");
    changed |= ImGui::SliderInt("Solver Iterations", &liquid.solverIterations, 1, 10);
    changed |= ImGui::Checkbox("Floor", &liquid.floor);
    if (liquid.floor) {
        changed |= ImGui::DragFloat("Floor Height", &liquid.floorHeight, 0.01f, -1.5f, 1.0f);
        changed |= ImGui::SliderFloat("Floor Friction", &liquid.floorFriction, 0.0f, 2.0f);
    }
    changed |= ImGui::DragFloat("Particle Lifetime", &liquid.particleLifetime, 0.01f, 0.0f, 30.0f,
                                liquid.particleLifetime <= 0.0f ? "infinite" : "%.2f s");
    Tooltip("正にすると飛沫が細りながら消えます。");
    return changed;
}

/// @name 部品 (発生源・力・障害物)

constexpr const char* kShapeNames[] = { "Sphere", "Box", "Cone", "Ring", "Texture", "Capsule", "Cylinder" };
constexpr const char* kForceTypeNames[] = { "Wind", "Attract", "Repulse", "Vortex", "Noise", "Drag" };
constexpr const char* kColliderShapeNames[] = { "Sphere", "Box", "Plane", "Capsule", "Cylinder" };
/// @brief ".sprite" = Sprite のコマも受ける (LoadFluidSourceMask が矩形で切り抜く)。FluidRecipe.cpp の
/// @brief SetFileExtensions と揃えること — Inspector のリフレクション欄が同じ値を使う。
constexpr const char* kSourceTextureFilter = ".sprite,.png,.tga,.jpg,.jpeg";

const char* ColliderShapeName(fluid::FluidColliderShape shape)
{
    const int index = static_cast<int>(shape);
    return index >= 0 && index < IM_ARRAYSIZE(kColliderShapeNames) ? kColliderShapeNames[index] : "?";
}

const char* ShapeName(fluid::FluidSourceShape shape)
{
    const int index = static_cast<int>(shape);
    return index >= 0 && index < IM_ARRAYSIZE(kShapeNames) ? kShapeNames[index] : "?";
}

const char* ForceTypeName(fluid::FluidForceType type)
{
    const int index = static_cast<int>(type);
    return index >= 0 && index < IM_ARRAYSIZE(kForceTypeNames) ? kForceTypeNames[index] : "?";
}

bool DragPair(const char* label, float& a, float& b, float speed, float min, float max)
{
    float values[2] = { a, b };
    if (!ImGui::DragFloat2(label, values, speed, min, max, "%.3f")) return false;
    a = values[0];
    b = values[1];
    return true;
}

/// @brief 部品の動き (時刻 → 中心からのずれ)。inheritTooltip が null なら «速さを引き継ぐ» を出さない (力には効かない)。
bool EditMotion(fluid::FluidMotion& motion, const char* inheritTooltip)
{
    bool changed = false;
    std::vector<fluid::FluidMotionKey>& keys = motion.keys;
    const bool open = ImGui::TreeNodeEx("##motion", ImGuiTreeNodeFlags_None, "Motion (%d keys)",
                                        static_cast<int>(keys.size()));
    Tooltip("時刻 → 中心からのずれ。キーの間は直線でつなぎ、先頭より前・末尾より後は端のキーで止まります。\n"
            "時刻はレシピの時計 (Warmup を含む) です。");
    if (!open) return false;

    if (inheritTooltip != nullptr) {
        changed |= ImGui::Checkbox("Inherit Velocity", &motion.inheritVelocity);
        Tooltip(inheritTooltip);
    }

    int removeIndex = -1;
    bool sortKeys = false;
    if (!keys.empty()
        && ImGui::BeginTable("##keys", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (int k = 0; k < static_cast<int>(keys.size()); ++k) {
            fluid::FluidMotionKey& key = keys[static_cast<std::size_t>(k)];
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##time", &key.time, 0.01f, 0.0f, 60.0f, "%.2f s");
            /// @note 並べ替えは手を離してから。ドラッグ中に行が入れ替わると、掴んでいる欄が別のキーを指してしまう。
            if (ImGui::IsItemDeactivatedAfterEdit()) sortKeys = true;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= widgets::DragAxes("##offset", key.offset, 0.01f, -2.0f, 2.0f);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X")) removeIndex = k;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const bool full = keys.size() >= static_cast<std::size_t>(fluid::kMaxFluidMotionKeys);
    ImGui::BeginDisabled(full);
    if (ImGui::SmallButton("Add Key")) {
        fluid::FluidMotionKey key;
        if (!keys.empty()) {
            key = keys.back();
            key.time += 0.25f;
        }
        keys.push_back(key);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", static_cast<int>(keys.size()), fluid::kMaxFluidMotionKeys);

    if (removeIndex >= 0) {
        keys.erase(keys.begin() + removeIndex);
        changed = true;
    }
    if (sortKeys) {
        std::stable_sort(keys.begin(), keys.end(),
                         [](const fluid::FluidMotionKey& a, const fluid::FluidMotionKey& b) { return a.time < b.time; });
        changed = true;
    }
    ImGui::TreePop();
    return changed;
}

/// @brief 部品の量のエンベロープ (時刻 → 倍率)。header は畳んだ見出しに出す «何に掛かるか»。
bool EditAmount(fluid::FluidAmount& amount, const char* header, const char* tooltip)
{
    bool changed = false;
    std::vector<fluid::FluidAmountKey>& keys = amount.keys;
    const bool open = ImGui::TreeNodeEx("##amount", ImGuiTreeNodeFlags_None, "%s (%d keys)", header,
                                        static_cast<int>(keys.size()));
    Tooltip(tooltip);
    if (!open) return false;

    int removeIndex = -1;
    bool sortKeys = false;
    if (!keys.empty()
        && ImGui::BeginTable("##amountkeys", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Scale", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (int k = 0; k < static_cast<int>(keys.size()); ++k) {
            fluid::FluidAmountKey& key = keys[static_cast<std::size_t>(k)];
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##time", &key.time, 0.01f, 0.0f, 60.0f, "%.2f s");
            /// @note 並べ替えは手を離してから (EditMotion と同じ理由: ドラッグ中に行が入れ替わると掴んだ欄がずれる)。
            if (ImGui::IsItemDeactivatedAfterEdit()) sortKeys = true;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##scale", &key.scale, 0.01f, 0.0f, 8.0f, "x%.2f");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X")) removeIndex = k;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const bool full = keys.size() >= static_cast<std::size_t>(fluid::kMaxFluidAmountKeys);
    ImGui::BeginDisabled(full);
    if (ImGui::SmallButton("Add Key##amount")) {
        fluid::FluidAmountKey key;
        if (!keys.empty()) {
            key = keys.back();
            key.time += 0.25f;
        }
        keys.push_back(key);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", static_cast<int>(keys.size()), fluid::kMaxFluidAmountKeys);

    if (removeIndex >= 0) {
        keys.erase(keys.begin() + removeIndex);
        changed = true;
    }
    if (sortKeys) {
        std::stable_sort(keys.begin(), keys.end(),
                         [](const fluid::FluidAmountKey& a, const fluid::FluidAmountKey& b) { return a.time < b.time; });
        changed = true;
    }
    ImGui::TreePop();
    return changed;
}

fluid::FluidSource MakeDefaultSource(FluidKind kind, fluid::FluidSourceShape shape)
{
    using Shape = fluid::FluidSourceShape;
    fluid::FluidSource source;
    source.shape = shape;
    const bool liquid = kind == FluidKind::Liquid;
    if (liquid) {
        /// @note 旧 Emitter の既定。重なって生まれた粒子は押し返されて弾けるため、0.15 秒に均して出す。
        source.velocity = { 0.0f, 2.5f, 0.0f };
        source.spread = 0.5f;
        source.count = 600;
        source.duration = 0.15f;
    }
    const float base = liquid ? 0.08f : 0.18f;
    switch (shape) {
    case Shape::Sphere: source.size = { base, base, base }; break;
    case Shape::Box:    source.size = { base * 1.5f, base * 0.5f, base * 1.5f }; break;
    case Shape::Cone:   source.size = { base, base * 3.0f, base }; break;
    case Shape::Ring:
        /// @note 2D は奥行きを見ないので、法線を奥へ向けておくと画面でもそのまま輪に見える。
        source.size = { base * 2.0f, base * 0.35f, base * 0.35f };
        source.direction = { 0.0f, 0.0f, 1.0f };
        break;
    case Shape::Texture:
        /// @note 手前向きの板にしておくと、画像が 2D でもそのまま正面に見える。
        source.center = { 0.0f, 0.0f, 0.0f };
        source.size = { base * 2.5f, base * 2.5f, base * 0.5f };
        source.direction = { 0.0f, 0.0f, 1.0f };
        break;
    case Shape::Capsule:
        /// @note 棒を寝かせた既定 (2D でも «横に伸びた形» として読める)。細く長いほうがカプセルらしい。
        source.size = { base * 0.5f, base * 2.0f, base * 0.5f };
        source.direction = { 1.0f, 0.0f, 0.0f };
        break;
    case Shape::Cylinder:
        /// @note 立ち上る柱。軸は上向きのままにする。
        source.size = { base, base * 2.5f, base };
        break;
    }
    return source;
}

fluid::FluidCollider MakeDefaultCollider(fluid::FluidColliderShape shape)
{
    using Shape = fluid::FluidColliderShape;
    fluid::FluidCollider collider;
    collider.shape = shape;
    switch (shape) {
    case Shape::Sphere:
        collider.center = { 0.0f, 0.1f, 0.0f };
        collider.size = { 0.2f, 0.2f, 0.2f };
        break;
    case Shape::Box:
        collider.center = { 0.0f, 0.1f, 0.0f };
        collider.size = { 0.3f, 0.08f, 0.3f };
        break;
    case Shape::Plane:
        /// @note 床は Simulation の Floor が持つので、既定は床と見分けの付く右の壁にする。
        collider.center = { 0.7f, 0.0f, 0.0f };
        collider.direction = { -1.0f, 0.0f, 0.0f };
        break;
    case Shape::Capsule:
        /// @note 横棒。煙や飛沫が «乗り越える» のが分かる置き方にする。
        collider.center = { 0.0f, 0.1f, 0.0f };
        collider.size = { 0.08f, 0.3f, 0.08f };
        collider.direction = { 1.0f, 0.0f, 0.0f };
        break;
    case Shape::Cylinder:
        collider.center = { 0.0f, 0.1f, 0.0f };
        collider.size = { 0.15f, 0.25f, 0.15f };
        break;
    }
    return collider;
}

fluid::FluidForce MakeDefaultForce(fluid::FluidForceType type)
{
    using Type = fluid::FluidForceType;
    fluid::FluidForce force;
    force.type = type;
    switch (type) {
    case Type::Wind:    force.direction = { 1.0f, 0.0f, 0.0f }; force.strength = 2.0f; break;
    case Type::Attract: force.strength = 3.0f; force.radius = 0.8f; break;
    case Type::Repulse: force.strength = 3.0f; force.radius = 0.5f; break;
    case Type::Vortex:  force.direction = { 0.0f, 0.0f, 1.0f }; force.strength = 3.0f; force.radius = 0.8f; break;
    case Type::Noise:   force.strength = 2.0f; break;
    case Type::Drag:    force.strength = 1.5f; break;
    }
    return force;
}

/// @brief 画像が読めているかと、縮めたマスクの見当 (16×16) を出す。
void DrawTextureSourceStatus(const std::string& texture)
{
    if (texture.empty()) {
        ImGui::TextColored(kWarnColor, "画像が未設定なので板の形で湧きます。");
        return;
    }
    const SourceMaskPreview* preview = FindSourceMaskPreview(texture);
    if (preview == nullptr || !preview->loaded) {
        ImGui::TextColored(kErrorColor, "画像を読めないので板の形で湧きます。");
        if (preview != nullptr && !preview->error.empty()) Tooltip(preview->error.c_str());
        return;
    }
    const float cell = (std::max)(std::floor(ImGui::GetFrameHeight() * 2.0f / kMaskPreviewCells), 1.0f);
    const float extent = cell * static_cast<float>(kMaskPreviewCells);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ extent, extent });
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int y = 0; y < kMaskPreviewCells; ++y) {
        for (int x = 0; x < kMaskPreviewCells; ++x) {
            const float value =
                std::clamp(preview->cells[static_cast<std::size_t>(y) * kMaskPreviewCells + x], 0.0f, 1.0f);
            const int gray = static_cast<int>(value * 255.0f + 0.5f);
            const ImVec2 a{ origin.x + static_cast<float>(x) * cell, origin.y + static_cast<float>(y) * cell };
            drawList->AddRectFilled(a, { a.x + cell, a.y + cell }, IM_COL32(gray, gray, gray, 255));
        }
    }
    drawList->AddRect(origin, { origin.x + extent, origin.y + extent }, ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::SameLine();
    std::string imagePath;
    std::string spriteToken;
    const bool isSprite = asset::ParseSpriteReference(texture, imagePath, spriteToken);
    ImGui::TextDisabled(isSprite ? "濃さ = 輝度 x α\n(コマを切り抜いて 256x256 に縮めます)"
                                 : "濃さ = 輝度 x α\n(256x256 に縮めて貼ります)");
}

/// @brief 色の鍵は Look の Albedo Ramp が点いているときだけ効く。切れていても値は残す (点けたときの配色になる)。
bool EditSourceColorKey(fluid::FluidSource& source, bool liquid, const fluid::FluidRenderSettings& look)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float fullWidth = ImGui::CalcItemWidth();
    if (look.useAlbedoRamp) {
        const float swatchSide = ImGui::GetFrameHeight();
        ImGui::ColorButton("##color_key_swatch",
                           RampDisplayColor(look.albedoRamp, std::clamp(source.colorKey, 0.0f, 1.0f), true),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, { swatchSide, swatchSide });
        Tooltip(liquid ? "この発生源が撃ち出す粒子の色 (Look > Albedo Ramp の Color Key の位置)。\n"
                         "粒子は一生この色を持ちます。"
                       : "この発生源が注ぐ煙の色 (Look > Albedo Ramp の Color Key の位置)。\n"
                         "色は煙に乗って運ばれ、複数の発生源の煙が混ざると色も混ざります。");
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(
            (std::max)(fullWidth - swatchSide - style.ItemInnerSpacing.x, ImGui::GetFontSize() * 3.0f));
    }
    const bool changed = ImGui::SliderFloat("Color Key", &source.colorKey, 0.0f, 1.0f,
                                            look.useAlbedoRamp ? "%.2f" : "%.2f (ramp off)",
                                            ImGuiSliderFlags_AlwaysClamp);
    if (look.useAlbedoRamp)
        Tooltip(liquid ? "Look > Albedo Ramp のどの色で描くか (0〜1)。粒子ごとに持つので、水と血を 1 枚に焼き分けられます。"
                       : "Look > Albedo Ramp のどの色で描くか (0〜1)。\n"
                         "複数の発生源の煙が混ざると色も混ざります (量で重み付けした平均)。");
    else
        Tooltip("Look > Albedo Ramp のどの色で描くか (0〜1)。\n"
                "Look > Use Albedo Ramp が切れているので、今は見た目に効きません。");
    return changed;
}

/// @brief 液体のソルバーは «詰まりすぎ» た発生源を黙って相似に広げてから湧かせる。広げたことを出さないと
/// @brief Inspector の半径とプレビューに写る大きさが食い違って見えるので、広げた分をここで知らせる。
/// @brief 2D (プレビュー / 2D ベイク) と 3D (Volume ベイク) では入る数が違うので倍率も変わりうる。
void DrawLiquidEmitScaleNote(const fluid::FluidSource& source, const fluid::FluidLiquidSettings& liquid)
{
    const float flat = fluid::FluidLiquidEmitScale(source, liquid, false);
    const float volume = fluid::FluidLiquidEmitScale(source, liquid, true);
    if (flat <= 1.01f && volume <= 1.01f) return;
    const bool sphere = source.shape == fluid::FluidSourceShape::Sphere;
    if (std::fabs(volume - flat) <= 0.01f) {
        if (sphere)
            ImGui::TextColored(kWarnColor, "詰まりすぎ: 実際は ×%.2f (半径 %.3f) で湧きます", flat,
                               source.size.x * flat);
        else
            ImGui::TextColored(kWarnColor, "詰まりすぎ: 実際は ×%.2f の大きさで湧きます", flat);
    } else if (sphere) {
        ImGui::TextColored(kWarnColor, "詰まりすぎ: 実際は 2D ×%.2f (半径 %.3f) / 3D ×%.2f (半径 %.3f) で湧きます",
                           flat, source.size.x * flat, volume, source.size.x * volume);
    } else {
        ImGui::TextColored(kWarnColor, "詰まりすぎ: 実際は 2D ×%.2f / 3D ×%.2f の大きさで湧きます", flat, volume);
    }
    Tooltip("Count の粒子は、この形には静止密度で収まりません。重ねたまま湧かせると弾け飛ぶので、\n"
            "ソルバーが形を中心から相似に広げてから湧かせます (上の倍率と半径は広げた後の値)。\n"
            "大きさを自分で決めたいときは Count を減らすか、形を広げてください。");
}

bool EditSourceBody(fluid::FluidSource& source, FluidKind kind, const fluid::FluidLiquidSettings& liquidSettings,
                    const fluid::FluidRenderSettings& look, const std::string& projectRoot)
{
    using Shape = fluid::FluidSourceShape;
    const bool liquid = kind == FluidKind::Liquid;
    bool changed = false;
    changed |= widgets::InputString("Name", source.name, 64);
    Tooltip("見分けるための名前です (解き方には関係しません)。空なら «Source N (形)» と出します。");
    int shape = static_cast<int>(source.shape);
    if (ImGui::Combo("Shape", &shape, kShapeNames, IM_ARRAYSIZE(kShapeNames))) {
        const Shape previous = source.shape;
        source.shape = static_cast<Shape>(shape);
        /// @note Cone / Ring の上向きのまま板にすると、2D では真横から見た細い帯になり画像が見えない。
        if (source.shape == Shape::Texture && previous != Shape::Texture) source.direction = { 0.0f, 0.0f, 1.0f };
        changed = true;
    }
    changed |= widgets::DragVec3("Center", source.center, 0.01f, -1.5f, 1.5f);
    Tooltip("領域は各軸 [-1, 1]。y は上向きです。Cone は頂点、Texture は板の中心です。");
    switch (source.shape) {
    case Shape::Sphere:
        changed |= ImGui::DragFloat("Radius", &source.size.x, 0.002f, 0.005f, 1.5f, "%.3f");
        break;
    case Shape::Box:
        changed |= widgets::DragVec3("Half Size", source.size, 0.005f, 0.005f, 1.5f);
        break;
    case Shape::Cone:
        changed |= DragPair("Base Radius / Length", source.size.x, source.size.y, 0.002f, 0.005f, 2.0f);
        Tooltip("頂点が Center、Direction へ開きます (噴流・火炎放射)。");
        break;
    case Shape::Ring:
        changed |= DragPair("Ring Radius / Tube Radius", source.size.x, source.size.y, 0.002f, 0.005f, 1.5f);
        Tooltip("輪の半径と、管の太さ (半径) です (衝撃波・煙の輪)。");
        break;
    case Shape::Texture:
        changed |= widgets::AssetPathField("Texture", source.texture, kSourceTextureFilter, projectRoot);
        DrawTextureSourceStatus(source.texture);
        changed |= DragPair("Half Width / Half Height", source.size.x, source.size.y, 0.002f, 0.005f, 1.5f);
        Tooltip("板の半分の幅と高さ。画像は 256x256 に縮めて貼るので、縦横比はここで合わせます。");
        changed |= ImGui::DragFloat("Half Thickness", &source.size.z, 0.002f, 0.005f, 1.5f, "%.3f");
        Tooltip("板の厚みの半分 (法線の向き)。3D で焼くときの奥行きです。\n"
                "2D で法線が手前向きなら効きません。");
        break;
    case Shape::Capsule:
        changed |= DragPair("Radius / Half Length", source.size.x, source.size.y, 0.002f, 0.005f, 1.5f);
        Tooltip("芯の線分の半分の長さと、その周りの肉の半径です。両端は半球になります (腕・脚・棒・パイプ)。\n"
                "端から端までは 2 x (Half Length + Radius) です。");
        break;
    case Shape::Cylinder:
        changed |= DragPair("Radius / Half Height", source.size.x, source.size.y, 0.002f, 0.005f, 1.5f);
        Tooltip("軸方向の半分の高さと、柱の半径です。両端は平ら (煙突・通気口から立ち上る柱)。\n"
                "端の近くだけ注ぐ量がなめらかに 0 へ落ちます。");
        break;
    }
    if (source.shape == Shape::Cone || source.shape == Shape::Ring || source.shape == Shape::Capsule
        || source.shape == Shape::Cylinder) {
        changed |= widgets::DragVec3("Direction", source.direction, 0.01f, -1.0f, 1.0f);
        if (source.shape == Shape::Cone) {
            Tooltip("開く向き。長さは問いません (0 なら上向き)。");
        } else if (source.shape == Shape::Ring) {
            Tooltip("輪の法線。長さは問いません (0 なら上向き)。\n"
                    "2D は奥行きを見ないため、(0,0,1) なら画面で輪に、上向きなら左右 2 つの塊に見えます。");
        } else {
            Tooltip("芯の軸。長さは問いません (0 なら上向き)。\n"
                    "2D は奥行きを見ないため、軸を奥へ倒すほど断面が細い楕円へ縮みます\n"
                    "(奥行き向きの棒を 2D で焼くなら Sphere のほうが素直です)。");
        }
    } else if (source.shape == Shape::Texture) {
        changed |= widgets::DragVec3("Normal", source.direction, 0.01f, -1.0f, 1.0f);
        Tooltip("板の法線 (画像の表の向き)。長さは問いません (0 なら手前向き)。\n"
                "(0,0,1) で画像がそのまま正面に見えます。2D は奥行きを見ないため、\n"
                "法線を画面内へ倒すと板を真横から見た帯になります。");
    }
    if (liquid) DrawLiquidEmitScaleNote(source, liquidSettings);

    if (!liquid) {
        changed |= ImGui::DragFloat("Density", &source.density, 0.05f, 0.0f, 100.0f);
        Tooltip("1 秒あたりに足す煙の量。");
        changed |= ImGui::DragFloat("Temperature", &source.temperature, 0.05f, 0.0f, 100.0f);
        Tooltip("1 秒あたりに足す熱。浮力 (Simulation > Buoyancy) と炎の明るさになります。");
        changed |= ImGui::DragFloat("Fuel", &source.fuel, 0.05f, 0.0f, 100.0f);
        Tooltip("1 秒あたりに足す燃料。Simulation > Combustion で燃え、熱・煤・膨張 (爆風) になります。");
        changed |= ImGui::SliderFloat("Noise", &source.noise, 0.0f, 1.0f);
        Tooltip("注入量の揺らぎ。0 だと左右対称な «きのこ雲» になりがちです。");
        changed |= widgets::DragVec3("Flow Velocity", source.velocity, 0.01f);
        Tooltip("発生源の中の流速 [領域単位/秒]。0 なら流速には触りません。");
    } else {
        changed |= widgets::DragVec3("Launch Velocity", source.velocity, 0.01f);
        Tooltip("撃ち出す速度 [領域単位/秒]。");
        changed |= ImGui::SliderFloat("Spread", &source.spread, 0.0f, 1.0f);
        Tooltip("速度のばらつき。1 で全方位に散ります (血しぶき)。");
        changed |= ImGui::DragInt("Count", &source.count, 5.0f, 1, 20000);
        Tooltip("撃ち出す総数。Simulation の Max Particles を超えた分は出ません。");
    }
    changed |= EditSourceColorKey(source, liquid, look);

    changed |= ImGui::DragFloat("Start", &source.startTime, 0.01f, 0.0f, 60.0f, "%.2f s");
    if (!liquid) {
        changed |= ImGui::DragFloat("Duration", &source.duration, 0.01f, 0.0f, 60.0f,
                                    source.duration <= 0.0f ? "continuous" : "%.2f s");
        Tooltip("注ぎ続ける時間。0 は最後まで出し続けます。");
    } else {
        changed |= ImGui::DragFloat("Duration", &source.duration, 0.01f, 0.0f, 60.0f,
                                    source.duration <= 0.0f ? "burst" : "%.2f s");
        Tooltip("0 は Start に一斉に出します。重なって生まれた粒子は押し返されて弾けるので、\n"
                "塊として飛ばしたいときは 0.1〜0.2 秒に分けて出します。");
    }
    changed |= EditMotion(source.motion, liquid ? "動く速さを撃ち出す速度に足します (振りながら撒く)。"
                                                : "動く速さを流速に足します。動く発生源が周りの煙を引きずります。");
    /// @note 掛かる先が無い液体では出さない (Density / Temperature / Fuel を隠すのと同じ理由)。
    if (!liquid)
        changed |= EditAmount(source.amount, "Amount",
                              "時刻 → Density / Temperature / Fuel に掛かる倍率。キーが無ければ 1 倍のままです。\n"
                              "キーの間は直線でつなぎ、先頭より前・末尾より後は端のキーで止まります。\n"
                              "流速と位置には掛かりません (勢いの向きは Flow Velocity と Motion が持ちます)。\n"
                              "噴き出して勢いが落ちる煙は 0 秒 x1、0.3 秒 x0.2 の 2 キーで作れます。");
    return changed;
}

bool EditForceBody(fluid::FluidForce& force)
{
    using Type = fluid::FluidForceType;
    bool changed = false;
    int type = static_cast<int>(force.type);
    if (ImGui::Combo("Type", &type, kForceTypeNames, IM_ARRAYSIZE(kForceTypeNames))) {
        force.type = static_cast<Type>(type);
        changed = true;
    }
    Tooltip("Wind   : 一方向へ押す\n"
            "Attract: 中心へ引く / Repulse: 中心から押し出す\n"
            "Vortex : 軸まわりに回す\n"
            "Noise  : 流れる渦のノイズで乱す\n"
            "Drag   : 流れを減速させる");
    changed |= widgets::InputString("Name", force.name, 64);

    if (force.type == Type::Drag) {
        changed |= ImGui::DragFloat("Damping", &force.strength, 0.01f, 0.0f, 50.0f, "%.2f /s");
        Tooltip("1 秒あたりの減衰係数。大きいほど早く止まります。");
    } else {
        changed |= ImGui::DragFloat("Strength", &force.strength, 0.01f, -50.0f, 50.0f, "%.2f");
        Tooltip("加速度 [領域単位/秒²]。負にすると向きが逆になります。");
    }
    changed |= ImGui::DragFloat("Radius", &force.radius, 0.005f, 0.0f, 4.0f,
                                force.radius <= 0.0f ? "everywhere" : "%.3f");
    Tooltip("影響半径。0 は領域全体に一様に効きます。");
    if (force.radius > 0.0f) {
        changed |= ImGui::DragFloat("Falloff", &force.falloffPower, 0.01f, 0.0f, 8.0f, "%.2f");
        Tooltip("influence = (1 - 距離 / Radius)^Falloff。0 で半径の内側は一様です。");
    }
    /// @note 中心は Attract / Repulse / Vortex と、半径を持つ力でしか意味を持たない。効かない欄は出さない。
    const bool usesCenter = force.radius > 0.0f || force.type == Type::Attract || force.type == Type::Repulse
                         || force.type == Type::Vortex;
    if (usesCenter) {
        changed |= widgets::DragVec3("Center", force.center, 0.01f, -1.5f, 1.5f);
        Tooltip("領域は各軸 [-1, 1]。y は上向きです。");
    }
    if (force.type == Type::Wind) {
        changed |= widgets::DragVec3("Direction", force.direction, 0.01f, -1.0f, 1.0f);
        Tooltip("押す向き。長さは問いません。");
    } else if (force.type == Type::Vortex) {
        changed |= widgets::DragVec3("Axis", force.direction, 0.01f, -1.0f, 1.0f);
        Tooltip("回転軸 (3D で焼くときに使います)。\n"
                "2D (プレビューと 2D ベイク) では常に画面の奥行き軸で回ります。逆回りは Strength を負に。");
    }
    if (force.type == Type::Noise) {
        changed |= ImGui::DragFloat("Noise Frequency", &force.noiseFrequency, 0.05f, 0.1f, 32.0f, "%.2f");
        Tooltip("細かさ (領域幅あたりの山の数)。");
        changed |= ImGui::DragFloat("Noise Speed", &force.noiseSpeed, 0.01f, 0.0f, 20.0f, "%.2f");
        Tooltip("ノイズが流れる速さ。0 で模様が止まります。");
    }
    changed |= ImGui::DragFloat("Start", &force.startTime, 0.01f, 0.0f, 60.0f, "%.2f s");
    changed |= ImGui::DragFloat("Duration", &force.duration, 0.01f, 0.0f, 60.0f,
                                force.duration <= 0.0f ? "always" : "%.2f s");
    Tooltip("効いている時間。0 はずっと効きます。");
    if (usesCenter) changed |= EditMotion(force.motion, nullptr);
    changed |= EditAmount(force.amount, "Amount",
                          force.type == Type::Drag
                              ? "時刻 → Damping に掛かる倍率。キーが無ければ 1 倍のままです。\n"
                                "キーの間は直線でつなぎ、先頭より前・末尾より後は端のキーで止まります。"
                              : "時刻 → Strength に掛かる倍率。キーが無ければ 1 倍のままです。\n"
                                "キーの間は直線でつなぎ、先頭より前・末尾より後は端のキーで止まります。\n"
                                "だんだん強まる渦は 0 秒 x0、1 秒 x1 の 2 キーで作れます。");
    return changed;
}

bool EditColliderBody(fluid::FluidCollider& collider, FluidKind kind)
{
    using Shape = fluid::FluidColliderShape;
    const bool liquid = kind == FluidKind::Liquid;
    bool changed = false;
    int shape = static_cast<int>(collider.shape);
    if (ImGui::Combo("Shape", &shape, kColliderShapeNames, IM_ARRAYSIZE(kColliderShapeNames))) {
        collider.shape = static_cast<Shape>(shape);
        changed = true;
    }
    Tooltip("Sphere  : 球\n"
            "Box     : 軸に沿った箱 (回転は持ちません)\n"
            "Plane   : 果てしなく広い面。法線の反対側がすべて固体です (壁・斜めの床)\n"
            "Capsule : 軸に沿った線分に肉を付けた形。両端は半球です (腕・脚・棒・パイプ)\n"
            "Cylinder: 軸に沿った平らな端の柱\n"
            "気体は中で流れが止まって煙も入らず、液体は粒子が表面の外へ押し出されます。\n"
            "床は Simulation の Floor で別に持ちます (障害物には含めません)。");
    changed |= widgets::InputString("Name", collider.name, 64);
    Tooltip("見分けるための名前です (解き方には関係しません)。空なら «Collider N (形)» と出します。");
    changed |= widgets::DragVec3("Center", collider.center, 0.01f, -1.5f, 1.5f);
    Tooltip(collider.shape == Shape::Plane ? "面が通る点。領域は各軸 [-1, 1]、y は上向きです。"
                                           : "領域は各軸 [-1, 1]。y は上向きです。");
    switch (collider.shape) {
    case Shape::Sphere:
        changed |= ImGui::DragFloat("Radius", &collider.size.x, 0.002f, 0.005f, 1.5f, "%.3f");
        break;
    case Shape::Box:
        changed |= widgets::DragVec3("Half Size", collider.size, 0.005f, 0.005f, 1.5f);
        break;
    case Shape::Plane:
        changed |= widgets::DragVec3("Normal", collider.direction, 0.01f, -1.0f, 1.0f);
        Tooltip("面の法線。こちら側が流体で、反対側はすべて固体です。長さは問いません (0 なら上向き)。\n"
                "2D は奥行きを見ないため、法線が真奥 / 真手前だと何も遮りません。");
        break;
    case Shape::Capsule:
    case Shape::Cylinder:
        changed |= DragPair(collider.shape == Shape::Capsule ? "Radius / Half Length" : "Radius / Half Height",
                            collider.size.x, collider.size.y, 0.002f, 0.005f, 1.5f);
        Tooltip(collider.shape == Shape::Capsule
                    ? "芯の線分の半分の長さと、その周りの肉の半径です。両端は半球になります。"
                    : "軸方向の半分の高さと、柱の半径です。両端は平らです。");
        changed |= widgets::DragVec3("Axis", collider.direction, 0.01f, -1.0f, 1.0f);
        Tooltip("芯の軸。長さは問いません (0 なら上向き)。\n"
                "2D は奥行きを見ないため、軸を奥へ倒すほど断面が細くなります。");
        break;
    }
    if (liquid) {
        changed |= ImGui::SliderFloat("Friction", &collider.friction, 0.0f, 2.0f);
        Tooltip("表面に沿った速度を落とす強さ。Simulation の Floor Friction と同じ意味です。");
    }
    changed |= ImGui::DragFloat("Start", &collider.startTime, 0.01f, 0.0f, 60.0f, "%.2f s");
    changed |= ImGui::DragFloat("Duration", &collider.duration, 0.01f, 0.0f, 60.0f,
                                collider.duration <= 0.0f ? "always" : "%.2f s");
    Tooltip("居る時間。0 はずっと居ます。");
    changed |= EditMotion(collider.motion, liquid
                                               ? "動く障害物が流体を押しのけます (動く速さで粒子を弾きます)。"
                                               : "動く障害物が流体を押しのけます (中の流速を動きの速度にします)。");
    return changed;
}

/// @name 見た目

/// @brief 4 点のグラデーション。位置は隣の点を越えられないようにして、昇順の約束を常に守る。
/// @brief id は発光 / Albedo の 2 本を同じ窓に並べても ID が衝突しないためのスコープ。
/// @brief perceptual なら色を sRGB で見せて編集し、リニアで保存する (Albedo)。そうでなければ HDR のリニアをそのまま (発光)。
bool EditColorRamp(fluid::FluidColorRamp& ramp, const char* id, const char* legend, bool perceptual)
{
    bool changed = false;
    ImGui::PushID(id);

    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barWidth = ImGui::CalcItemWidth();
    const float barHeight = ImGui::GetFrameHeight() * 0.6f;
    ImGui::Dummy({ barWidth, barHeight });
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr int kSegments = 48;
    for (int s = 0; s < kSegments; ++s) {
        const ImVec4 color = RampDisplayColor(ramp, (static_cast<float>(s) + 0.5f) / kSegments, perceptual);
        const float x0 = barMin.x + barWidth * static_cast<float>(s) / kSegments;
        const float x1 = barMin.x + barWidth * static_cast<float>(s + 1) / kSegments;
        drawList->AddRectFilled({ x0, barMin.y }, { x1 + 0.5f, barMin.y + barHeight },
                                ImGui::ColorConvertFloat4ToU32(color));
    }
    for (const fluid::FluidColorStop& stop : ramp.stops) {
        const float x = barMin.x + barWidth * std::clamp(stop.position, 0.0f, 1.0f);
        drawList->AddLine({ x, barMin.y }, { x, barMin.y + barHeight }, IM_COL32(255, 255, 255, 160));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", legend);

    auto& stops = ramp.stops;
    const float colorWidth = ImGui::CalcItemWidth() * 0.65f;
    for (int i = 0; i < fluid::kFluidRampStops; ++i) {
        fluid::FluidColorStop& stop = stops[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        float color[3] = { stop.color.x, stop.color.y, stop.color.z };
        if (perceptual)
            for (float& channel : color) channel = LinearToSrgb(channel);
        ImGui::SetNextItemWidth(colorWidth);
        const ImGuiColorEditFlags flags =
            perceptual ? ImGuiColorEditFlags_Float : (ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        if (ImGui::ColorEdit3("##color", color, flags)) {
            if (perceptual)
                stop.color = { SrgbToLinear(color[0]), SrgbToLinear(color[1]), SrgbToLinear(color[2]) };
            else
                stop.color = { (std::max)(color[0], 0.0f), (std::max)(color[1], 0.0f), (std::max)(color[2], 0.0f) };
            changed = true;
        }
        if (perceptual) Tooltip("見た目の色 (sRGB) で選び、リニアに直して保存します。");
        ImGui::SameLine();
        const float lo = i > 0 ? stops[static_cast<std::size_t>(i) - 1].position : 0.0f;
        const float hi = i + 1 < fluid::kFluidRampStops ? stops[static_cast<std::size_t>(i) + 1].position : 1.0f;
        ImGui::SetNextItemWidth((std::max)(ImGui::CalcItemWidth() - colorWidth - ImGui::GetStyle().ItemSpacing.x,
                                           ImGui::GetFontSize() * 3.0f));
        changed |= ImGui::SliderFloat("##position", &stop.position, lo, (std::max)(lo, hi), "%.2f",
                                      ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::TextDisabled("Stop %d", i);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

/// @brief Source / Force / Collider のどのリストかで型が変わる操作を 1 か所で振り分ける。
/// @brief fn は (std::vector<Part>& parts, std::size_t limit) -> bool の汎用ラムダ。
template <typename Fn>
bool VisitPartList(fluid::FluidRecipe& recipe, FluidSelectionKind list, Fn&& fn)
{
    switch (list) {
    case FluidSelectionKind::Source:
        return fn(recipe.sources, static_cast<std::size_t>(fluid::kMaxFluidSources));
    case FluidSelectionKind::Force:
        return fn(recipe.forces, static_cast<std::size_t>(fluid::kMaxFluidForces));
    case FluidSelectionKind::Collider:
        return fn(recipe.colliders, static_cast<std::size_t>(fluid::kMaxFluidColliders));
    default:
        return false;
    }
}

} /// @note namespace

/// @name 公開部品

bool EditSimulation(FluidWidgetContext& /*wc*/, fluid::FluidRecipe& recipe)
{
    bool changed = false;
    ImGui::PushID("fluid_simulation");
    static constexpr const char* kKinds[] = { "Gas (grid solver)", "Liquid (particles, PBF)" };
    int kind = static_cast<int>(recipe.kind);
    if (ImGui::Combo("Kind", &kind, kKinds, IM_ARRAYSIZE(kKinds))) {
        recipe.kind = static_cast<FluidKind>(kind);
        /// @note 種類を替えた直後に発生源が無いと、何も写らず «壊れた» ように見える。
        /// @note 発生源は両方の種類で共通なので、ある分はそのまま読み替える。
        if (recipe.sources.empty())
            recipe.sources.push_back(MakeDefaultSource(recipe.kind, fluid::FluidSourceShape::Sphere));
        changed = true;
    }
    if (recipe.kind == FluidKind::Gas) changed |= EditGasSettings(recipe);
    else                                changed |= EditLiquidSettings(recipe.liquid);
    changed |= ImGui::DragScalar("Seed", ImGuiDataType_U32, &recipe.seed, 1.0f);
    ImGui::PopID();
    return changed;
}

bool EditSource(FluidWidgetContext& wc, fluid::FluidRecipe& recipe, int index)
{
    if (index < 0 || index >= static_cast<int>(recipe.sources.size())) return false;
    ImGui::PushID("fluid_source");
    ImGui::PushID(index);
    const bool changed = EditSourceBody(recipe.sources[static_cast<std::size_t>(index)], recipe.kind, recipe.liquid,
                                        recipe.render, wc.projectRoot);
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

bool EditForce(FluidWidgetContext& /*wc*/, fluid::FluidRecipe& recipe, int index)
{
    if (index < 0 || index >= static_cast<int>(recipe.forces.size())) return false;
    ImGui::PushID("fluid_force");
    ImGui::PushID(index);
    const bool changed = EditForceBody(recipe.forces[static_cast<std::size_t>(index)]);
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

bool EditCollider(FluidWidgetContext& /*wc*/, fluid::FluidRecipe& recipe, int index)
{
    if (index < 0 || index >= static_cast<int>(recipe.colliders.size())) return false;
    ImGui::PushID("fluid_collider");
    ImGui::PushID(index);
    const bool changed = EditColliderBody(recipe.colliders[static_cast<std::size_t>(index)], recipe.kind);
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

bool EditLook(FluidWidgetContext& /*wc*/, fluid::FluidRecipe& recipe)
{
    bool changed = false;
    ImGui::PushID("fluid_look");
    fluid::FluidRenderSettings& look = recipe.render;
    if (recipe.kind == FluidKind::Gas) {
        static constexpr const char* kShadings[] = { "Smoke", "Fire", "Glow", "Distortion" };
        int shading = static_cast<int>(EffectiveShading(recipe));
        if (ImGui::Combo("Shading", &shading, kShadings, IM_ARRAYSIZE(kShadings))) {
            look.shading = static_cast<FluidShading>(shading);
            changed = true;
        }
        Tooltip("Smoke     : 自己影つきの煙 (Alpha)\n"
                "Fire      : 黒体放射の炎 + 煤 (Premultiplied)\n"
                "Glow      : 発光する靄 (Additive)\n"
                "Distortion: 流れを歪みマップへ (陽炎・衝撃波)\n"
                "どれも 2D・3D の両方で焼けます。");
        const FluidShading current = look.shading;
        /// @note Distortion は色を焼かない (RG が変位) ので、地の色のグラデーションは出さない。
        if (current != FluidShading::Distortion) {
            changed |= ImGui::Checkbox("Use Albedo Ramp", &look.useAlbedoRamp);
            Tooltip("発生源ごとの Color Key (0〜1) → 煙の地の色を 4 点のグラデーション (リニア) で決めます。\n"
                    "色は煙に乗って運ばれ、複数の発生源の煙が混ざると色も混ざります。\n"
                    "Glow では発光色になります。2D・3D どちらの焼きにも効きます。");
            if (look.useAlbedoRamp) changed |= EditColorRamp(look.albedoRamp, "albedo_ramp", "Color Key 0 → 1", true);
        }
        if (current == FluidShading::Smoke || current == FluidShading::Fire) {
            ImGui::BeginDisabled(look.useAlbedoRamp);
            changed |= widgets::ColorEdit4("Lit Color", look.smokeColor);
            ImGui::EndDisabled();
            if (look.useAlbedoRamp)
                ImGui::TextDisabled("地の色は Albedo Ramp が決めます (影の濃さには Lit と Shadow の明るさの比が効きます)。");
            changed |= widgets::ColorEdit4("Shadow Color", look.shadowColor);
            changed |= ImGui::DragFloat("Self Shadow", &look.selfShadow, 0.05f, 0.0f, 20.0f);
            changed |= widgets::DragVec3("Light Direction", look.lightDirection, 0.01f, -1.0f, 1.0f);
        }
        const bool emissive = current == FluidShading::Fire || current == FluidShading::Glow;
        if (current == FluidShading::Glow) {
            ImGui::BeginDisabled(look.useEmissionRamp || look.useAlbedoRamp);
            changed |= widgets::ColorEdit4("Glow Color", look.smokeColor);
            ImGui::EndDisabled();
            if (look.useAlbedoRamp && !look.useEmissionRamp)
                ImGui::TextDisabled("発光色は Albedo Ramp が決めます。");
        }
        changed |= ImGui::DragFloat("Opacity", &look.opacity, 0.05f, 0.0f, 50.0f);
        if (current != FluidShading::Distortion) {
            changed |= ImGui::SliderFloat("Detail", &look.detailStrength, 0.0f, 1.0f);
            Tooltip("格子より細かい起伏の強さ。流れに乗って動くノイズで、煙の縁のちぎれや\n"
                    "炎の細い舌を出します。0 で格子の解像度のまま。");
            changed |= ImGui::DragFloat("Detail Scale", &look.detailScale, 0.1f, 1.0f, 64.0f);
            Tooltip("細部の細かさ (領域幅あたりの山の数)。コマを大きく焼くほど上げられます。");
        }
        if (current == FluidShading::Fire) {
            ImGui::BeginDisabled(look.useEmissionRamp);
            changed |= ImGui::DragFloat("Fire Kelvin", &look.fireKelvin, 10.0f, 500.0f, 6000.0f, "%.0f K");
            ImGui::EndDisabled();
            Tooltip("温度 1.0 を何ケルビンとみなすか。焚き火 1200〜1500K、爆発の芯 1800K 以上。\n"
                    "Use Emission Ramp の間は色をグラデーションで決めるので使いません。");
            changed |= ImGui::DragFloat("Fire Intensity", &look.fireIntensity, 0.01f, 0.0f, 10.0f);
        }
        if (emissive) {
            changed |= ImGui::Checkbox("Use Emission Ramp", &look.useEmissionRamp);
            Tooltip("温度 (0〜1) → 発光の色を 4 点のグラデーションで決めます (HDR 可)。\n"
                    "外すと Fire は黒体放射、Glow は Glow Color で光ります。\n"
                    "3D で焼くときも、Blackbody Emission を切っていればこの色を使います。");
            if (look.useEmissionRamp) changed |= EditColorRamp(look.emissionRamp, "emission_ramp", "冷 → 熱", false);
        }
    } else {
        changed |= ImGui::Checkbox("Use Albedo Ramp", &look.useAlbedoRamp);
        Tooltip("発生源ごとの Color Key (0〜1) → 液の色を 4 点のグラデーション (リニア) で決めます。\n"
                "粒子は撃ち出した発生源の色を一生持つので、水と血を 1 枚に焼き分けられます。\n"
                "2D・3D どちらの焼きにも効きます。");
        if (look.useAlbedoRamp) changed |= EditColorRamp(look.albedoRamp, "albedo_ramp", "Color Key 0 → 1", true);
        ImGui::BeginDisabled(look.useAlbedoRamp);
        changed |= widgets::ColorEdit4("Liquid Color", look.liquidColor);
        ImGui::EndDisabled();
        Tooltip("A は液の透け具合。ハイライトは A に関わらず出ます。");
        if (look.useAlbedoRamp) {
            /// @note ランプは RGB だけを置き換え、透け具合は liquidColor の A が持ち続ける。灰色にした欄から A を触れなくならないよう別に出す。
            ImGui::TextDisabled("液の色は Albedo Ramp が決めます。透け具合は下の Liquid Alpha で。");
            changed |= ImGui::SliderFloat("Liquid Alpha", &look.liquidColor.w, 0.0f, 1.0f);
        }
        changed |= ImGui::DragFloat("Radius Scale", &look.liquidRadiusScale, 0.01f, 0.5f, 6.0f);
        Tooltip("描画時の粒子の広がり。上げるほど液面がつながって «一枚の水» になります。");
        changed |= ImGui::SliderFloat("Threshold", &look.liquidThreshold, 0.05f, 2.0f);
        changed |= ImGui::SliderFloat("Specular", &look.specular, 0.0f, 2.0f);
        changed |= widgets::DragVec3("Light Direction", look.lightDirection, 0.01f, -1.0f, 1.0f);
        if (ImGui::TreeNode("3D Surface")) {
            ImGui::TextDisabled("3D (Volume) で焼くときの液面です。2D の絵には効きません。");
            changed |= ImGui::DragFloat("Softness", &look.liquidSoftness, 0.002f, 0.005f, 0.5f, "%.3f");
            Tooltip("液面の縁の柔らかさ。");
            changed |= ImGui::DragFloat("Extinction", &look.liquidExtinction, 0.5f, 0.0f, 200.0f, "%.1f");
            Tooltip("大きいほど不透明 (血)、小さいほど透けます (水)。");
            changed |= ImGui::DragFloat("Gloss", &look.liquidGloss, 1.0f, 1.0f, 512.0f, "%.0f");
            Tooltip("ハイライトの鋭さ。");
            changed |= ImGui::SliderFloat("Fresnel F0", &look.liquidFresnel, 0.0f, 0.2f, "%.3f");
            Tooltip("正面から見たときの反射率。水は 0.02 前後。");
            ImGui::TreePop();
        }
    }
    ImGui::PopID();
    return changed;
}

bool EditOutput(FluidWidgetContext& /*wc*/, fluid::FluidRecipe& recipe)
{
    bool changed = false;
    ImGui::PushID("fluid_output");
    fluid::FluidOutputSettings& output = recipe.output;
    static constexpr int kSizes[] = { 64, 128, 256, 512 };
    static constexpr const char* kSizeLabels[] = { "64", "128", "256", "512" };
    int sizeIndex = 2;
    for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i)
        if (kSizes[i] == output.frameSize) sizeIndex = i;
    if (ImGui::Combo("Frame Size", &sizeIndex, kSizeLabels, IM_ARRAYSIZE(kSizeLabels))) {
        output.frameSize = kSizes[sizeIndex];
        changed = true;
    }
    Tooltip("1 コマの解像度。画面に大きく出る煙・爆発は 256 以上。遠景の小さな粒なら 128 で足ります。");
    changed |= ImGui::SliderInt("Supersampling", &output.supersampling, 1, 4, "x%d");
    Tooltip("1 コマを何倍で描いてから縮めるか。液面の縁のギザギザと、細かい煙のちらつきが消えます。\n"
            "描く手間は倍率の 2 乗で増えます (x2 で 4 倍)。");
    int grid[2] = { output.columns, output.rows };
    if (ImGui::DragInt2("Columns / Rows", grid, 0.1f, 1, 32)) {
        output.columns = std::clamp(grid[0], 1, 32);
        output.rows    = std::clamp(grid[1], 1, 32);
        changed = true;
    }
    ImGui::SeparatorText("Bake Range");
    float bakeStart = output.warmup;
    if (ImGui::DragFloat("Bake Start", &bakeStart, 0.01f, 0.0f, 30.0f, "%.2f s")) {
        output.warmup = std::clamp(bakeStart, 0.0f, 30.0f);
        changed = true;
    }
    Tooltip("シミュレーション開始から焼き始めるまでの時間。開始を動かしても収録する長さは変えません。");
    float bakeEnd = output.warmup + output.duration;
    if (ImGui::DragFloat("Bake End", &bakeEnd, 0.01f, output.warmup + 0.05f,
                         output.warmup + 30.0f, "%.2f s")) {
        output.duration = std::clamp(bakeEnd - output.warmup, 0.05f, 30.0f);
        changed = true;
    }
    Tooltip("シミュレーション開始から焼き終えるまでの時間。終了を動かすと収録長と再生 FPS が変わります。");
    ImGui::TextDisabled("Capture %.2f - %.2f s  |  %.2f s", output.warmup,
                        output.warmup + output.duration, output.duration);
    changed |= ImGui::SliderInt("Substeps", &output.substeps, 1, 8);
    changed |= ImGui::Checkbox("Loop", &output.loop);
    Tooltip("末尾を先頭へクロスフェードして、FPS 再生でつながるようにします。\n"
            "一度きりの煙・爆発 (Lifetime 再生) では外したままにします。2D・3D どちらの焼きでも効きます。");
    if (output.loop) {
        float blendPercent = output.loopBlendFraction * 100.0f;
        if (ImGui::SliderFloat("Loop Blend", &blendPercent, 0.0f, 50.0f, "%.0f%%")) {
            output.loopBlendFraction = blendPercent / 100.0f;
            changed = true;
        }
        Tooltip("先頭へ混ぜるコマの割合。0% は混ぜずに周回します。2D と 3D の Bake に共通です。");
    }
    changed |= ImGui::Checkbox("Motion Vectors", &output.motionVectors);
    Tooltip("ソルバーの実速度から Motion Vector を焼きます。少ないコマ数でも滑らかに流れます\n"
            "(使うと GPU シミュレーションは CPU へ縮退します)。");
    if (recipe.kind == FluidKind::Gas) {
        changed |= ImGui::Checkbox("Vector Field (PNG)", &output.vectorField);
        Tooltip("同じ流れを 3D で解き直し、時間平均した速度場を焼きます。\n"
                "FlowField の Baked に割り当てると、火の粉や塵を煙と同じ流れに乗せられます。");
        if (output.vectorField) {
            changed |= ImGui::SliderInt("Field Resolution", &output.vectorFieldResolution, 8, 64);
            changed |= widgets::DragVec3("Field Extents [m]", output.vectorFieldExtents, 0.05f, 0.1f, 500.0f);
        }
    }

    const int frames = FrameCount(recipe);
    const int atlasWidth = output.frameSize * output.columns;
    const int atlasHeight = output.frameSize * output.rows;
    ImGui::TextDisabled("%d frames  |  atlas %d x %d px  |  %.1f fps", frames, atlasWidth, atlasHeight,
                        static_cast<float>(frames) / (std::max)(output.duration, 0.05f));
    if (atlasWidth > 4096 || atlasHeight > 4096)
        ImGui::TextColored(kErrorColor, "4096px を超えるアトラスは GPU メモリを大きく使います。");
    ImGui::PopID();
    return changed;
}

/// @brief 3D の主なノブだけ。視点・光・露出の細かい調整は Volume Flipbook Baker が同じ [bake] へ書く。
bool EditBake(FluidWidgetContext& /*wc*/, fluid::FluidRecipe& recipe)
{
    fluid::FluidBakeSettings& bake = recipe.bake;
    bool changed = false;
    ImGui::PushID("fluid_bake");
    static constexpr const char* kModes[] = { "2D (Flat)", "3D (Volume)" };
    int mode = static_cast<int>(bake.mode);
    if (ImGui::Combo("Bake Mode", &mode, kModes, IM_ARRAYSIZE(kModes))) {
        bake.mode = static_cast<fluid::FluidBakeMode>(mode);
        changed = true;
    }
    Tooltip("2D: 平面で解いて焼きます (速い)。\n"
            "3D: 立体で解き直してレイマーチで焼きます。視点・光の向き・6 方向ライトマップを選べます。\n"
            "    ループ (Output > Loop) も Distortion (歪み) も焼けます。全プリセットが 3D で焼けます。\n"
            "AI の fluid.bake もこの設定で焼きます。");
    if (bake.mode == fluid::FluidBakeMode::Volume3D) {
        static constexpr int kResolutions[] = { 32, 48, 64, 96, 128, 160 };
        static constexpr const char* kResolutionLabels[] = { "32", "48", "64", "96", "128", "160" };
        int resolutionIndex = 2;
        for (int i = 0; i < IM_ARRAYSIZE(kResolutions); ++i)
            if (kResolutions[i] == bake.volumeResolution) resolutionIndex = i;
        if (ImGui::Combo("Volume Resolution", &resolutionIndex, kResolutionLabels, IM_ARRAYSIZE(kResolutionLabels))) {
            bake.volumeResolution = kResolutions[resolutionIndex];
            changed = true;
        }
        Tooltip("立体の格子の 1 辺。GPU は 160、CPU は 96 まで解けます。");
        changed |= widgets::RangeField("Ray Steps", bake.raySteps, 32, 512, "%d",
                                       "視線 1 本あたりの標本数です。\n"
                                       "Volume Resolution の 2 倍が目安 (1 ボクセルに 2 標本)。\n"
                                       "ここを据え置いたまま格子だけ上げても、細部は絵に出ません。");
        changed |= widgets::RangeField("Shadow Steps", bake.shadowSteps, 4, 64, "%d",
                                       "影を測る行進の標本数です。増やすと影の縞が減ります。\n"
                                       "焼き時間は Ray Steps との積で増えます。");
        static constexpr const char* kSolvers[] = { "Auto (GPU → CPU)", "GPU (Compute)", "CPU" };
        int solver = static_cast<int>(bake.solver);
        if (ImGui::Combo("Solver", &solver, kSolvers, IM_ARRAYSIZE(kSolvers))) {
            bake.solver = static_cast<fluid::FluidBakeSolver>(solver);
            changed = true;
        }
        Tooltip("Auto: GPU で解き、使えなければ CPU へ落とします (理由は焼きの結果に出ます)。\n"
                "GPU: GPU で解けなければ焼きません。同じ .fluid から必ず同じ絵が欲しいときはこちら。\n"
                "CPU: 同じレシピを CPU で解きます。GPU と見比べるときの基準です。");
        if (recipe.kind == FluidKind::Liquid && bake.solver == fluid::FluidBakeSolver::Gpu)
            ImGui::TextColored(kWarnColor, "液体の GPU ソルバーは新しく、CPU ほど実績がありません。\n"
                                           "粒子が多いほど CPU より速く焼けます。崩れ方が怪しければ CPU で焼き比べてください。");
        if (bake.solver == fluid::FluidBakeSolver::Cpu && bake.volumeResolution > kMaxCpuVolumeResolution)
            ImGui::TextColored(kWarnColor, "CPU ソルバーは %d までです。\n"
                                           "焼きもプレビューも黙って %d に落ちます (GPU なら 160 まで解けます)。",
                               kMaxCpuVolumeResolution, kMaxCpuVolumeResolution);
        changed |= ImGui::Checkbox("Six-way Lightmaps", &bake.sixWayLightmaps);
        Tooltip("6 方向ライトマップ (_6wayP / _6wayN) も焼きます。\n"
                "ランタイムで光の向きを変えても陰影が回り込みます (焼く時間は延びます)。");
        if (ImGui::TreeNode("Quality")) {
            changed |= widgets::RangeField("Scattering Octaves", bake.scatteringOctaves, 1, 6, "%d",
                                           "多重散乱の段数です (1 = 単散乱)。増やすほど煙の内側が明るく柔らかくなります。");
            changed |= widgets::RangeField("Sky Occlusion", bake.skyOcclusion, 0.0f, 1.0f, "%.2f",
                                           "環境光を上に積もった煙が遮る割合です。");
            changed |= ImGui::DragFloat("Density Scale", &bake.densityScale, 0.01f, 0.01f, 20.0f);
            changed |= ImGui::DragFloat("Exposure", &bake.exposure, 0.01f, 0.01f, 8.0f);
            Tooltip("HDR の色を 8bit へ落とすときの倍率。マテリアルの HDR Emissive に 1/Exposure が入ります。");
            changed |= ImGui::Checkbox("Blackbody Emission", &bake.blackbodyEmission);
            if (bake.blackbodyEmission)
                changed |= ImGui::DragFloatRange2("Kelvin", &bake.blackbodyMinKelvin, &bake.blackbodyMaxKelvin,
                                                  10.0f, 500.0f, 15000.0f, "%.0f K");
            ImGui::TreePop();
        }
    }
    ImGui::PopID();
    return changed;
}

/// @name 追加メニュー

bool AddSourceMenuItems(fluid::FluidRecipe& recipe, int& outNewIndex)
{
    const bool full = recipe.sources.size() >= static_cast<std::size_t>(fluid::kMaxFluidSources);
    bool added = false;
    ImGui::PushID("fluid_add_source");
    for (int s = 0; s < IM_ARRAYSIZE(kShapeNames); ++s) {
        if (ImGui::MenuItem(kShapeNames[s], nullptr, false, !full) && !added && !full) {
            recipe.sources.push_back(MakeDefaultSource(recipe.kind, static_cast<fluid::FluidSourceShape>(s)));
            outNewIndex = static_cast<int>(recipe.sources.size()) - 1;
            added = true;
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%d / %d", static_cast<int>(recipe.sources.size()), fluid::kMaxFluidSources);
    ImGui::PopID();
    return added;
}

bool AddForceMenuItems(fluid::FluidRecipe& recipe, int& outNewIndex)
{
    const bool full = recipe.forces.size() >= static_cast<std::size_t>(fluid::kMaxFluidForces);
    bool added = false;
    ImGui::PushID("fluid_add_force");
    for (int t = 0; t < IM_ARRAYSIZE(kForceTypeNames); ++t) {
        if (ImGui::MenuItem(kForceTypeNames[t], nullptr, false, !full) && !added && !full) {
            recipe.forces.push_back(MakeDefaultForce(static_cast<fluid::FluidForceType>(t)));
            outNewIndex = static_cast<int>(recipe.forces.size()) - 1;
            added = true;
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%d / %d", static_cast<int>(recipe.forces.size()), fluid::kMaxFluidForces);
    ImGui::PopID();
    return added;
}

bool AddColliderMenuItems(fluid::FluidRecipe& recipe, int& outNewIndex)
{
    const bool full = recipe.colliders.size() >= static_cast<std::size_t>(fluid::kMaxFluidColliders);
    bool added = false;
    ImGui::PushID("fluid_add_collider");
    for (int s = 0; s < IM_ARRAYSIZE(kColliderShapeNames); ++s) {
        if (ImGui::MenuItem(kColliderShapeNames[s], nullptr, false, !full) && !added && !full) {
            recipe.colliders.push_back(MakeDefaultCollider(static_cast<fluid::FluidColliderShape>(s)));
            outNewIndex = static_cast<int>(recipe.colliders.size()) - 1;
            added = true;
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%d / %d", static_cast<int>(recipe.colliders.size()), fluid::kMaxFluidColliders);
    ImGui::PopID();
    return added;
}

/// @name 部品の一覧操作

std::string PartDisplayName(const fluid::FluidRecipe& recipe, FluidSelectionKind list, int index)
{
    const auto format = [index](const std::string& name, const char* prefix, const char* kind) {
        char text[192];
        if (name.empty()) std::snprintf(text, sizeof(text), "%s %d (%s)", prefix, index, kind);
        else              std::snprintf(text, sizeof(text), "%s (%s)", name.c_str(), kind);
        return std::string(text);
    };
    if (index < 0) return {};
    const auto at = static_cast<std::size_t>(index);
    switch (list) {
    case FluidSelectionKind::Source:
        if (at >= recipe.sources.size()) return {};
        return format(recipe.sources[at].name, "Source", ShapeName(recipe.sources[at].shape));
    case FluidSelectionKind::Force:
        if (at >= recipe.forces.size()) return {};
        return format(recipe.forces[at].name, "Force", ForceTypeName(recipe.forces[at].type));
    case FluidSelectionKind::Collider:
        if (at >= recipe.colliders.size()) return {};
        return format(recipe.colliders[at].name, "Collider", ColliderShapeName(recipe.colliders[at].shape));
    default:
        return {};
    }
}

bool* PartEnabled(fluid::FluidRecipe& recipe, FluidSelectionKind list, int index)
{
    if (index < 0) return nullptr;
    const auto at = static_cast<std::size_t>(index);
    switch (list) {
    case FluidSelectionKind::Source:   return at < recipe.sources.size() ? &recipe.sources[at].enabled : nullptr;
    case FluidSelectionKind::Force:    return at < recipe.forces.size() ? &recipe.forces[at].enabled : nullptr;
    case FluidSelectionKind::Collider: return at < recipe.colliders.size() ? &recipe.colliders[at].enabled : nullptr;
    default:                           return nullptr;
    }
}

int PartCount(const fluid::FluidRecipe& recipe, FluidSelectionKind list)
{
    switch (list) {
    case FluidSelectionKind::Source:   return static_cast<int>(recipe.sources.size());
    case FluidSelectionKind::Force:    return static_cast<int>(recipe.forces.size());
    case FluidSelectionKind::Collider: return static_cast<int>(recipe.colliders.size());
    default:                           return 0;
    }
}

bool DuplicatePart(fluid::FluidRecipe& recipe, FluidSelectionKind list, int index)
{
    return VisitPartList(recipe, list, [index](auto& parts, std::size_t limit) {
        if (index < 0 || index >= static_cast<int>(parts.size()) || parts.size() >= limit) return false;
        auto copy = parts[static_cast<std::size_t>(index)];
        parts.insert(parts.begin() + index + 1, std::move(copy));
        return true;
    });
}

bool RemovePart(fluid::FluidRecipe& recipe, FluidSelectionKind list, int index)
{
    return VisitPartList(recipe, list, [index](auto& parts, std::size_t /*limit*/) {
        if (index < 0 || index >= static_cast<int>(parts.size())) return false;
        parts.erase(parts.begin() + index);
        return true;
    });
}

bool MovePart(fluid::FluidRecipe& recipe, FluidSelectionKind list, int from, int to)
{
    return VisitPartList(recipe, list, [from, to](auto& parts, std::size_t /*limit*/) {
        const int count = static_cast<int>(parts.size());
        if (from < 0 || from >= count || to < 0 || to >= count || from == to) return false;
        const auto first = parts.begin();
        if (from < to) std::rotate(first + from, first + from + 1, first + to + 1);
        else           std::rotate(first + to, first + from, first + from + 1);
        return true;
    });
}

} /// @note namespace fbzz::editor::fluidui
