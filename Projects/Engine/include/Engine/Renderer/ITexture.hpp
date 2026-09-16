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

    class ITexture
    {
        public:
        virtual ~ITexture() = default;

        virtual std::uint32_t GetWidth() const = 0;
        virtual std::uint32_t GetHeight() const = 0;

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
