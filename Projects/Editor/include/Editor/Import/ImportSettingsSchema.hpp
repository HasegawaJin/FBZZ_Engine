/// @file    ImportSettingsSchema.hpp
/// @brief   拡張子とテクスチャ型から「意味を持つインポート設定項目」を決める単一の判定表。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note Import Settings の UI は AssetBrowser のモーダル (モデル / テクスチャ) と Inspector の 3 箇所にあり、それぞれが独自に項目を並べていたため .hdr に sRGB チェックが出る等「その拡張子に無関係な項目」が表示されていた。判定をこのヘッダ 1 箇所へ集約し、UI 側は問い合わせるだけにする。拡張子 → ImportCategory の分類と、(category, TextureType) → 有効フィールドのマスクを提供する。UI は「隠す」ではなく「マスクが false なら描かない」で使う。
#pragma once
#include <Engine/Asset/TextureAsset.hpp>
#include <string_view>

namespace fbzz::editor {

/// インポート対象の大分類。どの Import Settings ウィンドウを開くかもこれで決める。
/// @note 拡張子の羅列を各所にコピーすると、対応形式を増やすたびに分岐が食い違うため。
enum class ImportCategory {
    None,            ///< インポート設定を持たない (.txt / .hpp / 不明な拡張子)
    Model,           ///< .fbx / .obj / .gltf / .glb — Assimp 経由で .fzasset へ変換
    Texture,         ///< .png / .jpg / .jpeg / .tga / .bmp — LDR 画像ソース
    TextureHdr,      ///< .hdr / .exr — 浮動小数リニア。sRGB と LDR ブロック圧縮は無意味
    TexturePrebaked, ///< .dds — GPU 直参照。再エンコード系の設定は全て効かない
    Audio,           ///< .wav / .mp3 / .ogg
    Native,          ///< .mat / .scene / .prefab 等 エディターで作る著作物
};

/// 小文字化済み拡張子 (".png" 形式、ドット込み) を分類する。
[[nodiscard]] ImportCategory CategoryForExtension(std::string_view lowerExt);

/// テクスチャ系 3 分類のいずれかか (Texture / TextureHdr / TexturePrebaked)。
[[nodiscard]] bool IsTextureCategory(ImportCategory category);

/// ウィンドウタイトルやバッジに出す表示名。
[[nodiscard]] const char* ImportCategoryLabel(ImportCategory category);

/// テクスチャ設定の各フィールドが、この拡張子 + この型で意味を持つか。
/// @note bool の集合にしておくと UI 側が `if (mask.srgb)` と書くだけで済み、「どの条件で出すか」の判断が UI に散らばらない。
struct TextureFieldMask {
    bool textureType   = true;  ///< Type コンボ自体を触れるか (.hdr は HDR 固定)
    bool srgb          = true;
    bool mipmaps       = true;  ///< Mipmaps チェックボックス
    bool mipDetail     = true;  ///< Mip Filter / Sharpen / Bias
    bool normalizeMips = false; ///< 法線マップ専用
    bool flipGreen     = false; ///< 法線マップ専用
    bool compression   = true;  ///< Format / Quality
    bool maxSize       = true;
    bool sampling      = true;  ///< Wrap U/V / Aniso / Filter
    bool alpha         = true;  ///< Alpha Mode / Dither
    bool sprite        = false; ///< Sprite Mode / Pixels Per Unit / Sprite Rects
};

/// (category, type) の組で有効なフィールドを返す。
/// mipmaps が OFF の設定では mipDetail / normalizeMips も落として返す。
[[nodiscard]] TextureFieldMask TextureFieldsFor(
    ImportCategory category, const asset::TextureImportSettings& settings);

/// この拡張子でその TextureType を選べるか (.png に HDR、.hdr に Color 等を防ぐ)。
[[nodiscard]] bool IsTextureTypeAllowed(ImportCategory category, asset::TextureType type);

/// この型でその圧縮形式を選べるか (法線マップに BC1、HDR に BC7 等を防ぐ)。
[[nodiscard]] bool IsCompressionAllowed(
    ImportCategory category, asset::TextureType type, asset::TextureCompression compression);

/// 拡張子・型に照らして矛盾した値を正す (HDR ソースの sRGB を強制 OFF 等)。
/// 書き換えが発生したら true。UI は読み込み直後と型変更直後に呼ぶ。
bool SanitizeTextureSettings(ImportCategory category, asset::TextureImportSettings& settings);

/// @note 「選べる値の集合」と「それを描くコンボ」が離れていると、片方だけ直したときに UI には出るが Sanitize で戻される食い違いが起きるため同じ場所に置く。シグネチャに ImGui 型を出さないことで、このヘッダの依存は Engine::Asset だけに保つ。

/// TextureType コンボ。IsTextureTypeAllowed が false の型は列挙しない。
/// 選択肢が 1 つしかない場合 (.hdr 等) は無効表示にする。変更されたら true。
bool DrawTextureTypeCombo(const char* label, ImportCategory category, asset::TextureType& type);

/// 圧縮形式コンボ。IsCompressionAllowed が false の形式は列挙しない。変更されたら true。
bool DrawCompressionCombo(const char* label, ImportCategory category,
                          asset::TextureType type, asset::TextureCompression& compression);

} // namespace fbzz::editor
