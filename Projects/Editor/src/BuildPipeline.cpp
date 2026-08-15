// FBZZ Engine
// BuildPipeline.cpp | fbzz::editor
// ゲームパッケージングステートマシンの実装
//
// WHAT: Start() でビルドを開始し、Tick() を毎フレーム呼ぶことで
//       段階的にファイルをコピーしてパッケージを生成する。
//       各ステップの詳細は BuildPipeline.hpp のコメントを参照。
#include <Editor/BuildPipeline.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

namespace fbzz::editor {

namespace {

// 今日の日付を YYYY-MM-DD 形式で返す
std::string TodayStr()
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

} // namespace

// =============================================================================
// 公開 API
// =============================================================================

void BuildPipeline::Start(const BuildSettings& settings,
                          const std::string& projectRoot,
                          const std::string& buildRoot,
                          const std::string& targetName,
                          const std::string& scriptsDllPath,
                          bool runAfterBuild)
{
    m_settings          = settings;
    m_projectRoot       = projectRoot;
    m_buildRoot         = buildRoot;
    m_targetName        = targetName.empty() ? "SandboxStandalone" : targetName;
    m_runtimeConfiguration = "Development";
    m_scriptsDllSrcPath = scriptsDllPath;
    m_runAfterBuild     = runAfterBuild;
    m_state         = State::Running;
    m_step          = Step::Compile;
    m_progress      = 0.0f;
    m_status        = "Starting build...";
    m_error.clear();
    m_assetFiles.clear();
    m_assetIdx    = 0;
    m_compileStarted = false;

    m_exeSrcPath.clear();
    m_outputDir  = settings.ResolveOutputPath(projectRoot);
    m_tmpDir     = m_outputDir;
    m_tmpDir    += L"_tmp";

    FBZZ_LOG_INFO("BuildPipeline: Start → %s", util::FileSystem::PathToUtf8(m_outputDir).c_str());
}

void BuildPipeline::Tick()
{
    if (m_state != State::Running) return;

    // Compile は CMake 子プロセスが複数フレーム継続するため、専用処理で状態を監視する。
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
            // WHY: Build Settings の Development Build は CMake の Development 構成に対応する。
            //      Debug を使うと Development 用に配置された EXE / DLL とずれてパッケージングに失敗する。
            config.exePath       = m_settings.developmentBuild
                ? (!toolchain.exeDevelopment.empty() ? toolchain.exeDevelopment : toolchain.exeDebug)
                : toolchain.exeRelease;
            config.target        = m_targetName;
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

            FBZZ_LOG_DEBUG("BuildPipeline: cmake=%s build=%s target=%s cfg=%s",
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

    // CopyAssets は 1 Tick = 1 ファイルで処理する
    if (m_step == Step::CopyAssets) {
        if (!TickCopyOneFile()) {
            // 全ファイルコピー完了 → 次ステップへ
            m_step = Step::CopyProjectFiles;
        }
        return;
    }

    // それ以外のステップは 1 Tick = 1 ステップで完了する
    if (!ExecuteStep()) {
        // ExecuteStep 内で SetFailed() 呼び済み
        return;
    }

    // ステップを進める
    m_step = static_cast<Step>(static_cast<int>(m_step) + 1);

    static constexpr const char* kStepNames[] = {
        "Compile", "PrepareTempDir", "CopyExecutable", "CopyDlls",
        "EnumerateAssets", "CopyAssets", "CopyProjectFiles", "WriteManifest", "CommitOutput", "Done"
    };
    const int stepIdx = static_cast<int>(m_step);
    if (stepIdx >= 0 && stepIdx < static_cast<int>(std::size(kStepNames)))
        FBZZ_LOG_DEBUG("BuildPipeline: step → %s", kStepNames[stepIdx]);

    // EnumerateAssets 完了後 → CopyAssets のカーソルを初期化する
    if (m_step == Step::CopyAssets) {
        BeginEnumerateAssets();
        m_assetIdx = 0;
    }

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

// =============================================================================
// ステップ実行
// =============================================================================

bool BuildPipeline::ExecuteStep()
{
    switch (m_step) {

    // ------------------------------------------------------------------
    case Step::PrepareTempDir:
        m_status = "Preparing temp directory...";
        // 前回の _tmp が残っていれば削除する
        if (!util::FileSystem::RemoveAll(m_tmpDir) ||
            !util::FileSystem::EnsureDirectory(m_tmpDir)) {
            SetFailed("Failed to create temp directory: " + util::FileSystem::PathToUtf8(m_tmpDir));
            return false;
        }
        m_progress = 0.05f;
        return true;

    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    case Step::CopyDlls: {
        m_status = "Copying DLLs...";
        // WHY: RuntimeBuild の構成はエディタ自身の構成ではなく BuildSettings で決まる。
        //      developmentBuild=true なら Development、false なら Release の成果物 exe 隣から
        //      実行時に必要な DLL をコピーする。fbzz_* は shared_runtime 化により EXE / Script DLL
        //      から同じ Engine 状態を参照するため、配布物にも必ず同梱する。
        const std::filesystem::path exeDir = m_exeSrcPath.parent_path();
        const std::wstring assimpDLL = m_runtimeConfiguration == "Debug"
            ? L"assimp-vc145-mtd.dll"
            : L"assimp-vc145-mt.dll";

        const std::array<std::wstring, 5> runtimeDlls = {
            L"imgui.dll",
            // WHY: 各共有ライブラリの CMake OUTPUT_NAME に合わせる。
            //      名前が異なるとコピー元が見つからず、配布物の起動時に DLL 不足となる。
            L"FBZZMath.dll",
            L"FBZZPhysics.dll",
            L"FBZZEngine.dll",
            assimpDLL,
        };

        for (const std::wstring& dllName : runtimeDlls) {
            const std::filesystem::path src = exeDir / dllName;
            if (!util::FileSystem::Exists(src)) {
                // WHAT: 実行に必須の DLL がないパッケージを成功扱いにしない。
                // WHY: コピーを黙ってスキップすると、Build complete 表示後の起動時にだけ
                //      DLL 不足が発覚し、原因となったビルド工程を特定しにくいため。
                SetFailed("Required runtime DLL not found: " + util::FileSystem::PathToUtf8(src));
                return false;
            }

            if (!util::FileSystem::CopyFile(src, m_tmpDir / dllName)) {
                SetFailed("Failed to copy DLL: " + util::FileSystem::PathToUtf8(src));
                return false;
            }
        }

        // スクリプト DLL をコピーする。
        // WHY: DLL 名はプロジェクトごとに異なるため、上の固定リストに含めず
        //      呼び出し元 (BuildSettingsPanel) が EditorContext.scriptsDllPath から渡す。
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

    // ------------------------------------------------------------------
    case Step::EnumerateAssets:
        // BeginEnumerateAssets() は CopyAssets に遷移する直前に呼ばれるため、
        // ここでは何もしない (ステップ遷移ロジックは Tick() 側が担う)
        m_status   = "Enumerating assets...";
        m_progress = 0.18f;
        return true;

    // ------------------------------------------------------------------
    case Step::CopyProjectFiles: {
        m_status = "Copying project files...";
        const std::filesystem::path root = util::FileSystem::PathFromUtf8(m_projectRoot);

        // 絶対パスをプロジェクトルートからの相対パス (/ 区切り) に変換するヘルパー。
        // すでに相対パスならそのまま返す。
        auto MakeRel = [&](const std::string& rawPath) -> std::string {
            if (rawPath.empty()) return rawPath;
            std::filesystem::path p = util::FileSystem::PathFromUtf8(rawPath);
            if (!p.is_absolute()) return p.generic_string();
            const auto rel = util::FileSystem::RelativePath(p, root);
            return !rel.empty() ? rel.generic_string() : rawPath;
        };

        // --- .fbzz_proj を読んで settings_path / default_scene を取得し相対パスに変換 ---
        // WHY (プレースホルダ対応): standard テンプレートの settings_path は "{{SETTINGS_PATH}}"
        //      のまま残る場合がある。"{{" で始まる値はプレースホルダと判断してデフォルトを使う。
        // WHY (絶対パス対応): GameHub Creator が settings_path を絶対パスで書き込む場合がある。
        //      ビルド出力では相対パスに変換することで別 PC 移動後も動作させる。
        std::string settingsRelPath = "ProjectSettings/ProjectSettings.toml";
        std::string defaultSceneRel = "Assets/Scenes/Main.scene";
        std::string startSceneRel;   // ProjectSettings の runtime.start_scene (自動コピー用)
        {
            const std::filesystem::path projFile = root / ".fbzz_proj";
            std::string projectText;
            if (util::FileSystem::ReadText(projFile, projectText)) {
                auto parsed = toml::parse(projectText);
                if (parsed) {
                    const auto sp = parsed.table()["project"]["settings_path"].value<std::string>();
                    if (sp && sp->size() >= 2 && sp->rfind("{{", 0) != 0)
                        settingsRelPath = MakeRel(*sp);

                    const auto ds = parsed.table()["project"]["default_scene"].value<std::string>();
                    if (ds && !ds->empty() && ds->rfind("{{", 0) != 0)
                        defaultSceneRel = MakeRel(*ds);
                }
            }
        }

        // --- ビルド向け最小 .fbzz_proj を書き出す (開発環境固有の絶対パスを除去) ---
        // WHY: 元の .fbzz_proj には engine.root / api_root / script_root 等の
        //      開発環境固有の絶対パスが含まれる。別 PC で起動した際にパスが無効になるため、
        //      ランタイムに必要な最小フィールドのみを相対パスで書き出す。
        {
            std::ostringstream proj;
            proj << "[project]\n";
            proj << "name          = \"" << m_settings.productName << "\"\n";
            proj << "settings_path = \"" << settingsRelPath << "\"\n";
            proj << "default_scene = \"" << defaultSceneRel << "\"\n";
            // WHY: StandaloneApp はこのフィールドを読んでスクリプト DLL をロードする。
            //      DLL 名はプロジェクトごとに異なるためハードコードせず、ここに記録する。
            if (!m_scriptsDllSrcPath.empty()) {
                const std::filesystem::path scriptsDll = util::FileSystem::PathFromUtf8(m_scriptsDllSrcPath);
                proj << "scripts_dll   = \"" << util::FileSystem::PathToUtf8(scriptsDll.filename()) << "\"\n";
            }

            const std::filesystem::path dst = m_tmpDir / ".fbzz_proj";
            if (!util::FileSystem::WriteText(dst, proj.str())) {
                SetFailed("Failed to write .fbzz_proj");
                return false;
            }
        }

        // --- ProjectSettings を相対パスに修正してコピー ---
        // WHY: default_scene / start_scene が絶対パスで保存されている場合、
        //      別 PC で起動した際にシーンが見つからなくなる。
        //      パースして絶対パスフィールドのみ相対変換し、それ以外は元の値を保持する。
        {
            const std::filesystem::path src = root / settingsRelPath;
            const std::filesystem::path dst = m_tmpDir / settingsRelPath;
            if (util::FileSystem::Exists(src)) {
                std::string settingsText;
                util::FileSystem::ReadText(src, settingsText);

                auto settingsParsed = toml::parse(settingsText);
                bool modified = false;

                if (settingsParsed) {
                    auto& settingsTbl = settingsParsed.table();

                    if (auto* projTbl = settingsTbl["project"].as_table()) {
                        if (auto* strNode = (*projTbl)["default_scene"].as_string()) {
                            const std::string fixed = MakeRel(strNode->get());
                            if (fixed != strNode->get()) { strNode->get() = fixed; modified = true; }
                        }
                    }
                    if (auto* runtimeTbl = settingsTbl["runtime"].as_table()) {
                        if (auto* strNode = (*runtimeTbl)["start_scene"].as_string()) {
                            const std::string fixed = MakeRel(strNode->get());
                            if (fixed != strNode->get()) { strNode->get() = fixed; modified = true; }
                            // 自動コピー対象として記録する
                            startSceneRel = fixed.empty() ? strNode->get() : fixed;
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
                    // パース失敗の場合はそのままコピーする
                    if (!util::FileSystem::CopyFile(src, dst)) {
                        SetFailed("Failed to copy settings file: " + util::FileSystem::PathToUtf8(src));
                        return false;
                    }
                }
            }
        }

        // --- 入力バインド (.inputactions) をコピー ---
        // WHY 必須か: ProjectSettings.toml と同じディレクトリの Input.inputactions を
        //      ランタイムが読む。これを配布物へ入れ忘れると、ビルドしたゲームだけ
        //      既定バインドに戻り、エディタで設定したキーコンフィグが反映されない。
        // WHY 存在しなくても失敗にしないか: 入力設定は任意。無ければ既定バインドで動く。
        {
            const std::filesystem::path settingsDir =
                std::filesystem::path(settingsRelPath).parent_path();
            const std::filesystem::path relative = settingsDir / "Input.inputactions";
            const std::filesystem::path src = root / relative;
            if (util::FileSystem::Exists(src)) {
                if (!util::FileSystem::CopyFile(src, m_tmpDir / relative)) {
                    SetFailed("Failed to copy input bindings: " + util::FileSystem::PathToUtf8(src));
                    return false;
                }
            }
        }

        // --- シーンをコピー ---
        // WHY: EnabledScenes() は BuildSettings パネルでユーザーが手動追加したシーン。
        //      それに加えて ProjectSettings の runtime.start_scene / project.default_scene も
        //      自動でコピーする。これにより BuildSettings パネルに何も追加しなくても
        //      ゲームが起動できる。
        {
            // コピー対象セット: EnabledScenes + start_scene + default_scene (重複排除)
            std::vector<std::string> scenesToCopy = m_settings.EnabledScenes();
            auto autoAdd = [&](const std::string& scene) {
                if (!scene.empty() && scene.rfind("{{", 0) != 0) {
                    const bool already = std::any_of(scenesToCopy.begin(), scenesToCopy.end(),
                        [&scene](const std::string& s) { return s == scene; });
                    if (!already) scenesToCopy.push_back(scene);
                }
            };
            autoAdd(startSceneRel);   // runtime.start_scene から取得
            autoAdd(defaultSceneRel); // .fbzz_proj の default_scene から取得

            for (const auto& scenePath : scenesToCopy) {
                const std::filesystem::path src = root / scenePath;
                const std::filesystem::path dst = m_tmpDir / scenePath;
                if (util::FileSystem::Exists(src)) {
                    if (!util::FileSystem::CopyFile(src, dst)) {
                        SetFailed("Failed to copy scene: " + util::FileSystem::PathToUtf8(src));
                        return false;
                    }
                } else {
                    // シーンが見つからなくてもビルドは続行する (警告のみ)
                    FBZZ_LOG_WARN("BuildPipeline: scene not found, skipping: %s", util::FileSystem::PathToUtf8(src).c_str());
                }
            }
        }

        m_progress = 0.88f;
        return true;
    }

    // ------------------------------------------------------------------
    case Step::WriteManifest: {
        m_status = "Writing manifest...";
        // game.manifest.toml: 製品名・バージョン・開発フラグを書き出す
        // WHY: コンパイルは行わないため development フラグはランタイムに引き渡すしかなく、
        //      このファイルを読んで StandaloneApp がログレベルを切り替える想定。
        std::ostringstream ss;
        ss << "product_name   = \"" << m_settings.productName << "\"\n";
        ss << "version        = \"" << m_settings.version     << "\"\n";
        ss << "engine_version = \"0.1.0\"\n";
        ss << "build_date     = \"" << TodayStr()             << "\"\n";
        ss << "development    = " << (m_settings.developmentBuild ? "true" : "false") << "\n";

        const std::filesystem::path dst = m_tmpDir / "game.manifest.toml";
        if (!util::FileSystem::WriteText(dst, ss.str())) {
            SetFailed("Failed to write game.manifest.toml");
            return false;
        }
        m_progress = 0.93f;
        return true;
    }

    // ------------------------------------------------------------------
    case Step::CommitOutput: {
        m_status = "Committing output...";
        // WHY: アトミックな rename で旧ビルドを保持する。
        //      rename の前に旧出力先を削除する必要がある。
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

void BuildPipeline::BeginEnumerateAssets()
{
    m_assetFiles.clear();
    const std::filesystem::path projectAssetsDir = util::FileSystem::PathFromUtf8(m_projectRoot) / "Assets";
    m_assetFiles = util::FileSystem::ListFilesRecursive(projectAssetsDir);
    m_status = "Copying assets... (0/" +
               std::to_string(m_assetFiles.size()) + ")";
}

bool BuildPipeline::TickCopyOneFile()
{
    if (m_assetIdx >= m_assetFiles.size()) return false;

    const auto& src = m_assetFiles[m_assetIdx];
    const std::filesystem::path projectAssetsDir = util::FileSystem::PathFromUtf8(m_projectRoot) / "Assets";
    const std::filesystem::path rel = util::FileSystem::RelativePath(src, projectAssetsDir);
    const std::filesystem::path dst = m_tmpDir / "Assets" / rel;

    if (rel.empty() || !util::FileSystem::CopyFile(src, dst)) {
        SetFailed("Failed to copy asset: " + util::FileSystem::PathToUtf8(src));
        return false;
    }

    ++m_assetIdx;
    // WHY: CopyAssets の進捗は全体の 18%〜88% に割り当てる
    const float ratio = (m_assetFiles.empty())
        ? 1.0f
        : static_cast<float>(m_assetIdx) / static_cast<float>(m_assetFiles.size());
    m_progress = 0.18f + ratio * 0.70f;
    m_status   = "Copying assets... ("
                 + std::to_string(m_assetIdx) + "/"
                 + std::to_string(m_assetFiles.size()) + ")";
    return true;
}

void BuildPipeline::SetFailed(const std::string& reason)
{
    m_state  = State::Failed;
    m_error  = reason;
    m_status = "Error";

    // 失敗した _tmp は後始末する
    util::FileSystem::RemoveAll(m_tmpDir);

    FBZZ_LOG_ERROR("BuildPipeline: %s", reason.c_str());
}

} // namespace fbzz::editor
