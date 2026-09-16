/// @file    ITexture.hpp
/// @brief   Texture の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// SRV / UAV などのネイティブ表現を上位から隠す。
/// ファイル読み込みと生成は ResourceManager / AssetManager 経由にする。
#pragma once
#include <cstdint>

namespace fbzz::renderer
{
    // 動的テクスチャ (CPU から部分更新できるテクスチャ) のピクセル形式。
    //
    // WHY: フォントアトラスのカバレッジ / SDF は 1 チャンネルで足りる。
    //      RGBA8 固定にすると 2048x2048 のアトラスで 16MB を無駄に食うため、
    //      R8 を選べるようにしておく。用途が増えたらここへ足す。
    enum class DynamicTextureFormat
    {
        R8,      // 単一チャンネル 8bit。シェーダーからは .r で読む
        RGBA8,   // 4 チャンネル 8bit
    };

    /// CPU で焼いたミップ連鎖 1 段ぶんの RGBA8 データ。
    /// @note rgba は width*height*4 バイトを指し、転送が終わるまで有効であること。
    struct TextureMipData
    {
        const std::uint8_t* rgba   = nullptr;
        std::uint32_t       width  = 0;
        std::uint32_t       height = 0;
    };

    // bindless 非対応、またはこのテクスチャが永続ディスクリプタ枠を持たないことを表す添字。
    // WHY 0 を使わないか: 0 は «ヒープ先頭の有効なディスクリプタ» なので、未設定と区別できない。
    inline constexpr std::uint32_t INVALID_BINDLESS_INDEX = 0xFFFFFFFFu;

    class ITexture
    {
        public:
        virtual ~ITexture() = default;

        virtual std::uint32_t GetWidth() const = 0;
        virtual std::uint32_t GetHeight() const = 0;

        // シェーダーが ResourceDescriptorHeap[] へ渡す永続ディスクリプタ添字。
        //
        // ディスクリプタテーブル経路と違い、この添字はテクスチャが生きている限り不変で、
        // 定数バッファやマテリアルに «テクスチャの識別子» として載せられる。
        //
        // WHY 既定を INVALID にするか: bindless は SM 6.6 + Resource Binding Tier 3 を要求し、
        //      満たさない機械ではテーブル経路へ縮退する。呼び出し側は必ず INVALID を判定し、
        //      その場合は DrawCall::textures 経由で束縛すること。
        // @see Docs/design/bindless.md
        virtual std::uint32_t GetBindlessIndex() const { return INVALID_BINDLESS_INDEX; }

        // 同じリソースの UAV 側の添字。SRV と UAV はディスクリプタが別物なので枠も別に取る。
        // UAV を持たないテクスチャ (通常のファイル由来など) は INVALID を返す。
        virtual std::uint32_t GetBindlessUavIndex() const { return INVALID_BINDLESS_INDEX; }

        // 3D テクスチャの奥行き。2D では 1 を返す。
        // WHY 既定実装を置くか: 奥行きを持つのはフロクセルボリュームのような
        //     一部の生成テクスチャだけで、全実装に強制する意味がない。
        virtual std::uint32_t GetDepth() const { return 1u; }

        // テクスチャ内の矩形領域を CPU 側のピクセルで差し替える。
        //
        // pixels      : 更新元のピクセル先頭 (更新矩形の左上に対応する画素)
        // srcRowPitch : 更新元 1 行のバイト数。CPU 側が大きなバッファの部分矩形を
        //               渡せるよう、width * 画素サイズ より大きい値を許す。
        //
        // ResourceManager::CreateDynamicTexture() で作ったテクスチャのみ対応する。
        // それ以外 (ファイル由来 / Immutable / RenderTarget 由来) は false を返す。
        //
        // WHY (既定実装を false にする): 全テクスチャ実装に更新経路を強制すると、
        //      Immutable なファイル由来テクスチャにまで CPU 書き込みの口が生えてしまう。
        //      対応しているものだけが override する形にして、誤用は false で弾く。
        virtual bool UpdateRegion(std::uint32_t /*x*/, std::uint32_t /*y*/,
                                  std::uint32_t /*width*/, std::uint32_t /*height*/,
                                  const void* /*pixels*/, std::uint32_t /*srcRowPitch*/)
        {
            return false;
        }
    };
} // namespace fbzz::renderer
