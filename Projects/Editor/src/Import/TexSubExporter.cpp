// FBZZ Engine
// TexSubExporter.cpp | fbzz::editor
// textures/ フォルダ内の生画像に "<画像>.meta" サイドカーを自動生成する
// MatSubExporter の後に実行されることを前提とする。
// GuessTextureType で型を推定し、flipGreen (法線マップ) を設定する。
#include <Editor/Import/TexSubExporter.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace fbzz::editor {

namespace {

bool IsImageExtension(const std::string& ext)
{
    static constexpr const char* kExts[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr", ".exr"
    };
    for (const auto* e : kExts)
        if (ext == e) return true;
    return false;
}

std::string LowerExt(const std::string& path)
{
    std::string ext = util::FileSystem::GetExtension(path);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace

bool TexSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;
    namespace fs = std::filesystem;

    if (!ctx.generateTexDescriptors) return true; // .meta 自動生成無効

    const fs::path texDir = util::FileSystem::PathFromUtf8(ctx.manifestDir) / "textures";
    if (!util::FileSystem::Exists(texDir)) return true; // テクスチャなし

    // OpenGL 法線マップ → G を反転する
    const bool flipGreenForNormal =
        (ctx.normalMapConvention == NormalMapConvention::OpenGL);

    for (const auto& entry : fs::directory_iterator(texDir)) {
        if (!entry.is_regular_file()) continue;
        const std::string absPath = util::FileSystem::PathToUtf8(entry.path());
        const std::string ext = LowerExt(absPath);
        if (!IsImageExtension(ext)) continue;

        // 二重拡張子でサイドカーを生成: "Foo.png" -> "Foo.png.meta"。
        // WHY: replace_extension は末尾拡張子を置換してしまうため、必ず文字列末尾へ付加する。
        const std::string metaPath = absPath + ".meta";
        if (util::FileSystem::Exists(metaPath)) continue; // 既存はスキップ

        const std::string filename = util::FileSystem::GetFilename(absPath);
        const TextureType type = GuessTextureType(filename);
        TextureImportSettings settings = DefaultSettingsForType(type);

        // 法線マップのみ flipGreen を適用
        if (flipGreenForNormal && type == TextureType::Normal)
            settings.flipGreen = true;

        // defaultCompression が Auto でなければ全テクスチャに一括適用
        if (ctx.defaultCompression != TextureCompression::Auto)
            settings.compression = ctx.defaultCompression;

        // サイドカーは元画像から一意に導出できるため source パスは保持しない。
        TextureAsset asset;
        asset.settings = settings;

        TexDescSerializer ser;
        if (!ser.Save(asset, metaPath)) return false;
    }

    return true;
}

} // namespace fbzz::editor
