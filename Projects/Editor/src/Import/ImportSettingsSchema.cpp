// FBZZ Engine
// ImportSettingsSchema.cpp | fbzz::editor
// 拡張子 / テクスチャ型に対する有効インポート設定の判定実装
#include <Editor/Import/ImportSettingsSchema.hpp>
#include <utility>
#include <imgui.h>

namespace fbzz::editor {

ImportCategory CategoryForExtension(std::string_view lowerExt)
{
    // モデルソース: Assimp で読み、Library 側の .fzasset へ変換する。
    if (lowerExt == ".fbx" || lowerExt == ".obj" ||
        lowerExt == ".gltf" || lowerExt == ".glb")
        return ImportCategory::Model;

    // LDR 画像ソース: ImageImporter が .meta の設定でエンコードする。
    if (lowerExt == ".png" || lowerExt == ".jpg" || lowerExt == ".jpeg" ||
        lowerExt == ".tga" || lowerExt == ".bmp")
        return ImportCategory::Texture;

    // 浮動小数リニア画像: ガンマも LDR ブロック圧縮も適用できない。
    if (lowerExt == ".hdr" || lowerExt == ".exr")
        return ImportCategory::TextureHdr;

    // 既にブロック圧縮済み。Editor 側で再エンコードする経路を持たない。
    if (lowerExt == ".dds")
        return ImportCategory::TexturePrebaked;

    if (lowerExt == ".wav" || lowerExt == ".mp3" || lowerExt == ".ogg")
        return ImportCategory::Audio;

    if (lowerExt == ".mat" || lowerExt == ".scene" || lowerExt == ".prefab" ||
        lowerExt == ".anim" || lowerExt == ".animcontroller" || lowerExt == ".animctrl" ||
        lowerExt == ".mask" ||
        lowerExt == ".terrain" || lowerExt == ".fzdata" || lowerExt == ".fnt" ||
        lowerExt == ".ibl" || lowerExt == ".hlsl" || lowerExt == ".vfx")
        return ImportCategory::Native;

    return ImportCategory::None;
}

bool IsTextureCategory(ImportCategory category)
{
    return category == ImportCategory::Texture ||
           category == ImportCategory::TextureHdr ||
           category == ImportCategory::TexturePrebaked;
}

const char* ImportCategoryLabel(ImportCategory category)
{
    switch (category) {
    case ImportCategory::None:            return "Unknown";
    case ImportCategory::Model:           return "Model";
    case ImportCategory::Texture:         return "Texture";
    case ImportCategory::TextureHdr:      return "Texture (HDR)";
    case ImportCategory::TexturePrebaked: return "Texture (Prebaked DDS)";
    case ImportCategory::Audio:           return "Audio";
    case ImportCategory::Native:          return "Asset";
    }
    return "Unknown";
}

namespace {

// リニアデータとして扱う型。sRGB 変換もアルファ合成も意味を持たない。
bool IsLinearDataType(asset::TextureType type)
{
    return type == asset::TextureType::Normal ||
           type == asset::TextureType::Data ||
           type == asset::TextureType::HDR;
}

} // namespace

TextureFieldMask TextureFieldsFor(
    ImportCategory category, const asset::TextureImportSettings& settings)
{
    TextureFieldMask mask;
    const asset::TextureType type = settings.type;

    if (category == ImportCategory::TexturePrebaked) {
        // .dds は既に GPU が読める形になっている。エンコードに関わる項目は全て効かず、
        // 実行時に効くのはサンプラー状態 (Wrap / Aniso / Filter) だけ。
        mask.textureType   = true;
        mask.srgb          = false;
        mask.mipmaps       = false;
        mask.mipDetail     = false;
        mask.normalizeMips = false;
        mask.flipGreen     = false;
        mask.compression   = false;
        mask.maxSize       = false;
        mask.sampling      = true;
        mask.alpha         = false;
        mask.sprite        = false;
        return mask;
    }

    if (category == ImportCategory::TextureHdr) {
        // .hdr / .exr は常にリニア浮動小数。型は HDR に固定し、ガンマとアルファを外す。
        mask.textureType   = false;
        mask.srgb          = false;
        mask.mipmaps       = true;
        mask.mipDetail     = settings.mipmaps;
        mask.normalizeMips = false;
        mask.flipGreen     = false;
        mask.compression   = true;   // 選択肢は BC6H / None に絞る (IsCompressionAllowed)
        mask.maxSize       = true;
        mask.sampling      = true;
        mask.alpha         = false;
        mask.sprite        = false;
        return mask;
    }

    if (category != ImportCategory::Texture) {
        // テクスチャ以外に対してテクスチャ設定を描く経路自体が誤り。全て落とす。
        mask = TextureFieldMask{};
        mask.textureType   = false;
        mask.srgb          = false;
        mask.mipmaps       = false;
        mask.mipDetail     = false;
        mask.compression   = false;
        mask.maxSize       = false;
        mask.sampling      = false;
        mask.alpha         = false;
        return mask;
    }

    // ── LDR 画像 (.png / .jpg / .tga / .bmp) ──────────────────────────────
    mask.textureType = true;
    // 法線・データマスク・HDR はリニアで読む必要があり、sRGB を選ばせてはいけない。
    mask.srgb          = !IsLinearDataType(type);
    mask.mipmaps       = true;
    mask.mipDetail     = settings.mipmaps;
    // Normalize Mipmaps は縮小で崩れた法線を再正規化する処理。法線マップ専用。
    mask.normalizeMips = settings.mipmaps && type == asset::TextureType::Normal;
    // G 反転は OpenGL 規約の法線マップを DirectX 規約へ直す処理。他の型では無意味。
    mask.flipGreen     = type == asset::TextureType::Normal;
    mask.compression   = true;
    mask.maxSize       = true;
    mask.sampling      = true;
    // 法線・データマスクのアルファはチャンネルとして使われるだけで、合成方式の概念がない。
    mask.alpha         = !IsLinearDataType(type);
    mask.sprite        = type == asset::TextureType::Sprite;
    return mask;
}

bool IsTextureTypeAllowed(ImportCategory category, asset::TextureType type)
{
    switch (category) {
    case ImportCategory::Texture:
        // LDR ソースを HDR 型として扱っても情報は増えない。
        return type != asset::TextureType::HDR;
    case ImportCategory::TextureHdr:
        return type == asset::TextureType::HDR;
    case ImportCategory::TexturePrebaked:
        // 圧縮済みブロックから矩形を切り出す経路を持たないため Sprite は除外する。
        return type != asset::TextureType::Sprite;
    default:
        return false;
    }
}

bool IsCompressionAllowed(
    ImportCategory category, asset::TextureType type, asset::TextureCompression compression)
{
    using C = asset::TextureCompression;

    // Auto と None はどの型でも常に選べる (Auto は型から自動選択する)。
    if (compression == C::Auto || compression == C::None) return true;

    if (category == ImportCategory::TextureHdr || type == asset::TextureType::HDR) {
        // 浮動小数を扱えるブロック圧縮は BC6H だけ。
        return compression == C::BC6H;
    }

    switch (type) {
    case asset::TextureType::Normal:
        // 2 チャンネル法線は BC5、高品質が要るときだけ BC7。BC1/BC3 は破綻する。
        return compression == C::BC5 || compression == C::BC7;
    case asset::TextureType::Data:
        // Roughness / Metallic / AO 等のマスク。単/2 チャンネルか BC7。
        return compression == C::BC4 || compression == C::BC5 || compression == C::BC7;
    case asset::TextureType::Color:
    case asset::TextureType::UI:
    case asset::TextureType::Sprite:
        return compression == C::BC1 || compression == C::BC3 || compression == C::BC7;
    case asset::TextureType::HDR:
        return compression == C::BC6H;
    }
    return false;
}

bool SanitizeTextureSettings(ImportCategory category, asset::TextureImportSettings& settings)
{
    const asset::TextureImportSettings before = settings;

    // 型が拡張子に対して不正なら、その拡張子の既定型へ寄せる。
    if (!IsTextureTypeAllowed(category, settings.type)) {
        const asset::TextureType fallback = (category == ImportCategory::TextureHdr)
            ? asset::TextureType::HDR
            : asset::TextureType::Color;
        // 型が変わると既定値一式も変わるため、DefaultSettingsForType で作り直す。
        // ただしユーザーが編集しうる Sprite 矩形だけは引き継ぐ。
        auto sprites = std::move(settings.sprites);
        settings = asset::DefaultSettingsForType(fallback);
        settings.sprites = std::move(sprites);
    }

    const TextureFieldMask mask = TextureFieldsFor(category, settings);

    // マスクで無効化した項目が有効値を持ったままだと、UI に出ないのに
    // インポータ側が拾って挙動が食い違う。ここで無害な既定へ落とす。
    if (!mask.srgb)          settings.srgb = false;
    if (!mask.mipmaps)       settings.mipmaps = false;
    if (!mask.normalizeMips) settings.normalizeMipmaps = false;
    if (!mask.flipGreen)     settings.flipGreen = false;
    if (!mask.alpha) {
        settings.alphaMode   = asset::AlphaMode::None;
        settings.alphaDither = false;
    }
    // Sprite 矩形はユーザーが手で切った著作物なので、型が Sprite でなくなっても消さない。
    // WHY: 型を一時的に Color へ戻して確認しただけで切り直しが消えるのは復旧不能な損失。
    //      使われないだけで害はないため、モードだけ既定へ戻して矩形は保持する。
    if (!mask.sprite) settings.spriteMode = asset::SpriteMode::Single;
    if (!IsCompressionAllowed(category, settings.type, settings.compression))
        settings.compression = asset::TextureCompression::Auto;

    return !(settings == before);
}

// ── UI ヘルパー ──────────────────────────────────────────────────────────────

namespace {

const char* TextureTypeLabel(asset::TextureType type)
{
    switch (type) {
    case asset::TextureType::Color:  return "Color";
    case asset::TextureType::Normal: return "Normal";
    case asset::TextureType::Data:   return "Data";
    case asset::TextureType::HDR:    return "HDR";
    case asset::TextureType::UI:     return "UI";
    case asset::TextureType::Sprite: return "Sprite";
    }
    return "Color";
}

const char* CompressionLabel(asset::TextureCompression compression)
{
    switch (compression) {
    case asset::TextureCompression::Auto: return "Auto";
    case asset::TextureCompression::BC1:  return "BC1 (RGB)";
    case asset::TextureCompression::BC3:  return "BC3 (RGBA)";
    case asset::TextureCompression::BC4:  return "BC4 (R)";
    case asset::TextureCompression::BC5:  return "BC5 (RG)";
    case asset::TextureCompression::BC6H: return "BC6H (HDR)";
    case asset::TextureCompression::BC7:  return "BC7 (High)";
    case asset::TextureCompression::None: return "None (Uncompressed)";
    }
    return "Auto";
}

constexpr asset::TextureType kAllTextureTypes[] = {
    asset::TextureType::Color,  asset::TextureType::Normal, asset::TextureType::Data,
    asset::TextureType::HDR,    asset::TextureType::UI,     asset::TextureType::Sprite,
};

constexpr asset::TextureCompression kAllCompressions[] = {
    asset::TextureCompression::Auto, asset::TextureCompression::BC1,
    asset::TextureCompression::BC3,  asset::TextureCompression::BC4,
    asset::TextureCompression::BC5,  asset::TextureCompression::BC6H,
    asset::TextureCompression::BC7,  asset::TextureCompression::None,
};

} // namespace

bool DrawTextureTypeCombo(const char* label, ImportCategory category, asset::TextureType& type)
{
    int allowedCount = 0;
    for (const asset::TextureType t : kAllTextureTypes)
        if (IsTextureTypeAllowed(category, t)) ++allowedCount;

    // 選択肢が 1 つしかないなら、触れるように見せない方が誤解が少ない。
    const bool locked = allowedCount <= 1;
    if (locked) ImGui::BeginDisabled();

    bool changed = false;
    if (ImGui::BeginCombo(label, TextureTypeLabel(type))) {
        for (const asset::TextureType t : kAllTextureTypes) {
            if (!IsTextureTypeAllowed(category, t)) continue;
            const bool selected = (t == type);
            if (ImGui::Selectable(TextureTypeLabel(t), selected)) {
                if (!selected) { type = t; changed = true; }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (locked) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("この拡張子では %s 固定です", TextureTypeLabel(type));
    }
    return changed;
}

bool DrawCompressionCombo(const char* label, ImportCategory category,
                          asset::TextureType type, asset::TextureCompression& compression)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, CompressionLabel(compression))) {
        for (const asset::TextureCompression c : kAllCompressions) {
            if (!IsCompressionAllowed(category, type, c)) continue;
            const bool selected = (c == compression);
            if (ImGui::Selectable(CompressionLabel(c), selected)) {
                if (!selected) { compression = c; changed = true; }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace fbzz::editor
