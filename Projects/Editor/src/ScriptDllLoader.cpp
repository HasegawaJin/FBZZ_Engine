// FBZZ Engine
// ScriptDllLoader.cpp | fbzz::editor
// スクリプト DLL のロード / アンロード / ホットリロード管理
#include <Editor/ScriptDllLoader.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Script.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fbzz::editor {

namespace {

// SandboxScripts.dll のエクスポート関数シグネチャ
using RegisterFnPtr = void(*)(
    void(*)(const char*, std::function<std::unique_ptr<fbzz::scene::Script>()>)
);

// DLL エクスポート名 (全プロジェクト共通)
// WHY: プロジェクト固有名 (SandboxScripts_Register 等) を使うと ScriptDllLoader が
//      プロジェクトごとに変わる。FBZZScripts_Register をエンジン規約として統一し、
//      任意のプロジェクトの DLL をロードできるようにする。
constexpr const char* kRegisterFnName = "FBZZScripts_Register";

// タイムスタンプ文字列を生成する (コピー先ファイル名の一部に使う)
std::wstring MakeTimestamp()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_wstring(ms);
}

} // namespace

// =============================================================================
// 公開 API
// =============================================================================

bool ScriptDllLoader::Load(const std::filesystem::path& dllPath)
{
    if (m_hDll) {
        FBZZ_LOG_WARN("ScriptDllLoader::Load: DLL is already loaded");
        return false;
    }

    m_dllPath = dllPath;

    std::error_code ec;
    if (!std::filesystem::exists(dllPath, ec)) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Load: DLL not found: %ls",
                       dllPath.wstring().c_str());
        return false;
    }

    // _hot/ にコピーしてからロードする (元ファイルを再ビルドできるようにするため)
    CleanHotDir();
    m_hotCopy = CopyToHot(dllPath);
    if (m_hotCopy.empty()) return false;

    FBZZ_LOG_DEBUG("ScriptDllLoader: loading hot copy: %ls", m_hotCopy.wstring().c_str());
    m_hDll = LoadLibraryW(m_hotCopy.wstring().c_str());
    if (!m_hDll) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Load: LoadLibrary failed: %ls (GLE=%lu)",
                       m_hotCopy.wstring().c_str(), GetLastError());
        m_hotCopy.clear();
        return false;
    }

    RegisterScripts();
    FBZZ_LOG_INFO("ScriptDllLoader: DLL loaded -> %ls", m_hotCopy.wstring().c_str());
    return true;
}

void ScriptDllLoader::Unload(scene::Scene* scene)
{
    if (!m_hDll) return;

    // FreeLibrary 前に仮想デストラクタが DLL コード内にある Script インスタンスをすべて破棄する。
    // WHY: FreeLibrary 後に仮想デストラクタを呼ぶとアクセス違反になるため。
    if (scene) {
        FBZZ_LOG_DEBUG("ScriptDllLoader: destroying all script instances before unload");
        DestroyAllScripts(*scene);
    }

    scene::ScriptFactory::UnregisterAll();
    FBZZ_LOG_DEBUG("ScriptDllLoader: ScriptFactory unregistered all");

    FreeLibrary(m_hDll);
    m_hDll = nullptr;

    FBZZ_LOG_INFO("ScriptDllLoader: DLL unloaded");
}

bool ScriptDllLoader::Reload(scene::Scene& scene, const std::filesystem::path& newDllPath)
{
    // シーン全体をシリアライズしてスクリプトフィールドを保存する
    const std::string snapshot = SceneIO::Serialize(scene);
    if (snapshot.empty()) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Reload: failed to serialize scene");
        return false;
    }

    // 現在の DLL をアンロードする (Script インスタンスもここで破棄)
    Unload(&scene);

    // 新しい DLL をロードして ScriptFactory に再登録する
    if (!Load(newDllPath.empty() ? m_dllPath : newDllPath)) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Reload: failed to load the new DLL");
        if (!SceneIO::Deserialize(scene, snapshot))
            FBZZ_LOG_ERROR("ScriptDllLoader::Reload: failed to restore scene after DLL load failure");
        return false;
    }

    // シーンをスナップショットから復元する (新しいファクトリでスクリプトが再生成される)
    if (!SceneIO::Deserialize(scene, snapshot)) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Reload: failed to restore scene");
        return false;
    }

    FBZZ_LOG_INFO("ScriptDllLoader: hot reload complete");
    return true;
}

// =============================================================================
// 内部実装
// =============================================================================

std::filesystem::path ScriptDllLoader::CopyToHot(const std::filesystem::path& src) const
{
    const std::filesystem::path hotDir = src.parent_path() / L"_hot";
    std::error_code ec;
    std::filesystem::create_directories(hotDir, ec);
    if (ec) {
        FBZZ_LOG_ERROR("ScriptDllLoader: failed to create _hot directory: %s", ec.message().c_str());
        return {};
    }

    // タイムスタンプ付きファイル名でコピーする
    const std::wstring stem = src.stem().wstring();
    const std::filesystem::path dst = hotDir / (stem + L"_" + MakeTimestamp() + src.extension().wstring());

    std::filesystem::copy_file(src, dst,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        FBZZ_LOG_ERROR("ScriptDllLoader: failed to copy DLL: %s", ec.message().c_str());
        return {};
    }
    return dst;
}

void ScriptDllLoader::CleanHotDir() const
{
    if (m_dllPath.empty()) return;
    const std::filesystem::path hotDir = m_dllPath.parent_path() / L"_hot";

    std::error_code ec;
    if (!std::filesystem::exists(hotDir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(hotDir, ec)) {
        // 現在ロード中のコピーは削除しない
        if (entry.path() == m_hotCopy) continue;
        std::filesystem::remove(entry.path(), ec);
    }
}

void ScriptDllLoader::RegisterScripts()
{
    if (!m_hDll) return;

    auto registerFn = reinterpret_cast<RegisterFnPtr>(
        GetProcAddress(m_hDll, kRegisterFnName));

    // WHY: FBZZScripts_Register への改名前にビルドされた DLL との後方互換。
    //      旧エクスポート名で見つかった場合は警告してロードを続行し、クラッシュを防ぐ。
    if (!registerFn) {
        registerFn = reinterpret_cast<RegisterFnPtr>(
            GetProcAddress(m_hDll, "SandboxScripts_Register"));
        if (registerFn)
            FBZZ_LOG_WARN("ScriptDllLoader: DLL は旧エクスポート名 'SandboxScripts_Register' を使用しています。Scripts DLL を再ビルドしてください。");
    }

    if (!registerFn) {
        FBZZ_LOG_ERROR("ScriptDllLoader: export %s not found (DLL を再ビルドしてください)", kRegisterFnName);
        return;
    }

    // EXE 側の ScriptFactory::Register を関数ポインタとして渡す。
    // WHY: DLL 内で ScriptFactory::Register() を直接呼ぶと DLL の registry コピーに
    //      登録されてしまう。EXE 側の関数ポインタを渡すことで EXE の registry に登録する。
    registerFn([](const char* typeName,
                  std::function<std::unique_ptr<fbzz::scene::Script>()> factory) {
        scene::ScriptFactory::Register(typeName, std::move(factory));
    });

    FBZZ_LOG_DEBUG("ScriptDllLoader: scripts registered via %s (%d types)",
                  kRegisterFnName,
                  static_cast<int>(scene::ScriptFactory::RegisteredTypeNames().size()));
}

void ScriptDllLoader::DestroyAllScripts(scene::Scene& scene)
{
    for (auto& go : scene.GameObjects()) {
        auto* sc = go.GetComponent<scene::ScriptComponent>();
        if (!sc) continue;
        for (auto& entry : sc->scripts) {
            entry.script.reset();
            entry.m_awoken = false;
            entry.m_started = false;
        }
    }
}

} // namespace fbzz::editor
