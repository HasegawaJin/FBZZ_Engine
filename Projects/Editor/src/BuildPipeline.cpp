/// @file    BuildPipeline.cpp
/// @brief   Start() でビルドを開始し、Tick() を毎フレーム呼んで進める。
/// @author  Hasegawa Jin
/// @date    2026-05-31

/// @note 各ステップの詳細は BuildPipeline.hpp のコメントを参照。
#include <Editor/BuildPipeline.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AppIconWriter.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Graphics/Renderer/RuntimePackageValidation.hpp>
#include <toml++/toml.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

/// @note 今日の日付を YYYY-MM-DD 形式で返す
std::string TodayString()
{
    const auto now      = std::chrono::system_clock::now();
    const auto time     = std::chrono::system_clock::to_time_t(now);
    tm tm_val{};
    localtime_s(&tm_val, &time);
    char buf[16]{};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
    return buf;
}

std::string LowerUtf8(const std::filesystem::path& path)
{
    return util::StringUtils::ToLower(util::FileSystem::PathToUtf8(path));
}

/// @note Windows 7 以降の CNG 管理バッファを使い、SHA256 の所有権をこの関数へ閉じる。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash BCryptCreateHash buffer ownership
bool FileSha256(const std::filesystem::path& path, std::string& result)
{
    std::vector<std::uint8_t> bytes;
    if (!util::FileSystem::ReadBinary(path, bytes)) return false;
    struct HashHandles {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~HashHandles()
        {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto size = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        if (BCryptHashData(handles.hash, bytes.data() + offset, size, 0) < 0) return false;
        offset += size;
    }
    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char HEX[] = "0123456789abcdef";
    result.clear();
    for (const auto value : digest) {
        result.push_back(HEX[value >> 4]);
        result.push_back(HEX[value & 15]);
    }
    return true;
}

/// @note 配布物に入れないファイルか (relative は Assets/ からの相対パス)。
/// @note .hlsl/.hlsli を残すのは、ShaderDependencyTracker が本体または include 先を
/// @note 見失うと CSO を stale 判定し実行時再コンパイルへ落ちるため。.meta を残すのは
/// @note AssetDatabase の guid 索引が .meta から作られ、落とすと guid: 参照が全滅するため。
bool IsEditorOnlyAsset(const std::filesystem::path& relative)
{
    if (relative.begin() != relative.end()) {
        const std::string topDir = LowerUtf8(*relative.begin());
        /// @note EditorConfig (レイアウト/個人設定/Play退避)・Scripts (原本、ランタイムは
        /// @note コンパイル済みのみ読む)・Docs (孤児 meta を防ぐ) は拡張子でなく置き場ごと
        /// @note 落とす。.playmode_snapshot.scene のように拡張子だけでは配布可否を区別できない。
        if (topDir == "editorconfig" || topDir == "scripts" || topDir == "docs") return true;
    }

    /// @note "_" で始まるフォルダ/ファイルは作業用の置き場として配布から外す。DCC の原本と
    /// @note 退避コピーが Assets に同居し (GreenWare の Assets/_src だけで 4.5GB = 97%)、
    /// @note 拡張子だけでは規則が増えるたび漏れるため、置き場の名前を正の規約にする。
    for (const std::filesystem::path& component : relative) {
        const std::string name = LowerUtf8(component);
        if (!name.empty() && name.front() == '_') return true;
    }

    const std::string ext = LowerUtf8(relative.extension());
    static constexpr std::array kSourceExts = std::to_array<const char*>({
        ".hpp", ".h", ".cpp", ".c", ".inl", ".md",
        ".bat", ".cmd", ".ps1", ".py", ".sh", ".sln", ".vcxproj", ".filters", ".user",
        /// @note DCC の原本と作業ファイル。ランタイムが読む形式ではない。
        ".blend", ".blend1", ".blend2", ".psd", ".xcf", ".npz",
    });
    if (std::find(kSourceExts.begin(), kSourceExts.end(), ext) != kSourceExts.end())
        return true;

    /// @note 編集ツールが原本の隣へ残す退避コピー (Boss.fbx.bak / Stage_02.scene.bak 等)。
    static constexpr std::array kBackupExts = std::to_array<const char*>({
        ".bak", ".tmp", ".orig", ".rej",
    });
    if (std::find(kBackupExts.begin(), kBackupExts.end(), ext) != kBackupExts.end())
        return true;

    const std::string name = LowerUtf8(relative.filename());
    if (name == "thumbs.db" || name == ".ds_store") return true;
    /// @note シェーダーコンパイルのログ (compile_log.txt / compile_ui_log.txt)
    if (ext == ".txt" && name.rfind("compile_", 0) == 0) return true;

    return false;
}

/// @note .meta は本体 (ファイルでもフォルダでも) の扱いに従わせる。
/// @note 実体を失った .meta は AssetDatabase が起動時に「孤児」として削除しにいく。
/// @note 配布先ディレクトリへの書き込みが発生するうえ、索引の警告も無意味に増える。
bool ShouldSkipAsset(const std::filesystem::path& absPath, const std::filesystem::path& relative)
{
    if (LowerUtf8(relative.extension()) != ".meta")
        return IsEditorOnlyAsset(relative);

    std::filesystem::path companionAbs = absPath;
    companionAbs.replace_extension();
    std::error_code ec;
    /// @note 実体が消えている .meta は判断材料が無い。既存の索引を壊さないよう残す。
    if (!std::filesystem::exists(companionAbs, ec)) return false;

    std::filesystem::path companionRel = relative;
    companionRel.replace_extension();
    return IsEditorOnlyAsset(companionRel);
}

} /// @note namespace

bool BuildPipeline::ValidateRuntimePackage(const std::filesystem::path& exePath,
    const std::filesystem::path& sdkRoot, const std::string& configuration,
    bool finalDistribution, std::string& reason, bool* dx12Enabled)
{
    if (dx12Enabled) *dx12Enabled = false;
    const auto runtimeDir = exePath.parent_path();
    const auto sdkManifest = sdkRoot / L"fbzz-sdk.toml";
    const bool sharedSdk = !sdkRoot.empty() && util::FileSystem::Exists(sdkManifest);
    const auto manifestPath = sharedSdk ? sdkManifest : runtimeDir / L"fbzz-runtime.toml";
    std::string manifestText;
    if (!util::FileSystem::ReadText(manifestPath, manifestText)) {
        reason = "Runtime manifest not found: " + util::FileSystem::PathToUtf8(manifestPath);
        return false;
    }
    const auto parsed = toml::parse(manifestText);
    if (!parsed || parsed.table()["sdk"]["schema"].value_or(0) != 2) {
        reason = "Unsupported runtime manifest schema: " + util::FileSystem::PathToUtf8(manifestPath);
        return false;
    }
    const auto& manifest = parsed.table();
    if (sharedSdk) {
        const auto fingerprint = manifest["runtime"]["fingerprint"].value_or(std::string{});
        const auto record = manifest["configurations"][configuration];
        if (fingerprint.size() != 64 || !record["validated"].value_or(false) ||
            record["fingerprint"].value_or(std::string{}) != fingerprint) {
            reason = "SDK configuration is not validated for its runtime contract: " + configuration;
            return false;
        }
    }
    const auto manifestDx12Enabled = manifest["runtime"]["dx12_enabled"].value<bool>();
    if (!manifestDx12Enabled) {
        reason = "Runtime manifest dx12_enabled is missing";
        return false;
    }
    if (dx12Enabled) *dx12Enabled = *manifestDx12Enabled;
    if (!*manifestDx12Enabled) return true;
    if (manifest["runtime"]["agility_sdk_version"].value_or(0) <= 0 ||
        manifest["runtime"]["agility_path"].value_or(std::string{}) != ".\\D3D12\\" ||
        manifest["runtime"]["agility_package"].value_or(std::string{}).empty() ||
        manifest["runtime"]["core_file_version"].value_or(std::string{}).empty()) {
        reason = "Runtime manifest Agility version or path contract is invalid";
        return false;
    }
    if (!renderer::ValidateGraphicsRuntimePackage(exePath, reason)) return false;
    std::vector<std::pair<std::filesystem::path, std::string>> required = {
        { L"D3D12/D3D12Core.dll", "core_sha256" },
        { L"dxcompiler.dll", "dxc_compiler_sha256" }, { L"dxil.dll", "dxc_validator_sha256" },
    };
    if (!finalDistribution && configuration != "Release") required.emplace_back(L"D3D12/d3d12SDKLayers.dll", "layers_sha256");
    if (finalDistribution && util::FileSystem::Exists(runtimeDir / L"D3D12/d3d12SDKLayers.dll")) {
        reason = "Final distribution must exclude D3D12/d3d12SDKLayers.dll";
        return false;
    }
    for (const auto& [relativePath, hashField] : required) {
        const auto file = runtimeDir / relativePath;
        if (!util::FileSystem::Exists(file)) {
            reason = "Required runtime file not found: " + util::FileSystem::PathToUtf8(file);
            return false;
        }
        const auto expectedHash = manifest["runtime"][hashField].value_or(std::string{});
        if (expectedHash.size() != 64) {
            reason = "Runtime manifest hash is invalid: " + hashField;
            return false;
        }
        if (finalDistribution) {
            std::string actualHash;
            if (!FileSha256(file, actualHash) || actualHash != expectedHash) {
                reason = "Runtime SHA256 mismatch: " + util::FileSystem::PathToUtf8(file);
                return false;
            }
        }
    }
    for (const auto& [package, notices] : std::vector<std::pair<std::wstring, std::vector<std::wstring>>>{
        { L"AgilitySDK", { L"LICENSE", L"LICENSE.txt", L"LICENSE-CODE.txt", L"VERSION", L"distributable files.txt" } },
        { L"DXC", { L"LICENSE", L"LICENCE-MIT.txt", L"LICENSE-LLVM.txt", L"LICENSE-MS.txt", L"VERSION" } },
        { L"WinPixEventRuntime", { L"LICENSE", L"VERSION", L"ThirdPartyNotices.txt" } },
    }) {
        for (const auto& name : notices) {
            const auto file = runtimeDir / L"EngineLicenses" / package / name;
            if (!util::FileSystem::Exists(file)) {
                reason = "Required runtime notice not found: " + util::FileSystem::PathToUtf8(file);
                return false;
            }
            if (finalDistribution) {
                const auto relative = std::filesystem::path(L"share/fbzz/licenses") / package / name;
                const auto expectedHash = sharedSdk
                    ? manifest["files"][configuration][relative.generic_string()].value_or(std::string{})
                    : manifest["notices"][(std::filesystem::path(package) / name).generic_string()].value_or(std::string{});
                std::string actualHash;
                if (expectedHash.size() != 64 || !FileSha256(file, actualHash) || actualHash != expectedHash) {
                    reason = "Runtime notice SHA256 mismatch: " + util::FileSystem::PathToUtf8(file);
                    return false;
                }
            }
        }
    }
    return true;
}

/// @note 開始 / 進行

void BuildPipeline::Start(const BuildSettings& settings,
                          const std::string& projectRoot,
                          const std::string& buildRoot,
                          const std::string& targetName,
                          const std::string& scriptsDllPath,
                          const std::string& engineRoot,
                          bool runAfterBuild)
{
    m_settings          = settings;
    m_projectRoot       = projectRoot;
    m_buildRoot         = buildRoot;
    m_engineRoot        = engineRoot;
    m_targetName        = targetName.empty() ? "SandboxStandalone" : targetName;
    m_runtimeConfiguration = "Development";
    m_scriptsDllSrcPath = scriptsDllPath;
    m_runAfterBuild     = runAfterBuild;

    m_state         = State::Running;
    m_step          = Step::Compile;
    m_progress      = 0.0f;
    m_status        = "Starting build...";
    m_error.clear();
    m_copyJobs.clear();
    m_copyIdx     = 0;
    m_compileStarted = false;
    m_exeSrcPath.clear();

    m_outputDir  = settings.ResolveOutputPath(projectRoot);
    m_tmpDir     = m_outputDir;
    m_tmpDir    += L"_tmp";

    /// @note CommitOutput は出力先を remove_all するため、数分かけたコンパイル後ではなく
    /// @note ここで止めないと削除まで走ってしまう。
    std::string outputReason;
    if (!m_settings.ValidateOutputPath(m_projectRoot, outputReason)) {
        /// @note まだ作っていない。SetFailed の後始末対象にしない
        m_tmpDir.clear();
        SetFailed("Invalid output directory: " + outputReason);
        return;
    }

    ResolveProjectLayout();

    FBZZ_LOG_INFO("BuildPipeline: Start → %s", util::FileSystem::PathToUtf8(m_outputDir).c_str());
}

void BuildPipeline::Tick()
{
    if (m_state != State::Running) return;

    /// @note Compile は CMake 子プロセスが複数フレーム継続するため、専用処理で状態を監視する。
    if (m_step == Step::Compile) {
        if (!m_compileStarted) {
            ToolchainLocator::Result toolchain = ToolchainLocator::Locate(util::FileSystem::PathFromUtf8(m_buildRoot));
            if (!toolchain.found) {
                SetFailed("RuntimeBuild toolchain not found: " + toolchain.error);
                return;
            }

            Compiler::Config config;
            config.cmakeExe      = toolchain.cmakeExe;
            config.buildDir      = toolchain.buildDir;
            /// @note Build Settings の Development Build は CMake の Development 構成に対応する。
            /// @note Debug を使うと Development 用に配置された EXE/DLL とずれてパッケージングに失敗する。
            config.exePath       = m_settings.developmentBuild
                ? (!toolchain.exeDevelopment.empty() ? toolchain.exeDevelopment : toolchain.exeDebug)
                : toolchain.exeRelease;
            config.target        = m_targetName;
            /// @note ゲーム側 CMakeLists は FBZZ_SDK_ROOT が無いと configure で落ちる。
            /// @note ビルド中に ZERO_CHECK が再 configure を走らせても SDK を見失わせない。
            config.sdkRoot       = m_engineRoot;
            config.configuration = m_settings.developmentBuild
                ? (!toolchain.exeDevelopment.empty() ? "Development" : "Debug")
                : "Release";
            m_runtimeConfiguration = config.configuration;

            if (m_runtimeConfiguration == "Development" && !toolchain.scriptsDllDevelopment.empty()) {
                m_scriptsDllSrcPath = util::FileSystem::PathToUtf8(toolchain.scriptsDllDevelopment);
            } else if (m_runtimeConfiguration == "Release" && !toolchain.scriptsDllRelease.empty()) {
                m_scriptsDllSrcPath = util::FileSystem::PathToUtf8(toolchain.scriptsDllRelease);
            } else if (m_runtimeConfiguration == "Debug" && !toolchain.scriptsDllDebug.empty()) {
                m_scriptsDllSrcPath = util::FileSystem::PathToUtf8(toolchain.scriptsDllDebug);
            }

            FBZZ_LOG_DEBUG("BuildPipeline: cmake=%s build=%s target=%s config=%s",
                util::FileSystem::PathToUtf8(config.cmakeExe).c_str(),
                util::FileSystem::PathToUtf8(config.buildDir).c_str(),
                config.target.c_str(),
                config.configuration.c_str());

            if (!m_compiler.Start(config)) {
                SetFailed("Failed to start RuntimeBuild compiler.");
                return;
            }
            m_exeSrcPath = config.exePath;
            m_status    = "Compiling " + m_targetName + "...";
            m_progress  = 0.02f;
            m_compileStarted = true;
            return;
        }

        m_compiler.Tick();
        m_status = "Compiling " + m_targetName + "...";
        m_progress = 0.04f;

        if (m_compiler.GetState() == Compiler::State::Done) {
            m_exeSrcPath = m_compiler.GetOutputExePath();
            FBZZ_LOG_DEBUG("BuildPipeline: compile done → %s", util::FileSystem::PathToUtf8(m_exeSrcPath).c_str());
            m_step = Step::PrepareTempDir;
        } else if (m_compiler.GetState() == Compiler::State::Failed) {
            SetFailed("RuntimeBuild compile failed. Exit code: " + std::to_string(m_compiler.GetExitCode()));
        } else if (m_compiler.GetState() == Compiler::State::Cancelled) {
            SetFailed("RuntimeBuild compile was cancelled.");
        }
        return;
    }

    /// @note CopyFiles は 1 Tick = 1 ファイルで処理する
    if (m_step == Step::CopyFiles) {
        if (!TickCopyOneFile()) {
            /// @note 全ファイルコピー完了 → 次ステップへ
            m_step = Step::CopyProjectFiles;
        }
        return;
    }

    /// @note それ以外のステップは 1 Tick = 1 ステップで完了する
    if (!ExecuteStep()) {
        /// @note ExecuteStep 内で SetFailed() 呼び済み
        return;
    }

    m_step = static_cast<Step>(static_cast<int>(m_step) + 1);

    static constexpr const char* kStepNames[] = {
        "Compile", "PrepareTempDir", "CopyExecutable", "ApplyIcon", "CopyDlls",
        "EnumerateFiles", "CopyFiles", "CopyProjectFiles", "WriteManifest", "CommitOutput", "Done"
    };
    const int stepIdx = static_cast<int>(m_step);
    if (stepIdx >= 0 && stepIdx < static_cast<int>(std::size(kStepNames)))
        FBZZ_LOG_DEBUG("BuildPipeline: step → %s", kStepNames[stepIdx]);

    /// @note EnumerateFiles 完了後 → CopyFiles のカーソルを初期化する
    if (m_step == Step::CopyFiles)
        BeginEnumerateFiles();

    if (m_step == Step::Done) {
        m_state    = State::Done;
        m_progress = 1.0f;
        m_status   = "Build complete";
        FBZZ_LOG_INFO("BuildPipeline: Done → %s", util::FileSystem::PathToUtf8(m_outputDir).c_str());
    }
}

void BuildPipeline::Reset()
{
    m_state    = State::Idle;
    m_progress = 0.0f;
    m_status.clear();
    m_error.clear();
}

void BuildPipeline::Cancel()
{
    if (m_state != State::Running) return;
    if (m_step == Step::Compile)
        m_compiler.Cancel();
    SetFailed("Build cancelled.");
}

std::string BuildPipeline::GetOutputExePath() const
{
    const std::filesystem::path exePath = m_outputDir / (m_settings.productName + ".exe");
    return util::FileSystem::PathToUtf8(exePath);
}

std::string BuildPipeline::GetOutputDir() const
{
    return util::FileSystem::PathToUtf8(m_outputDir);
}

/// @note プロジェクト構成の解決

void BuildPipeline::ResolveProjectLayout()
{
    m_settingsRelPath = "ProjectSettings/ProjectSettings.toml";
    m_defaultSceneRel = "Assets/Scenes/Main.scene";
    m_startSceneRel.clear();
    m_rendererBackend.clear();

    const std::filesystem::path root = util::FileSystem::PathFromUtf8(m_projectRoot);

    /// @note 絶対パスをプロジェクトルート相対 (/ 区切り) に変換する。GameHub Creator が
    /// @note settings_path を絶対パスで書き込むことがあり、そのまま配布物へ入れると
    /// @note 別 PC で解決できないため。
    const auto makeRelative = [&root](const std::string& rawPath) -> std::string {
        if (rawPath.empty()) return rawPath;
        std::filesystem::path p = util::FileSystem::PathFromUtf8(rawPath);
        if (!p.is_absolute()) return p.generic_string();
        const auto rel = util::FileSystem::RelativePath(p, root);
        return !rel.empty() ? rel.generic_string() : rawPath;
    };

    /// @note standard テンプレートの settings_path は "{{SETTINGS_PATH}}" のまま残る場合が
    /// @note あり、"{{" で始まる値はプレースホルダと判断してデフォルトを使う。
    std::string projectText;
    if (util::FileSystem::ReadText(root / L".fbzz_proj", projectText)) {
        auto parsed = toml::parse(projectText);
        if (parsed) {
            const auto settingsPath = parsed.table()["project"]["settings_path"].value<std::string>();
            if (settingsPath && settingsPath->size() >= 2 && settingsPath->rfind("{{", 0) != 0)
                m_settingsRelPath = makeRelative(*settingsPath);

            const auto defaultScene = parsed.table()["project"]["default_scene"].value<std::string>();
            if (defaultScene && !defaultScene->empty() && defaultScene->rfind("{{", 0) != 0)
                m_defaultSceneRel = makeRelative(*defaultScene);
        }
    }

    /// @note .fbzz_proj の default_scene は実体と拡張子がずれていることがある
    /// @note (GameHub が生成した ".fbzz" のまま等)。そのまま配布物へ書くと
    /// @note ランタイムの開始シーン解決が失敗して起動できない。
    if (!util::FileSystem::Exists(root / util::FileSystem::PathFromUtf8(m_defaultSceneRel))) {
        std::filesystem::path fixed = util::FileSystem::PathFromUtf8(m_defaultSceneRel);
        fixed.replace_extension(L".scene");
        if (util::FileSystem::Exists(root / fixed)) {
            FBZZ_LOG_WARN("BuildPipeline: .fbzz_proj default_scene [%s] does not exist; using [%s]",
                          m_defaultSceneRel.c_str(), fixed.generic_string().c_str());
            m_defaultSceneRel = fixed.generic_string();
        }
    }

    std::string settingsText;
    if (util::FileSystem::ReadText(root / util::FileSystem::PathFromUtf8(m_settingsRelPath), settingsText)) {
        auto parsed = toml::parse(settingsText);
        if (parsed) {
            /// @note ProjectSettings の renderer は [app] テーブルにあるため先に読む。
            /// @note 旧形式のトップレベルも fallback として読み続ける。
            std::string backend = parsed.table()["app"]["renderer"].value_or(std::string{});
            if (backend.empty())
                backend = parsed.table()["renderer"].value_or(std::string{});
            m_rendererBackend = util::StringUtils::ToLower(backend);
            const auto startScene = parsed.table()["runtime"]["start_scene"].value<std::string>();
            if (startScene && !startScene->empty() && startScene->rfind("{{", 0) != 0)
                m_startSceneRel = makeRelative(*startScene);
        }
    }
}

/// @note ステップ実行

bool BuildPipeline::ExecuteStep()
{
    switch (m_step) {

    case Step::PrepareTempDir:
        m_status = "Preparing temp directory...";
        /// @note 前回の _tmp が残っていれば削除する
        if (!util::FileSystem::RemoveAll(m_tmpDir) ||
            !util::FileSystem::EnsureDirectory(m_tmpDir)) {
            SetFailed("Failed to create temp directory: " + util::FileSystem::PathToUtf8(m_tmpDir));
            return false;
        }
        m_progress = 0.05f;
        return true;

    case Step::CopyExecutable: {
        m_status = "Copying executable...";
        const std::filesystem::path dst = m_tmpDir / (m_settings.productName + ".exe");
        if (!util::FileSystem::CopyFile(m_exeSrcPath, dst)) {
            SetFailed("Failed to copy exe: " + util::FileSystem::PathToUtf8(m_exeSrcPath));
            return false;
        }
        m_progress = 0.10f;
        return true;
    }

    case Step::ApplyIcon: {
        m_progress = 0.12f;
        if (m_settings.iconPath.empty()) return true;

        m_status = "Applying icon...";
        const std::filesystem::path icon = m_settings.ResolveIconPath(m_projectRoot);
        if (!util::FileSystem::Exists(icon)) {
            SetFailed("Icon image not found: " + util::FileSystem::PathToUtf8(icon));
            return false;
        }

        const std::filesystem::path exe = m_tmpDir / (m_settings.productName + ".exe");
        std::string reason;
        /// @note アイコンは «出来上がった exe を見て» しか確認できない。警告で済ませて
        /// @note 既定アイコンのまま配ると、配った後にしか気付けない。
        if (!AppIconWriter::Apply(icon, exe, reason)) {
            SetFailed("Failed to apply icon: " + reason);
            return false;
        }
        FBZZ_LOG_INFO("BuildPipeline: icon applied from %s",
                      util::FileSystem::PathToUtf8(icon).c_str());
        return true;
    }

    case Step::CopyDlls: {
        m_status = "Copying DLLs...";
        const std::filesystem::path exeDir = m_exeSrcPath.parent_path();
        std::string runtimeReason;
        bool dx12Enabled = false;
        if (!ValidateRuntimePackage(m_exeSrcPath, util::FileSystem::PathFromUtf8(m_engineRoot),
                                    m_runtimeConfiguration, false, runtimeReason, &dx12Enabled)) {
            SetFailed(runtimeReason);
            return false;
        }
        const std::wstring assimpDLL = m_runtimeConfiguration == "Debug"
            ? L"assimp-vc145-mtd.dll"
            : L"assimp-vc145-mt.dll";

        /// @note ランタイム DLL の内訳を決めるのは SDK のステージング (fbzz_stage_runtime) で、
        /// @note 固定リストで列挙すると SDK が 1 つ増やすたびに配布物だけ欠けるため、
        /// @note exe 隣を丸ごと同期する。
        std::vector<std::filesystem::path> runtimeDlls;
        for (const std::filesystem::path& file : util::FileSystem::ListFiles(exeDir)) {
            if (LowerUtf8(file.extension()) != ".dll") continue;
            const auto name = LowerUtf8(file.filename());
            if (name == "d3d12sdklayers.dll" || name == "d3d12core.dll") continue;
            if (!dx12Enabled && (name == "dxcompiler.dll" || name == "dxil.dll" || name == "winpixeventruntime.dll")) continue;
            runtimeDlls.push_back(file);
        }

        /// @note 実行に必須の DLL がないパッケージを成功扱いにしない。黙ってスキップすると
        /// @note Build complete 表示後の起動時にだけ不足が発覚し、原因の工程を特定しにくい。
        std::vector<std::wstring> required = {
            L"imgui.dll", L"FBZZMath.dll", L"FBZZPhysics.dll", L"FBZZFluid.dll", L"FBZZCore.dll", L"FBZZGraphics.dll", L"FBZZEngine.dll", assimpDLL,
        };
        /// @note DX12 は .cso (DXIL) を読むだけの経路でも、頂点入力と MaterialConstants の
        /// @note リフレクションに dxcompiler.dll が要る。無いと DX12Shader::Init が
        /// @note 全シェーダーで失敗し、何も描かれないまま起動する。
        if (dx12Enabled) {
            required.push_back(L"dxcompiler.dll");
            required.push_back(L"dxil.dll");
            required.push_back(L"WinPixEventRuntime.dll");
        }

        for (const std::wstring& dllName : required) {
            const std::filesystem::path src = exeDir / dllName;
            if (!util::FileSystem::Exists(src)) {
                SetFailed("Required runtime DLL not found: " + util::FileSystem::PathToUtf8(src));
                return false;
            }
        }

        /// @note The imported PIX event runtime requires its SDK notices even when capture is disabled.
        if (dx12Enabled) {
            for (const wchar_t* name : { L"LICENSE", L"VERSION", L"ThirdPartyNotices.txt" }) {
                const auto src = exeDir / L"EngineLicenses" / L"WinPixEventRuntime" / name;
                if (!util::FileSystem::Exists(src)) {
                    SetFailed("Required runtime notice not found: " + util::FileSystem::PathToUtf8(src));
                    return false;
                }
            }
        }

        for (const std::filesystem::path& src : runtimeDlls) {
            if (!util::FileSystem::CopyFile(src, m_tmpDir / src.filename())) {
                SetFailed("Failed to copy DLL: " + util::FileSystem::PathToUtf8(src));
                return false;
            }
        }
        if (dx12Enabled) {
            const auto core = exeDir / L"D3D12" / L"D3D12Core.dll";
            if (!util::FileSystem::CopyFile(core, m_tmpDir / L"D3D12" / L"D3D12Core.dll")) {
                SetFailed("Failed to copy Agility runtime: " + util::FileSystem::PathToUtf8(core));
                return false;
            }
            /// @note 最終ゲームは Core だけを指定して運び、開発レイヤーや PDB を含めない。
            /// @see https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html#d3d12-debug-layer
        }
        const auto localManifest = exeDir / L"fbzz-runtime.toml";
        if (util::FileSystem::Exists(localManifest) &&
            !util::FileSystem::CopyFile(localManifest, m_tmpDir / L"fbzz-runtime.toml")) {
            SetFailed("Failed to copy runtime manifest: " + util::FileSystem::PathToUtf8(localManifest));
            return false;
        }
        FBZZ_LOG_DEBUG("BuildPipeline: copied %zu runtime DLLs", runtimeDlls.size());

        /// @note スクリプト DLL をコピーする。DLL 名はプロジェクトごとに異なり、
        /// @note `Binaries/<Config>` ではなく別ディレクトリに出るプロジェクトは exe 隣に無い。
        if (!m_scriptsDllSrcPath.empty()) {
            const std::filesystem::path scriptsSrc = util::FileSystem::PathFromUtf8(m_scriptsDllSrcPath);
            if (util::FileSystem::Exists(scriptsSrc)) {
                if (!util::FileSystem::CopyFile(scriptsSrc, m_tmpDir / scriptsSrc.filename())) {
                    SetFailed("Failed to copy scripts DLL: " + m_scriptsDllSrcPath);
                    return false;
                }
            } else {
                FBZZ_LOG_WARN("BuildPipeline: scripts DLL not found, skipping: %s", m_scriptsDllSrcPath.c_str());
            }
        }

        m_progress = 0.15f;
        return true;
    }

    case Step::EnumerateFiles:
        /// @note BeginEnumerateFiles() は CopyFiles に遷移した直後に呼ばれるため、
        /// @note ここでは何もしない (ステップ遷移ロジックは Tick() 側が担う)
        m_status   = "Enumerating files...";
        m_progress = 0.18f;
        return true;

    case Step::CopyProjectFiles: {
        m_status = "Copying project files...";

        const std::filesystem::path root = util::FileSystem::PathFromUtf8(m_projectRoot);
        const auto makeRelative = [&root](const std::string& rawPath) -> std::string {
            if (rawPath.empty()) return rawPath;
            std::filesystem::path p = util::FileSystem::PathFromUtf8(rawPath);
            if (!p.is_absolute()) return p.generic_string();
            const auto rel = util::FileSystem::RelativePath(p, root);
            return !rel.empty() ? rel.generic_string() : rawPath;
        };

        /// @note ビルド向け最小 .fbzz_proj を書き出す (開発環境固有の絶対パスを除去)
        /// @note 元の .fbzz_proj は engine.root/api_root/script_root 等の開発環境固有の
        /// @note 絶対パスを含み、別 PC では無効になる。ランタイムが読むフィールドだけを
        /// @note 相対パスで書き直す。
        {
            std::ostringstream proj;
            proj << "[project]\n";
            proj << "name          = \"" << m_settings.productName << "\"\n";
            proj << "settings_path = \"" << m_settingsRelPath << "\"\n";
            proj << "default_scene = \"" << m_defaultSceneRel << "\"\n";
            /// @note スクリプトを DLL で持つランタイムはこのフィールドを読んでロードする。
            /// @note DLL 名はプロジェクトごとに異なるためハードコードせず、ここに記録する。
            if (!m_scriptsDllSrcPath.empty()) {
                const std::filesystem::path scriptsDll = util::FileSystem::PathFromUtf8(m_scriptsDllSrcPath);
                proj << "scripts_dll   = \"" << util::FileSystem::PathToUtf8(scriptsDll.filename()) << "\"\n";
            }

            const std::filesystem::path dst = m_tmpDir / L".fbzz_proj";
            if (!util::FileSystem::WriteText(dst, proj.str())) {
                SetFailed("Failed to write .fbzz_proj");
                return false;
            }
        }

        /// @note ProjectSettings を相対パスに修正してコピー
        /// @note default_scene / start_scene が絶対パスで保存されている場合、パースして
        /// @note 絶対パスフィールドのみ相対変換し、それ以外は元の値を保持する。
        {
            const std::filesystem::path src = root / util::FileSystem::PathFromUtf8(m_settingsRelPath);
            const std::filesystem::path dst = m_tmpDir / util::FileSystem::PathFromUtf8(m_settingsRelPath);
            if (util::FileSystem::Exists(src)) {
                std::string settingsText;
                util::FileSystem::ReadText(src, settingsText);
                auto settingsParsed = toml::parse(settingsText);

                bool modified = false;
                if (settingsParsed) {
                    auto& settingsTbl = settingsParsed.table();
                    if (auto* projTbl = settingsTbl["project"].as_table()) {
                        if (auto* strNode = (*projTbl)["default_scene"].as_string()) {
                            const std::string fixed = makeRelative(strNode->get());
                            if (fixed != strNode->get()) { strNode->get() = fixed; modified = true; }
                        }
                    }
                    if (auto* runtimeTbl = settingsTbl["runtime"].as_table()) {
                        if (auto* strNode = (*runtimeTbl)["start_scene"].as_string()) {
                            const std::string fixed = makeRelative(strNode->get());
                            if (fixed != strNode->get()) { strNode->get() = fixed; modified = true; }
                        }
                    }
                    if (modified) {
                        std::ostringstream ss;
                        ss << settingsTbl;
                        if (!util::FileSystem::WriteText(dst, ss.str())) {
                            SetFailed("Failed to write patched ProjectSettings: " + util::FileSystem::PathToUtf8(src));
                            return false;
                        }
                    } else {
                        if (!util::FileSystem::CopyFile(src, dst)) {
                            SetFailed("Failed to copy settings file: " + util::FileSystem::PathToUtf8(src));
                            return false;
                        }
                    }
                } else {
                    if (!util::FileSystem::CopyFile(src, dst)) {
                        SetFailed("Failed to copy settings file: " + util::FileSystem::PathToUtf8(src));
                        return false;
                    }
                }
            }
        }

        /// @note 入力バインド (.inputactions) をコピー
        /// @note ProjectSettings.toml と同じディレクトリの Input.inputactions をランタイムが
        /// @note 読む。入れ忘れるとビルドだけ既定バインドに戻る。入力設定自体は任意なので
        /// @note ファイルが無くても失敗にしない。
        {
            const std::filesystem::path settingsDir =
                util::FileSystem::PathFromUtf8(m_settingsRelPath).parent_path();
            const std::filesystem::path relative = settingsDir / L"Input.inputactions";
            const std::filesystem::path src = root / relative;
            if (util::FileSystem::Exists(src)) {
                if (!util::FileSystem::CopyFile(src, m_tmpDir / relative)) {
                    SetFailed("Failed to copy input bindings: " + util::FileSystem::PathToUtf8(src));
                    return false;
                }
            }
        }

        /// @note Assets の外に置かれたシーンを拾う
        /// @note Assets 配下のシーンは CopyFiles で既にコピー済み。ここが効くのは Build
        /// @note Settings に Assets 外の絶対パス/別ディレクトリを足した場合だけ。
        {
            std::vector<std::string> scenesToCopy = m_settings.EnabledScenes();
            const auto autoAdd = [&scenesToCopy](const std::string& scene) {
                if (scene.empty() || scene.rfind("{{", 0) == 0) return;
                const bool already = std::any_of(scenesToCopy.begin(), scenesToCopy.end(),
                    [&scene](const std::string& s) { return s == scene; });
                if (!already) scenesToCopy.push_back(scene);
            };
            autoAdd(m_startSceneRel);
            autoAdd(m_defaultSceneRel);

            for (const auto& scenePath : scenesToCopy) {
                const std::filesystem::path src = root / util::FileSystem::PathFromUtf8(scenePath);
                const std::filesystem::path dst = m_tmpDir / util::FileSystem::PathFromUtf8(scenePath);
                if (!util::FileSystem::Exists(src)) {
                    /// @note シーンが見つからなくてもビルドは続行する (警告のみ)
                    FBZZ_LOG_WARN("BuildPipeline: scene not found, skipping: %s", util::FileSystem::PathToUtf8(src).c_str());
                    continue;
                }
                /// @note CopyFiles でコピー済み
                if (util::FileSystem::Exists(dst)) continue;
                if (!util::FileSystem::CopyFile(src, dst)) {
                    SetFailed("Failed to copy scene: " + util::FileSystem::PathToUtf8(src));
                    return false;
                }
            }
        }

        m_progress = 0.94f;
        return true;
    }

    case Step::WriteManifest: {
        m_status = "Writing manifest...";
        std::string runtimeReason;
        if (!ValidateRuntimePackage(m_tmpDir / (m_settings.productName + ".exe"),
                                    util::FileSystem::PathFromUtf8(m_engineRoot), m_runtimeConfiguration,
                                    true, runtimeReason)) {
            SetFailed(runtimeReason);
            return false;
        }
        /// @note game.manifest.toml: 製品名・バージョン・ビルド構成を書き出す。出来上がった
        /// @note 配布物だけを見てどの構成/バックエンドで焼いたか判別できるようにする。
        /// @note ランタイムは development フラグを読む。
        std::ostringstream ss;
        ss << "product_name   = \"" << m_settings.productName << "\"\n";
        ss << "version        = \"" << m_settings.version     << "\"\n";
        /// @note 版は CMake の project(VERSION) から FBZZSDK.cmake が FBZZEditor へ注入する。直書きするとリリースごとに食い違う。
        ss << "engine_version = \"" << FBZZ_ENGINE_VERSION_STRING << "\"\n";
        ss << "build_date     = \"" << TodayString()          << "\"\n";
        ss << "development    = " << (m_settings.developmentBuild ? "true" : "false") << "\n";
        ss << "configuration  = \"" << m_runtimeConfiguration << "\"\n";
        if (!m_rendererBackend.empty())
            ss << "renderer       = \"" << m_rendererBackend << "\"\n";

        const std::filesystem::path dst = m_tmpDir / L"game.manifest.toml";
        if (!util::FileSystem::WriteText(dst, ss.str())) {
            SetFailed("Failed to write game.manifest.toml");
            return false;
        }
        m_progress = 0.96f;
        return true;
    }

    case Step::CommitOutput: {
        m_status = "Committing output...";
        /// @note Start から数分経っており、その間に出力先が別物に差し替わっていても
        /// @note remove_all の直前ならここで気付ける。
        std::string outputReason;
        if (!m_settings.ValidateOutputPath(m_projectRoot, outputReason)) {
            SetFailed("Invalid output directory: " + outputReason);
            return false;
        }
        /// @note アトミックな rename で旧ビルドを保持する。rename の前に旧出力先の削除が要る。
        if (!util::FileSystem::RemoveAll(m_outputDir) ||
            !util::FileSystem::Rename(m_tmpDir, m_outputDir)) {
            SetFailed("Failed to rename output: " + util::FileSystem::PathToUtf8(m_outputDir));
            return false;
        }
        m_progress = 0.98f;
        return true;
    }

    default:
        return true;
    }
}

/// @note ファイルコピー

void BuildPipeline::BeginEnumerateFiles()
{
    m_copyJobs.clear();
    m_copyIdx = 0;

    const std::filesystem::path root = util::FileSystem::PathFromUtf8(m_projectRoot);

    struct TreeStats {
        size_t filtered = 0;   ///< @note 配布対象外として落とした数
        size_t shadowed = 0;   ///< @note プロジェクト側に同じ相対パスがあって落とした数
    };

    /// @note shadowRoot を渡すと、そこに同じ相対パスのファイルがある分を列挙しない。
    const auto enqueueTree = [this](const std::filesystem::path& srcDir,
                                    const std::filesystem::path& dstDir,
                                    bool applyFilter,
                                    const std::filesystem::path& shadowRoot = {}) -> TreeStats {
        TreeStats stats;
        for (const std::filesystem::path& src : util::FileSystem::ListFilesRecursive(srcDir)) {
            const std::filesystem::path rel = util::FileSystem::RelativePath(src, srcDir);
            if (rel.empty()) continue;
            /// @note 配布先の元画像に合わせて再生成するため、既存のストリームキャッシュはコピーしない。
            if (LowerUtf8(src.extension()) == ".fztc") continue;
            if (applyFilter && ShouldSkipAsset(src, rel)) { ++stats.filtered; continue; }
            if (!shadowRoot.empty() && util::FileSystem::Exists(shadowRoot / rel)) {
                ++stats.shadowed;
                continue;
            }
            m_copyJobs.push_back({ src, dstDir / rel });
        }
        return stats;
    };

    /// @note 1) プロジェクトの Assets
    const std::filesystem::path assetsDir = root / L"Assets";
    const size_t skipped =
        enqueueTree(assetsDir, m_tmpDir / L"Assets", m_settings.stripEditorAssets).filtered;
    if (skipped > 0)
        FBZZ_LOG_INFO("BuildPipeline: skipped %zu editor-only asset files", skipped);

    /// @note 2) Library/Baked — FBX から焼いた .fzasset/.anim/.skel/.mat の実体。.scene と
    /// @note .animcontroller は `` "guid:<derived>|Library/Baked/..." `` でこれらを参照し、
    /// @note 導出 GUID は Library/Baked の走査でしか索引に載らないため、無いと参照が全部切れる。
    const std::filesystem::path bakedDir = root / L"Library" / L"Baked";
    if (util::FileSystem::Exists(bakedDir)) {
        enqueueTree(bakedDir, m_tmpDir / L"Library" / L"Baked", false);
    } else {
        FBZZ_LOG_WARN("BuildPipeline: Library/Baked not found; imported models and clips "
                      "will fall back to re-importing the source FBX at runtime");
    }

    /// @note 3) EngineAssets — SDK 共有アセット。既定フォントのようにプロジェクト側に実体を
    /// @note 持たないアセットがあり、AssetManager は exe 隣の EngineAssets/ を engine base
    /// @note path として解決する。AssetManager::ResolvePath は «プロジェクトの Assets/ に
    /// @note 無ければ EngineAssets/ を見る» というフォールバックでプロジェクト側が常に勝つため、
    /// @note 同じ相対パスがプロジェクトにある分は複製を配布物へ入れない。
    const std::filesystem::path engineAssetsDir = m_exeSrcPath.parent_path() / L"EngineAssets";
    if (util::FileSystem::Exists(engineAssetsDir)) {
        const TreeStats engineStats = enqueueTree(
            engineAssetsDir, m_tmpDir / L"EngineAssets", m_settings.stripEditorAssets, assetsDir);
        if (engineStats.shadowed > 0)
            FBZZ_LOG_INFO("BuildPipeline: skipped %zu EngineAssets files already provided "
                          "by the project", engineStats.shadowed);
    } else {
        FBZZ_LOG_WARN("BuildPipeline: EngineAssets not found next to the exe; "
                      "assets that only exist in the SDK (default font, etc.) will be missing");
    }

    /// @note Runtime notices are not assets and must survive editor-source stripping and project shadowing.
    const std::filesystem::path engineLicensesDir = m_exeSrcPath.parent_path() / L"EngineLicenses";
    if (util::FileSystem::Exists(engineLicensesDir))
        enqueueTree(engineLicensesDir, m_tmpDir / L"EngineLicenses", false);

    m_status = "Copying files... (0/" + std::to_string(m_copyJobs.size()) + ")";
}

bool BuildPipeline::TickCopyOneFile()
{
    if (m_copyIdx >= m_copyJobs.size()) return false;

    const CopyJob& job = m_copyJobs[m_copyIdx];
    if (!util::FileSystem::CopyFile(job.src, job.dst)) {
        SetFailed("Failed to copy file: " + util::FileSystem::PathToUtf8(job.src));
        return false;
    }
    std::string cacheError;
    if (!asset::texturecache::BakeDistributionTexture(util::FileSystem::PathToUtf8(job.dst), cacheError)) {
        SetFailed("Failed to bake streaming texture: " + cacheError);
        return false;
    }
    ++m_copyIdx;

    /// @note CopyFiles の進捗は全体の 18%〜88% に割り当てる
    const float ratio = m_copyJobs.empty()
        ? 1.0f
        : static_cast<float>(m_copyIdx) / static_cast<float>(m_copyJobs.size());
    m_progress = 0.18f + ratio * 0.70f;
    m_status   = "Copying files... ("
                 + std::to_string(m_copyIdx) + "/"
                 + std::to_string(m_copyJobs.size()) + ")";
    return true;
}

void BuildPipeline::SetFailed(const std::string& reason)
{
    m_state  = State::Failed;
    m_error  = reason;
    m_status = "Error";
    /// @note 失敗した _tmp は後始末する
    util::FileSystem::RemoveAll(m_tmpDir);
    FBZZ_LOG_ERROR("BuildPipeline: %s", reason.c_str());
}

} /// @note namespace fbzz::editor
