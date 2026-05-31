// FBZZ Engine
// BuildPipeline.cpp | fbzz::editor
// ゲームパッケージングステートマシンの実装
//
// WHAT: Start() でビルドを開始し、Tick() を毎フレーム呼ぶことで
//       段階的にファイルをコピーしてパッケージを生成する。
//       各ステップの詳細は BuildPipeline.hpp のコメントを参照。
#include <Editor/BuildPipeline.hpp>
#include <Engine/Core/Logger.hpp>
#include <toml++/toml.hpp>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::editor {

namespace {

// GetModuleFileNameW で自身の exe パスを取得する
std::filesystem::path GetSelfExePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf);
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

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
                          bool runAfterBuild)
{
    m_settings      = settings;
    m_projectRoot   = projectRoot;
    m_runAfterBuild = runAfterBuild;
    m_state         = State::Running;
    m_step          = Step::PrepareTempDir;
    m_progress      = 0.0f;
    m_status        = "Starting build...";
    m_error.clear();
    m_assetFiles.clear();
    m_assetIdx    = 0;

    m_exeSrcPath = GetSelfExePath();
    m_outputDir  = settings.ResolveOutputPath(projectRoot);
    m_tmpDir     = std::filesystem::path(m_outputDir.wstring() + L"_tmp");

    FBZZ_LOG_INFO("BuildPipeline: Start → %s", WideToUtf8(m_outputDir.wstring()).c_str());
}

void BuildPipeline::Tick()
{
    if (m_state != State::Running) return;

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

    // EnumerateAssets 完了後 → CopyAssets のカーソルを初期化する
    if (m_step == Step::CopyAssets) {
        BeginEnumerateAssets();
        m_assetIdx = 0;
    }

    if (m_step == Step::Done) {
        m_state    = State::Done;
        m_progress = 1.0f;
        m_status   = "Build complete";
        FBZZ_LOG_INFO("BuildPipeline: Done");
    }
}

void BuildPipeline::Reset()
{
    m_state    = State::Idle;
    m_progress = 0.0f;
    m_status.clear();
    m_error.clear();
}

std::string BuildPipeline::GetOutputExePath() const
{
    const std::filesystem::path exePath = m_outputDir / (m_settings.productName + ".exe");
    return WideToUtf8(exePath.wstring());
}

// =============================================================================
// ステップ実行
// =============================================================================

bool BuildPipeline::ExecuteStep()
{
    std::error_code ec;
    switch (m_step) {

    // ------------------------------------------------------------------
    case Step::PrepareTempDir:
        m_status = "Preparing temp directory...";
        // 前回の _tmp が残っていれば削除する
        if (std::filesystem::exists(m_tmpDir, ec))
            std::filesystem::remove_all(m_tmpDir, ec);
        std::filesystem::create_directories(m_tmpDir, ec);
        if (ec) { SetFailed("Failed to create temp directory: " + m_tmpDir.string()); return false; }
        m_progress = 0.05f;
        return true;

    // ------------------------------------------------------------------
    case Step::CopyExecutable: {
        m_status = "Copying executable...";
        const std::filesystem::path dst = m_tmpDir / (m_settings.productName + ".exe");
        std::filesystem::copy_file(m_exeSrcPath, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { SetFailed("Failed to copy exe: " + m_exeSrcPath.string()); return false; }
        m_progress = 0.10f;
        return true;
    }

    // ------------------------------------------------------------------
    case Step::CopyDlls: {
        m_status = "Copying DLLs...";
        // WHY: Debug ビルドでは assimp-vc145-mtd.dll、Release では assimp-vc145-mt.dll が
        //      CMakeLists によって exe 隣にコピーされる。NDEBUG マクロでビルド構成を判定し、
        //      対応する DLL だけを配布パッケージに含める。
        //      配布用パッケージは Release ビルドで作成することを推奨する。
        const std::filesystem::path exeDir = m_exeSrcPath.parent_path();
#ifdef NDEBUG
        const std::wstring assimpDLL = L"assimp-vc145-mt.dll";   // Release
#else
        const std::wstring assimpDLL = L"assimp-vc145-mtd.dll";  // Debug
#endif
        const std::filesystem::path src = exeDir / assimpDLL;
        if (std::filesystem::exists(src, ec)) {
            std::filesystem::copy_file(src, m_tmpDir / assimpDLL,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { SetFailed("Failed to copy DLL: " + WideToUtf8(src.wstring())); return false; }
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
        const std::filesystem::path root(m_projectRoot);

        // 絶対パスをプロジェクトルートからの相対パス (/ 区切り) に変換するヘルパー。
        // すでに相対パスならそのまま返す。
        auto MakeRel = [&](const std::string& rawPath) -> std::string {
            if (rawPath.empty()) return rawPath;
            std::filesystem::path p(Utf8ToWide(rawPath));
            if (!p.is_absolute()) return p.generic_string();
            std::error_code relEc;
            const auto rel = std::filesystem::relative(p, root, relEc);
            return (!relEc && !rel.empty()) ? rel.generic_string() : rawPath;
        };

        // --- .fbzz_proj を読んで settings_path / default_scene を取得し相対パスに変換 ---
        // WHY (プレースホルダ対応): standard テンプレートの settings_path は "{{SETTINGS_PATH}}"
        //      のまま残る場合がある。"{{" で始まる値はプレースホルダと判断してデフォルトを使う。
        // WHY (絶対パス対応): GameHub Creator が settings_path を絶対パスで書き込む場合がある。
        //      ビルド出力では相対パスに変換することで別 PC 移動後も動作させる。
        std::string settingsRelPath = "ProjectSettings/ProjectSettings.toml";
        std::string defaultSceneRel = "Assets/Scenes/Main.fbzz";
        {
            const std::filesystem::path projFile = root / ".fbzz_proj";
            std::ifstream projIfs(projFile, std::ios::binary);
            if (projIfs) {
                std::ostringstream ss;
                ss << projIfs.rdbuf();
                auto parsed = toml::parse(ss.str());
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

            const std::filesystem::path dst = m_tmpDir / ".fbzz_proj";
            std::ofstream ofs(dst, std::ios::binary);
            if (!ofs) { SetFailed("Failed to write .fbzz_proj"); return false; }
            ofs << proj.str();
        }

        // --- ProjectSettings を相対パスに修正してコピー ---
        // WHY: default_scene / start_scene が絶対パスで保存されている場合、
        //      別 PC で起動した際にシーンが見つからなくなる。
        //      パースして絶対パスフィールドのみ相対変換し、それ以外は元の値を保持する。
        {
            const std::filesystem::path src = root / settingsRelPath;
            const std::filesystem::path dst = m_tmpDir / settingsRelPath;
            std::filesystem::create_directories(dst.parent_path(), ec);
            if (std::filesystem::exists(src, ec)) {
                std::ifstream settingsIfs(src, std::ios::binary);
                std::string settingsText;
                if (settingsIfs) {
                    std::ostringstream ss;
                    ss << settingsIfs.rdbuf();
                    settingsText = ss.str();
                }

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
                        }
                    }

                    if (modified) {
                        std::ostringstream ss;
                        ss << settingsTbl;
                        std::ofstream ofs(dst, std::ios::binary);
                        if (!ofs) { SetFailed("Failed to write patched ProjectSettings: " + src.string()); return false; }
                        ofs << ss.str();
                    } else {
                        std::filesystem::copy_file(src, dst,
                            std::filesystem::copy_options::overwrite_existing, ec);
                        if (ec) { SetFailed("Failed to copy settings file: " + src.string()); return false; }
                    }
                } else {
                    // パース失敗の場合はそのままコピーする
                    std::filesystem::copy_file(src, dst,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) { SetFailed("Failed to copy settings file: " + src.string()); return false; }
                }
            }
        }

        // --- enabled=true のシーンをコピー ---
        // WHY: シーンファイルはエンジンの assets/ ディレクトリには含まれない。
        //      BuildSettings に追加されたシーンだけをプロジェクトルートからの
        //      相対パスを保ってコピーすることで、配布 exe がシーンを発見できるようにする。
        for (const auto& scenePath : m_settings.EnabledScenes()) {
            const std::filesystem::path src = root / scenePath;
            const std::filesystem::path dst = m_tmpDir / scenePath;
            std::filesystem::create_directories(dst.parent_path(), ec);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::copy_file(src, dst,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) { SetFailed("Failed to copy scene: " + src.string()); return false; }
            } else {
                // シーンが見つからなくてもビルドは続行する (警告のみ)
                FBZZ_LOG_WARN("BuildPipeline: scene not found, skipping: %s", src.string().c_str());
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
        std::ofstream ofs(dst, std::ios::binary);
        if (!ofs) { SetFailed("Failed to write game.manifest.toml"); return false; }
        ofs << ss.str();
        m_progress = 0.93f;
        return true;
    }

    // ------------------------------------------------------------------
    case Step::CommitOutput: {
        m_status = "Committing output...";
        // WHY: アトミックな rename で旧ビルドを保持する。
        //      rename の前に旧出力先を削除する必要がある。
        if (std::filesystem::exists(m_outputDir, ec))
            std::filesystem::remove_all(m_outputDir, ec);
        std::filesystem::rename(m_tmpDir, m_outputDir, ec);
        if (ec) { SetFailed("Failed to rename output: " + ec.message()); return false; }
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
    const std::filesystem::path exeAssetsDir = m_exeSrcPath.parent_path() / "assets";
    std::error_code ec;
    for (auto& entry : std::filesystem::recursive_directory_iterator(exeAssetsDir, ec)) {
        if (entry.is_regular_file(ec))
            m_assetFiles.push_back(entry.path());
    }
    m_status = "Copying assets... (0/" +
               std::to_string(m_assetFiles.size()) + ")";
}

bool BuildPipeline::TickCopyOneFile()
{
    if (m_assetIdx >= m_assetFiles.size()) return false;

    const auto& src = m_assetFiles[m_assetIdx];
    const std::filesystem::path exeAssetsDir = m_exeSrcPath.parent_path() / "assets";
    const std::filesystem::path rel = std::filesystem::relative(src, exeAssetsDir);
    const std::filesystem::path dst = m_tmpDir / "assets" / rel;

    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst,
        std::filesystem::copy_options::overwrite_existing, ec);

    if (ec) {
        SetFailed("Failed to copy asset: " + WideToUtf8(src.wstring()));
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
    std::error_code ec;
    if (std::filesystem::exists(m_tmpDir, ec))
        std::filesystem::remove_all(m_tmpDir, ec);

    FBZZ_LOG_ERROR("BuildPipeline: %s", reason.c_str());
}

} // namespace fbzz::editor
