/// @file    ManagedGamePackage.hpp
/// @brief   Standalone の managed assembly と配布 manifest を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace fbzz::scene {
/// @note project が空なら C# を使わない。複数の Scripts.csproj は曖昧な起動を避けて拒否する。
[[nodiscard]] bool FindManagedGameProject(const std::filesystem::path& root,
    std::filesystem::path& project, std::string& diagnostic);
/// @note DLL、deps と型 GUID sidecar は一組で必要。Host の通常 Load が内容の互換性も検証する。
[[nodiscard]] bool ValidateManagedGameFiles(const std::filesystem::path& assembly, std::string& diagnostic);
/// @note 配布 manifest があれば必ず優先し、不正時に開発用 Library へ逃げない。
/// @note 開発時は適用済み current.txt のみを使い、候補ディレクトリの順序から選ばない。
[[nodiscard]] bool ResolveManagedGameAssembly(const std::filesystem::path& root, std::string_view configuration,
    std::filesystem::path& assembly, std::string& diagnostic);
/// @note Scene 復元前に呼ぶ。C# なしは成功、Host/runtime/依存/型 manifest の不足は起動失敗。
/// @see https://learn.microsoft.com/dotnet/core/tutorials/netcore-hosting Framework-dependent native hosting。
[[nodiscard]] bool InitializeManagedGame(const std::filesystem::path& root,
    const std::filesystem::path& hostDirectory, std::string_view configuration, bool& loaded, std::string& diagnostic);
} /// @note namespace fbzz::scene
