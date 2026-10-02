/// @file    ProjectSettings.cpp
/// @brief   プロジェクト設定の TOML 永続化実装。
/// @author  Hasegawa Jin
/// @date    2026-05-23
/// @note タグ・レイヤーなどエディタとランタイムで共有する設定を読み書きする。失敗時は bool を返す。
#include <Engine/ProjectSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include "Asset/RenderPipelineAssetCodec.hpp"
#include <toml++/toml.hpp>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

namespace fbzz {

std::vector<audio::BusDesc> AudioSettings::BuildBusLayout() const
{
    std::vector<audio::BusDesc> layout =
        buses.empty() ? audio::DefaultBusLayout() : buses;
    /// @note Master の音量はここで masterVolume に一本化する。設定 UI は Master を «全体音量» として
    /// @note 1 本のスライダーで見せるため、バス側にも書けると、どちらが効くか読めなくなる。
    for (audio::BusDesc& desc : layout) {
        if (desc.name == audio::kMasterBusName) {
            desc.volume = masterVolume;
            break;
        }
    }
    return layout;
}

namespace {

double RoundTomlFloat(double value)
{
    constexpr double SCALE = 1000000.0;
    const double rounded = std::round(value * SCALE) / SCALE;
    return rounded == 0.0 ? 0.0 : rounded;
}

void NormalizeTomlFloats(toml::node& node)
{
    if (auto* value = node.as_floating_point()) {
        value->get() = RoundTomlFloat(value->get());
        return;
    }

    if (auto* table = node.as_table()) {
        for (auto&& [key, child] : *table) {
            (void)key;
            NormalizeTomlFloats(child);
        }
        return;
    }

    if (auto* array = node.as_array()) {
        for (auto& child : *array)
            NormalizeTomlFloats(child);
    }
}

toml::array Vec3ToArr(const math::Vector3& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    a.push_back((double)v.z);
    return a;
}

math::Vector3 ArrToVec3(const toml::array* arr, const math::Vector3& def)
{
    if (!arr || arr->size() < 3) return def;
    return {
        (float)(*arr)[0].value_or((double)def.x),
        (float)(*arr)[1].value_or((double)def.y),
        (float)(*arr)[2].value_or((double)def.z)
    };
}

bool ReadBool(const toml::table& table, const char* key, bool fallback)
{
    if (auto value = table[key].value<bool>())
        return *value;
    return fallback;
}

bool ReadRayRequest(const toml::table& table, const char* key)
{
    /// @note TOML の整数を bool へ変換して RT 要求を有効化しない。旧設定の ReadBool は維持する。
    const auto* value = table[key].as_boolean();
    return value && value->get();
}

float ReadFloat(const toml::table& table, const char* key, float fallback)
{
    if (auto value = table[key].value<double>())
        return static_cast<float>(*value);
    if (auto value = table[key].value<int64_t>())
        return static_cast<float>(*value);
    return fallback;
}

renderer::RenderingPipeline RenderingPipelineFromString(std::string_view value,
                                                        renderer::RenderingPipeline fallback)
{
    /// @note 手編集された旧表記も受け付け、既存の ProjectSettings.toml を壊さず移行する。
    if (value == "forward" || value == "Forward")
        return renderer::RenderingPipeline::Forward;
    if (value == "deferred" || value == "Deferred")
        return renderer::RenderingPipeline::Deferred;
    if (value == "forward_plus" || value == "forward+" || value == "Forward+")
        return renderer::RenderingPipeline::ForwardPlus;
    if (value == "deferred_plus" || value == "deferred+" || value == "Deferred+")
        return renderer::RenderingPipeline::DeferredPlus;
    return fallback;
}

const char* RenderingPipelineToString(renderer::RenderingPipeline pipeline)
{
    switch (pipeline) {
    /// @note 保存値も Editor の表示名に揃える。Load 側は旧 lowercase / underscore 表記も受け付ける。
    case renderer::RenderingPipeline::Forward:      return "Forward";
    case renderer::RenderingPipeline::Deferred:     return "Deferred";
    case renderer::RenderingPipeline::ForwardPlus:  return "Forward+";
    case renderer::RenderingPipeline::DeferredPlus: return "Deferred+";
    }
    return "Forward";
}

renderer::RenderMode RenderModeFromString(std::string_view value)
{
    if (value == "Hybrid") return renderer::RenderMode::HYBRID;
    if (value == "PathTracing") return renderer::RenderMode::PATH_TRACING;
    return renderer::RenderMode::RASTER;
}

const char* RenderModeToString(renderer::RenderMode mode)
{
    switch (mode) {
    case renderer::RenderMode::HYBRID: return "Hybrid";
    case renderer::RenderMode::PATH_TRACING: return "PathTracing";
    default: return "Raster";
    }
}

} /// @note namespace

void CursorAppearance::Apply(const std::string& projectRoot) const
{
    /// @note 先に全部畳む。前のプロジェクト / 前の Play で読んだ絵が «設定を空にしても
    /// @note 残り続ける» のを防ぐ。
    core::Cursor::ClearShapeImages();
    if (!hardwareCursor) return;

    const std::filesystem::path root(projectRoot);
    for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
        const ShapeImage& entry = shapes[i];
        if (entry.path.empty()) continue;

        std::filesystem::path full(entry.path);
        if (full.is_relative() && !root.empty()) full = root / full;
        core::Cursor::SetShapeImage(static_cast<core::CursorShape>(i),
                                    full.string().c_str(), entry.hotspotX, entry.hotspotY);
    }
}

ProjectSettings ProjectSettings::Default()
{
    ProjectSettings ps;
    ps.game.project.defaultScene = "Assets/Scenes/Main.scene";
    ps.game.runtime.startScene   = "Assets/Scenes/Main.scene";
    ps.game.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                     "MainCamera", "Player", "GameController" };
    ps.game.layerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", ""
    };
    ps.physics.hz       = 60;
    ps.physics.substeps = 1;
    ps.physics.gravity  = { 0.0f, -9.81f, 0.0f };
    return ps;
}

bool ProjectSettings::Load(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        /// @note Load 失敗時に既存設定を代入で破棄すると、呼び出し側が保持していた設定や UI 参照まで巻き戻る。
        /// @note 失敗は bool で伝え、現在の設定はそのまま残す。
        FBZZ_LOG_WARN("ProjectSettings: read failed: %s", path.c_str());
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("ProjectSettings: parse failed: %s", path.c_str());
        return false;
    }
    auto& tbl = result.table();
    renderer::HybridQualitySettings hybridQuality;
    if (const auto* node = tbl["render"]["hybrid"].node()) {
        const auto* quality = node->as_table();
        if (!quality || !asset::RenderPipelineAssetCodec::LoadHybridQuality(*quality, hybridQuality)) {
            FBZZ_LOG_WARN("ProjectSettings: invalid render.hybrid quality: %s", path.c_str());
            return false;
        }
    }
    std::string pipelineAsset;
    if (const auto* node = tbl["render"]["pipelineAsset"].node()) {
        const auto* token = node->as_string();
        if (!token) {
            FBZZ_LOG_WARN("ProjectSettings: render.pipelineAsset must be a string: %s", path.c_str());
            return false;
        }
        /// @note GUID resolution requires the later AssetManager initialization; preserve unresolved references rather than dropping missing assets.
        pipelineAsset = token->get();
    }
    renderPipelineAssetPath = std::move(pipelineAsset);
    /// @note 新しいキーがない旧設定は Raster。別プロジェクトの RT 要求を引き継がない。
    render.modeRequest = {};
    render.hybridQuality = hybridQuality;

    if (auto* projectTbl = tbl["project"].as_table()) {
        game.project.name         = (*projectTbl)["name"].value_or(game.project.name);
        game.project.defaultScene = (*projectTbl)["default_scene"].value_or(game.project.defaultScene);
    }

    if (auto* runtimeTbl = tbl["runtime"].as_table()) {
        game.runtime.startScene = (*runtimeTbl)["start_scene"].value_or(game.runtime.startScene);
    }

    if (auto* tagArr = tbl["tags"]["list"].as_array()) {
        game.tags.clear();
        for (auto& elem : *tagArr)
            if (auto v = elem.value<std::string>())
                game.tags.push_back(*v);
    }

    if (auto* layArr = tbl["layers"]["names"].as_array()) {
        for (int i = 0; i < 32 && i < (int)layArr->size(); ++i)
            if (auto v = (*layArr)[i].value<std::string>())
                game.layerNames[i] = *v;
    }

    if (auto* physicsTbl = tbl["physics"].as_table()) {
        physics.hz      = (int)(*physicsTbl)["hz"].value_or((int64_t)physics.hz);
        physics.substeps = (int)(*physicsTbl)["substeps"].value_or((int64_t)physics.substeps);
        physics.gravity = ArrToVec3((*physicsTbl)["gravity"].as_array(), physics.gravity);

        /// @note 衝突行列は «ぶつからない組» だけを書く。32x32 = 1024 個の true を並べても
            /// @note 読めないうえ、レイヤーを 1 つ足すたびに差分が全面になる。
        physics.collisionMatrix = LayerCollisionMatrix{};
        if (auto* ignoreArr = (*physicsTbl)["ignoreCollisions"].as_array()) {
            for (const auto& entry : *ignoreArr) {
                const auto* pair = entry.as_array();
                if (!pair || pair->size() < 2) continue;
                const auto a = (*pair)[0].value<int64_t>();
                const auto b = (*pair)[1].value<int64_t>();
                if (!a || !b) continue;
                physics.collisionMatrix.Set(static_cast<int>(*a), static_cast<int>(*b), false);
            }
        }
    }

    if (physics.hz < 1)        physics.hz = 1;
    if (physics.hz > 1000)     physics.hz = 1000;
    if (physics.substeps < 1)  physics.substeps = 1;
    if (physics.substeps > 32) physics.substeps = 32;

    /// @note [render] はシーンをまたいでも変わらない構成 (パイプライン・シャドウ品質・デバッグ表示・
    /// @note パーティクル予算) だけを持つ。Bloom/SSR 等の «ルック» は場所ごとに変わるため
    /// @note PostProcessVolume + PostProcessProfile (.fzdata) の所有。旧 bloom = … 等のキーは無視される。
    if (auto* renderTbl = tbl["render"].as_table()) {
        render.modeRequest.mode = RenderModeFromString(
            (*renderTbl)["mode"].value_or(std::string("Raster")));
        render.modeRequest.pathProfile =
            (*renderTbl)["pathProfile"].value_or(std::string("Reference")) == "Game"
                ? renderer::PathTracingProfile::GAME : renderer::PathTracingProfile::REFERENCE;
        render.modeRequest.rayShadow = ReadRayRequest(*renderTbl, "rayShadow");
        render.modeRequest.rayReflection = ReadRayRequest(*renderTbl, "rayReflection");
        render.modeRequest.rayDiffuseGi = ReadRayRequest(*renderTbl, "rayDiffuseGi");
        {
            const auto s = (*renderTbl)["pipeline"].value_or(std::string("forward"));
            render.pipeline = RenderingPipelineFromString(s, render.pipeline);
        }
        {
            const int vm = (*renderTbl)["viewMode"].value_or(static_cast<int>(render.viewMode));
            render.viewMode = static_cast<renderer::ViewMode>(vm);
        }

        /// @name シャドウ
        render.shadowEnabled = (*renderTbl)["shadow"].value_or(render.shadowEnabled);
        render.shadow.mapResolution = static_cast<uint32_t>(
            (*renderTbl)["shadowResolution"].value_or(static_cast<int64_t>(render.shadow.mapResolution)));
        render.shadow.pcfRadius = static_cast<int>(
            (*renderTbl)["shadowPcfRadius"].value_or(static_cast<int64_t>(render.shadow.pcfRadius)));
        render.shadow.autoFitDistance =
            ReadFloat(*renderTbl, "shadowAutoFitDistance", render.shadow.autoFitDistance);
        render.shadow.cascadeCount = static_cast<int>(
            (*renderTbl)["shadowCascadeCount"].value_or(static_cast<int64_t>(render.shadow.cascadeCount)));
        render.shadow.cascadeSplitLambda =
            ReadFloat(*renderTbl, "shadowCascadeSplitLambda", render.shadow.cascadeSplitLambda);
        render.shadow.cascadeBlend =
            ReadFloat(*renderTbl, "shadowCascadeBlend", render.shadow.cascadeBlend);
        render.shadow.pcssEnabled     = ReadBool(*renderTbl,  "pcssEnabled",     render.shadow.pcssEnabled);
        render.shadow.pcssLightRadius = ReadFloat(*renderTbl, "pcssLightRadius", render.shadow.pcssLightRadius);

        render.clustered.enabled = ReadBool(*renderTbl, "clusteredEnabled", render.clustered.enabled);
        render.clustered.maxDistance = ReadFloat(
            *renderTbl, "clusteredMaxDistance", render.clustered.maxDistance);
        render.clustered.debugHeatmap = ReadBool(
            *renderTbl, "clusteredDebugHeatmap", render.clustered.debugHeatmap);
        render.clustered.forceAllLights = ReadBool(
            *renderTbl, "clusteredForceAllLights", render.clustered.forceAllLights);

        /// @name デバッグ表示
        render.showColliders        = (*renderTbl)["showColliders"].value_or(render.showColliders);
        render.showUIRects          = (*renderTbl)["showUIRects"].value_or(render.showUIRects);
        render.showDecalBounds      = (*renderTbl)["showDecalBounds"].value_or(render.showDecalBounds);
        render.showSelectionOutline = (*renderTbl)["showSelectionOutline"].value_or(render.showSelectionOutline);
        render.passViewerEnabled    = (*renderTbl)["passViewerEnabled"].value_or(render.passViewerEnabled);

        render.schedulePolicy =
            (*renderTbl)["schedulePolicy"].value_or(std::string{}) == "MinimizeLifetimes"
                ? renderer::RenderGraphSchedulePolicy::MinimizeLifetimes
                : renderer::RenderGraphSchedulePolicy::RegistrationOrder;

        /// @name RenderGraph のパス上書き
        render.passOverrides.clear();
        if (auto* overrideArray = (*renderTbl)["passOverride"].as_array()) {
            for (const auto& node : *overrideArray) {
                const auto* overrideTbl = node.as_table();
                if (!overrideTbl) continue;
                renderer::RenderPassOverride entry;
                entry.name = (*overrideTbl)["name"].value_or(std::string{});
                if (entry.name.empty()) continue;
                entry.enabled      = (*overrideTbl)["enabled"].value_or(true);
                entry.allowCulling = (*overrideTbl)["allowCulling"].value_or(true);
                if (auto* readArray = (*overrideTbl)["extraReads"].as_array()) {
                    for (const auto& read : *readArray) {
                        if (auto value = read.value<std::string>(); value && !value->empty())
                            entry.extraReads.push_back(*value);
                    }
                }
                /// @note 既定に戻された行は読み捨てる。表に «何も変えていない» 行を残さない。
                if (!entry.IsDefault()) render.passOverrides.push_back(std::move(entry));
            }
        }

        /// @name パーティクル予算
        render.particleBudget        = (int)(*renderTbl)["particleBudget"].value_or((int64_t)render.particleBudget);
        render.particleBudgetEnabled = (*renderTbl)["particleBudgetEnabled"].value_or(render.particleBudgetEnabled);

        /// @name 選択アウトライン
        render.outlineWidth = (float)(*renderTbl)["outlineWidth"].value_or((double)render.outlineWidth);
        if (auto* outlineColorArr = (*renderTbl)["outlineColor"].as_array(); outlineColorArr && outlineColorArr->size() >= 4) {
            render.outlineColor[0] = (float)(*outlineColorArr)[0].value_or((double)render.outlineColor[0]);
            render.outlineColor[1] = (float)(*outlineColorArr)[1].value_or((double)render.outlineColor[1]);
            render.outlineColor[2] = (float)(*outlineColorArr)[2].value_or((double)render.outlineColor[2]);
            render.outlineColor[3] = (float)(*outlineColorArr)[3].value_or((double)render.outlineColor[3]);
        }
    }

    if (auto* audioTbl = tbl["audio"].as_table()) {
        const auto unitRange = [](double v) {
            return (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        };
        audio.masterVolume = unitRange((*audioTbl)["masterVolume"]
                                           .value_or((double)audio.masterVolume));
        audio.voiceLimit = (int)(*audioTbl)["voiceLimit"]
                               .value_or((int64_t)audio.voiceLimit);
        if (audio.voiceLimit < 4) audio.voiceLimit = 4;

        std::vector<audio::BusDesc> buses;
        if (auto* busArray = (*audioTbl)["bus"].as_array()) {
            for (const auto& node : *busArray) {
                const auto* busTbl = node.as_table();
                if (!busTbl) continue;
                audio::BusDesc desc;
                desc.name = (*busTbl)["name"].value_or(std::string{});
                if (desc.name.empty()) continue;
                desc.parent        = (*busTbl)["parent"].value_or(std::string{});
                desc.volume        = unitRange((*busTbl)["volume"].value_or(1.0));
                desc.lowPassCutoff = unitRange((*busTbl)["lowPassCutoff"].value_or(1.0));
                desc.reverb        = (*busTbl)["reverb"].value_or(false);
                buses.push_back(std::move(desc));
            }
        }

        if (buses.empty()) {
            /// @note 旧形式 (bgmVolume / seVolume の 2 スライダー) からの移行。既定構成へ写すのは、
            /// @note 旧設定を捨てると更新しただけで音量が 1.0 へ戻るため。
            buses = audio::DefaultBusLayout();
            const float bgm = unitRange((*audioTbl)["bgmVolume"].value_or(1.0));
            const float se  = unitRange((*audioTbl)["seVolume"].value_or(1.0));
            for (audio::BusDesc& desc : buses) {
                if (desc.name == "BGM") desc.volume = bgm;
                if (desc.name == "SE")  desc.volume = se;
            }
        }
        audio.buses = std::move(buses);
    }

    if (auto* screenTbl = tbl["screen"].as_table()) {
        screen.width  = (int)(*screenTbl)["width"].value_or((int64_t)screen.width);
        screen.height = (int)(*screenTbl)["height"].value_or((int64_t)screen.height);
        if (screen.width  < 1) screen.width  = 1;
        if (screen.height < 1) screen.height = 1;
    }

    if (auto* appTbl = tbl["app"].as_table()) {
        app.targetFps = (int)(*appTbl)["targetFps"].value_or((int64_t)app.targetFps);
        if (app.targetFps < 0) app.targetFps = 0;
        if (auto backend = (*appTbl)["renderer"].value<std::string>()) {
            /// @note 終了済みトークンを黙って DX12 へ倒すと、利用者は設定が効いていると
                /// @note 誤解したまま動いてしまう。倒すこと自体は変えず、名指しで伝える。
            if (renderer::IsRetiredBackendToken(*backend)) {
                FBZZ_LOG_WARN("ProjectSettings: renderer=\"%s\" は v1.0 でサポートを終了しました。"
                              "DirectX 12 で起動します (Docs/design/dx11-removal.md)",
                              backend->c_str());
            }
            app.rendererBackend = renderer::BackendFromString(*backend);
        }
    }

    if (auto* windowTbl = tbl["window"].as_table()) {
        window.title      = (*windowTbl)["title"].value_or(window.title);
        window.width      = (int)(*windowTbl)["width"].value_or((int64_t)window.width);
        window.height     = (int)(*windowTbl)["height"].value_or((int64_t)window.height);
        window.fullscreen = (*windowTbl)["fullscreen"].value_or(window.fullscreen);
        if (window.width  < 1) window.width  = 1;
        if (window.height < 1) window.height = 1;
    }

    /// @note 旧 [cursor] の lock_mode / visible は読まない。拘束と表示はスクリプトが持つ
    /// @note ランタイム状態になり、設定ファイルは «絵» だけを持つ (CursorAppearance を参照)。
    /// @note 古いファイルに残っていてもここで黙って捨てられ、次の Save で消える。
    if (auto* cursorTbl = tbl["cursor"].as_table()) {
        cursor.hardwareCursor = (*cursorTbl)["hardware"].value_or(cursor.hardwareCursor);
        if (auto* shapesTbl = (*cursorTbl)["shapes"].as_table()) {
            for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
                const auto shape = static_cast<core::CursorShape>(i);
                auto* shapeTbl = (*shapesTbl)[core::ToString(shape)].as_table();
                if (!shapeTbl) continue;
                auto& entry = cursor.shapes[i];
                entry.path = (*shapeTbl)["image"].value_or(entry.path);
                if (auto* hotspot = (*shapeTbl)["hotspot"].as_array();
                    hotspot && hotspot->size() >= 2) {
                    entry.hotspotX = (float)hotspot->get(0)->value_or(0.0);
                    entry.hotspotY = (float)hotspot->get(1)->value_or(0.0);
                }
            }
        }
    }

    if (auto* uiTbl = tbl["ui"].as_table())
        ui.defaultFontPath = (*uiTbl)["default_font"].value_or(ui.defaultFontPath);

    /// @name 入力バインド
    /// @note ProjectSettings.toml と同じディレクトリの Input.inputactions を読む。別ファイルなのは、
    /// @note バインド定義の配列が深く混ぜると読みにくいのと、プレイヤーが実行時に書き換える対象で
    /// @note 開発者向け設定とは更新頻度も責務も違うため。
    /// @note 読めなくても Load 全体は失敗させない。入力ファイルは無くて当然 (既定バインドで動く) で、
    /// @note ここで false を返すとプロジェクト設定自体が読めなかった扱いになる。
    {
        const std::filesystem::path settingsPath = util::FileSystem::PathFromUtf8(path);
        const std::filesystem::path inputPath =
            settingsPath.has_parent_path()
                ? settingsPath.parent_path() / "Input.inputactions"
                : std::filesystem::path("Input.inputactions");
        /// @note 既定バインドは Move/Look/Jump/Attack/Dodge/Interact/Pause のみで、メニューが使う
        /// @note Submit/Cancel が無い。このファイルが配布物から抜けると «UI だけ反応しない» のに
        /// @note 記録が残らないため、無いことを名指しで警告する。
        std::error_code ec;
        if (!std::filesystem::exists(inputPath, ec)) {
            FBZZ_LOG_WARN("ProjectSettings: %s が見つかりません。"
                          "入力は既定バインドへ落ちます (Submit / Cancel は既定に無いため、"
                          "メニューの決定・戻るが効かなくなります)",
                          util::FileSystem::PathToUtf8(inputPath).c_str());
        } else {
            (void)input::InputActionMap::LoadFromFile(util::FileSystem::PathToUtf8(inputPath));
        }
    }

    return true;
}

bool ProjectSettings::Save(const std::string& path) const
{
    if (!renderer::IsHybridQualityValid(render.hybridQuality)) {
        FBZZ_LOG_WARN("ProjectSettings: invalid Hybrid quality cannot be saved: %s", path.c_str());
        return false;
    }
    return util::FileSystem::WriteText(path, ToToml());
}

std::string ProjectSettings::ToToml() const
{
    toml::array tagArr;
    for (const auto& t : game.tags)
        tagArr.push_back(t);

    toml::array layArr;
    for (const auto& n : game.layerNames)
        layArr.push_back(n);

    toml::table tagTbl;
    tagTbl.insert("list", std::move(tagArr));

    toml::table layTbl;
    layTbl.insert("names", std::move(layArr));

    toml::table physicsTbl;
    physicsTbl.insert("hz",       (int64_t)physics.hz);
    physicsTbl.insert("substeps", (int64_t)physics.substeps);
    physicsTbl.insert("gravity",  Vec3ToArr(physics.gravity));

    /// @note 対称行列なので下三角 (a <= b) だけ書く。両方書くと、手で片方を消したときに
    /// @note «消したのに効いている» が起きる。
    toml::array ignoreArr;
    for (int a = 0; a < 32; ++a)
        for (int b = a; b < 32; ++b) {
            if (physics.collisionMatrix.CanCollide(a, b)) continue;
            toml::array pair;
            pair.push_back((int64_t)a);
            pair.push_back((int64_t)b);
            ignoreArr.push_back(std::move(pair));
        }
    if (!ignoreArr.empty()) physicsTbl.insert("ignoreCollisions", std::move(ignoreArr));

    toml::array outlineColorArr;
    outlineColorArr.push_back((double)render.outlineColor[0]);
    outlineColorArr.push_back((double)render.outlineColor[1]);
    outlineColorArr.push_back((double)render.outlineColor[2]);
    outlineColorArr.push_back((double)render.outlineColor[3]);

    /// @note [render] の保存対象は Load と対になる「プロジェクト全体で固定の構成」のみ。
    /// @note ルック (ポストプロセス / 高度グラフィクス) は PostProcessProfile (.fzdata) が保存する。
    toml::table renderTbl;
    renderTbl.insert("pipeline", RenderingPipelineToString(render.pipeline));
    renderTbl.insert("mode", RenderModeToString(render.modeRequest.mode));
    renderTbl.insert("pathProfile", render.modeRequest.pathProfile == renderer::PathTracingProfile::GAME
        ? "Game" : "Reference");
    renderTbl.insert("rayShadow", render.modeRequest.rayShadow);
    renderTbl.insert("rayReflection", render.modeRequest.rayReflection);
    renderTbl.insert("rayDiffuseGi", render.modeRequest.rayDiffuseGi);
    toml::table hybridQuality;
    asset::RenderPipelineAssetCodec::SaveHybridQuality(render.hybridQuality, hybridQuality);
    renderTbl.insert("hybrid", std::move(hybridQuality));
    if (!renderPipelineAssetPath.empty())
        renderTbl.insert("pipelineAsset", asset::EncodeGuidRef(renderPipelineAssetPath));
    renderTbl.insert("viewMode", static_cast<int>(render.viewMode));

    /// @name Shadow 品質
    renderTbl.insert("shadow",            render.shadowEnabled);
    renderTbl.insert("shadowResolution",  (int64_t)render.shadow.mapResolution);
    renderTbl.insert("shadowPcfRadius",   (int64_t)render.shadow.pcfRadius);
    renderTbl.insert("shadowAutoFitDistance", (double)render.shadow.autoFitDistance);
    renderTbl.insert("shadowCascadeCount",       (int64_t)render.shadow.cascadeCount);
    renderTbl.insert("shadowCascadeSplitLambda", (double)render.shadow.cascadeSplitLambda);
    renderTbl.insert("shadowCascadeBlend",       (double)render.shadow.cascadeBlend);
    renderTbl.insert("pcssEnabled",       render.shadow.pcssEnabled);
    renderTbl.insert("pcssLightRadius",   (double)render.shadow.pcssLightRadius);

    renderTbl.insert("clusteredEnabled",        render.clustered.enabled);
    renderTbl.insert("clusteredMaxDistance",    (double)render.clustered.maxDistance);
    renderTbl.insert("clusteredDebugHeatmap",   render.clustered.debugHeatmap);
    renderTbl.insert("clusteredForceAllLights", render.clustered.forceAllLights);

    /// @name デバッグ表示
    renderTbl.insert("showColliders",        render.showColliders);
    renderTbl.insert("showUIRects",          render.showUIRects);
    renderTbl.insert("showDecalBounds",      render.showDecalBounds);
    renderTbl.insert("showSelectionOutline", render.showSelectionOutline);
    renderTbl.insert("passViewerEnabled",    render.passViewerEnabled);
    if (render.schedulePolicy == renderer::RenderGraphSchedulePolicy::MinimizeLifetimes)
        renderTbl.insert("schedulePolicy", "MinimizeLifetimes");
    {
        toml::array overrideArray;
        for (const renderer::RenderPassOverride& entry : render.passOverrides) {
            if (entry.IsDefault()) continue;
            toml::table overrideTbl;
            overrideTbl.insert("name", entry.name);
            if (!entry.enabled)      overrideTbl.insert("enabled", false);
            if (!entry.allowCulling) overrideTbl.insert("allowCulling", false);
            if (!entry.extraReads.empty()) {
                toml::array readArray;
                for (const std::string& read : entry.extraReads)
                    readArray.push_back(read);
                overrideTbl.insert("extraReads", std::move(readArray));
            }
            overrideArray.push_back(std::move(overrideTbl));
        }
        if (!overrideArray.empty()) renderTbl.insert("passOverride", std::move(overrideArray));
    }

    /// @name パーティクル予算
    renderTbl.insert("particleBudget",        (int64_t)render.particleBudget);
    renderTbl.insert("particleBudgetEnabled", render.particleBudgetEnabled);

    /// @name 選択アウトライン
    renderTbl.insert("outlineWidth", (double)render.outlineWidth);
    renderTbl.insert("outlineColor", std::move(outlineColorArr));

    toml::table audioTbl;
    audioTbl.insert("masterVolume", (double)audio.masterVolume);
    audioTbl.insert("voiceLimit", (int64_t)audio.voiceLimit);
    {
        toml::array busArray;
        for (const audio::BusDesc& desc : audio.buses) {
            toml::table busTbl;
            busTbl.insert("name", desc.name);
            if (!desc.parent.empty()) busTbl.insert("parent", desc.parent);
            busTbl.insert("volume", (double)desc.volume);
            if (desc.lowPassCutoff < 1.0f)
                busTbl.insert("lowPassCutoff", (double)desc.lowPassCutoff);
            if (desc.reverb) busTbl.insert("reverb", true);
            busArray.push_back(std::move(busTbl));
        }
        audioTbl.insert("bus", std::move(busArray));
    }

    toml::table screenTbl;
    screenTbl.insert("width",  (int64_t)screen.width);
    screenTbl.insert("height", (int64_t)screen.height);

    toml::table appTbl;
    appTbl.insert("targetFps", (int64_t)app.targetFps);
    appTbl.insert("renderer", renderer::ToString(app.rendererBackend));

    toml::table windowTbl;
    windowTbl.insert("title",      window.title);
    windowTbl.insert("width",      (int64_t)window.width);
    windowTbl.insert("height",     (int64_t)window.height);
    windowTbl.insert("fullscreen", window.fullscreen);

    toml::table projectTbl;
    projectTbl.insert("name", game.project.name);
    projectTbl.insert("default_scene", game.project.defaultScene);

    toml::table runtimeTbl;
    runtimeTbl.insert("start_scene", game.runtime.startScene);

    toml::table cursorTbl;
    cursorTbl.insert("hardware", cursor.hardwareCursor);
    {
        /// @note 画像を割り当てていない種類は書き出さない。全種類を空文字で並べても
    /// @note «設定してあるのはどれか» が読めなくなるだけで、既定へ倒す判断は Load 側が持つ。
        toml::table shapesTbl;
        for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
            const auto& entry = cursor.shapes[i];
            if (entry.path.empty()) continue;
            toml::table shapeTbl;
            shapeTbl.insert("image", entry.path);
            shapeTbl.insert("hotspot", toml::array{ entry.hotspotX, entry.hotspotY });
            shapesTbl.insert(core::ToString(static_cast<core::CursorShape>(i)),
                             std::move(shapeTbl));
        }
        if (!shapesTbl.empty()) cursorTbl.insert("shapes", std::move(shapesTbl));
    }

    toml::table uiTbl;
    uiTbl.insert("default_font", ui.defaultFontPath);

    toml::table root;
    root.insert("project", std::move(projectTbl));
    root.insert("runtime", std::move(runtimeTbl));
    root.insert("tags",    std::move(tagTbl));
    root.insert("layers",  std::move(layTbl));
    root.insert("physics", std::move(physicsTbl));
    root.insert("render",  std::move(renderTbl));
    root.insert("audio",   std::move(audioTbl));
    root.insert("screen",  std::move(screenTbl));
    root.insert("app",     std::move(appTbl));
    root.insert("window",  std::move(windowTbl));
    root.insert("cursor",  std::move(cursorTbl));
    root.insert("ui",      std::move(uiTbl));

    NormalizeTomlFloats(root);

    std::ostringstream ss;
    ss << root;
    return ss.str();
}

} /// @note namespace fbzz
