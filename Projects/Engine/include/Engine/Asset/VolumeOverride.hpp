/// @file    VolumeOverride.hpp
/// @brief   ポストプロセス / 高度グラフィクスの「効果 1 つ分」を表す差分オーバーライドの基底。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// @note 効果 1 つ = クラス 1 つにする理由: 巨大な設定構造体 + 個別の bool マスクだと、効果を足すたびに
///       設定・マスク・Lerp・Reflect・Inspector の 5 か所を同時に触る必要があり、使わないセクションの
///       値もファイルに残り、Inspector が全セクションを並べて目的の 1 つを探しにくくなる。リストに
///       載っているものだけが効く実体にすれば解決する (Unity の Volume Override と同じ考え方)。
/// @note Engine/Renderer でなく Engine/Asset に置くのは Reflect(scene::IReflector&) を持つため。
///       Renderer 層に置くと RenderSettings.hpp が Script.hpp へ依存し、依存方向が逆流する。
#pragma once
/// @note `scene::IReflector` 用。
#include <Engine/Scene/Script.hpp>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::renderer { struct VolumeSettings; }

namespace fbzz::asset {

/// @brief Add Override メニューの並びと、Inspector のカード配色に使う分類。
/// @note 効果が 27 種あり、平坦なリストでは目的のものを探せないため、「今いじりたいのは色か、影か、
///       レンズか」で辿れるようにする。
enum class VolumeOverrideCategory {
    Exposure,         ///< 露出・トーン
    AntiAliasing,     ///< FXAA / TAA (排他スロット)
    AmbientOcclusion, ///< SSAO / GTAO (排他スロット)
    Color,            ///< カラーグレーディング・フィルタ
    Lens,             ///< レンズ由来の光学現象
    Atmosphere,       ///< 霧・体積光
    Shadowing,        ///< コンタクトシャドウ・反射
    Stylize,          ///< セピア・反転など演出寄り
    Custom            ///< 自作 HLSL パス
};

[[nodiscard]] const char* ToString(VolumeOverrideCategory category);

/// @brief 効果 1 つ分の差分。プロファイルはこれのリストを持つ。
/// @note Apply の規約: weight は 0..1 (0=無変化、1=完全置き換え)。連続量は線形補間、bool/整数は
///       weight >= 0.5 で切り替える。対応する XxxSettings::enabled は Apply 側で立て、リストに
///       載っていること自体が使用の意思表示なので enabled フィールドは持たない。
class VolumeOverride {
public:
    virtual ~VolumeOverride() = default;

    /// 保存キー。.fzdata の overrides 配列要素の "type" に入る。
    virtual const char* GetTypeName() const = 0;
    /// Inspector のカード見出し。
    virtual const char* GetDisplayName() const = 0;
    virtual VolumeOverrideCategory GetCategory() const = 0;

    /// 解決中の設定へ自分の差分を weight で混ぜる。
    virtual void Apply(renderer::VolumeSettings& target, float weight) const = 0;

    /// パラメーターの読み書き。TOML 永続化と Inspector の両方が通る。
    virtual void Reflect(scene::IReflector& r) = 0;

    /// 複製 (プロファイルのコピー・Undo 用)。
    virtual std::unique_ptr<VolumeOverride> Clone() const = 0;

    /// 1 プロファイルに複数入れられるか。既定は不可 (Bloom が 2 つあっても意味がない)。
    /// Custom Effect だけは別々のシェーダーを重ねるため true を返す。
    [[nodiscard]] virtual bool AllowsMultiple() const { return false; }

    /// Inspector のカード左のチェックボックス。false でも設定値は保持したまま無効化する。
    /// @note 削除でなく無効化にするのは、効果を一時的に外して見比べる作業がオーサリング中に頻繁に
    ///       起きるため。そのたびに値を捨てさせない。
    bool active = true;
};

/// @brief 型名から実体を作るレジストリ。DataAssetFactory と同方針。
/// @note .fzdata に保存されているのは型名の文字列だけで、読み込み側はそこから具体型を復元する手段を
///       持たないため。
class VolumeOverrideFactory {
public:
    using Factory = std::function<std::unique_ptr<VolumeOverride>()>;

    /// Add Override メニューを組むためのメタ情報。
    struct Entry {
        std::string            typeName;
        std::string            displayName;
        VolumeOverrideCategory category = VolumeOverrideCategory::Color;
        bool                   allowsMultiple = false;
    };

    template<typename T>
    static bool Register()
    {
        static_assert(std::is_base_of_v<VolumeOverride, T>);
        return Register(T::TYPE_NAME, []() { return std::unique_ptr<VolumeOverride>(new T()); });
    }

    static bool Register(const std::string& typeName, Factory factory);
    static std::unique_ptr<VolumeOverride> Create(const std::string& typeName);

    /// 登録済み全種を、カテゴリ順 → 表示名順で返す (Add Override メニューの並び)。
    static const std::vector<Entry>& RegisteredEntries();
};

} // namespace fbzz::asset

/// VolumeOverride 派生を静的登録するマクロ。
#define FBZZ_VOLUME_OVERRIDE_CONCAT_INNER(a, b) a##b
#define FBZZ_VOLUME_OVERRIDE_CONCAT(a, b) FBZZ_VOLUME_OVERRIDE_CONCAT_INNER(a, b)
#define FBZZ_REGISTER_VOLUME_OVERRIDE(T)                                                  \
    namespace {                                                                           \
        [[maybe_unused]] const bool                                                       \
        FBZZ_VOLUME_OVERRIDE_CONCAT(s_fbzzVolumeOverrideRegistered_, __COUNTER__) = []() { \
            ::fbzz::asset::VolumeOverrideFactory::Register<T>();                           \
            return true;                                                                  \
        }();                                                                              \
    }
