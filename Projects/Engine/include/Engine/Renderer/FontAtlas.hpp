/// @file    FontAtlas.hpp
/// @brief   BMFont 互換フォントアトラスのロードとグリフメトリクス管理
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// WHY: 旧実装は「全グリフが同じセル幅を占める」独自 .fnt 形式しか読めず、
///      per-glyph の矩形・オフセットもカーニングも持てなかった。ASCII 128 エントリの
///      生配列だったため日本語も入らない。AngelCode BMFont のテキスト形式へ移行し、
///      BMFont / Hiero / msdf-atlas-gen / TextMeshPro が共通で吐ける事実上の標準に乗る。
///
/// WHY (旧形式の互換維持): 既存の Roboto / Cinzel / BungeeOutline は生成元の TTF が
///      リポジトリに無く、アトラスを焼き直せない。先頭トークンで形式を自動判別し、
///      旧形式は「全グリフが xOffset=0 / yOffset=0 / width=cellW / height=lineHeight を
///      持つ BMFont」として読み込むことで、描画結果を 1px も変えずに新パスへ統合する。
///
/// WHY (画素の意味を公開しないか): 静的アトラスはカバレッジ、動的アトラスは SDF を
///      持つが、UIText.hlsl の `saturate((v - 0.5) / fwidth(v) + 0.5)` が両方の 1px AA
///      式としてそのまま成立するため、描画側に区別が要らない (SDF は onedge_value=128
///      が UNORM で 0.502 になり、式の 0.5 判定と一致する)。MSDF を入れる日が来たら
///      「中央値を取る」サンプリングごと設計する話になり、種別フラグだけでは足りない。
///
/// WHY (焼く解像度を呼び出し側が決めるか): SDF は拡大には強いが、縮小には強くない。
///      距離が padding px で飽和するため、実画面 1px がアトラスの数テクセルに相当する
///      ところまで縮めると、字画の内側と外側の区別が付かなくなって字が潰れる。
///      Editor の Game ビューのように「1920x1080 の Canvas を数百 px の枠へ入れる」場面が
///      まさにそれで、固定 48px で焼いていると文字だけが崩れる。実際に出る大きさを
///      知っているのは UISystem だけなので、そこから解像度を受け取って焼く。
///
/// 使い方:
///   FontAtlas atlas;
///   atlas.Load("Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght", resources,
///              FontAtlas::ResolveRasterPixelHeight(48.0f));
///   const FontGlyph* g = atlas.GetGlyph(U'あ');
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

// 1 グリフのアトラス上の位置と配置メトリクス。
// 長さの単位はすべて「アトラス生成時のピクセル」。
// 実画面へ変換するときは UISystem 側で scale (= fontSize / lineHeight) を掛ける。
struct FontGlyph {
    // アトラステクスチャ上の UV (正規化 0.0〜1.0)
    float u0 = 0.0f, v0 = 0.0f;   // 左上
    float u1 = 0.0f, v1 = 0.0f;   // 右下

    // グリフ画像の寸法 (px)。空白文字は 0 になり、描画をスキップする目印になる。
    float width  = 0.0f;
    float height = 0.0f;

    // ペン位置 (行の左上) からグリフ画像の左上へのオフセット (px)。
    // BMFont の xoffset / yoffset に対応し、yOffset は行の上端から下向きに測る。
    float xOffset = 0.0f;
    float yOffset = 0.0f;

    // 次グリフの原点までの水平移動量 (px)。BMFont の xadvance。
    float advance = 0.0f;

    // マルチページアトラスのページ番号。単一ページなら常に 0。
    int page = 0;
};

// フォントアトラスのロードと参照。
// UISystem が「パス + 焼いた解像度」をキーにキャッシュし、重複ロードを防ぐ。
class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();
    FontAtlas(FontAtlas&&) noexcept;
    FontAtlas& operator=(FontAtlas&&) noexcept;
    FontAtlas(const FontAtlas&)            = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // フォントを読み込む。path の拡張子で 2 つのモードを自動的に選ぶ。
    //
    //   ".ttf" / ".ttc" / ".otf"  → 動的モード。使われたグリフを実行時に SDF で焼く。
    //                               日本語のように字種が多いフォントはこちら。
    //   それ以外                   → 静的モード。path + ".fnt" (+ PNG) を読む。
    //
    // 失敗しても例外は投げない。IsValid() で成否を確認すること。
    //
    // rasterPixelHeight は動的モードで焼くときの em サイズ (px)。静的モードでは無視される。
    // ResolveRasterPixelHeight() を通した値を渡すこと。
    bool Load(const std::string& path, ResourceManager& resources, float rasterPixelHeight);

    // 拡張子が動的モード (TrueType/OpenType) のものかどうか。
    // WHY 公開するか: 「同じフォントを解像度別に焼き分けるか」は呼び出し側の
    //     キャッシュキーの問題であり、動的モードでしか意味を持たない。
    [[nodiscard]] static bool IsDynamicFontPath(const std::string& path);

    // 欲しい解像度を、実際に焼く段階の値へ丸める。
    //
    // WHY 丸めるか: 要求どおりの px で焼くと、ビューポートを 1px 動かすたびに別の
    //     アトラスが生まれる。段にしておけば、リサイズ中も同じ実体を使い回せる。
    [[nodiscard]] static float ResolveRasterPixelHeight(float desiredPixelHeight);

    // テキストに含まれるグリフのうち、まだアトラスに無いものを焼いて GPU へ反映する。
    // 静的モードでは何もしない。UISystem がレイアウト前に 1 度だけ呼ぶ。
    //
    // WHY (GetGlyph 内で遅延生成しない): GetGlyph はレイアウト計算から何度も呼ばれる
    //     const アクセサであり、そこで GPU 転送を起こすと 1 文字ごとに転送が走る。
    //     「先に全部焼く → あとは読むだけ」に分けることで、転送をフレーム 1 回に抑え、
    //     GetGlyph を const のまま保てる。
    void PrepareText(std::string_view utf8Text, ResourceManager& resources);

    // ロード成功かつ 1 枚以上のページテクスチャが有効なら true。
    [[nodiscard]] bool IsValid() const;

    // コードポイントに対応するグリフを返す。未登録なら nullptr。
    [[nodiscard]] const FontGlyph* GetGlyph(char32_t codePoint) const;

    // 連続する 2 文字間のカーニング量 (px) を返す。定義が無ければ 0。
    [[nodiscard]] float GetKerning(char32_t previous, char32_t next) const;

    // アトラス生成時の行高さ (px)。fontSize / lineHeight がスケール係数になる。
    // WHY: fontSize は「em サイズ」ではなく「行の高さ」として解釈する。
    //      既存シーンの fontSize 値と見た目を一致させ続けるための互換仕様。
    [[nodiscard]] float GetLineHeight() const { return m_lineHeight; }

    // 行の上端からベースラインまでの距離 (px)。
    [[nodiscard]] float GetBase() const { return m_base; }

    // 未登録グリフに使うフォールバック幅 (px)。
    // 旧形式では均一セル幅、BMFont では半角スペース相当の幅を入れる。
    [[nodiscard]] float GetFallbackAdvance() const { return m_fallbackAdvance; }

    // ページ番号に対応するテクスチャ。範囲外なら無効ハンドル。
    [[nodiscard]] ResourceHandle<TextureTag> GetTexture(int page = 0) const;

    // ページ数 (1 以上)。
    [[nodiscard]] std::size_t GetPageCount() const { return m_pages.size(); }

    // 焼いたページ (テクスチャ) を ResourceManager へ返す。
    //
    // WHY 明示的な関数も要るか: 破棄時には ResourceManager::Active() 経由で返すが、
    //     «まだ生きているアトラスを捨てる» (フォント差し替え・アセットリロード) では
    //     破棄を待たずにその場で返したい。動的モードの SDF ページは 1 枚 MB 級で、
    //     キャッシュを捨てるたびに残ると効いてくる。
    void ReleaseGpuResources(ResourceManager& resources);

private:
    // BMFont テキスト形式をパースする。成功したら true。
    bool ParseBMFont(const std::string& fntText,
                     const std::string& fntPath,
                     ResourceManager&   resources);

    // gen_font_atlas.py が吐く旧独自形式をパースする。成功したら true。
    bool ParseLegacy(const std::string& fntText,
                     const std::string& basePath,
                     ResourceManager&   resources);

    // TTF/TTC/OTF を動的モードで開く。成功したら true。
    bool LoadDynamic(const std::string& fontPath, float rasterPixelHeight);

    // カーニング表のキー: 上位 32bit に前の文字、下位 32bit に次の文字を詰める。
    // WHY: pair<char32_t,char32_t> + 自作ハッシュより、64bit 整数キーの方が
    //      unordered_map の標準ハッシュに素直に乗り、レイアウトのホットパスで速い。
    [[nodiscard]] static std::uint64_t MakeKerningKey(char32_t previous, char32_t next)
    {
        return (static_cast<std::uint64_t>(previous) << 32) | static_cast<std::uint32_t>(next);
    }

    std::vector<ResourceHandle<TextureTag>> m_pages;
    std::unordered_map<char32_t, FontGlyph> m_glyphs;
    std::unordered_map<std::uint64_t, float> m_kernings;

    // 動的モードのときだけ非 null。静的モードでは常に null。
    std::unique_ptr<DynamicFontSource> m_dynamic;

    float m_lineHeight      = 0.0f;
    float m_base            = 0.0f;
    float m_fallbackAdvance = 0.0f;
};

} // namespace fbzz::renderer
