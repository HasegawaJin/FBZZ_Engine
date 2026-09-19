/// @file    FontAtlas.hpp
/// @brief   BMFont 互換フォントアトラスのロードとグリフメトリクス管理
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// @note BMFont テキスト形式を採用。旧 .fnt は全グリフ同じセル幅の ASCII 128 固定形式で、
///       per-glyph 矩形/オフセット/カーニングも日本語も持てなかった。
/// @note 旧形式は先頭トークンで自動判別し「全グリフが xOffset=0/yOffset=0/width=cellW/
///       height=lineHeight の BMFont」として読む (生成元 TTF が無く焼き直せないため)。
/// @note 静的 (カバレッジ) / 動的 (SDF) の種別は公開しない。UIText.hlsl の 1px AA 式が両方に
///       成立するため描画側の区別が不要 (SDF の onedge_value=128 は UNORM で 0.502)。
/// @note 焼く解像度は呼び出し側 (UISystem) が渡す。SDF は縮小に弱く実画面 1px がアトラスの数
///       テクセル相当まで縮むと字が潰れるため、実際の表示サイズを知る側に委ねる。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;
class DynamicFontSource;

/// @brief 1 グリフのアトラス上の位置と配置メトリクス。
/// @note 長さの単位はすべて「アトラス生成時のピクセル」。実画面へ変換するときは UISystem 側で
///       scale (= fontSize / lineHeight) を掛ける。
struct FontGlyph {
    /// @brief アトラステクスチャ上の UV (正規化 0.0〜1.0)。
    float u0 = 0.0f, v0 = 0.0f;   ///< 左上
    float u1 = 0.0f, v1 = 0.0f;   ///< 右下

    /// @brief グリフ画像の寸法 (px)。空白文字は 0 になり、描画をスキップする目印になる。
    float width  = 0.0f;
    float height = 0.0f;

    /// @brief ペン位置 (行の左上) からグリフ画像の左上へのオフセット (px)。
    /// @note BMFont の xoffset / yoffset に対応し、yOffset は行の上端から下向きに測る。
    float xOffset = 0.0f;
    float yOffset = 0.0f;

    /// @brief 次グリフの原点までの水平移動量 (px)。BMFont の xadvance。
    float advance = 0.0f;

    /// @brief マルチページアトラスのページ番号。単一ページなら常に 0。
    int page = 0;
};

/// @brief フォントアトラスのロードと参照。
/// @note UISystem が「パス + 焼いた解像度」をキーにキャッシュし、重複ロードを防ぐ。
class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();
    FontAtlas(FontAtlas&&) noexcept;
    FontAtlas& operator=(FontAtlas&&) noexcept;
    FontAtlas(const FontAtlas&)            = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    /// @brief フォントを読み込む。path の拡張子で 2 つのモードを自動的に選ぶ。
    /// @note ".ttf"/".ttc"/".otf" → 動的モード (使われたグリフを実行時に SDF で焼く。日本語向き)。
    ///       それ以外 → 静的モード (path + ".fnt" (+ PNG) を読む)。
    /// @param rasterPixelHeight 動的モードで焼く em サイズ (px)。静的モードでは無視される。
    ///        ResolveRasterPixelHeight() を通した値を渡すこと。
    /// @return 失敗しても例外は投げない。IsValid() で成否を確認すること。
    bool Load(const std::string& path, ResourceManager& resources, float rasterPixelHeight);

    /// @brief 拡張子が動的モード (TrueType/OpenType) のものかどうか。
    /// @note 「同じフォントを解像度別に焼き分けるか」は呼び出し側のキャッシュキーの問題であり、
    ///       動的モードでしか意味を持たないため公開する。
    [[nodiscard]] static bool IsDynamicFontPath(const std::string& path);

    /// @brief 欲しい解像度を、実際に焼く段階の値へ丸める。
    /// @note 要求どおりの px で焼くと、ビューポートを 1px 動かすたびに別のアトラスが生まれる。
    ///       段にしておけば、リサイズ中も同じ実体を使い回せる。
    [[nodiscard]] static float ResolveRasterPixelHeight(float desiredPixelHeight);

    /// @brief テキストに含まれるグリフのうち、まだアトラスに無いものを焼いて GPU へ反映する。
    /// @note 静的モードでは何もしない。UISystem がレイアウト前に 1 度だけ呼ぶ。
    /// @note GetGlyph 内で遅延生成しないのは、GetGlyph がレイアウト計算から何度も呼ばれる const
    ///       アクセサだから。「先に全部焼く→あとは読むだけ」に分け、転送をフレーム 1 回に抑える。
    void PrepareText(std::string_view utf8Text, ResourceManager& resources);

    /// @brief ロード成功かつ 1 枚以上のページテクスチャが有効なら true。
    [[nodiscard]] bool IsValid() const;

    /// @brief コードポイントに対応するグリフを返す。未登録なら nullptr。
    [[nodiscard]] const FontGlyph* GetGlyph(char32_t codePoint) const;

    /// @brief 連続する 2 文字間のカーニング量 (px) を返す。定義が無ければ 0。
    [[nodiscard]] float GetKerning(char32_t previous, char32_t next) const;

    /// @brief アトラス生成時の行高さ (px)。fontSize / lineHeight がスケール係数になる。
    /// @note fontSize は「em サイズ」でなく「行の高さ」として解釈する (既存シーンの fontSize
    ///       値と見た目を一致させ続けるための互換仕様)。
    [[nodiscard]] float GetLineHeight() const { return m_lineHeight; }

    /// @brief 行の上端からベースラインまでの距離 (px)。
    [[nodiscard]] float GetBase() const { return m_base; }

    /// @brief 未登録グリフに使うフォールバック幅 (px)。
    /// @note 旧形式では均一セル幅、BMFont では半角スペース相当の幅を入れる。
    [[nodiscard]] float GetFallbackAdvance() const { return m_fallbackAdvance; }

    /// @brief ページ番号に対応するテクスチャ。範囲外なら無効ハンドル。
    [[nodiscard]] ResourceHandle<TextureTag> GetTexture(int page = 0) const;

    /// @brief ページ数 (1 以上)。
    [[nodiscard]] std::size_t GetPageCount() const { return m_pages.size(); }

    /// @brief 焼いたページ (テクスチャ) を ResourceManager へ返す。
    /// @note «まだ生きているアトラスを捨てる» (フォント差し替え・アセットリロード) では破棄を
    ///       待たずにその場で返したい。動的モードの SDF ページは 1 枚 MB 級で、キャッシュを
    ///       捨てるたびに残ると効いてくる。
    void ReleaseGpuResources(ResourceManager& resources);

private:
    /// @brief BMFont テキスト形式をパースする。成功したら true。
    bool ParseBMFont(const std::string& fntText,
                     const std::string& fntPath,
                     ResourceManager&   resources);

    /// @brief gen_font_atlas.py が吐く旧独自形式をパースする。成功したら true。
    bool ParseLegacy(const std::string& fntText,
                     const std::string& basePath,
                     ResourceManager&   resources);

    /// @brief TTF/TTC/OTF を動的モードで開く。成功したら true。
    bool LoadDynamic(const std::string& fontPath, float rasterPixelHeight);

    /// @brief カーニング表のキー。上位 32bit に前の文字、下位 32bit に次の文字を詰める。
    /// @note pair<char32_t,char32_t> + 自作ハッシュより、64bit 整数キーの方が unordered_map の
    ///       標準ハッシュに素直に乗り、レイアウトのホットパスで速い。
    [[nodiscard]] static std::uint64_t MakeKerningKey(char32_t previous, char32_t next)
    {
        return (static_cast<std::uint64_t>(previous) << 32) | static_cast<std::uint32_t>(next);
    }

    std::vector<ResourceHandle<TextureTag>> m_pages;
    std::unordered_map<char32_t, FontGlyph> m_glyphs;
    std::unordered_map<std::uint64_t, float> m_kernings;

    /// @note 動的モードのときだけ非 null。静的モードでは常に null。
    std::unique_ptr<DynamicFontSource> m_dynamic;

    float m_lineHeight      = 0.0f;
    float m_base            = 0.0f;
    float m_fallbackAdvance = 0.0f;
};

} // namespace fbzz::renderer
