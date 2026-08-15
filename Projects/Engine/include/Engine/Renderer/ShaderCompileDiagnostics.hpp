// FBZZ Engine
// ShaderCompileDiagnostics.hpp | fbzz::renderer
// DX11/DX12と外部HLSLビルドのコンパイル診断をEditorへ公開する共有レジストリ
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

// 1回のシェーダーコンパイルが返した診断。
// WHY: LoggerだけではVFXEditor起動前のエラーを拾えず、Consoleを持たない独立Appでも表示できない。
struct ShaderCompileDiagnostic {
    std::uint64_t sequence = 0;
    std::string path;
    std::string entryPoint;
    std::string target;
    std::string message;
    bool isError = true;
};

// コンパイラ境界から診断を登録する。Renderer backend間で同じ表示面を使うための唯一の入口。
void ReportShaderCompileDiagnostic(const std::string& path,
                                   const std::string& entryPoint,
                                   const std::string& target,
                                   const std::string& message,
                                   bool isError);

// UI描画中にロックを保持しないよう、現在の診断をスナップショットで返す。
[[nodiscard]] std::vector<ShaderCompileDiagnostic> GetShaderCompileDiagnostics();

// 同じshader stageの再試行前に古い結果だけを除き、修正済みエラーを表示へ残さない。
void ClearShaderCompileDiagnosticsFor(const std::string& path,
                                      const std::string& entryPoint,
                                      const std::string& target);

// 明示的な再コンパイル開始時、またはDebugメニューのClearから過去の診断を破棄する。
void ClearShaderCompileDiagnostics();

} // namespace fbzz::renderer
