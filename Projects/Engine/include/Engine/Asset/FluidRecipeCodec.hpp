/// @file    FluidRecipeCodec.hpp
/// @brief   .fluid — 流体レシピのファイル形式・Reflect・プリセット。
/// @author  Hasegawa Jin
/// @date    2026-09-16

/// @note 設定型そのものは Fluid/FluidRecipe.hpp (FBZZFluid) が持つ。ここに置くのは
/// @note «ディスクと Inspector と AI へのつなぎ» だけで、解く側はこのヘッダーを知らない。
/// @note 焼いたフリップブック / Motion Vector / 速度場 PNG は «結果» でしかなく、浮力を 1 つ
/// @note 変えて焼き直すには元の設定が要る。レシピを残しておけば、同じ流体から解像度違い・
/// @note コマ数違いを何度でも焼き直せる。
/// @see Docs/design/fluid-library.md
#pragma once
#include <Engine/Api.hpp>

#include <Fluid/FluidRecipe.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::scene { struct IReflector; }

namespace fbzz::asset {

/// @note 放電 (PlasmaBurst / ArcHaze) が «雷» そのものでないのは、稲妻の線が VFX Line
/// @note (手続きメッシュ) の担当で、格子で解くと線が数セルに潰れて消えるため。流体が
/// @note 受け持つのは線の «周りの空気» — 弾けた瞬間の球と、後に残る靄。
enum class FluidPreset : uint8_t {
    Smoke = 0, Fire, Explosion, Steam, DustBurst, Ink, MagicWisp, HeatHaze,
    WaterSplash, WaterJet, BloodBurst, LavaBlob, PlasmaBurst, ArcHaze,
    GroundRing, ColdMist, ChargeVortex, EmberBurst, SigilFlare,
    Count
};

[[nodiscard]] FBZZ_ENGINE_API const char* FluidPresetName(FluidPreset preset);

/// @brief 焼けば «それらしい絵» が出る出発点。
/// @note 中身はユーザーが組むのと同じ部品 (sources / forces) で書いてある。
[[nodiscard]] FBZZ_ENGINE_API fluid::FluidRecipe MakeFluidPreset(FluidPreset preset);

/// @brief .fluid (TOML) を読む。欠けたキーは既定値のまま。
/// @return 読めなければ false。outError には理由が入る。
/// @note version 1 の gas_source / liquid_emitter は、読むときに今の kind の側だけを sources へ移す。
[[nodiscard]] FBZZ_ENGINE_API bool LoadFluidRecipe(const std::string& absPath, fluid::FluidRecipe& outRecipe,
                                   std::string* outError = nullptr);

/// @note ファイルI/Oなしで同じ形式を読む。失敗時はoutRecipeを変更しない。
[[nodiscard]] FBZZ_ENGINE_API bool LoadFluidRecipeFromToml(std::string_view text, fluid::FluidRecipe& outRecipe, std::string* outError = nullptr);
/// @note explicitDefaultsは編集API用の空配列も含める。通常保存の形式は変更しない。
[[nodiscard]] FBZZ_ENGINE_API std::string SerializeFluidRecipe(const fluid::FluidRecipe& recipe, bool explicitDefaults = false);

/// @brief .fluid (TOML) を書く。保存は version 2 (部品は [[source]] / [[force]])。
/// @return 書けなければ false。
[[nodiscard]] FBZZ_ENGINE_API bool SaveFluidRecipe(const std::string& absPath, const fluid::FluidRecipe& recipe);

/// @brief AI (JSON)・スキーマ問い合わせ用の項目一覧。
/// @note フィールド名は .fluid (TOML) のキーと同じ (部品の配列は "source" / "force"、
/// @note 動きは "motion"・量のエンベロープは "amount" の中の "key" 配列)。
/// @note TOML の読み書きをこれで置き換えないのは、TOML が enum を文字列で書いており
/// @note IReflector 経由の書き方と一致しないため。既存ファイルを壊さないよう TOML は
/// @note 手書きのまま残し、両者の一致はテスト (FluidRecipeBakeTests) で縛る。
FBZZ_ENGINE_API void ReflectFluidRecipe(fluid::FluidRecipe& recipe, scene::IReflector& r);

} /// @note namespace fbzz::asset
