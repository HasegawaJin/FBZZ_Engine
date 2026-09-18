/// @file    AssetHandlers.cpp
/// @brief   asset.* / sprite.* / vfx.generateMotionVectors のハンドラー。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FlipbookMotionVectors.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <stb_image.h>
#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

/// @brief projectRoot 相対の dir を非再帰列挙する (projectRoot 外は拒否)。
Outcome DoAssetList(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");

    std::error_code ec;
    fs::path base = fs::path(ctx.projectRoot);
    const std::string dir = StringField(payload, "dir");
    if (!dir.empty()) base /= fs::path(dir);

    const fs::path canonicalBase = fs::weakly_canonical(base, ec);
    const fs::path canonicalRoot = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    if (ec) return Outcome::Err("BAD_PATH", "パス解決に失敗しました");

    /// @note projectRoot の外 (`..` を含む) は列挙させない。
    const std::string baseStr = canonicalBase.generic_string();
    const std::string rootStr = canonicalRoot.generic_string();
    if (baseStr.rfind(rootStr, 0) != 0) return Outcome::Err("BAD_PATH", "projectRoot の外は列挙できません");
    if (!fs::is_directory(canonicalBase, ec)) return Outcome::Err("NOT_A_DIR", "ディレクトリではありません: " + dir);

    JsonValue entries = JsonValue::MakeArray();
    for (const auto& item : fs::directory_iterator(canonicalBase, fs::directory_options::skip_permission_denied, ec)) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(item.path().filename().generic_string()));
        entry.Set("dir", JsonValue(item.is_directory(ec)));
        entries.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("dir", JsonValue(dir));
    result.Set("entries", std::move(entries));
    return Outcome::Ok(std::move(result));
}

bool IsTextAssetExtension(const std::string& extension)
{
    static const std::unordered_set<std::string> extensions = {
        ".scene", ".prefab", ".mat", ".tex", ".terrain", ".animcontroller", ".vfx", ".fluid",
        ".toml", ".json", ".meta", ".hpp", ".cpp", ".hlsl", ".hlsli"
    };
    return extensions.contains(LowerAscii(extension));
}

bool ReadSmallTextFile(const std::filesystem::path& path, std::string& outText)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > 2u * 1024u * 1024u || !IsTextAssetExtension(path.extension().string())) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    outText.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> ExtractAssetReferences(const std::string& text)
{
    std::vector<std::string> references;
    const std::string lower = LowerAscii(text);
    size_t position = 0;
    while ((position = lower.find("assets/", position)) != std::string::npos) {
        size_t end = position;
        while (end < text.size()) {
            const char character = text[end];
            if (character == '"' || character == '\'' || character == '\r' || character == '\n'
                || character == ']' || character == '}' || character == ',') break;
            ++end;
        }
        std::string reference = text.substr(position, end - position);
        std::replace(reference.begin(), reference.end(), '\\', '/');
        while (!reference.empty() && std::isspace(static_cast<unsigned char>(reference.back()))) reference.pop_back();
        if (!reference.empty()
            && std::find(references.begin(), references.end(), reference) == references.end()) {
            references.push_back(std::move(reference));
        }
        position = end > position ? end : position + 1;
    }
    return references;
}

Outcome DoAssetInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    fs::path path;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), path, relative)) {
        return Outcome::Err("BAD_PATH", "projectRoot 配下のアセットを指定してください");
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return Outcome::Err("ASSET_NOT_FOUND", "アセットが見つかりません: " + relative);

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("extension", JsonValue(LowerAscii(path.extension().string())));
    result.Set("sizeBytes", JsonValue(static_cast<std::int64_t>(fs::file_size(path, ec))));
    result.Set("modifiedTicks", JsonValue(static_cast<std::int64_t>(fs::last_write_time(path, ec).time_since_epoch().count())));
    const std::string extension = LowerAscii(path.extension().string());
    result.Set("directThumbnail", JsonValue(extension == ".png" || extension == ".jpg" || extension == ".jpeg"));

    std::string text;
    JsonValue references = JsonValue::MakeArray();
    if (ReadSmallTextFile(path, text)) {
        for (const std::string& reference : ExtractAssetReferences(text)) references.Push(JsonValue(reference));
    }
    result.Set("references", std::move(references));

    JsonValue referencedBy = JsonValue::MakeArray();
    int totalReferrers = 0;
    const std::string needle = LowerAscii(relative);
    const fs::path assetsRoot = fs::path(ctx.projectRoot) / "Assets";
    for (fs::recursive_directory_iterator iterator(assetsRoot, fs::directory_options::skip_permission_denied, ec), end;
         iterator != end; iterator.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!iterator->is_regular_file(ec) || iterator->path() == path) continue;
        std::string candidateText;
        if (!ReadSmallTextFile(iterator->path(), candidateText)
            || LowerAscii(candidateText).find(needle) == std::string::npos) continue;
        ++totalReferrers;
        if (referencedBy.AsArray().size() < 100) {
            referencedBy.Push(JsonValue(fs::relative(iterator->path(), fs::path(ctx.projectRoot), ec).generic_string()));
        }
    }
    result.Set("referencedBy", std::move(referencedBy));
    result.Set("referrerCount", JsonValue(totalReferrers));
    return Outcome::Ok(std::move(result));
}

Outcome DoAssetFindUnused(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 100, 1, 500);
    const fs::path projectRoot(ctx.projectRoot);
    const fs::path assetsRoot = projectRoot / "Assets";
    std::error_code ec;
    std::unordered_set<std::string> referenced;
    std::vector<fs::path> candidates;
    static const std::unordered_set<std::string> candidateExtensions = {
        ".mat", ".mesh", ".tex", ".prefab", ".animcontroller", ".vfx", ".wav", ".ogg", ".fbx", ".png", ".jpg", ".dds"
    };
    for (fs::recursive_directory_iterator iterator(assetsRoot, fs::directory_options::skip_permission_denied, ec), end;
         iterator != end; iterator.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!iterator->is_regular_file(ec)) continue;
        const std::string extension = LowerAscii(iterator->path().extension().string());
        if (candidateExtensions.contains(extension)) candidates.push_back(iterator->path());
        std::string text;
        if (!ReadSmallTextFile(iterator->path(), text)) continue;
        for (const std::string& reference : ExtractAssetReferences(text)) referenced.insert(LowerAscii(reference));
    }

    JsonValue unused = JsonValue::MakeArray();
    int total = 0;
    for (const fs::path& candidate : candidates) {
        const std::string relative = fs::relative(candidate, projectRoot, ec).generic_string();
        if (referenced.contains(LowerAscii(relative))) continue;
        ++total;
        if (static_cast<int>(unused.AsArray().size()) < limit) unused.Push(JsonValue(relative));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("assets", std::move(unused));
    result.Set("count", JsonValue(total));
    result.Set("truncated", JsonValue(total > limit));
    result.Set("heuristic", JsonValue(true));
    return Outcome::Ok(std::move(result));
}

Outcome DoAssetThumbnail(editor::EditorContext& ctx, const JsonValue& payload)
{
    namespace fs = std::filesystem;
    fs::path path;
    std::string relative;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), path, relative)) {
        return Outcome::Err("BAD_PATH", "projectRoot 配下の画像を指定してください");
    }
    const std::string extension = LowerAscii(path.extension().string());
    const char* mime = extension == ".png" ? "image/png"
        : (extension == ".jpg" || extension == ".jpeg" ? "image/jpeg" : nullptr);
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (mime == nullptr || ec || size == 0 || size > 16u * 1024u * 1024u) {
        return Outcome::Err("NO_DIRECT_THUMBNAIL", "PNG/JPEG の直接サムネイルだけを取得できます");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Outcome::Err("ASSET_READ_FAILED", "画像を読み取れません: " + relative);
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>{});
    JsonValue result = JsonValue::MakeObject();
    result.Set("mimeType", JsonValue(mime));
    result.Set("base64", JsonValue(Base64Encode(bytes)));
    result.Set("path", JsonValue(relative));
    return Outcome::Ok(std::move(result));
}
} // namespace

/// @name Sprite (Texture のサブアセット)
/// @note Sprite は独立ファイルではないため `asset.list` / `asset.thumbnail` から見えない。
///       一覧・切り抜き (`sprite.list` / `sprite.thumbnail`) が無いと AI は UUID を書き写す以外に割り当てられない。
/// @see Docs/design/sprite-reference.md

/// @brief sprites を持たない Single Texture の暗黙参照 (`ResolveSpriteReference` の Single) かを判定する。
/// @note この 1 枚は ID を持たないため、ID が引けないことを壊れていると判定してはいけない。
bool IsImplicitSingleSprite(const std::string& texturePath, const std::string& token)
{
    asset::TextureImportSettings settings;
    if (!asset::GetCachedTextureImportSettings(texturePath, settings)) return false;
    if (settings.type != asset::TextureType::Sprite
        || settings.spriteMode != asset::SpriteMode::Single
        || !settings.sprites.empty()) return false;
    return token == util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(texturePath).stem());
}
namespace {

/// @brief 参照文字列は「そのまま貼れる完成形」を返す。
/// @note AI に組み立てさせると `::sprite::` 区切りの写し間違いが静かにアトラス全面へ化ける。
std::string MakeSpriteReferenceFor(const std::string& relativeTexturePath,
                                   const asset::SpriteRect& sprite,
                                   bool nameIsUnique)
{
    const bool useName = nameIsUnique && !sprite.name.empty();
    return asset::MakeSpriteReference(relativeTexturePath,
                                      useName ? sprite.name : sprite.id);
}

bool LoadSpriteSettings(editor::EditorContext& ctx, const JsonValue& payload,
                        std::filesystem::path& outFile, std::string& outRelative,
                        asset::TextureImportSettings& outSettings, Outcome& err)
{
    std::error_code ec;
    if (!ResolveProjectFile(ctx, StringField(payload, "path"), outFile, outRelative)
        || !std::filesystem::is_regular_file(outFile, ec)) {
        err = Outcome::Err("ASSET_NOT_FOUND", "projectRoot 配下のテクスチャを指定してください");
        return false;
    }
    if (!asset::GetCachedTextureImportSettings(outFile.generic_string(), outSettings)) {
        err = Outcome::Err("NO_SPRITE_META",
            "この画像に .meta がありません。Inspector で Texture Type を Sprite にしてください");
        return false;
    }
    if (outSettings.type != asset::TextureType::Sprite) {
        err = Outcome::Err("NOT_A_SPRITE",
            "Texture Type が Sprite ではありません: " + outRelative);
        return false;
    }
    return true;
}

/// @brief 名前がテクスチャ内で一意かを引けるようにする。
/// @note 重複した名前はどちらを指すか決まらないため、その Sprite だけ ID で返す。
std::unordered_map<std::string, int> CountSpriteNames(
    const asset::TextureImportSettings& settings)
{
    std::unordered_map<std::string, int> counts;
    for (const asset::SpriteRect& sprite : settings.sprites) ++counts[sprite.name];
    return counts;
}

Outcome DoSpriteList(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::filesystem::path file;
    std::string relative;
    asset::TextureImportSettings settings;
    Outcome err = Outcome::Ok(JsonValue::MakeObject());
    if (!LoadSpriteSettings(ctx, payload, file, relative, settings, err)) return err;

    const auto nameCounts = CountSpriteNames(settings);
    JsonValue sprites = JsonValue::MakeArray();
    for (const asset::SpriteRect& sprite : settings.sprites) {
        const auto found = nameCounts.find(sprite.name);
        const bool unique = found != nameCounts.end() && found->second == 1;
        JsonValue item = JsonValue::MakeObject();
        item.Set("id", JsonValue(sprite.id));
        item.Set("name", JsonValue(sprite.name));
        item.Set("x", JsonValue(static_cast<int>(sprite.x)));
        item.Set("y", JsonValue(static_cast<int>(sprite.y)));
        item.Set("width", JsonValue(static_cast<int>(sprite.width)));
        item.Set("height", JsonValue(static_cast<int>(sprite.height)));
        item.Set("pivotX", JsonValue(static_cast<double>(sprite.pivotX)));
        item.Set("pivotY", JsonValue(static_cast<double>(sprite.pivotY)));
        item.Set("reference", JsonValue(MakeSpriteReferenceFor(relative, sprite, unique)));
        if (!unique) item.Set("nameIsAmbiguous", JsonValue(true));
        sprites.Push(std::move(item));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    result.Set("spriteMode", JsonValue(settings.spriteMode == asset::SpriteMode::Multiple
                                       ? "Multiple" : "Single"));
    result.Set("pixelsPerUnit", JsonValue(static_cast<double>(settings.pixelsPerUnit)));
    result.Set("count", JsonValue(static_cast<int>(settings.sprites.size())));
    result.Set("sprites", std::move(sprites));
    result.Set("hint", JsonValue(std::string(
        "reference をそのまま UIImage.texturePath / SpriteRenderer.spritePath / "
        ".mat の albedo (メッシュ描画の .mat のみ — Particle / UI / Decal は切り抜きが効きません) / "
        ".fluid の texture 発生源へ入れてください (component_set)。"
        "どのコマがどの絵かは sprite_thumbnail で確認できます。"
        "名前は sprite_rename で付け直せます (ID は変わらないので既存の参照は切れません)。")));
    return Outcome::Ok(std::move(result));
}

/// @brief `sprite.thumbnail`: 1 コマだけを切り抜いた PNG を返す。
/// @note シート全体を返しても何番目かは見えず、絵を見て選べる形が名前付け・割り当てを AI へ任せる最低条件になる。
Outcome DoSpriteThumbnail(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::filesystem::path file;
    std::string relative;
    asset::TextureImportSettings settings;
    Outcome err = Outcome::Ok(JsonValue::MakeObject());
    if (!LoadSpriteSettings(ctx, payload, file, relative, settings, err)) return err;

    const std::string token = StringField(payload, "sprite");
    if (token.empty()) return Outcome::Err("BAD_ARG", "sprite (ID または名前) が必要です");
    const asset::SpriteRect* sprite = asset::FindSprite(settings, token);
    if (sprite == nullptr)
        return Outcome::Err("SPRITE_NOT_FOUND",
            "この ID / 名前の Sprite がありません: " + token + " (sprite_list で確認してください)");

    int sourceWidth = 0;
    int sourceHeight = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(util::FileSystem::PathToUtf8(file).c_str(),
                                &sourceWidth, &sourceHeight, &channels, 4);
    if (pixels == nullptr || sourceWidth <= 0 || sourceHeight <= 0) {
        if (pixels) stbi_image_free(pixels);
        return Outcome::Err("DECODE_FAILED", "画像をデコードできません: " + relative);
    }

    /// @note 幅 / 高さ 0 は「画像全体」を意味する (Single Sprite の表現)。
    const int rectX = std::clamp(static_cast<int>(sprite->x), 0, sourceWidth - 1);
    const int rectY = std::clamp(static_cast<int>(sprite->y), 0, sourceHeight - 1);
    const int rectW = sprite->width > 0
        ? std::min(static_cast<int>(sprite->width), sourceWidth - rectX) : sourceWidth - rectX;
    const int rectH = sprite->height > 0
        ? std::min(static_cast<int>(sprite->height), sourceHeight - rectY) : sourceHeight - rectY;
    if (rectW <= 0 || rectH <= 0) {
        stbi_image_free(pixels);
        return Outcome::Err("EMPTY_RECT", "Sprite の矩形が空です");
    }

    /// @note 転送量を抑えるため長辺 256 までへ間引く。コマの識別に等倍は要らない。
    constexpr int kMaxSide = 256;
    const int step = std::max(1, (std::max(rectW, rectH) + kMaxSide - 1) / kMaxSide);
    const int outWidth = std::max(1, rectW / step);
    const int outHeight = std::max(1, rectH / step);
    std::vector<uint8_t> cropped(static_cast<size_t>(outWidth) * outHeight * 4);
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            const size_t src = (static_cast<size_t>(rectY + y * step) * sourceWidth
                                + static_cast<size_t>(rectX + x * step)) * 4;
            const size_t dst = (static_cast<size_t>(y) * outWidth + x) * 4;
            std::memcpy(cropped.data() + dst, pixels + src, 4);
        }
    }
    stbi_image_free(pixels);

    size_t pngSize = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(
        cropped.data(), outWidth, outHeight, 4, &pngSize, 6, MZ_FALSE);
    if (png == nullptr || pngSize == 0) {
        if (png) mz_free(png);
        return Outcome::Err("ENCODE_FAILED", "PNG へ変換できません");
    }
    const std::vector<uint8_t> bytes(static_cast<uint8_t*>(png),
                                     static_cast<uint8_t*>(png) + pngSize);
    mz_free(png);

    const auto nameCounts = CountSpriteNames(settings);
    const auto found = nameCounts.find(sprite->name);
    JsonValue result = JsonValue::MakeObject();
    result.Set("mimeType", JsonValue("image/png"));
    result.Set("base64", JsonValue(Base64Encode(bytes)));
    result.Set("width", JsonValue(outWidth));
    result.Set("height", JsonValue(outHeight));
    result.Set("sourceWidth", JsonValue(rectW));
    result.Set("sourceHeight", JsonValue(rectH));
    result.Set("name", JsonValue(sprite->name));
    result.Set("reference", JsonValue(MakeSpriteReferenceFor(
        relative, *sprite, found != nameCounts.end() && found->second == 1)));
    return Outcome::Ok(std::move(result));
}

/// @brief `sprite.rename` / `sprite.slice`: `.meta` の `[texture].sprites` を書き換える。
/// @note ID はこの Sprite への参照そのもの。名前だけ変えて ID まで振り直すと `.scene` の参照がアトラス全面に化ける。
/// @note slice も重なりで ID を引き継ぐ (Sprite Editor の Smart と同じ規則)。
std::unique_ptr<ICommand> BuildSpriteCommand(editor::EditorContext& ctx,
                                             const std::string& type,
                                             const JsonValue& payload,
                                             Outcome& err,
                                             JsonValue* detailSink)
{
    namespace fs = std::filesystem;
    fs::path absolute;
    std::string relative;
    asset::TextureImportSettings settings;
    if (!LoadSpriteSettings(ctx, payload, absolute, relative, settings, err)) return nullptr;

    const std::string metaPath = absolute.generic_string() + ".meta";
    const asset::TextureImportSettings before = settings;

    if (type == "sprite.rename") {
        const std::string token = StringField(payload, "sprite");
        const std::string newName = StringField(payload, "name");
        if (token.empty() || newName.empty()) {
            err = Outcome::Err("BAD_ARG", "sprite (ID または名前) と name が必要です");
            return nullptr;
        }
        const asset::SpriteRect* target = asset::FindSprite(settings, token);
        if (target == nullptr) {
            err = Outcome::Err("SPRITE_NOT_FOUND", "この ID / 名前の Sprite がありません: " + token);
            return nullptr;
        }
        /// @note 添字で指す。ID が空の (移行前の) `.meta` でも 1 件だけ確実に書き換えるため。
        const std::size_t targetIndex =
            static_cast<std::size_t>(target - settings.sprites.data());
        const std::string targetId = target->id;
        for (std::size_t i = 0; i < settings.sprites.size(); ++i) {
            if (i != targetIndex && settings.sprites[i].name == newName) {
                err = Outcome::Err("DUPLICATE_NAME",
                    "同じ名前の Sprite が既にあります: " + newName
                    + " (名前は参照キーを兼ねるのでテクスチャ内で一意である必要があります)");
                return nullptr;
            }
        }
        settings.sprites[targetIndex].name = newName;

        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("id", JsonValue(targetId));
            detailSink->Set("name", JsonValue(newName));
            detailSink->Set("reference",
                JsonValue(asset::MakeSpriteReference(relative, newName)));
        }
    } else if (type == "sprite.slice") {
        /// @note 生成も畳み込みも Sprite Editor と同じ実装を通す (`Editor/Util/SpriteSlicer.hpp`)。
        const std::string imagePath = util::FileSystem::PathToUtf8(absolute);
        const auto intField = [&payload](const char* key, int fallback) {
            const JsonValue* value = payload.Find(key);
            return value != nullptr ? value->AsInt(fallback) : fallback;
        };
        const auto floatField = [&payload](const char* key, float fallback) {
            const JsonValue* value = payload.Find(key);
            return value != nullptr
                ? static_cast<float>(value->AsNumber(static_cast<double>(fallback)))
                : fallback;
        };
        const std::string modeName = StringField(payload, "mode");
        const spriteslice::ExistingMode mode =
            modeName == "replace" ? spriteslice::ExistingMode::DeleteExisting
          : modeName == "safe"    ? spriteslice::ExistingMode::Safe
                                  : spriteslice::ExistingMode::Smart;
        const std::string prefix = StringField(payload, "prefix").empty()
            ? util::FileSystem::PathToUtf8(absolute.stem()) + "_"
            : StringField(payload, "prefix");

        spriteslice::Result generated;
        if (StringField(payload, "type") == "automatic") {
            spriteslice::AutoTrimParams params;
            params.pivotX   = floatField("pivotX", 0.5f);
            params.pivotY   = floatField("pivotY", 0.5f);
            params.baseName = prefix;
            generated = spriteslice::GenerateAutoTrim(imagePath, params);
        } else {
            int imageWidth = 0;
            int imageHeight = 0;
            if (!stbi_info(imagePath.c_str(), &imageWidth, &imageHeight, nullptr)
                || imageWidth <= 0 || imageHeight <= 0) {
                err = Outcome::Err("DECODE_FAILED", "画像の寸法を取得できません: " + relative);
                return nullptr;
            }
            spriteslice::GridParams params;
            params.columns        = intField("columns", 0);
            params.rows           = intField("rows", 0);
            params.cellWidth      = intField("cellWidth", 0);
            params.cellHeight     = intField("cellHeight", 0);
            params.byCellCount    = params.columns > 0 || params.rows > 0;
            if (!params.byCellCount && (params.cellWidth <= 0 || params.cellHeight <= 0)) {
                err = Outcome::Err("BAD_ARG",
                    "columns/rows か cellWidth/cellHeight のどちらかを指定してください "
                    "(type=automatic なら不要です)");
                return nullptr;
            }
            if (params.byCellCount) {
                params.columns = std::max(1, params.columns);
                params.rows    = std::max(1, params.rows);
            }
            params.offsetX        = intField("offsetX", 0);
            params.offsetY        = intField("offsetY", 0);
            params.paddingX       = intField("paddingX", 0);
            params.paddingY       = intField("paddingY", 0);
            params.pivotX         = floatField("pivotX", 0.5f);
            params.pivotY         = floatField("pivotY", 0.5f);
            params.keepEmptyRects = payload.Find("keepEmptyRects") != nullptr
                ? payload.Find("keepEmptyRects")->AsBool() : true;
            params.baseName       = prefix;
            generated = spriteslice::GenerateGrid(
                imagePath, static_cast<uint32_t>(imageWidth),
                static_cast<uint32_t>(imageHeight), params);
        }
        if (!generated.error.empty()) {
            err = Outcome::Err("EMPTY_SLICE", generated.error);
            return nullptr;
        }

        int reused = 0;
        settings.sprites = spriteslice::MergeIntoExisting(
            before.sprites, std::move(generated.sprites), mode, &reused);
        settings.spriteMode = asset::SpriteMode::Multiple;

        if (detailSink != nullptr) {
            detailSink->Set("path", JsonValue(relative));
            detailSink->Set("count", JsonValue(static_cast<int>(settings.sprites.size())));
            detailSink->Set("reusedIds", JsonValue(reused));
            /// @note replace 以外は既存の矩形を消さないため、参照が切れるのは replace のときだけ。
            detailSink->Set("brokenReferences",
                JsonValue(mode == spriteslice::ExistingMode::DeleteExisting
                          ? static_cast<int>(before.sprites.size()) : 0));
        }
    } else {
        err = Outcome::Err("UNKNOWN_TYPE", "未対応の sprite 操作です: " + type);
        return nullptr;
    }

    const auto write = [metaPath, absolute](const asset::TextureImportSettings& value) {
        asset::TextureAsset asset;
        asset.sourcePath = absolute.generic_string();
        asset.settings = value;
        const asset::TexDescSerializer serializer;
        (void)serializer.Save(asset, metaPath);
    };
    editor::EditorContext* context = &ctx;
    const asset::TextureImportSettings after = settings;
    return std::make_unique<LambdaCommand>("AI: " + type,
        [write, after, context]() {
            write(after);
            context->requestAssetBrowserRefresh = true;
        },
        [write, before, context]() {
            write(before);
            context->requestAssetBrowserRefresh = true;
        });
}

std::unique_ptr<ICommand> BuildGenerateMotionVectorsCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    /// @note モーションベクター生成はテクスチャファイルだけを扱い Scene を必要としない。
    ///       Scene 必須チェックより前に置くこと (独立 VFX Editor にはゲーム Scene が無い)。
    if (type == "vfx.generateMotionVectors") {
        namespace fs = std::filesystem;
        fs::path textureFile;
        std::string texturePath;
        if (!ResolveProjectFile(ctx, StringField(payload, "texturePath"), textureFile, texturePath)
            || !fs::is_regular_file(textureFile)) {
            err = Outcome::Err("TEXTURE_NOT_FOUND", "projectRoot 配下のテクスチャを指定してください");
            return nullptr;
        }
        asset::FlipbookMotionVectorSettings settings;
        settings.columns = payload.Find("columns") != nullptr ? payload.Find("columns")->AsInt() : 1;
        settings.rows = payload.Find("rows") != nullptr ? payload.Find("rows")->AsInt() : 1;
        if (const JsonValue* radius = payload.Find("searchRadius"); radius != nullptr)
            settings.searchRadius = radius->AsInt();
        if (const JsonValue* loopValue = payload.Find("loop"); loopValue != nullptr)
            settings.loop = loopValue->AsBool();
        if (const JsonValue* rowValue = payload.Find("rowSequences"); rowValue != nullptr)
            settings.rowSequences = rowValue->AsBool();

        /// @note 任意で生成した MV と推奨 strength を `.mat` へ書き込む。strength は生成結果 (最大移動量) でしか決まらないため。
        /// @note 生成は dryRun でもファイルを書かないよう Lambda 内で行うため、適用も同じ場所で行う。
        fs::path materialFile;
        std::string materialPath;
        asset::MaterialAsset oldMaterial;
        const bool applyToMaterial = !StringField(payload, "materialPath").empty();
        if (applyToMaterial) {
            if (!ResolveProjectFile(ctx, StringField(payload, "materialPath"), materialFile, materialPath)
                || LowerAscii(materialFile.extension().string()) != ".mat"
                || !fs::is_regular_file(materialFile)) {
                err = Outcome::Err("MATERIAL_NOT_FOUND", "projectRoot 配下の .mat を指定してください");
                return nullptr;
            }
            if (!asset::LoadMaterialAssetFromFile(materialFile.generic_string(), oldMaterial)) {
                err = Outcome::Err("MATERIAL_READ_FAILED", "MaterialAsset を読み取れません: " + materialPath);
                return nullptr;
            }
        }
        fs::path motionRelative(texturePath);
        motionRelative.replace_extension();
        motionRelative += "_mv.png";

        /// @note MV の生成は新規ファイルを足すだけなので Undo で消さない。同名の MV が既にあった場合、
        ///       ユーザーが手で用意したアトラスを破壊しうるため、生成物の削除は AssetBrowser から明示的に行わせる。
        /// @note `.mat` を書き換えた場合だけ、その書き換えを Undo で元に戻す。
        editor::EditorContext* context = &ctx;
        const std::string resolved = textureFile.generic_string();
        const std::string motionPath = motionRelative.generic_string();
        return std::make_unique<LambdaCommand>("AI: Generate Motion Vectors",
            [context, resolved, settings, applyToMaterial, materialFile, oldMaterial, motionPath]() {
                const auto result = asset::GenerateFlipbookMotionVectors(resolved, settings);
                if (!result.success) return;
                context->requestAssetBrowserRefresh = true;
                if (!applyToMaterial) return;
                asset::MaterialAsset updated = oldMaterial;
                updated.textures["tex5"] = motionPath;
                updated.particle.flipbook.motionVectorFlipbook = true;
                updated.particle.flipbook.motionVectorStrength = result.recommendedStrength;
                updated.particle.flipbook.flipbookFrameBlending = true;
                if (asset::SaveMaterialAssetToFile(materialFile.generic_string(), updated))
                    asset::AssetManager::ReloadPath(materialFile.generic_string());
            },
            [context, applyToMaterial, materialFile, oldMaterial]() {
                if (!applyToMaterial) return;
                if (asset::SaveMaterialAssetToFile(materialFile.generic_string(), oldMaterial))
                    asset::AssetManager::ReloadPath(materialFile.generic_string());
                context->requestAssetBrowserRefresh = true;
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

Outcome DoAssetImport(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    namespace fs = std::filesystem;
    if (ctx.projectRoot.empty()) return Outcome::Err("NO_PROJECT", "projectRoot が未設定です");
    const std::string src = StringField(payload, "src");
    const std::string dst = StringField(payload, "dst");
    if (src.empty() || dst.empty()) return Outcome::Err("BAD_ARG", "src / dst が必要です");
    std::error_code ec;
    const fs::path srcPath = fs::weakly_canonical(fs::path(src), ec);
    const fs::path dstPath = fs::weakly_canonical(fs::path(ctx.projectRoot) / fs::path(dst), ec);
    const fs::path rootPath = fs::weakly_canonical(fs::path(ctx.projectRoot), ec);
    if (ec) return Outcome::Err("BAD_PATH", "パス解決に失敗しました");
    /// @note 取り込み先は projectRoot 配下限定 (取り込み元は任意許可)。
    if (dstPath.generic_string().rfind(rootPath.generic_string(), 0) != 0) {
        return Outcome::Err("BAD_PATH", "dst は projectRoot の外です");
    }
    if (!fs::exists(srcPath, ec) || fs::is_directory(srcPath, ec)) {
        return Outcome::Err("SRC_NOT_FOUND", "src ファイルがありません: " + src);
    }
    if (dryRun) {
        JsonValue result = JsonValue::MakeObject();
        result.Set("dryRun", JsonValue(true));
        result.Set("would", JsonValue("asset.import"));
        result.Set("dst", JsonValue(dstPath.generic_string()));
        return Outcome::Ok(std::move(result));
    }
    fs::create_directories(dstPath.parent_path(), ec);
    fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing, ec);
    if (ec) return Outcome::Err("IMPORT_FAILED", "コピーに失敗しました: " + ec.message());
    /// @note 取り込み後に AssetBrowser を更新させる。
    ctx.requestAssetBrowserRefresh = true;
    JsonValue result = JsonValue::MakeObject();
    result.Set("imported", JsonValue(dst));
    return Outcome::Ok(std::move(result));
}
} // namespace

void RegisterAssetHandlers(BusHandlerTable& table)
{
    table.AddQuery("asset.list", [](BusCall& call) { return DoAssetList(call.ctx, call.payload); });
    table.AddQuery("asset.inspect", [](BusCall& call) { return DoAssetInspect(call.ctx, call.payload); });
    table.AddQuery("asset.findUnused", [](BusCall& call) { return DoAssetFindUnused(call.ctx, call.payload); });
    table.AddQuery("asset.thumbnail", [](BusCall& call) { return DoAssetThumbnail(call.ctx, call.payload); });
    table.AddQuery("sprite.list", [](BusCall& call) { return DoSpriteList(call.ctx, call.payload); });
    table.AddQuery("sprite.thumbnail", [](BusCall& call) { return DoSpriteThumbnail(call.ctx, call.payload); });

    table.AddCommand("asset.import", [](BusCall& call) { return DoAssetImport(call.ctx, call.payload, call.dryRun); });

    /// @note Sprite の編集は .meta の中で完結し Scene を要らない (Sprite Editor と同じ)。
    const BuilderFn sprite = [](editor::EditorContext& ctx, const std::string& type, const JsonValue& payload,
                                Outcome& err, std::shared_ptr<std::string>, JsonValue* detailSink) {
        return BuildSpriteCommand(ctx, type, payload, err, detailSink);
    };
    table.AddBuilder("sprite.rename", sprite);
    table.AddBuilder("sprite.slice", sprite);
    table.AddBuilder("vfx.generateMotionVectors", BuildGenerateMotionVectorsCommand);
}

} // namespace fbzz::editor::ai::bus
