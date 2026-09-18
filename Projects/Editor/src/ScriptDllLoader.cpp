/// @file    ScriptDllLoader.cpp
/// @brief   スクリプト DLL のロード / アンロード / ホットリロード管理。
/// @author  Hasegawa Jin
/// @date    2026-06-03
#include <Editor/ScriptDllLoader.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/PrefabPool.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace fbzz::editor {

namespace {

/// SandboxScripts.dll のエクスポート関数シグネチャ
using RegisterFnPtr = void(*)(
    void(*)(const char*, std::function<std::unique_ptr<fbzz::scene::Script>()>)
);

/// @note DLL エクスポート名 (全プロジェクト共通)。プロジェクト固有名 (SandboxScripts_Register 等)
///       を使うと ScriptDllLoader がプロジェクトごとに変わるため、FBZZScripts_Register を
///       エンジン規約として統一し、任意のプロジェクトの DLL をロードできるようにする。
constexpr const char* kRegisterFnName = "FBZZScripts_Register";
constexpr const char* kAbiInfoFnName  = "FBZZScripts_GetAbiInfo";
using AbiInfoFnPtr = scene::ScriptDllAbiInfo(*)();

/// タイムスタンプ文字列を生成する (コピー先ファイル名の一部に使う)
std::wstring MakeTimestamp()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_wstring(ms);
}

/// 実行中の FBZZEngine.dll の最終更新時刻。取れなければ nullopt。
std::optional<std::filesystem::file_time_type> EngineModuleWriteTime()
{
    HMODULE engine = GetModuleHandleW(L"FBZZEngine.dll");
    if (engine == nullptr) return std::nullopt;

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(engine, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::nullopt;

    std::error_code ec;
    const auto time = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) return std::nullopt;
    return time;
}

/// @brief SEH は C++ のデストラクタを持つ自動変数と同居できないので、素の関数へ切り出す。
/// @note DllMain の途中で受け止めた場合、その DLL は «半分だけ初期化された» 状態で残る。
///       だから受けたら必ず読み込みを失敗として扱い、二度と触らない (再ビルドへ回す)。
HMODULE LoadLibraryGuarded(const wchar_t* path, DWORD& outExceptionCode)
{
    __try {
        return LoadLibraryW(path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outExceptionCode = GetExceptionCode();
        return nullptr;
    }
}

bool GetAbiInfoGuarded(AbiInfoFnPtr fn, scene::ScriptDllAbiInfo& out, DWORD& outExceptionCode)
{
    __try {
        out = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outExceptionCode = GetExceptionCode();
        return false;
    }
}

using RegisterCallback = void(*)(const char*, std::function<std::unique_ptr<scene::Script>()>);

bool RegisterGuarded(RegisterFnPtr fn, RegisterCallback callback, DWORD& outExceptionCode)
{
    __try {
        fn(callback);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outExceptionCode = GetExceptionCode();
        return false;
    }
}

} // namespace

/// 公開 API

bool ScriptDllLoader::Load(const std::filesystem::path& dllPath)
{
    if (m_hDll) {
        FBZZ_LOG_WARN("ScriptDllLoader::Load: DLL is already loaded");
        return false;
    }

    m_dllPath = dllPath;

    return LoadCopy(dllPath);
}

bool ScriptDllLoader::LoadCopy(const std::filesystem::path& dllPath)
{
    if (!util::FileSystem::Exists(dllPath)) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Load: DLL not found: %ls",
                       dllPath.wstring().c_str());
        return false;
    }

    /// @note リンク順序だけでも日時は前後するため、互換性の判定は ABI 検査で行う。
    if (const auto engineTime = EngineModuleWriteTime()) {
        std::error_code ec;
        const auto dllTime = std::filesystem::last_write_time(dllPath, ec);
        if (!ec && dllTime < *engineTime) {
            FBZZ_LOG_WARN("ScriptDllLoader::Load: Scripts DLL は FBZZEngine.dll より古いため "
                          "ABI を検査します: %ls",
                          dllPath.wstring().c_str());
        }
    }

    /// @note _hot/ にコピーしてからロードする (元ファイルを再ビルドできるようにするため)
    m_hotCopy = CopyToHot(dllPath);
    if (m_hotCopy.empty()) return false;

    FBZZ_LOG_DEBUG("ScriptDllLoader: loading hot copy: %ls", m_hotCopy.wstring().c_str());
    DWORD exceptionCode = 0;
    m_hDll = LoadLibraryGuarded(m_hotCopy.wstring().c_str(), exceptionCode);
    if (!m_hDll) {
        if (exceptionCode != 0) {
            FBZZ_LOG_ERROR("ScriptDllLoader::Load: DLL の初期化中に例外 (0x%08lX): %ls "
                           "─ 古い DLL の可能性が高いので再ビルドへ回します",
                           exceptionCode, m_hotCopy.wstring().c_str());
        } else {
            FBZZ_LOG_ERROR("ScriptDllLoader::Load: LoadLibrary failed: %ls (GLE=%lu)",
                           m_hotCopy.wstring().c_str(), GetLastError());
        }
        m_hotCopy.clear();
        return false;
    }

    if (!ValidateAbi()) {
        Unload();
        m_hotCopy.clear();
        return false;
    }

    if (!RegisterScripts()) {
        Unload();
        m_hotCopy.clear();
        return false;
    }
    FBZZ_LOG_INFO("ScriptDllLoader: DLL loaded -> %ls", m_hotCopy.wstring().c_str());
    return true;
}

void ScriptDllLoader::Unload(scene::Scene* scene)
{
    if (!m_hDll) return;

    /// @note FreeLibrary 前に仮想デストラクタが DLL コード内にある Script インスタンスをすべて破棄する。
    ///       FreeLibrary 後に仮想デストラクタを呼ぶとアクセス違反になるため。
    if (scene) {
        FBZZ_LOG_DEBUG("ScriptDllLoader: destroying all script instances before unload");
        DestroyAllScripts(*scene);
    }

    scene::ScriptFactory::UnregisterAll();
    scene::ScriptSerializableFactory::UnregisterAll();
    FBZZ_LOG_DEBUG("ScriptDllLoader: ScriptFactory unregistered all");

    /// @note イベント購読とオブジェクトプールを破棄する。購読ハンドラのラムダ本体は DLL 側の
    ///       コードにあるため FreeLibrary 後に呼ぶとアクセス違反になり、scene が渡されない経路も
    ///       あるため DestroyAllScripts に頼らずここでも必ず空にする (待機列の EntityID も含め)。
    scene::ScriptEventBus::Clear();
    scene::PrefabPool::ClearAll();
    FBZZ_LOG_DEBUG("ScriptDllLoader: script event bus & prefab pool cleared");

    /// @note DataAsset も DLL コード内に仮想デストラクタ/ファクトリを持つため、FreeLibrary 前に
    ///       共有キャッシュを破棄し DLL 由来の型登録を外す。次回 Resolve でディスクから遅延再ロードされる。
    /// @note 全消しにしない: Engine 組み込みの型 (PostProcessProfile 等) は Engine の静的初期化
    ///       でしか登録されず、DLL を読み直しても再登録されない。以前は一緒に消しており、
    ///       スクリプトを 1 回ホットリロードすると .fzdata が「型が未登録」で読めなくなっていた。
    asset::DataAssetRegistry::ClearCache();
    asset::DataAssetFactory::UnregisterScriptTypes();
    FBZZ_LOG_DEBUG("ScriptDllLoader: DataAsset cache cleared & factory unregistered");

    FreeLibrary(m_hDll);
    m_hDll = nullptr;

    FBZZ_LOG_INFO("ScriptDllLoader: DLL unloaded");
}

bool ScriptDllLoader::Reload(scene::Scene& scene, const std::filesystem::path& newDllPath)
{
    /// @note シーン全体をシリアライズしてスクリプトフィールドを保存する
    const std::string snapshot = SceneIO::Serialize(scene);
    if (snapshot.empty()) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Reload: failed to serialize scene");
        return false;
    }

    const auto previousCopy = m_hDll ? m_hotCopy : std::filesystem::path{};
    const auto previousPath = m_dllPath;
    const auto targetPath = newDllPath.empty() ? m_dllPath : newDllPath;
    if (!util::FileSystem::Exists(targetPath)) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Reload: replacement DLL is missing; keeping current scripts");
        return false;
    }

    /// @note 静的 DataAsset 登録が同じレジストリを書き換えるため、新旧 DLL は同時ロードしない。
    Unload(&scene);

    /// @note 新しい DLL をロードして ScriptFactory に再登録する
    if (!Load(targetPath) || !SceneIO::Deserialize(scene, snapshot)) {
        FBZZ_LOG_ERROR("ScriptDllLoader::Reload: replacement failed; restoring previous DLL");
        Unload(&scene);
        m_dllPath = previousPath.empty() ? targetPath : previousPath;
        if (!previousCopy.empty() && !LoadCopy(previousCopy))
            FBZZ_LOG_ERROR("ScriptDllLoader::Reload: previous DLL could not be loaded");
        if (!SceneIO::Deserialize(scene, snapshot))
            FBZZ_LOG_ERROR("ScriptDllLoader::Reload: previous scene could not be restored");
        else if (m_hDll)
            FBZZ_LOG_WARN("ScriptDllLoader::Reload: previous scripts and saved scene restored");
        return false;
    }

    CleanHotDir();
    FBZZ_LOG_INFO("ScriptDllLoader: hot reload complete");
    return true;
}

/// 内部実装

std::filesystem::path ScriptDllLoader::CopyToHot(const std::filesystem::path& src) const
{
    const std::filesystem::path hotDir = m_dllPath.parent_path() / L"_hot";
    if (!util::FileSystem::EnsureDirectory(hotDir)) {
        FBZZ_LOG_ERROR("ScriptDllLoader: failed to create _hot directory: %s",
                       util::FileSystem::PathToUtf8(hotDir).c_str());
        return {};
    }

    /// @note タイムスタンプ付きファイル名でコピーする
    const std::wstring stem = m_dllPath.stem().wstring() + L"_" + MakeTimestamp();
    std::filesystem::path dst = hotDir / (stem + src.extension().wstring());
    for (unsigned int suffix = 1; util::FileSystem::Exists(dst); ++suffix)
        dst = hotDir / (stem + L"_" + std::to_wstring(suffix) + src.extension().wstring());

    if (!util::FileSystem::CopyFile(src, dst)) {
        FBZZ_LOG_ERROR("ScriptDllLoader: failed to copy DLL: %s",
                       util::FileSystem::PathToUtf8(src).c_str());
        return {};
    }
    return dst;
}

void ScriptDllLoader::CleanHotDir() const
{
    if (m_dllPath.empty()) return;
    const std::filesystem::path hotDir = m_dllPath.parent_path() / L"_hot";

    if (!util::FileSystem::Exists(hotDir)) return;

    for (const auto& path : util::FileSystem::ListFiles(hotDir)) {
        /// @note 現在ロード中のコピーは削除しない
        if (path == m_hotCopy) continue;
        util::FileSystem::RemoveAll(path);
    }
}

bool ScriptDllLoader::RegisterScripts()
{
    if (!m_hDll) return false;

    auto registerFn = reinterpret_cast<RegisterFnPtr>(
        GetProcAddress(m_hDll, kRegisterFnName));

    if (!registerFn) {
        FBZZ_LOG_ERROR("ScriptDllLoader: export %s not found (DLL を再ビルドしてください)", kRegisterFnName);
        return false;
    }

    /// @note EXE 側の ScriptFactory::Register を関数ポインタとして渡す。DLL 内で直接呼ぶと
    ///       DLL の registry コピーに登録されてしまうため、EXE の関数ポインタ経由で EXE の
    ///       registry に登録する。
    DWORD exceptionCode = 0;
    const bool registered = RegisterGuarded(
        registerFn,
        [](const char* typeName,
           std::function<std::unique_ptr<fbzz::scene::Script>()> factory) {
            scene::ScriptFactory::Register(typeName, std::move(factory));
        },
        exceptionCode);
    if (!registered) {
        FBZZ_LOG_ERROR("ScriptDllLoader: %s の実行中に例外 (0x%08lX)。"
                       "古い DLL の可能性が高いので再ビルドしてください",
                       kRegisterFnName, exceptionCode);
        return false;
    }

    FBZZ_LOG_DEBUG("ScriptDllLoader: scripts registered via %s (%d types)",
                  kRegisterFnName,
                  static_cast<int>(scene::ScriptFactory::RegisteredTypeNames().size()));
    return true;
}

bool ScriptDllLoader::ValidateAbi() const
{
    if (!m_hDll) return false;

    const auto infoFn = reinterpret_cast<AbiInfoFnPtr>(
        GetProcAddress(m_hDll, kAbiInfoFnName));
    if (!infoFn) {
        FBZZ_LOG_INFO(
            "ScriptDllLoader: stale Scripts DLL has no %s export; rebuild required",
            kAbiInfoFnName);
        return false;
    }

    const scene::ScriptDllAbiInfo host = scene::GetScriptDllAbiInfo();
    scene::ScriptDllAbiInfo dll{};
    DWORD exceptionCode = 0;
    if (!GetAbiInfoGuarded(infoFn, dll, exceptionCode)) {
        FBZZ_LOG_WARN("ScriptDllLoader: %s の呼び出しで例外 (0x%08lX)。stale DLL として扱います",
                      kAbiInfoFnName, exceptionCode);
        return false;
    }

    if (host.signature == dll.signature) return true;

    /// @note 現行 schema 同士の差だけを診断する。自動再ビルド対象なので ERROR ではなく WARNING とする。
    FBZZ_LOG_WARN("ScriptDllLoader: ABI mismatch; Scripts DLL rebuild required");
    if (host.sizeofScript != dll.sizeofScript)
        FBZZ_LOG_WARN("  sizeof(Script):          host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.sizeofScript),
            static_cast<unsigned long long>(dll.sizeofScript));
    if (host.sizeofScene != dll.sizeofScene)
        FBZZ_LOG_WARN("  sizeof(Scene):           host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.sizeofScene),
            static_cast<unsigned long long>(dll.sizeofScene));
    if (host.sizeofScriptComponent != dll.sizeofScriptComponent)
        FBZZ_LOG_WARN("  sizeof(ScriptComponent): host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.sizeofScriptComponent),
            static_cast<unsigned long long>(dll.sizeofScriptComponent));
    if (host.componentCount != dll.componentCount)
        FBZZ_LOG_WARN("  ComponentList count:     host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.componentCount),
            static_cast<unsigned long long>(dll.componentCount));
    if (host.componentLayoutHash != dll.componentLayoutHash)
        FBZZ_LOG_WARN("  ComponentList layout:    host=%llu  dll=%llu"
                      "  (コンポーネントのフィールド追加/並べ替え)",
            static_cast<unsigned long long>(host.componentLayoutHash),
            static_cast<unsigned long long>(dll.componentLayoutHash));
    if (host.msvcVersion != dll.msvcVersion)
        FBZZ_LOG_WARN("  _MSC_VER:                host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.msvcVersion),
            static_cast<unsigned long long>(dll.msvcVersion));
    if (host.msvcFullVersion != dll.msvcFullVersion)
        FBZZ_LOG_WARN("  _MSC_FULL_VER:           host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.msvcFullVersion),
            static_cast<unsigned long long>(dll.msvcFullVersion));
    if (host.iteratorDebugLevel != dll.iteratorDebugLevel)
        FBZZ_LOG_WARN("  _ITERATOR_DEBUG_LEVEL:   host=%llu  dll=%llu  (Debug/Release 設定の不一致)",
            static_cast<unsigned long long>(host.iteratorDebugLevel),
            static_cast<unsigned long long>(dll.iteratorDebugLevel));
    if (host.engineVersion != dll.engineVersion)
        FBZZ_LOG_WARN("  Engine version:          host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.engineVersion),
            static_cast<unsigned long long>(dll.engineVersion));
    if (host.buildConfiguration != dll.buildConfiguration)
        FBZZ_LOG_WARN("  Build configuration:     host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.buildConfiguration),
            static_cast<unsigned long long>(dll.buildConfiguration));
    if (host.pointerSize != dll.pointerSize)
        FBZZ_LOG_WARN("  Pointer size:            host=%llu  dll=%llu",
            static_cast<unsigned long long>(host.pointerSize),
            static_cast<unsigned long long>(dll.pointerSize));
    if (host.dynamicRuntime != dll.dynamicRuntime)
        FBZZ_LOG_WARN("  Runtime library:         host=%llu  dll=%llu (/MD or /MDd required)",
            static_cast<unsigned long long>(host.dynamicRuntime),
            static_cast<unsigned long long>(dll.dynamicRuntime));
    return false;
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
