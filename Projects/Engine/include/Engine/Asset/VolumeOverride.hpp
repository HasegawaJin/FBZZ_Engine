// FBZZ Engine
// VolumeOverride.hpp | fbzz::asset
// ポストプロセス / 高度グラフィクスの「効果 1 つ分」を表す差分オーバーライドの基底。
//
// WHY 効果単位のクラスにするか:
//   以前は PostProcessProfile が 20 セクション分の設定を常に丸ごと抱え、
//   「何を上書きするか」を別の bool マスク (PostProcessOverrides) で表していた。
//   この形には 3 つの問題があった:
//     1. 効果を 1 つ足すたびに、設定構造体・マスク・Lerp 関数・Reflect・Inspector の
//        5 か所を同時に触る必要があり、どこか 1 つ忘れると静かに壊れる。
//     2. 「洞窟では fog だけ変えたい」プロファイルでも、使わない 19 セクション分の
//        値がファイルに書かれる。差分であることがファイルから読み取れない。
//     3. Inspector が常に全 20 セクションを並べるため、実際に効いている 1 つを
//        目で探すことになる。
//   オーバーライドを「リストに入っているものだけが効く」実体にすると、
//   これらがすべて構造として解決される。Unity の Volume Override と同じ考え方。
//
// WHY Engine/Asset に置くか (Engine/Renderer ではなく):
//   Reflect(scene::IReflector&) を持つため。Renderer 層に置くと
//   RenderSettings.hpp が Script.hpp へ依存し、依存方向が逆流する。
//   オーバーライドは「保存されるオーサリングデータ」なのでアセット層が正しい。
#pragma once
#include <Engine/Scene/Script.hpp>   // scene::IReflector
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::renderer { struct VolumeSettings; }

namespace fbzz::asset {

// Add Override メニューの並びと、Inspector のカード配色に使う分類。
// WHY 分類が要るか: 効果が 27 種あり、平坦なリストでは目的のものを探せない。
//     「今いじりたいのは色か、影か、レンズか」で辿れるようにする。
enum class VolumeOverrideCategory {
    Exposure,        // 露出・トーン
    AntiAliasing,    // FXAA / TAA (排他スロット)
    AmbientOcclusion,// SSAO / GTAO (排他スロット)
    Color,           // カラーグレーディング・フィルタ
    Lens,            // レンズ由来の光学現象
    Atmosphere,      // 霧・体積光
    Shadowing,       // コンタクトシャドウ・反射
    Stylize,         // セピア・反転など演出寄り
    Custom           // 自作 HLSL パス
};

[[nodiscard]] const char* ToString(VolumeOverrideCategory category);

// 効果 1 つ分の差分。プロファイルはこれのリストを持つ。
//
// 適用の規約 (Apply の実装が守るべきこと):
//   - weight は 0..1。0 なら何も変えない、1 なら自分の値で完全に置き換える。
//   - 連続量は線形補間、bool と整数は weight >= 0.5 で切り替える。
//     (ブレンド途中で品質パラメーターが半端な値を経由しても意味がないため)
//   - 対応する XxxSettings::enabled は Apply 側で立てる。オーバーライドが
//     リストに載っていること自体が「その効果を使う」という意思表示なので、
//     各オーバーライドは enabled フィールドを持たない。
class VolumeOverride {
public:
    virtual ~VolumeOverride() = default;

    // 保存キー。.fzdata の overrides 配列要素の "type" に入る。
    virtual const char* GetTypeName() const = 0;
    // Inspector のカード見出し。
    virtual const char* GetDisplayName() const = 0;
    virtual VolumeOverrideCategory GetCategory() const = 0;

    // 解決中の設定へ自分の差分を weight で混ぜる。
    virtual void Apply(renderer::VolumeSettings& target, float weight) const = 0;

    // パラメーターの読み書き。TOML 永続化と Inspector の両方が通る。
    virtual void Reflect(scene::IReflector& r) = 0;

    // 複製 (プロファイルのコピー・Undo 用)。
    virtual std::unique_ptr<VolumeOverride> Clone() const = 0;

    // 1 プロファイルに複数入れられるか。既定は不可 (Bloom が 2 つあっても意味がない)。
    // Custom Effect だけは別々のシェーダーを重ねるため true を返す。
    [[nodiscard]] virtual bool AllowsMultiple() const { return false; }

    // Inspector のカード左のチェックボックス。false でも設定値は保持したまま無効化する。
    // WHY 削除ではなく無効化を用意するか: 効果を一時的に外して見比べる作業は
    //     オーサリング中に頻繁に起きる。そのたびに値を捨てさせない。
    bool active = true;
};

// 型名から実体を作るレジストリ。DataAssetFactory と同方針。
// WHY 必要か: .fzdata に保存されているのは型名の文字列だけで、
//     読み込み側はそこから具体型を復元する手段を持たない。
class VolumeOverrideFactory {
public:
    using Factory = std::function<std::unique_ptr<VolumeOverride>()>;

    // Add Override メニューを組むためのメタ情報。
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

    // 登録済み全種を、カテゴリ順 → 表示名順で返す (Add Override メニューの並び)。
    static const std::vector<Entry>& RegisteredEntries();
};

} // namespace fbzz::asset

// VolumeOverride 派生を静的登録するマクロ。
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
