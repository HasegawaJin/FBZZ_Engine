/// @file    TextureImportOperators.cpp
/// @brief   テクスチャの全インポート設定と矩形編集を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/ImportSettingsSchema.hpp>
#include <Editor/Panels/SpriteEditorPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace fbzz::editor {
namespace {
using asset::TextureImportSettings;
constexpr const char* typeValues[] = {"Color", "Normal", "Data", "HDR", "UI", "Sprite"};
constexpr const char* compressionValues[] = {"Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None"};
constexpr const char* compressionQualityValues[] = {"Fast", "Normal", "High"};
constexpr const char* mipFilterValues[] = {"Box", "Kaiser", "Lanczos"};
constexpr const char* wrapUValues[] = {"Repeat", "Clamp", "Mirror", "Border"};
constexpr const char* wrapVValues[] = {"Repeat", "Clamp", "Mirror", "Border"};
constexpr const char* filterValues[] = {"Point", "Bilinear", "Trilinear", "Anisotropic"};
constexpr const char* alphaModeValues[] = {"Straight", "Premultiplied", "None"};
constexpr const char* spriteModeValues[] = {"Single", "Multiple"};

template<std::size_t N, typename T>
bool ReadEnum(const OpData& data, T& output, const char* const (&values)[N])
{
    if (!data.IsString()) return false;
    for (std::size_t i = 0; i < N; ++i) if (data.AsString() == values[i]) {
        output = static_cast<T>(i); return true;
    }
    return false;
}
bool ReadNumber(const OpData& value, double lo, double hi, double& output)
{
    if (!value.IsInt() && !value.IsFloat()) return false;
    const double number = value.IsInt() ? static_cast<double>(value.AsInt64()) : value.AsDouble();
    if (!std::isfinite(number) || number < lo || number > hi) return false;
    output = number; return true;
}
OpData Describe(const TextureImportSettings& settings)
{
    OpData data = OpData::MakeObject();
    data.Set("type", OpData(typeValues[static_cast<int>(settings.type)]));
    data.Set("compression", OpData(compressionValues[static_cast<int>(settings.compression)]));
    data.Set("compressionQuality", OpData(compressionQualityValues[static_cast<int>(settings.compressionQuality)]));
    data.Set("srgb", OpData(settings.srgb));
    data.Set("mipmaps", OpData(settings.mipmaps));
    data.Set("mipFilter", OpData(mipFilterValues[static_cast<int>(settings.mipFilter)]));
    data.Set("mipSharpen", OpData(settings.mipSharpen));
    data.Set("mipBias", OpData(settings.mipBias));
    data.Set("normalizeMipmaps", OpData(settings.normalizeMipmaps));
    data.Set("flipGreen", OpData(settings.flipGreen));
    data.Set("maxSize", OpData(settings.maxSize));
    data.Set("wrapU", OpData(wrapUValues[static_cast<int>(settings.wrapU)]));
    data.Set("wrapV", OpData(wrapVValues[static_cast<int>(settings.wrapV)]));
    data.Set("filter", OpData(filterValues[static_cast<int>(settings.filter)]));
    data.Set("anisoLevel", OpData(settings.anisoLevel));
    data.Set("alphaMode", OpData(alphaModeValues[static_cast<int>(settings.alphaMode)]));
    data.Set("alphaDither", OpData(settings.alphaDither));
    data.Set("spriteMode", OpData(spriteModeValues[static_cast<int>(settings.spriteMode)]));
    data.Set("pixelsPerUnit", OpData(settings.pixelsPerUnit));
    OpData sprites = OpData::MakeArray();
    for (const auto& sprite : settings.sprites) {
        OpData row = OpData::MakeObject();
        row.Set("id", OpData(sprite.id));
        row.Set("name", OpData(sprite.name));
        row.Set("x", OpData(sprite.x));
        row.Set("y", OpData(sprite.y));
        row.Set("width", OpData(sprite.width));
        row.Set("height", OpData(sprite.height));
        row.Set("pivotX", OpData(sprite.pivotX));
        row.Set("pivotY", OpData(sprite.pivotY));
        row.Set("borderLeft", OpData(sprite.borderLeft));
        row.Set("borderTop", OpData(sprite.borderTop));
        row.Set("borderRight", OpData(sprite.borderRight));
        row.Set("borderBottom", OpData(sprite.borderBottom));
        sprites.Push(std::move(row));
    }
    data.Set("sprites", std::move(sprites)); return data;
}
bool ReadSprites(const OpData& data, std::vector<asset::SpriteRect>& sprites)
{
    if (!data.IsArray() || data.AsArray().size() > 16384) return false;
    std::vector<asset::SpriteRect> result;
    std::unordered_set<std::string> names, ids;
    for (const auto& row : data.AsArray()) {
        if (!row.IsObject()) return false;
        asset::SpriteRect sprite;
        for (const auto& [key, value] : row.AsObject()) {
            if (key == "id") { if (!value.IsString()) return false; sprite.id = value.AsString(); }
            else if (key == "name") { if (!value.IsString()) return false; sprite.name = value.AsString(); }
            else if (key == "x") { double number = 0; if (!value.IsInt() || !ReadNumber(value, 0, 1000000, number)) return false; sprite.x = static_cast<uint32_t>(number); }
            else if (key == "y") { double number = 0; if (!value.IsInt() || !ReadNumber(value, 0, 1000000, number)) return false; sprite.y = static_cast<uint32_t>(number); }
            else if (key == "width") { double number = 0; if (!value.IsInt() || !ReadNumber(value, 0, 1000000, number)) return false; sprite.width = static_cast<uint32_t>(number); }
            else if (key == "height") { double number = 0; if (!value.IsInt() || !ReadNumber(value, 0, 1000000, number)) return false; sprite.height = static_cast<uint32_t>(number); }
            else if (key == "pivotX") { double number = 0; if (!ReadNumber(value, 0, 1, number)) return false; sprite.pivotX = static_cast<float>(number); }
            else if (key == "pivotY") { double number = 0; if (!ReadNumber(value, 0, 1, number)) return false; sprite.pivotY = static_cast<float>(number); }
            else if (key == "borderLeft") { double number = 0; if (!ReadNumber(value, 0, 1000000, number)) return false; sprite.borderLeft = static_cast<float>(number); }
            else if (key == "borderTop") { double number = 0; if (!ReadNumber(value, 0, 1000000, number)) return false; sprite.borderTop = static_cast<float>(number); }
            else if (key == "borderRight") { double number = 0; if (!ReadNumber(value, 0, 1000000, number)) return false; sprite.borderRight = static_cast<float>(number); }
            else if (key == "borderBottom") { double number = 0; if (!ReadNumber(value, 0, 1000000, number)) return false; sprite.borderBottom = static_cast<float>(number); }
            else return false;
        }
        if (sprite.name.empty() || !names.insert(sprite.name).second || (!sprite.id.empty() && !ids.insert(sprite.id).second)) return false;
        result.push_back(std::move(sprite));
    }
    sprites = std::move(result); return true;
}
bool Patch(const OpData& data, TextureImportSettings& settings)
{
    if (!data.IsObject()) return false;
    if (const auto* type = data.Find("type")) {
        asset::TextureType next = settings.type;
        if (!ReadEnum(*type, next, typeValues)) return false;
        if (next != settings.type) {
            auto sprites = std::move(settings.sprites);
            settings = asset::DefaultSettingsForType(next);
            settings.sprites = std::move(sprites);
        }
    }
    for (const auto& [key, value] : data.AsObject()) {
        if (key == "type") { if (!ReadEnum(value, settings.type, typeValues)) return false; }
        else if (key == "compression") { if (!ReadEnum(value, settings.compression, compressionValues)) return false; }
        else if (key == "compressionQuality") { if (!ReadEnum(value, settings.compressionQuality, compressionQualityValues)) return false; }
        else if (key == "srgb") { if (!value.IsBool()) return false; settings.srgb = value.AsBool(); }
        else if (key == "mipmaps") { if (!value.IsBool()) return false; settings.mipmaps = value.AsBool(); }
        else if (key == "mipFilter") { if (!ReadEnum(value, settings.mipFilter, mipFilterValues)) return false; }
        else if (key == "mipSharpen") { double number = 0; if (!ReadNumber(value, 0, 10, number)) return false; settings.mipSharpen = static_cast<float>(number); }
        else if (key == "mipBias") { double number = 0; if (!ReadNumber(value, -16, 16, number)) return false; settings.mipBias = static_cast<float>(number); }
        else if (key == "normalizeMipmaps") { if (!value.IsBool()) return false; settings.normalizeMipmaps = value.AsBool(); }
        else if (key == "flipGreen") { if (!value.IsBool()) return false; settings.flipGreen = value.AsBool(); }
        else if (key == "maxSize") { double number = 0; if (!value.IsInt() || !ReadNumber(value, 1, 16384, number)) return false; settings.maxSize = static_cast<uint32_t>(number); }
        else if (key == "wrapU") { if (!ReadEnum(value, settings.wrapU, wrapUValues)) return false; }
        else if (key == "wrapV") { if (!ReadEnum(value, settings.wrapV, wrapVValues)) return false; }
        else if (key == "filter") { if (!ReadEnum(value, settings.filter, filterValues)) return false; }
        else if (key == "anisoLevel") { double number = 0; if (!value.IsInt() || !ReadNumber(value, 1, 16, number)) return false; settings.anisoLevel = static_cast<uint32_t>(number); }
        else if (key == "alphaMode") { if (!ReadEnum(value, settings.alphaMode, alphaModeValues)) return false; }
        else if (key == "alphaDither") { if (!value.IsBool()) return false; settings.alphaDither = value.AsBool(); }
        else if (key == "spriteMode") { if (!ReadEnum(value, settings.spriteMode, spriteModeValues)) return false; }
        else if (key == "pixelsPerUnit") { double number = 0; if (!ReadNumber(value, 0.001, 100000, number)) return false; settings.pixelsPerUnit = static_cast<float>(number); }
        else if (key == "sprites") { if (!ReadSprites(value, settings.sprites)) return false; }
        else return false;
    }
    return true;
}
bool Resolve(EditorContext& ctx, const std::string& input, std::string& output)
{
    if (ctx.projectRoot.empty() || input.empty()) return false;
    const std::string resolved = input.starts_with("guid:") ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    auto path = util::FileSystem::PathFromUtf8(resolved);
    if (path.is_relative()) path = util::FileSystem::PathFromUtf8(ctx.projectRoot) / path;
    std::error_code error;
    path = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_regular_file(path, error)) return false;
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
    if (error) return false;
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return false;
    output = util::FileSystem::PathToUtf8(path);
    return IsTextureCategory(CategoryForExtension(util::StringUtils::ToLower(path.extension().string())));
}
TextureImportSettings LoadSettings(const std::string& path)
{
    TextureImportSettings settings = asset::DefaultSettingsForType(asset::GuessTextureType(path));
    static_cast<void>(asset::GetCachedTextureImportSettings(path, settings));
    SanitizeTextureSettings(CategoryForExtension(util::StringUtils::ToLower(util::FileSystem::GetExtension(path))), settings);
    return settings;
}
bool SaveSettings(const std::string& path, const TextureImportSettings& settings)
{
    if (asset::AssetDatabase::EnsureGuidMetaUnindexed(path).empty()) return false;
    asset::TextureAsset texture; texture.sourcePath = path; texture.settings = settings;
    return asset::TexDescSerializer{}.Save(texture, path + ".meta");
}
} /// @note namespace

void SpriteEditorPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const std::string_view action : {"inspect", "open", "configure", "control"}) {
        EditorOperator op;
        op.id = "sprite.editor." + std::string(action);
        op.label = op.id;
        op.category = "Sprite Editor";
        op.kind = action == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "Sprite Editorの作業コピーと選択・ズーム・パン。controlでapply/revert/frame_allを行う。";
        if (action == "open") {
            op.params = {{"path", OpParamType::String, "Texture source path", true, {}}};
        } else if (action == "configure") {
            op.params = {
                {"settings", OpParamType::Data, "Partial texture and sprite settings", false, {}},
                {"selected", OpParamType::Int, "Sprite index or -1", false, {}},
                {"zoom", OpParamType::Float, "Canvas zoom", false, {}},
                {"panX", OpParamType::Float, "Horizontal offset in pixels", false, {}},
                {"panY", OpParamType::Float, "Vertical offset in pixels", false, {}}
            };
        } else if (action == "control") {
            OpParam control;
            control.name = "action";
            control.enumValues = {"apply", "revert", "frame_all"};
            op.params.push_back(std::move(control));
        }
        op.poll = [action](const OpContext& context, const OpArgs&) {
            return action == "inspect" || (!context.ctx.projectRoot.empty()
                && (!context.ctx.playMode || context.ctx.playMode->IsInEditor()));
        };
        op.exec = [this, action](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            std::string path;
            const bool active = Resolve(ctx, m_texturePath, path);
            if (action == "open") {
                if (!Resolve(ctx, args.GetString("path"), path)) return OpResult::Err("BAD_PATH", "path");
                if (!OpenWorkingCopy(path + ".meta")) return OpResult::Err("SPRITE_OPEN_FAILED", m_status);
            } else if (action == "inspect") {
                auto data = OpData::MakeObject();
                data.Set("open", active);
                if (active) {
                    data.Set("path", path);
                    data.Set("dirty", m_dirty);
                    data.Set("settings", Describe(m_working.settings));
                    data.Set("selected", m_selected);
                    data.Set("zoom", m_zoom);
                    data.Set("panX", m_pan.x);
                    data.Set("panY", m_pan.y);
                    data.Set("status", m_status);
                }
                return OpResult::Data(std::move(data));
            } else if (!active) {
                return OpResult::Err("SPRITE_NOT_OPEN", "sprite.editor.open");
            } else if (action == "configure") {
                const auto before = m_working.settings;
                auto after = before;
                if (const auto* patch = args.GetData("settings"); patch && (!Patch(*patch, after) || after.type != asset::TextureType::Sprite))
                    return OpResult::Err("BAD_ARG", "settings");
                const int selected = args.GetInt("selected", std::min(m_selected, static_cast<int>(after.sprites.size()) - 1));
                const float zoom = args.GetFloat("zoom", m_zoom);
                const float panX = args.GetFloat("panX", m_pan.x);
                const float panY = args.GetFloat("panY", m_pan.y);
                if (selected < -1 || selected >= static_cast<int>(after.sprites.size()) || !std::isfinite(zoom)
                    || zoom < 0.1f || zoom > 16.0f || !std::isfinite(panX) || !std::isfinite(panY))
                    return OpResult::Err("BAD_ARG", "selected/zoom/panX/panY");
                if (before != after) {
                    m_working.settings = std::move(after);
                    m_dirty = true;
                    PushUndoSnapshot(ctx, before, m_working.settings, "Edit Sprite Draft");
                }
                m_selected = selected;
                m_zoom = zoom;
                m_pan = {panX, panY};
            } else {
                const auto control = args.GetString("action");
                if (control == "frame_all") FrameAll();
                else if (control == "revert") {
                    if (!Revert()) return OpResult::Err("SPRITE_REVERT_FAILED", m_status);
                } else if (control == "apply") {
                    if (!LoadTextureDimensions(ctx)) return OpResult::Err("TEXTURE_UNAVAILABLE", path);
                    ExpandFullSpriteRects();
                    const auto validation = Validate(m_textureWidth, m_textureHeight);
                    if (!validation.valid) return OpResult::Err("INVALID_SPRITES", validation.message);
                    if (!Apply(ctx)) return OpResult::Err("SPRITE_APPLY_FAILED", m_status);
                } else return OpResult::Err("BAD_ARG", "action");
            }
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }
}

void RegisterTextureImportOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "configure", "reset", "import", "trim"}) {
        EditorOperator op;
        const std::string name = action;
        op.id = "asset.texture." + name; op.label = op.id; op.category = "Texture Import";
        op.kind = name == "inspect" ? OpKind::Query : name == "import" ? OpKind::Action : OpKind::Mutation;
        op.desc = "テクスチャの全設定を取得/部分変更/初期化/再読込する。type変更は型の既定設定へ切り替え、spritesは全置換する。設定編集はUndo対応。";
        OpParam path; path.name = "path"; path.type = OpParamType::String; op.params.push_back(std::move(path));
        if (name == "configure") {
            OpParam patch; patch.name = "settings"; patch.type = OpParamType::Data; op.params.push_back(std::move(patch));
        }
        if (name == "trim") { OpParam sprite; sprite.name = "sprite"; sprite.type = OpParamType::String; sprite.desc = "SpriteのIDまたは一意な名前"; op.params.push_back(sprite); }
        op.exec = [name](OpContext& context, const OpArgs& args) {
            EditorContext& ctx = context.ctx;
            std::string path;
            if (!Resolve(ctx, args.GetString("path"), path)) return OpResult::Err("BAD_PATH", "Assets配下のテクスチャ原本を指定してください。");
            if (name != "inspect" && (AssetDirtyRegistry::IsDirty(path) || AssetDirtyRegistry::IsDirty(path + ".meta"))) return OpResult::Err("UNSAVED_ASSET", "編集中のテクスチャを先に保存してください。");
            const auto before = LoadSettings(path);
            auto after = before;
            OpResult result;
            if (name == "configure" || name == "reset" || name == "trim") {
                const auto category = CategoryForExtension(util::StringUtils::ToLower(util::FileSystem::GetExtension(path)));
                if (name == "reset") {
                    after = asset::DefaultSettingsForType(asset::GuessTextureType(path));
                    SanitizeTextureSettings(category, after);
                } else if (name == "trim") {
                    const auto* sprite = asset::FindSprite(before, args.GetString("sprite"));
                    if (!sprite) return OpResult::Err("SPRITE_NOT_FOUND", "Spriteが見つかりません。");
                    const auto index = static_cast<std::size_t>(sprite - before.sprites.data()); std::string error;
                    if (!spriteslice::Trim(path, after.sprites[index], error)) return OpResult::Err("TRIM_FAILED", error);
                } else {
                    const OpData* patch = args.GetData("settings");
                    if (!patch || !Patch(*patch, after) || !IsTextureTypeAllowed(category, after.type)
                        || !IsCompressionAllowed(category, after.type, after.compression))
                        return OpResult::Err("BAD_ARG", "未知の設定キー、型不一致または範囲外です。");
                    auto sanitized = after;
                    if (SanitizeTextureSettings(category, sanitized))
                        return OpResult::Err("BAD_ARG", "テクスチャ型に無効な設定が含まれています。");
                }
                if (after == before) result.noChange = true;
                else {
                    if (!SaveSettings(path, after)) return OpResult::Err("SAVE_FAILED", "テクスチャ設定を保存できません。");
                    after = LoadSettings(path);
                    const std::string project = ctx.projectRoot;
                    const auto apply = [&ctx, project, path](const TextureImportSettings& expected, const TextureImportSettings& next, std::string& reason) {
                        if (ctx.projectRoot != project || !(LoadSettings(path) == expected)) {
                            reason = "Texture settings changed after this history entry."; return false;
                        }
                        if (!SaveSettings(path, next)) { reason = "Texture settings could not be saved."; return false; }
                        ctx.requestAssetBrowserRefresh = true; return true;
                    };
                    result.command = std::make_unique<CheckedLambdaCommand>("Edit Texture Import Settings",
                        [apply, before, after](std::string& reason) { return apply(before, after, reason); },
                        [apply, before, after](std::string& reason) { return apply(after, before, reason); });
                    ctx.requestAssetBrowserRefresh = true;
                }
            } else if (name == "import") {
                asset::InvalidateTextureImportSettings(path);
                asset::AssetManager::Unload<asset::TextureAsset>(path);
                if (!ctx.resources) return OpResult::Err("NO_RESOURCES", "描画リソースがありません。");
                const auto texture = ctx.resources->ReloadTexture(path);
                if (!texture.IsValid()) return OpResult::Err("IMPORT_FAILED", "テクスチャを再読込できません。");
            }
            OpData data = OpData::MakeObject(); data.Set("path", OpData(path));
            data.Set("settings", Describe(LoadSettings(path)));
            result.data = std::move(data); return result;
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
