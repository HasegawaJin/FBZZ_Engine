/// @file    FlipbookBakeOperators.cpp
/// @brief   連番アトラスと手続きVFX画像を生成して材質へ適用する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/FlipbookInspector.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FlipbookAtlasBaker.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cmath>
#include <filesystem>
namespace fbzz::editor {
namespace {
std::string Path(const EditorContext& ctx, std::string value, const char* extension)
{
    if (ctx.projectRoot.empty() || value.empty() || value.find('\0') != std::string::npos) return {};
    if (value.starts_with("guid:")) value = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(value));
    if (value.empty()) return {};
    auto file = util::FileSystem::PathFromUtf8(value); const auto root = util::FileSystem::PathFromUtf8(ctx.projectRoot);
    if (file.is_relative()) file = root / file;
    std::error_code error; file = std::filesystem::weakly_canonical(file, error); const auto assets = std::filesystem::weakly_canonical(root / "Assets", error);
    const auto relative = file.lexically_relative(assets);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == ".." || (*extension && file.extension() != extension)) return {};
    return util::FileSystem::PathToUtf8(file);
}
} /// @note namespace
void RegisterFlipbookBakeOperators(OperatorRegistry& registry)
{
    for (const char* action : {"scan_sequence", "bake_atlas", "generate"}) {
        EditorOperator op; const std::string name = action; op.id = "asset.flipbook." + name; op.label = op.id; op.category = "Assets";
        op.kind = name == "scan_sequence" ? OpKind::Query : OpKind::Action;
        op.desc = "Flipbook Inspectorと同じ画像処理で、連番の照会・アトラス結合・手続き画像生成を行う。material省略時は画像のみ生成し、指定時は設定も保存する。材質の接続は共通Undoへ記録する。";
        if (name == "scan_sequence") { OpParam p; p.name = "firstFrame"; op.params.push_back(p); }
        else {
            OpParam p; p.name = "settings"; p.type = OpParamType::Data; op.params.push_back(p);
            p = {}; p.name = "material"; p.required = false; op.params.push_back(p);
        }
        op.poll = [name](const OpContext& context, const OpArgs&) { return name == "scan_sequence" || !context.ctx.playMode || context.ctx.playMode->IsInEditor(); };
        op.exec = [name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            if (name == "scan_sequence") {
                const auto first = Path(ctx, args.GetString("firstFrame"), ""); std::string error;
                if (first.empty()) return OpResult::Err("BAD_PATH", "Assets内の連番画像を指定してください。");
                const auto frames = CollectFlipbookSequenceFrames(first, error);
                if (!error.empty() || frames.size() > 1024) return OpResult::Err("SEQUENCE_NOT_FOUND", error.empty() ? "連番は1024枚までです。" : error);
                OpData data = OpData::MakeObject(), list = OpData::MakeArray();
                for (const auto& frame : frames) { if (Path(ctx, frame, "").empty()) return OpResult::Err("BAD_PATH", "連番にAssets外の画像があります。"); list.Push(OpData(frame)); }
                data.Set("frames", std::move(list)); data.Set("frameCount", OpData(static_cast<std::int64_t>(frames.size()))); return OpResult::Data(std::move(data));
            }
            const auto* patch = args.GetData("settings"); if (!patch || !patch->IsObject()) return OpResult::Err("BAD_ARG", "settings");
            const auto requestedMaterial = args.GetString("material"); const auto materialPath = requestedMaterial.empty() ? "" : Path(ctx, requestedMaterial, ".mat");
            asset::MaterialAssetAuthoring material; std::string before;
            if (!requestedMaterial.empty() && (materialPath.empty() || AssetDirtyRegistry::IsDirty(materialPath)
                || !asset::LoadMaterialAssetFromFile(materialPath, material) || !util::FileSystem::ReadText(materialPath, before))) return OpResult::Err("MATERIAL_NOT_READY", "Assets内の保存済み材質を指定してください。");
            std::string output; int columns = 0, rows = 0, frameCount = 0;
            if (name == "bake_atlas") {
                asset::FlipbookAtlasBakeSettings settings;
                for (const auto& [key, value] : patch->AsObject()) {
                    if (key == "frames" && value.IsArray() && value.AsArray().size() >= 2 && value.AsArray().size() <= 1024) {
                        for (const auto& frame : value.AsArray()) { if (!frame.IsString()) return OpResult::Err("BAD_ARG", key); const auto file = Path(ctx, frame.AsString(), ""); if (file.empty()) return OpResult::Err("BAD_PATH", "frames"); settings.framePaths.push_back(file); }
                    } else if (key == "output" && value.IsString()) settings.outputPath = Path(ctx, value.AsString(), ".png");
                    else if (key == "columns" && value.IsInt() && value.AsInt64() >= 0 && value.AsInt64() <= 64) settings.columns = static_cast<int>(value.AsInt64());
                    else if (key == "overwrite" && value.IsBool()) settings.overwrite = value.AsBool();
                    else return OpResult::Err("BAD_ARG", key);
                }
                if (settings.framePaths.size() < 2 || settings.outputPath.empty()) return OpResult::Err("BAD_ARG", "frames/outputが必要です。");
                const auto result = asset::BakeFlipbookAtlas(settings); if (!result.success) return OpResult::Err("BAKE_FAILED", result.message);
                output = result.outputPath; columns = result.columns; rows = result.rows; frameCount = result.frameCount;
                auto& flipbook = material.particle.flipbook; flipbook.spriteColumns = columns; flipbook.spriteRows = rows;
                flipbook.spriteStartFrame = 0; flipbook.spriteEndFrame = 0; flipbook.spriteRandomRow = false;
            } else {
                asset::ProceduralFlipbookSettings settings; std::string directory = Path(ctx, "Assets/Textures/Generated/VFX", "");
                for (const auto& [key, value] : patch->AsObject()) {
                    if (key == "preset" && value.IsString()) {
                        bool found = false; for (int i = 0; i < 4; ++i) { const auto preset = static_cast<asset::ProceduralFlipbookPreset>(i); if (value.AsString() == asset::ProceduralFlipbookPresetName(preset)) { settings.preset = preset; found = true; } }
                        if (!found) return OpResult::Err("BAD_ARG", key);
                    } else if (key == "outputDirectory" && value.IsString()) directory = Path(ctx, value.AsString(), "");
                    else if (key == "frameSize" && value.IsInt() && value.AsInt64() >= 32 && value.AsInt64() <= 512) settings.frameSize = static_cast<int>(value.AsInt64());
                    else if (key == "columns" && value.IsInt() && value.AsInt64() >= 2 && value.AsInt64() <= 32) settings.columns = static_cast<int>(value.AsInt64());
                    else if (key == "rows" && value.IsInt() && value.AsInt64() >= 1 && value.AsInt64() <= 16) settings.rows = static_cast<int>(value.AsInt64());
                    else if (key == "seed" && value.IsInt() && value.AsInt64() >= 0 && value.AsInt64() <= 1000000) settings.seed = static_cast<std::uint32_t>(value.AsInt64());
                    else if ((key == "noiseScale" || key == "warpStrength") && (value.IsFloat() || value.IsInt())) {
                        const double number = value.IsFloat() ? value.AsDouble() : static_cast<double>(value.AsInt64());
                        if (!std::isfinite(number) || (key == "noiseScale" && (number < 0.25 || number > 32)) || (key == "warpStrength" && (number < 0 || number > 3))) return OpResult::Err("BAD_ARG", key);
                        if (key == "noiseScale") settings.noiseScale = static_cast<float>(number); else settings.warpStrength = static_cast<float>(number);
                    } else return OpResult::Err("BAD_ARG", key);
                }
                if (directory.empty()) return OpResult::Err("BAD_PATH", "outputDirectoryはAssets内です。");
                const auto result = asset::GenerateProceduralFlipbook(directory, settings); if (!result.success) return OpResult::Err("GENERATE_FAILED", result.message);
                output = result.albedoPath; columns = settings.columns; rows = settings.rows; frameCount = columns * rows;
                auto& flipbook = material.particle.flipbook; flipbook.spriteColumns = columns; flipbook.spriteRows = rows; flipbook.spriteStartFrame = 0;
                flipbook.spriteEndFrame = frameCount - 1; flipbook.flipbookFrameBlending = true; flipbook.spriteRandomStartFrame = false; flipbook.spriteRandomRow = rows > 1;
                flipbook.motionVectorFlipbook = false; material.textures["tex5"].clear();
                const bool distortion = settings.preset == asset::ProceduralFlipbookPreset::Distortion;
                material.particle.distortion = distortion;
                flipbook.flipbookMode = distortion ? scene::ParticleFlipbookMode::FramesPerSecond : scene::ParticleFlipbookMode::Lifetime;
                if (distortion) flipbook.flipbookFramesPerSecond = 24;
                material.blendMode = settings.preset == asset::ProceduralFlipbookPreset::Fire ? renderer::BlendMode::ADDITIVE
                    : settings.preset == asset::ProceduralFlipbookPreset::Explosion ? renderer::BlendMode::PREMULTIPLIED : renderer::BlendMode::ALPHA_BLEND;
            }
            OpResult result = OpResult::Ok(); result.data = OpData::MakeObject();
            const auto reference = "guid:" + asset::AssetDatabase::GuidFromPath(output); result.data.Set("path", OpData(output)); result.data.Set("reference", OpData(reference));
            result.data.Set("columns", OpData(columns)); result.data.Set("rows", OpData(rows)); result.data.Set("frameCount", OpData(frameCount));
            if (!materialPath.empty()) {
                material.renderPath = asset::RenderPath::Particle;
                material.textures["albedo"] = reference;
                if (!asset::SaveMaterialAssetToFile(materialPath, material)) { result.ok = false; result.errorCode = "MATERIAL_SAVE_FAILED"; result.message = "画像は生成済みですが材質を保存できません。"; return result; }
                std::string after; if (!util::FileSystem::ReadText(materialPath, after)) return OpResult::Err("READ_FAILED", "保存した材質を検証できません。");
                const std::string project = ctx.projectRoot;
                const auto apply = [&ctx, project, materialPath](const std::string& expected, const std::string& text, std::string& reason) {
                    std::string current;
                    if (ctx.projectRoot != project || !util::FileSystem::ReadText(materialPath, current) || current != expected) { reason = "Material changed."; return false; }
                    if (!util::FileSystem::WriteTextAtomic(materialPath, text)) { reason = "Material write failed."; return false; }
                    asset::AssetManager::ReloadPath(materialPath); return true;
                };
                result.command = std::make_unique<CheckedLambdaCommand>("Apply Generated Flipbook", [apply, before, after](std::string& reason) { return apply(before, after, reason); }, [apply, before, after](std::string& reason) { return apply(after, before, reason); });
                asset::AssetManager::ReloadPath(materialPath);
            }
            InvalidateFlipbookAtlasPreview(); ctx.requestAssetBrowserRefresh = true; return result;
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
