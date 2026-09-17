/// @file    PostProcessProfile.cpp
/// @brief   オーバーライドリストの適用・複製と、TOML (.fzdata) との相互変換。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace fbzz::asset {

void PostProcessProfile::ApplyTo(renderer::VolumeSettings& target, float weight) const
{
    for (const auto& entry : overrides) {
        if (!entry || !entry->active) continue;
        entry->Apply(target, weight);
    }
}

bool PostProcessProfile::Contains(const char* typeName) const
{
    if (!typeName) return false;
    for (const auto& entry : overrides) {
        if (entry && std::strcmp(entry->GetTypeName(), typeName) == 0)
            return true;
    }
    return false;
}

std::vector<std::unique_ptr<VolumeOverride>> PostProcessProfile::CloneOverrides() const
{
    std::vector<std::unique_ptr<VolumeOverride>> copy;
    copy.reserve(overrides.size());
    for (const auto& entry : overrides)
        if (entry) copy.push_back(entry->Clone());
    return copy;
}

/// .fzdata では overrides を「テーブルの配列」として書き、読み込み時は先頭の type からファクトリで実体を作ってから残りを読ませる。
///   [[overrides]]
///   type = 'Bloom'
///   active = true
///   intensity = 0.9
/// @note type を要素スコープの直後に通すと、Field(name, std::string&) が書き込みでは
///       既存実体の型名を出力し、読み込みではファイルの型名を代入する。同じコードで
///       双方向を賄える (IReflector の性質)。
void PostProcessProfile::Reflect(scene::IReflector& r)
{
    r.BeginField("overrides", "Overrides");
    const std::size_t count = r.BeginObjectList("overrides", overrides.size());

    /// @note 読み込み時は count がファイル側の要素数になる。既存要素より多ければ広げ、
    ///       少なければ切り詰める。ここで作る空きスロットへ、型名を見てから実体を入れる。
    if (count != overrides.size()) overrides.resize(count);

    for (std::size_t i = 0; i < count; ++i) {
        r.BeginObjectElement(i);

        std::string typeName = overrides[i] ? overrides[i]->GetTypeName() : std::string{};
        r.BeginField("type", "type");
        r.Field("type", typeName);

        /// @note 既存実体が無い (= 読み込み) か、型名が食い違う (= ファイル側で差し替えられた)
        ///       場合は作り直す。未登録の型名なら nullptr のまま残し、後で捨てる。
        if (!overrides[i] || typeName != overrides[i]->GetTypeName())
            overrides[i] = VolumeOverrideFactory::Create(typeName);

        if (overrides[i]) {
            r.BeginField("active", "Active");
            r.Field("active", overrides[i]->active);
            overrides[i]->Reflect(r);
        }

        r.EndObjectElement();
    }

    const std::size_t removeIndex = r.EndObjectList();
    if (removeIndex < overrides.size())
        overrides.erase(overrides.begin() + static_cast<std::ptrdiff_t>(removeIndex));

    /// @note 復元できなかった要素 (未登録の型名) を落とす。nullptr が混ざったリストは
    ///       以降のすべての利用側に null チェックを強いるため、読めなかったものは無かった
    ///       ことにする方が単純 (.fzdata を開き直せば警告なしに元へ戻り、保存しなければ無傷)。
    std::erase_if(overrides, [](const std::unique_ptr<VolumeOverride>& entry) {
        return entry == nullptr;
    });
}

} // namespace fbzz::asset

/// 組み込み登録。ここは Engine の静的初期化でプロセス起動時に 1 回しか走らないため、
/// スクリプト DLL のアンロードで消される側に置くと二度と復活しない。
FBZZ_REGISTER_BUILTIN_DATA_ASSET(::fbzz::asset::PostProcessProfile);
