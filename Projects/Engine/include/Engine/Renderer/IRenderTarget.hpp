/// @file    IRenderTarget.hpp
/// @brief   RenderTarget の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// バックバッファ以外の描画先をバックエンド非依存に扱う。
/// 複数カラーバッファや ImGui 表示用 SRV は実装側が管理する。
#pragma once
#include <cstdint>

namespace fbzz::renderer {

    class IRenderTarget {
    public:
        virtual ~IRenderTarget() = default;

        virtual uint32_t GetWidth()      const = 0;
        virtual uint32_t GetHeight()     const = 0;
        virtual uint32_t GetColorCount() const = 0;

        // ImGui Viewport 表示用ネイティブテクスチャハンドル
        // DX12 は GPU descriptor handle を uint64_t として扱う
        virtual void* GetNativeSRV(int slot = 0) const = 0;
    };

} // namespace fbzz::renderer
