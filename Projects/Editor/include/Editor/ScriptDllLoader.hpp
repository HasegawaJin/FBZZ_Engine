/// @file    ScriptDllLoader.hpp
/// @brief   スクリプト DLL のロード / アンロード / ホットリロード管理。
/// @author  Hasegawa Jin
/// @date    2026-06-03
///
/// @note LoadLibrary は DLL をロックするため、ロード前に _hot/ へタイムスタンプ付きコピーを作り、そのコピーをロードする。
/// @note 仮想デストラクタが DLL 内にあるため FreeLibrary 前に全 Script を破棄する。失敗時は旧 DLL と保存シーンへ復帰する。
/// @see Docs/design/script-dll-recovery.md
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fbzz::scene { class Script; }
struct HINSTANCE__;

namespace fbzz::editor {

class ScriptDllLoader {
public:
    ScriptDllLoader()  = default;
    ~ScriptDllLoader() { Unload(); }

    ScriptDllLoader(const ScriptDllLoader&)            = delete;
    ScriptDllLoader& operator=(const ScriptDllLoader&) = delete;

    /// DLL を _hot/ にコピーしてロードし、ScriptFactory に登録する。
    /// @return ロード成功なら true
    [[nodiscard]] bool Load(const std::filesystem::path& dllPath);

    /// Script インスタンスをすべて破棄してから FreeLibrary する。
    /// @param scene Script を保持するシーン。nullptr なら破棄はスキップ。
    void Unload(scene::Scene* scene = nullptr);

    /// シーン状態を維持したまま DLL を差し替える (ホットリロード本体)。
    /// 手順: Serialize → Script 全破棄 → FreeLibrary → Load → Deserialize
    /// 呼び出し側は Play 中の差し替えを禁止すること。
    /// @return リロード成功なら true
    [[nodiscard]] bool Reload(scene::Scene& scene, const std::filesystem::path& newDllPath);

    [[nodiscard]] bool IsLoaded()  const { return m_hDll != nullptr; }
    [[nodiscard]] const std::filesystem::path& GetDllPath() const { return m_dllPath; }

private:
    /// DLL を _hot/ ディレクトリにタイムスタンプ付きでコピーして返す
    [[nodiscard]] std::filesystem::path CopyToHot(const std::filesystem::path& src) const;

    /// _hot/ 内の古いコピーを削除する (現在ロード中のファイルは除く)
    void CleanHotDir() const;

    /// DLL エクスポート関数を使って ScriptFactory に登録する
    [[nodiscard]] bool RegisterScripts();
    /// Scene / Script の型レイアウトがホスト側と一致するか検証する
    [[nodiscard]] bool ValidateAbi() const;

    [[nodiscard]] bool LoadCopy(const std::filesystem::path& source);

    /// シーン内の全 Script インスタンスを破棄する (FreeLibrary 前に呼ぶ)
    static void DestroyAllScripts(scene::Scene& scene);

    HINSTANCE__*          m_hDll    = nullptr;
    std::filesystem::path m_dllPath;     ///< 元の DLL パス (ビルド出力先)
    std::filesystem::path m_hotCopy;     ///< 現在ロード中のコピーパス
};

} // namespace fbzz::editor
