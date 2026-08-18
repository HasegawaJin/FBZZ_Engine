// FBZZ Engine
// DynamicFontSource.hpp | fbzz::renderer
// TTF/TTC/OTF から実行時にグリフをラスタライズして育てる動的フォントアトラス
//
// WHY: 日本語は JIS 第 1 水準だけで約 3,000 字あり、事前生成の静的アトラスに
//      全部載せると PNG が数 MB になる。しかも実際に画面へ出るのはその一部でしかない。
//      TextMeshPro の Dynamic Font Asset と同じく「使われた文字だけをその場で焼く」方式にすると、
//      アトラスは実使用ぶんで済み、プレイヤー名のような動的テキストにも対応できる。
//
// WHY (SDF で焼く): 48px 固定のカバレッジでは fontSize を上げたときに字形が角張る。
//      stb_truetype の stbtt_GetCodepointSDF が符号付き距離場を直接生成できるため、
//      焼く段階で SDF にしておけば任意サイズでシャープに出せる。
//      シェーダー側は UIText.hlsl の fwidth 正規化がそのまま距離場の AA として機能するので、
//      カバレッジと SDF で分岐する必要がない。
//
// 使い方 (FontAtlas 経由。直接触るのは FontAtlas のみ):
//   FontAtlas が fontPath の拡張子で .ttf/.ttc/.otf を検出したときに生成し、
//   UISystem がレイアウト前に FontAtlas::PrepareText() を呼ぶと未登録グリフが焼かれる。
#pragma once
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;

class DynamicFontSource {
public:
    DynamicFontSource();
    ~DynamicFontSource();
    DynamicFontSource(DynamicFontSource&&) noexcept;
    DynamicFontSource& operator=(DynamicFontSource&&) noexcept;
    DynamicFontSource(const DynamicFontSource&)            = delete;
    DynamicFontSource& operator=(const DynamicFontSource&) = delete;

    // フォントファイルを読み込み、メトリクスを確定する。
    // pixelHeight はアトラスへ焼く際のラスタライズ解像度 (行高さではなく em 相当の px)。
    // テクスチャは最初のグリフ追加時に遅延生成されるため、ここでは GPU に触らない。
    bool Load(const std::string& fontPath, float pixelHeight);

    [[nodiscard]] bool IsValid() const;

    // フォントのメトリクス (アトラスピクセル単位)
    [[nodiscard]] float GetLineHeight() const;
    [[nodiscard]] float GetBase() const;
    [[nodiscard]] float GetFallbackAdvance() const;

    // 未登録のコードポイント群をラスタライズしてアトラスへ配置し、glyphTable へ登録する。
    // 新しいページが必要になった場合は pages へテクスチャハンドルを追加する。
    // 追加した内容は関数の最後に 1 度だけ GPU へ転送される (ページごとにダーティ矩形 1 回)。
    //
    // 戻り値: 1 つでもグリフを追加できたら true。
    bool AddGlyphs(const std::vector<char32_t>&             codePoints,
                   std::unordered_map<char32_t, FontGlyph>& glyphTable,
                   std::vector<ResourceHandle<TextureTag>>& pages,
                   ResourceManager&                         resources);

    // 2 文字間のカーニング量 (アトラスピクセル単位)。
    // NOTE: stb_truetype が読むのは旧来の `kern` テーブルのみで、GPOS ベースの
    //       カーニングしか持たない現代的なフォントでは 0 が返る。
    [[nodiscard]] float GetKerning(char32_t previous, char32_t next) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fbzz::renderer
