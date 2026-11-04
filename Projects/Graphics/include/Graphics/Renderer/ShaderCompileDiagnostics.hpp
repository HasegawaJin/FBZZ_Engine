/// @file    ShaderCompileDiagnostics.hpp
/// @brief   バックエンドと外部 HLSL ビルドのコンパイル診断を Editor へ公開する共有レジストリ。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Graphics/Api.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

/// @note 1回のシェーダーコンパイルが返した診断。
/// @note Logger だけでは Console 生成前のエラーを拾えず、Console を持たないアプリでも表示できない。
struct ShaderCompileDiagnostic {
    std::uint64_t sequence = 0;
    std::string path;
    std::string entryPoint;
    std::string target;
    std::string message;
    bool isError = true;
};

/// @note コンパイラ境界から診断を登録する。Renderer backend間で同じ表示面を使うための唯一の入口。
FBZZ_GRAPHICS_API void ReportShaderCompileDiagnostic(const std::string& path,
                                   const std::string& entryPoint,
                                   const std::string& target,
                                   const std::string& message,
                                   bool isError);

/// @note UI描画中にロックを保持しないよう、現在の診断をスナップショットで返す。
[[nodiscard]] FBZZ_GRAPHICS_API std::vector<ShaderCompileDiagnostic> GetShaderCompileDiagnostics();

/// @note 同じshader stageの再試行前に古い結果だけを除き、修正済みエラーを表示へ残さない。
FBZZ_GRAPHICS_API void ClearShaderCompileDiagnosticsFor(const std::string& path,
                                      const std::string& entryPoint,
                                      const std::string& target);

/// @note 明示的な再コンパイル開始時、またはDebugメニューのClearから過去の診断を破棄する。
FBZZ_GRAPHICS_API void ClearShaderCompileDiagnostics();

} /// @note namespace fbzz::renderer
