/// @file    ITexture.hpp
/// @brief   Texture の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note SRV / UAV などのネイティブ表現を上位から隠す。
/// @note ファイル読み込みと生成は ResourceManager / AssetManager 経由にする。
#pragma once
#include <cstdint>

namespace fbzz::renderer
{
    /// @note 動的テクスチャ (CPU から部分更新できるテクスチャ) のピクセル形式。
    /// @note フォントアトラスのカバレッジ / SDF は 1 チャンネルで足りる。RGBA8 固定だと
    /// @note       2048x2048 アトラスで 16MB 無駄になるため R8 を選べるようにしておく。
    enum class DynamicTextureFormat
    {
        R8,      ///< @note 単一チャンネル 8bit。シェーダーからは .r で読む
        RGBA8,   ///< @note 4 チャンネル 8bit
    };

    /// @note CPU で焼いたミップ連鎖 1 段ぶんの RGBA8 データ。
    /// @note rgba は width*height*4 バイトを指し、転送が終わるまで有効であること。
    struct TextureMipData
    {
        const std::uint8_t* rgba   = nullptr;
        std::uint32_t       width  = 0;
        std::uint32_t       height = 0;
    };

    /// @note bindless 非対応、またはこのテクスチャが永続ディスクリプタ枠を持たないことを表す添字。
    /// @note 0 は «ヒープ先頭の有効なディスクリプタ» なので未設定と区別できず使えない。
    inline constexpr std::uint32_t INVALID_BINDLESS_INDEX = 0xFFFFFFFFu;

    class ITexture
    {
        public:
        virtual ~ITexture() = default;

        virtual std::uint32_t GetWidth() const = 0;
        virtual std::uint32_t GetHeight() const = 0;

        /// @note シェーダーが ResourceDescriptorHeap[] へ渡す永続ディスクリプタ添字。ディスクリプタ
        /// @note テーブル経路と違い、テクスチャが生きている限り不変で «テクスチャの識別子» として載せられる。
        /// @note bindless は SM 6.6 + Resource Binding Tier 3 を要求し、満たさない機械では
        /// @note       テーブル経路へ縮退する。呼び出し側は必ず INVALID を判定し、その場合は
        /// @note       DrawCall::textures 経由で束縛すること。
        /// @see Docs/design/bindless.md
        virtual std::uint32_t GetBindlessIndex() const { return INVALID_BINDLESS_INDEX; }

        /// @note 同じリソースの UAV 側の添字。SRV と UAV はディスクリプタが別物なので枠も別に取る。
        /// @note UAV を持たないテクスチャ (通常のファイル由来など) は INVALID を返す。
        virtual std::uint32_t GetBindlessUavIndex() const { return INVALID_BINDLESS_INDEX; }

        /// @note 3D テクスチャの奥行き。2D では 1 を返す。
        /// @note 奥行きを持つのはフロクセルボリュームのような一部の生成テクスチャだけで、
        /// @note       全実装に強制する意味がない。
        virtual std::uint32_t GetDepth() const { return 1u; }

        /// @note テクスチャ内の矩形領域を CPU 側のピクセルで差し替える。ResourceManager::CreateDynamicTexture()
        /// @note で作ったテクスチャのみ対応し、それ以外 (ファイル由来 / Immutable / RenderTarget 由来) は false。
        /// @note pixels は更新矩形左上に対応する画素へのポインタ、srcRowPitch は更新元 1 行の
        /// @note       バイト数で width * 画素サイズより大きい値を許す (部分矩形コピー用)。
        /// @note 全実装に更新経路を強制すると Immutable なファイル由来テクスチャにも CPU 書き込みの
        /// @note       口が生えるため、対応するものだけが override し誤用は false で弾く。
        virtual bool UpdateRegion(std::uint32_t /*x*/, std::uint32_t /*y*/,
                                  std::uint32_t /*width*/, std::uint32_t /*height*/,
                                  const void* /*pixels*/, std::uint32_t /*srcRowPitch*/)
        {
            return false;
        }
    };
} /// @note namespace fbzz::renderer
