/// @file    ShaderCompileDiagnostics.cpp
/// @brief   シェーダーコンパイル診断レジストリのスレッドセーフ実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>

#include <algorithm>
#include <mutex>

namespace fbzz::renderer {
namespace {

/// ResourceManager 生成前の起動時コンパイルも記録する必要があるため、所有者を持たないプロセス寿命の
/// 状態にする。パネルや ConsoleSink を所有者にすると、それらの生成前に出た最重要エラーだけが失われる。
struct ShaderCompileDiagnosticState {
    std::mutex mutex;
    std::vector<ShaderCompileDiagnostic> diagnostics;
    std::uint64_t nextSequence = 1;
};

/// プロセス内で共有する診断状態を、初回利用時に安全に構築して返す。
ShaderCompileDiagnosticState& GetState()
{
    static ShaderCompileDiagnosticState state;
    return state;
}

} // namespace

void ReportShaderCompileDiagnostic(const std::string& path,
                                   const std::string& entryPoint,
                                   const std::string& target,
                                   const std::string& message,
                                   bool isError)
{
    if (message.empty()) return;
    constexpr std::size_t MAX_MESSAGE_BYTES = 64 * 1024;
    const std::string storedMessage = message.size() <= MAX_MESSAGE_BYTES
        ? message : message.substr(message.size() - MAX_MESSAGE_BYTES);
    auto& state = GetState();
    std::scoped_lock lock(state.mutex);
    const auto duplicate = std::find_if(state.diagnostics.begin(), state.diagnostics.end(),
        [&](const ShaderCompileDiagnostic& item) {
            return item.path == path && item.entryPoint == entryPoint && item.target == target
                && item.message == storedMessage && item.isError == isError;
        });
    if (duplicate != state.diagnostics.end()) return;

    constexpr std::size_t MAX_DIAGNOSTICS = 128;
    if (state.diagnostics.size() >= MAX_DIAGNOSTICS)
        state.diagnostics.erase(state.diagnostics.begin());
    state.diagnostics.push_back({
        state.nextSequence++, path, entryPoint, target, storedMessage, isError
    });
}

std::vector<ShaderCompileDiagnostic> GetShaderCompileDiagnostics()
{
    auto& state = GetState();
    std::scoped_lock lock(state.mutex);
    return state.diagnostics;
}

void ClearShaderCompileDiagnosticsFor(const std::string& path,
                                      const std::string& entryPoint,
                                      const std::string& target)
{
    auto& state = GetState();
    std::scoped_lock lock(state.mutex);
    std::erase_if(state.diagnostics, [&](const ShaderCompileDiagnostic& item) {
        return item.path == path && item.entryPoint == entryPoint && item.target == target;
    });
}

void ClearShaderCompileDiagnostics()
{
    auto& state = GetState();
    std::scoped_lock lock(state.mutex);
    state.diagnostics.clear();
}

} // namespace fbzz::renderer
