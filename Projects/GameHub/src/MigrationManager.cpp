// FBZZ Engine
// MigrationManager.cpp | fbzz::hub
// Standalone project migration support
#include "MigrationManager.hpp"
#include <Engine/Util/FileSystem.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

std::string TimestampForPath()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm local{};
    localtime_s(&local, &time);

    std::ostringstream ss;
    ss << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return ss.str();
}

bool CopyIfExists(const std::filesystem::path& from, const std::filesystem::path& to, std::string& errorMessage)
{
    std::error_code ec;
    if (!engine_util::FileSystem::Exists(from)) {
        return true;
    }

    if (!engine_util::FileSystem::EnsureParentDirectory(to)) {
        errorMessage = "Failed to create backup directory.";
        return false;
    }

    if (std::filesystem::is_directory(from, ec)) {
        if (!engine_util::FileSystem::CopyDirectoryRecursive(from, to)) {
            errorMessage = "Failed to copy backup files.";
            return false;
        }
    } else {
        if (!engine_util::FileSystem::CopyFile(from, to)) {
            errorMessage = "Failed to copy backup files.";
            return false;
        }
    }
    return true;
}

bool EnsureTextFile(const std::filesystem::path& path, const std::string& text, std::string& errorMessage)
{
    if (engine_util::FileSystem::Exists(path)) {
        return true;
    }

    if (!engine_util::FileSystem::WriteText(path, text)) {
        errorMessage = "Failed to create a generated project file.";
        return false;
    }
    return true;
}

} // namespace

bool MigrationManager::MigrateProject(const std::string& projectPath, std::string& errorMessage)
{
    const std::filesystem::path projectRoot = engine_util::FileSystem::PathFromUtf8(projectPath);
    if (!engine_util::FileSystem::Exists(projectRoot / ".fbzz_proj")) {
        errorMessage = "The project file was not found.";
        return false;
    }

    if (!BackupProjectFiles(projectRoot, errorMessage)) {
        return false;
    }

    if (!EnsureGeneratedLayout(projectRoot, errorMessage)) {
        return false;
    }

    return UpdateProjectVersion(projectRoot, errorMessage);
}

bool MigrationManager::BackupProjectFiles(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path backupRoot = projectRoot / "Backups" / (TimestampForPath() + "_before_" + FBZZ_VERSION);

    const std::vector<std::filesystem::path> targets = {
        ".fbzz_proj",
        "ProjectSettings",
        "CMakeLists.txt",
        "CMakePresets.json",
        "Include",
        "Src/GameMain.cpp"
    };

    for (const auto& target : targets) {
        if (!CopyIfExists(projectRoot / target, backupRoot / target, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool MigrationManager::EnsureGeneratedLayout(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    if (!engine_util::FileSystem::EnsureDirectory(projectRoot / "Lib") ||
        !engine_util::FileSystem::EnsureDirectory(projectRoot / "Binaries") ||
        !engine_util::FileSystem::EnsureDirectory(projectRoot / "Build") ||
        !engine_util::FileSystem::EnsureDirectory(projectRoot / "ProjectSettings")) {
        errorMessage = "Failed to create generated project directories.";
        return false;
    }

    if (!EnsureTextFile(projectRoot / "Binaries/.gitignore", "*\n!.gitignore\n", errorMessage)) {
        return false;
    }

    if (!EnsureTextFile(projectRoot / "Build/.gitignore", "*\n!.gitignore\n", errorMessage)) {
        return false;
    }

    if (!EnsureTextFile(
        projectRoot / "ProjectSettings/ProjectSettings.toml",
        "[project]\ndefault_scene = \"Assets/Scenes/Main.scene\"\n\n[runtime]\nstart_scene = \"Assets/Scenes/Main.scene\"\n",
        errorMessage)) {
        return false;
    }

    if (!PatchCMakeLists(projectRoot, errorMessage))
        return false;

    return PatchCMakePresets(projectRoot, errorMessage);
}

bool MigrationManager::PatchCMakeLists(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path cmakePath = projectRoot / "CMakeLists.txt";
    if (!engine_util::FileSystem::Exists(cmakePath))
        return true;

    std::string text;
    if (!engine_util::FileSystem::ReadText(cmakePath, text)) {
        errorMessage = "Failed to read CMakeLists.txt.";
        return false;
    }

    // すでに設定済みなら何もしない
    if (text.find("CMAKE_CONFIGURATION_TYPES") != std::string::npos)
        return true;

    // "set(CMAKE_CXX_EXTENSIONS OFF)" の直後を挿入ポイントにする
    const std::string marker = "set(CMAKE_CXX_EXTENSIONS OFF)";
    const size_t markerPos = text.find(marker);
    if (markerPos == std::string::npos)
        return true; // マーカーが見つからない場合は警告なしでスキップ

    const size_t lineEnd   = text.find('\n', markerPos);
    const size_t insertPos = (lineEnd == std::string::npos) ? text.size() : lineEnd + 1;

    // WHY: エンジン側で設定している Development 構成をゲームプロジェクト側でも宣言する。
    //      これがないと VS プロジェクトに Development が生成されず MSB8013 が発生する。
    const std::string insertion =
        "\n"
        "# WHY: ゲームプロジェクトはエンジン本体と独立した CMake ルートを持つため、\n"
        "#      エンジン側で設定している Development 構成をここでも明示的に宣言する。\n"
        "#      これがないと VS プロジェクトに Development が生成されず、\n"
        "#      エディタが --config Development でビルドしようとしたときに MSB8013 が発生する。\n"
        "set(CMAKE_CONFIGURATION_TYPES \"Debug;Release;Development\" CACHE STRING \"Build configurations\" FORCE)\n"
        "set(CMAKE_CXX_FLAGS_DEVELOPMENT           \"/Zi /O2 /Ob1\"                CACHE STRING \"Development CXX flags\"        FORCE)\n"
        "set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT    \"/DEBUG:FULL /INCREMENTAL:NO\" CACHE STRING \"Development EXE linker flags\" FORCE)\n"
        "set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT \"/DEBUG:FULL /INCREMENTAL:NO\" CACHE STRING \"Development DLL linker flags\" FORCE)\n";

    text.insert(insertPos, insertion);

    if (!engine_util::FileSystem::WriteText(cmakePath, text)) {
        errorMessage = "Failed to write patched CMakeLists.txt.";
        return false;
    }

    // WHY: 古い Build/VS には Development なしで生成された vcxproj が残っているため削除する。
    //      次回 cmake --preset fbzz-vs 実行時に Development 付きで再生成される。
    const std::filesystem::path buildVS = projectRoot / "Build" / "VS";
    if (engine_util::FileSystem::Exists(buildVS))
        engine_util::FileSystem::RemoveAll(buildVS);

    return true;
}

bool MigrationManager::PatchCMakePresets(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path presetsPath = projectRoot / "CMakePresets.json";
    if (!engine_util::FileSystem::Exists(presetsPath))
        return true;

    std::string text;
    if (!engine_util::FileSystem::ReadText(presetsPath, text)) {
        errorMessage = "Failed to read CMakePresets.json.";
        return false;
    }

    // WHY: cmake preset の cacheVariables は CMakeLists.txt より先にキャッシュへ書き込まれる。
    //      ここに Development が含まれていないと VS プロジェクトが Debug;Release のみで生成され、
    //      エディタが --config Development でビルドしたときに MSB8013 が発生する。
    const std::string oldTypes = "\"Debug;Release\"";
    const std::string newTypes = "\"Debug;Release;Development\"";
    if (text.find("Development") != std::string::npos)
        return true; // すでにパッチ済み

    const size_t pos = text.find(oldTypes);
    if (pos == std::string::npos)
        return true; // 期待するパターンがなければスキップ

    text.replace(pos, oldTypes.size(), newTypes);

    // Development ビルドプリセットを追加する
    const std::string releaseBuildPreset =
        "    {\n"
        "      \"name\": \"fbzz-release\",\n"
        "      \"configurePreset\": \"fbzz-vs\",\n"
        "      \"configuration\": \"Release\"\n"
        "    }\n"
        "  ]";
    const std::string releasePlusDevPreset =
        "    {\n"
        "      \"name\": \"fbzz-release\",\n"
        "      \"configurePreset\": \"fbzz-vs\",\n"
        "      \"configuration\": \"Release\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"fbzz-development\",\n"
        "      \"configurePreset\": \"fbzz-vs\",\n"
        "      \"configuration\": \"Development\"\n"
        "    }\n"
        "  ]";

    const size_t releasePos = text.find(releaseBuildPreset);
    if (releasePos != std::string::npos)
        text.replace(releasePos, releaseBuildPreset.size(), releasePlusDevPreset);

    if (!engine_util::FileSystem::WriteText(presetsPath, text)) {
        errorMessage = "Failed to write patched CMakePresets.json.";
        return false;
    }

    // WHY: CMakePresets.json の変更は既存の Build/VS には反映されないため削除して再生成させる。
    const std::filesystem::path buildVS = projectRoot / "Build" / "VS";
    if (engine_util::FileSystem::Exists(buildVS))
        engine_util::FileSystem::RemoveAll(buildVS);

    return true;
}

bool MigrationManager::UpdateProjectVersion(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path projectFile = projectRoot / ".fbzz_proj";
    std::string text;
    engine_util::FileSystem::ReadText(projectFile, text);
    if (text.empty()) {
        errorMessage = "Failed to read .fbzz_proj.";
        return false;
    }

    const std::string key = "engine_version";
    const size_t keyPos = text.find(key);
    if (keyPos == std::string::npos) {
        errorMessage = "project.engine_version was not found.";
        return false;
    }

    const size_t equalsPos = text.find('=', keyPos);
    const size_t lineEnd = text.find_first_of("\r\n", keyPos);
    if (equalsPos == std::string::npos || (lineEnd != std::string::npos && equalsPos > lineEnd)) {
        errorMessage = "project.engine_version is invalid.";
        return false;
    }

    const size_t valueEnd = lineEnd == std::string::npos ? text.size() : lineEnd;
    text.replace(equalsPos + 1, valueEnd - equalsPos - 1, " \"" FBZZ_VERSION "\"");

    if (!engine_util::FileSystem::WriteText(projectFile, text)) {
        errorMessage = "Failed to update .fbzz_proj.";
        return false;
    }

    return true;
}

} // namespace fbzz::hub
