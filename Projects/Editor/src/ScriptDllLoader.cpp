/// @file    ScriptDllLoader.cpp
/// @brief   スクリプト DLL のロード / アンロード / ホットリロード管理。
/// @author  Hasegawa Jin
/// @date    2026-06-03
#include <Editor/ScriptDllLoader.hpp>
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
constexpr const char* kAbiInfoFnName  = "FBZZScripts_GetAbiInfo";
using AbiInfoFnPtr = scene::ScriptDllAbiInfo(*)();

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

    if (!util::FileSystem::Exists(dllPath)) {
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

    if (!ValidateAbi()) {
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
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
    scene::ScriptSerializableFactory::UnregisterAll();
    FBZZ_LOG_DEBUG("ScriptDllLoader: ScriptFactory unregistered all");

    // イベント購読とオブジェクトプールを破棄する。
    // WHY: 購読ハンドラのラムダ本体は DLL 側のコードにあるため、FreeLibrary 後に
    //      呼ばれるとアクセス違反になる。DestroyAllScripts が通れば ~Script 経由で
    //      解除されるはずだが、scene が渡されない経路もあるためここでも必ず空にする。
    //      プールの待機列も破棄済み GameObject の EntityID を抱えたままにしない。
    scene::ScriptEventBus::Clear();
    scene::PrefabPool::ClearAll();
    FBZZ_LOG_DEBUG("ScriptDllLoader: script event bus & prefab pool cleared");

    // DataAsset も DLL コード内に仮想デストラクタ/ファクトリを持つため、FreeLibrary 前に
    // 共有キャッシュを破棄し DLL 由来の型登録を外す。次回 Resolve でディスクから遅延再ロードされる。
    //
    // WHY 全消しにしないか: Engine 組み込みの型 (PostProcessProfile 等) は Engine の
    //     静的初期化でしか登録されず、DLL を読み直しても再登録されない。以前は
    //     一緒に消していたため、スクリプトを 1 回ホットリロードすると .fzdata が
    //     「型が未登録」で読めなくなり、エディターを再起動するまで直らなかった。
    asset::DataAssetRegistry::ClearCache();
    asset::DataAssetFactory::UnregisterScriptTypes();
    FBZZ_LOG_DEBUG("ScriptDllLoader: DataAsset cache cleared & factory unregistered");

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
    if (!util::FileSystem::EnsureDirectory(hotDir)) {
        FBZZ_LOG_ERROR("ScriptDllLoader: failed to create _hot directory: %s",
                       util::FileSystem::PathToUtf8(hotDir).c_str());
        return {};
    }

    // タイムスタンプ付きファイル名でコピーする
    const std::wstring stem = src.stem().wstring();
    const std::filesystem::path dst = hotDir / (stem + L"_" + MakeTimestamp() + src.extension().wstring());

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
        // 現在ロード中のコピーは削除しない
        if (path == m_hotCopy) continue;
        util::FileSystem::RemoveAll(path);
    }
}

void ScriptDllLoader::RegisterScripts()
{
    if (!m_hDll) return;

    auto registerFn = reinterpret_cast<RegisterFnPtr>(
        GetProcAddress(m_hDll, kRegisterFnName));

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
    const scene::ScriptDllAbiInfo dll  = infoFn();

    if (host.signature == dll.signature) return true;

    // 現行 schema 同士の差だけを診断する。自動再ビルド対象なので ERROR ではなく WARNING とする。
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
