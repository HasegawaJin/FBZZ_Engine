/// @file    BuildPipeline.cpp
/// @brief   WHAT: Start() でビルドを開始し、Tick() を毎フレーム呼ぶことで。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// 各ステップの詳細は BuildPipeline.hpp のコメントを参照。
#include <Editor/BuildPipeline.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AppIconWriter.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>

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

// 今日の日付を YYYY-MM-DD 形式で返す
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

// 配布物に入れないファイルか (relative は Assets/ からの相対パス)。
//
// WHY .hlsl / .hlsli を残すか:
//   ShaderDependencyTracker は HLSL 本体または include 先が見つからないと CSO を
//   「stale」と判定し、実行時再コンパイルへ落ちる。ソースだけ削ると、事前に
//   焼いた .cso を持っているのに毎回コンパイルし直す配布物になる。
// WHY .meta を残すか:
//   AssetDatabase の guid 索引は .meta から作る。落とすと "guid:..." 参照が全滅する。
bool IsEditorOnlyAsset(const std::filesystem::path& relative)
{
    if (relative.begin() != relative.end()) {
        const std::string topDir = LowerUtf8(*relative.begin());
        // EditorConfig: レイアウト・個人設定・Play の退避スナップショット。
        //   WHY 拡張子で弾けないか: .playmode_snapshot.scene が入っており、
        //   拡張子だけ見ると「配布すべきシーン」と区別が付かない。
        // Scripts: スクリプト原本の置き場。ランタイムはコンパイル済みしか読まない。
        // Docs: 設計メモの置き場。中身の .md は拡張子でも落ちるが、フォルダ側は
        //   Docs.meta / archive.meta が実体を失った孤児 meta として残ってしまう。
        //   置き場ごと落として、将来ここへ画像や PDF が混ざっても配布されないようにする。
        if (topDir == "editorconfig" || topDir == "scripts" || topDir == "docs") return true;
    }

    // "_" で始まるフォルダ / ファイルは作業用の置き場として配布から外す。
    // WHY 名前で切るか: DCC の原本と退避コピーが Assets に同居している
    //     (GreenWare の Assets/_src だけで 4.5 GB = 配布物の 97%)。拡張子だけを
    //     潰していくと DCC やバックアップ規則が増えるたびに漏れるため、
    //     「置き場の名前」を規約として切り、拡張子リストは補助に留める。
    for (const std::filesystem::path& component : relative) {
        const std::string name = LowerUtf8(component);
        if (!name.empty() && name.front() == '_') return true;
    }

    const std::string ext = LowerUtf8(relative.extension());
    static constexpr std::array kSourceExts = std::to_array<const char*>({
        ".hpp", ".h", ".cpp", ".c", ".inl", ".md",
        ".bat", ".cmd", ".ps1", ".py", ".sh", ".sln", ".vcxproj", ".filters", ".user",
        // DCC の原本と作業ファイル。ランタイムが読む形式ではない。
        ".blend", ".blend1", ".blend2", ".psd", ".xcf", ".npz",
    });
    if (std::find(kSourceExts.begin(), kSourceExts.end(), ext) != kSourceExts.end())
        return true;

    // 編集ツールが原本の隣へ残す退避コピー (Boss.fbx.bak / Stage_02.scene.bak 等)。
    static constexpr std::array kBackupExts = std::to_array<const char*>({
        ".bak", ".tmp", ".orig", ".rej",
    });
    if (std::find(kBackupExts.begin(), kBackupExts.end(), ext) != kBackupExts.end())
        return true;

    const std::string name = LowerUtf8(relative.filename());
    if (name == "thumbs.db" || name == ".ds_store") return true;
    // シェーダーコンパイルのログ (compile_log.txt / compile_ui_log.txt)
    if (ext == ".txt" && name.rfind("compile_", 0) == 0) return true;

    return false;
}

// .meta は本体 (ファイルでもフォルダでも) の扱いに従わせる。
// WHY: 実体を失った .meta は AssetDatabase が起動時に「孤児」として削除しにいく。
//      配布先ディレクトリへの書き込みが発生するうえ、索引の警告も無意味に増える。
bool ShouldSkipAsset(const std::filesystem::path& absPath, const std::filesystem::path& relative)
{
    if (LowerUtf8(relative.extension()) != ".meta")
        return IsEditorOnlyAsset(relative);

    std::filesystem::path companionAbs = absPath;
    companionAbs.replace_extension();
    std::error_code ec;
    // 実体が消えている .meta は判断材料が無い。既存の索引を壊さないよう残す。
    if (!std::filesystem::exists(companionAbs, ec)) return false;

    std::filesystem::path companionRel = relative;
    companionRel.replace_extension();
    return IsEditorOnlyAsset(companionRel);
}

} // namespace

// =============================================================================
// 開始 / 進行
// =============================================================================

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

    // WHY 開始前に弾くか: CommitOutput は出力先を remove_all する。数分かけて
    //     コンパイルし終えた後ではなく、ここで止めないと削除まで走ってしまう。
    std::string outputReason;
    if (!m_settings.ValidateOutputPath(m_projectRoot, outputReason)) {
        m_tmpDir.clear();   // まだ作っていない。SetFailed の後始末対象にしない
        SetFailed("Invalid output directory: " + outputReason);
        return;
    }

    ResolveProjectLayout();

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
            // WHY: ゲーム側 CMakeLists は FBZZ_SDK_ROOT が無いと configure で落ちる。
            //      ビルド中に ZERO_CHECK が再 configure を走らせても SDK を見失わせない。
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

    // CopyFiles は 1 Tick = 1 ファイルで処理する
    if (m_step == Step::CopyFiles) {
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

    m_step = static_cast<Step>(static_cast<int>(m_step) + 1);

    static constexpr const char* kStepNames[] = {
        "Compile", "PrepareTempDir", "CopyExecutable", "ApplyIcon", "CopyDlls",
        "EnumerateFiles", "CopyFiles", "CopyProjectFiles", "WriteManifest", "CommitOutput", "Done"
    };
    const int stepIdx = static_cast<int>(m_step);
    if (stepIdx >= 0 && stepIdx < static_cast<int>(std::size(kStepNames)))
        FBZZ_LOG_DEBUG("BuildPipeline: step → %s", kStepNames[stepIdx]);

    // EnumerateFiles 完了後 → CopyFiles のカーソルを初期化する
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

// =============================================================================
// プロジェクト構成の解決
// =============================================================================

void BuildPipeline::ResolveProjectLayout()
{
    m_settingsRelPath = "ProjectSettings/ProjectSettings.toml";
    m_defaultSceneRel = "Assets/Scenes/Main.scene";
    m_startSceneRel.clear();
    m_rendererBackend.clear();

    const std::filesystem::path root = util::FileSystem::PathFromUtf8(m_projectRoot);

    // 絶対パスをプロジェクトルート相対 (/ 区切り) に変換する。
    // WHY: GameHub Creator は settings_path を絶対パスで書き込むことがある。
    //      配布物へそのまま入れると別 PC で解決できない。
    const auto makeRelative = [&root](const std::string& rawPath) -> std::string {
        if (rawPath.empty()) return rawPath;
        std::filesystem::path p = util::FileSystem::PathFromUtf8(rawPath);
        if (!p.is_absolute()) return p.generic_string();
        const auto rel = util::FileSystem::RelativePath(p, root);
        return !rel.empty() ? rel.generic_string() : rawPath;
    };

    // WHY (プレースホルダ対応): standard テンプレートの settings_path は "{{SETTINGS_PATH}}"
    //      のまま残る場合がある。"{{" で始まる値はプレースホルダと判断してデフォルトを使う。
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

    // WHY: .fbzz_proj の default_scene は実体と拡張子がずれていることがある
    //      (GameHub が生成した ".fbzz" のまま等)。そのまま配布物へ書くと
    //      ランタイムの開始シーン解決が失敗して起動できない。
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
            // WHY [app] を先に見るか: ProjectSettings の renderer は [app] テーブルにある。
            //      トップレベルだけを読んでいたため常に空になり、DX12 で dxcompiler.dll /
            //      dxil.dll を必須にする CopyDlls のガードが一度も発火していなかった。
            //      旧形式のトップレベルも読み続ける。
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
        // WHY 警告で済ませないか: アイコンは «出来上がった exe を見て» しか確認できない。
        //     黙って既定アイコンのまま配ると、配った後に気付くことになる。
        if (!AppIconWriter::Apply(icon, exe, reason)) {
            SetFailed("Failed to apply icon: " + reason);
            return false;
        }
        FBZZ_LOG_INFO("BuildPipeline: icon applied from %s",
                      util::FileSystem::PathToUtf8(icon).c_str());
        return true;
    }

    // ------------------------------------------------------------------
    case Step::CopyDlls: {
        m_status = "Copying DLLs...";
        const std::filesystem::path exeDir = m_exeSrcPath.parent_path();
        const std::wstring assimpDLL = m_runtimeConfiguration == "Debug"
            ? L"assimp-vc145-mtd.dll"
            : L"assimp-vc145-mt.dll";

        // WHY 固定リストではなく exe 隣を丸ごと同期するか:
        //   ランタイム DLL の内訳を決めているのは SDK のステージング (fbzz_stage_runtime) で、
        //   ここに列挙を置くと SDK が 1 つ増やすたびに配布物だけが欠けた状態になる。
        //   実際 DX12 の dxcompiler.dll / dxil.dll がこの形で漏れていた。
        std::vector<std::filesystem::path> runtimeDlls;
        for (const std::filesystem::path& file : util::FileSystem::ListFiles(exeDir)) {
            if (LowerUtf8(file.extension()) == ".dll")
                runtimeDlls.push_back(file);
        }

        // WHAT: 実行に必須の DLL がないパッケージを成功扱いにしない。
        // WHY: コピーを黙ってスキップすると、Build complete 表示後の起動時にだけ
        //      DLL 不足が発覚し、原因となったビルド工程を特定しにくいため。
        std::vector<std::wstring> required = {
            L"imgui.dll", L"FBZZMath.dll", L"FBZZPhysics.dll", L"FBZZEngine.dll", assimpDLL,
        };
        // WHY DX12 で DXC を必須にするか:
        //   compiled_dx12 の .cso (DXIL) を読むだけの経路でも、頂点入力と
        //   MaterialConstants のリフレクションに dxcompiler.dll が要る。無いと
        //   DX12Shader::Init が全シェーダーで失敗し、何も描かれないまま起動する。
        if (m_rendererBackend == "dx12") {
            required.push_back(L"dxcompiler.dll");
            required.push_back(L"dxil.dll");
        }

        for (const std::wstring& dllName : required) {
            const std::filesystem::path src = exeDir / dllName;
            if (!util::FileSystem::Exists(src)) {
                SetFailed("Required runtime DLL not found: " + util::FileSystem::PathToUtf8(src));
                return false;
            }
        }

        for (const std::filesystem::path& src : runtimeDlls) {
            if (!util::FileSystem::CopyFile(src, m_tmpDir / src.filename())) {
                SetFailed("Failed to copy DLL: " + util::FileSystem::PathToUtf8(src));
                return false;
            }
        }
        FBZZ_LOG_DEBUG("BuildPipeline: copied %zu runtime DLLs", runtimeDlls.size());

        // スクリプト DLL をコピーする。
        // WHY: DLL 名はプロジェクトごとに異なるため、exe 隣に無い場合もある
        //      (Binaries/<Config> ではなく別ディレクトリに出るプロジェクト)。
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
    case Step::EnumerateFiles:
        // BeginEnumerateFiles() は CopyFiles に遷移した直後に呼ばれるため、
        // ここでは何もしない (ステップ遷移ロジックは Tick() 側が担う)
        m_status   = "Enumerating files...";
        m_progress = 0.18f;
        return true;

    // ------------------------------------------------------------------
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

        // --- ビルド向け最小 .fbzz_proj を書き出す (開発環境固有の絶対パスを除去) ---
        // WHY: 元の .fbzz_proj には engine.root / api_root / script_root 等の
        //      開発環境固有の絶対パスが含まれる。別 PC で起動した際にパスが無効になるため、
        //      ランタイムが読むフィールドだけを相対パスで書き直す。
        {
            std::ostringstream proj;
            proj << "[project]\n";
            proj << "name          = \"" << m_settings.productName << "\"\n";
            proj << "settings_path = \"" << m_settingsRelPath << "\"\n";
            proj << "default_scene = \"" << m_defaultSceneRel << "\"\n";
            // WHY: スクリプトを DLL で持つランタイムはこのフィールドを読んでロードする。
            //      DLL 名はプロジェクトごとに異なるためハードコードせず、ここに記録する。
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

        // --- ProjectSettings を相対パスに修正してコピー ---
        // WHY: default_scene / start_scene が絶対パスで保存されている場合、
        //      パースして絶対パスフィールドのみ相対変換し、それ以外は元の値を保持する。
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

        // --- 入力バインド (.inputactions) をコピー ---
        // WHY 必須か: ProjectSettings.toml と同じディレクトリの Input.inputactions を
        //      ランタイムが読む。これを配布物へ入れ忘れると、ビルドしたゲームだけ
        //      既定バインドに戻り、エディタで設定したキーコンフィグが反映されない。
        // WHY 存在しなくても失敗にしないか: 入力設定は任意。無ければ既定バインドで動く。
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

        // --- Assets の外に置かれたシーンを拾う ---
        // WHY: Assets 配下のシーンは CopyFiles で既にコピー済み。ここが効くのは
        //      Build Settings に Assets 外の絶対パス / 別ディレクトリを足した場合だけ。
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
                    // シーンが見つからなくてもビルドは続行する (警告のみ)
                    FBZZ_LOG_WARN("BuildPipeline: scene not found, skipping: %s", util::FileSystem::PathToUtf8(src).c_str());
                    continue;
                }
                if (util::FileSystem::Exists(dst)) continue; // CopyFiles でコピー済み
                if (!util::FileSystem::CopyFile(src, dst)) {
                    SetFailed("Failed to copy scene: " + util::FileSystem::PathToUtf8(src));
                    return false;
                }
            }
        }

        m_progress = 0.94f;
        return true;
    }

    // ------------------------------------------------------------------
    case Step::WriteManifest: {
        m_status = "Writing manifest...";
        // game.manifest.toml: 製品名・バージョン・ビルド構成を書き出す。
        // WHY: 出来上がった配布物だけを見て「どの構成のどのバックエンドで焼いたか」を
        //      判別できるようにする。ランタイムは development フラグを読む。
        std::ostringstream ss;
        ss << "product_name   = \"" << m_settings.productName << "\"\n";
        ss << "version        = \"" << m_settings.version     << "\"\n";
        ss << "engine_version = \"0.1.0\"\n";
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

    // ------------------------------------------------------------------
    case Step::CommitOutput: {
        m_status = "Committing output...";
        // WHY もう一度検証するか: Start から数分経っている。その間に出力先が
        //      別物に差し替わっていても、remove_all の直前なら気付ける。
        std::string outputReason;
        if (!m_settings.ValidateOutputPath(m_projectRoot, outputReason)) {
            SetFailed("Invalid output directory: " + outputReason);
            return false;
        }
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

// =============================================================================
// ファイルコピー
// =============================================================================

void BuildPipeline::BeginEnumerateFiles()
{
    m_copyJobs.clear();
    m_copyIdx = 0;

    const std::filesystem::path root = util::FileSystem::PathFromUtf8(m_projectRoot);

    struct TreeStats {
        size_t filtered = 0;   // 配布対象外として落とした数
        size_t shadowed = 0;   // プロジェクト側に同じ相対パスがあって落とした数
    };

    // shadowRoot を渡すと、そこに同じ相対パスのファイルがある分を列挙しない。
    const auto enqueueTree = [this](const std::filesystem::path& srcDir,
                                    const std::filesystem::path& dstDir,
                                    bool applyFilter,
                                    const std::filesystem::path& shadowRoot = {}) -> TreeStats {
        TreeStats stats;
        for (const std::filesystem::path& src : util::FileSystem::ListFilesRecursive(srcDir)) {
            const std::filesystem::path rel = util::FileSystem::RelativePath(src, srcDir);
            if (rel.empty()) continue;
            if (applyFilter && ShouldSkipAsset(src, rel)) { ++stats.filtered; continue; }
            if (!shadowRoot.empty() && util::FileSystem::Exists(shadowRoot / rel)) {
                ++stats.shadowed;
                continue;
            }
            m_copyJobs.push_back({ src, dstDir / rel });
        }
        return stats;
    };

    // 1) プロジェクトの Assets
    const std::filesystem::path assetsDir = root / L"Assets";
    const size_t skipped =
        enqueueTree(assetsDir, m_tmpDir / L"Assets", m_settings.stripEditorAssets).filtered;
    if (skipped > 0)
        FBZZ_LOG_INFO("BuildPipeline: skipped %zu editor-only asset files", skipped);

    // 2) Library/Baked — FBX から焼いた .fzasset / .anim / .skel / .mat の実体。
    // WHY 必須か: .scene と .animcontroller は "guid:<derived>|Library/Baked/..." で
    //      これらを参照する。導出 GUID は Library/Baked の走査でしか索引に載らないため、
    //      同梱しないとアニメーションとモデル由来マテリアルの参照が全部切れる。
    const std::filesystem::path bakedDir = root / L"Library" / L"Baked";
    if (util::FileSystem::Exists(bakedDir)) {
        enqueueTree(bakedDir, m_tmpDir / L"Library" / L"Baked", false);
    } else {
        FBZZ_LOG_WARN("BuildPipeline: Library/Baked not found; imported models and clips "
                      "will fall back to re-importing the source FBX at runtime");
    }

    // 3) EngineAssets — SDK 共有アセット。
    // WHY: 既定フォントのようにプロジェクト側へ実体を持たないアセットがあり、
    //      AssetManager は exe 隣の EngineAssets/ を engine base path として解決する。
    //
    // WHY プロジェクトに同じ相対パスがある分を落とすか:
    //      AssetManager::ResolvePath は «プロジェクトの Assets/ に無ければ
    //      EngineAssets/ を見る» というフォールバックで、プロジェクト側が常に勝つ。
    //      GameHub のテンプレートが SDK の Assets を丸ごと複製しているため、
    //      GreenWare では 345 MB / 1106 ファイルが «絶対に読まれない複製» だった
    //      (sky_env.dds 256 MB が配布物に 2 つ入る)。
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
    ++m_copyIdx;

    // WHY: CopyFiles の進捗は全体の 18%〜88% に割り当てる
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
    // 失敗した _tmp は後始末する
    util::FileSystem::RemoveAll(m_tmpDir);
    FBZZ_LOG_ERROR("BuildPipeline: %s", reason.c_str());
}

} // namespace fbzz::editor
