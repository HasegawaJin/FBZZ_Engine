/// @file    RendererBackend.hpp
/// @brief   描画バックエンド種別の軽量列挙。RendererFactory / ProjectSettings / Application が共有する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
/// @note enum を RendererFactory.hpp から切り出してあるのは、ProjectSettings や Application が
/// @note       IRenderer 一式 (重いインターフェース) を include せずに種別だけを参照できるようにするため。
/// @note DirectX 11 サポートは v1.0 で終了した。実装が 1 つになった今も列挙と文字列変換を
/// @note       残しているのは、次のバックエンドを足すときに上位層の形を変えずに済ませるため。
/// @see  Docs/design/dx11-removal.md
#pragma once

#include <cstdint>
#include <string_view>

namespace fbzz::renderer {

enum class RendererBackend : uint8_t {
    DX12,
};

/// @brief 設定ファイル用の小文字トークンへ変換する。
inline const char* ToString(RendererBackend backend)
{
    switch (backend) {
    case RendererBackend::DX12: return "dx12";
    }
    return "dx12";
}

/// @brief v1.0 で終了したバックエンドを指す設定値かどうか。
/// @note 呼び出し側が「黙って DX12 へ倒した」ことを利用者へ伝えられるように、
/// @note       未知トークンとは区別して判定できるようにしてある。
inline bool IsRetiredBackendToken(std::string_view text)
{
    return text == "dx11";
}

/// @brief 設定ファイルのトークンから種別を解決する。
/// @note 解決できないトークンは DX12 とみなす。プロジェクト設定に "dx11" が残っていても
/// @note       起動を止めないための意図的な挙動で、警告は呼び出し側が IsRetiredBackendToken で出す。
inline RendererBackend BackendFromString(std::string_view text)
{
    (void)text;
    return RendererBackend::DX12;
}

} /// @note namespace fbzz::renderer
