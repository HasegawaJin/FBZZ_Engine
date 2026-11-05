/// @file    AssetDirtyRegistry.hpp
/// @brief   Unreal 方式の未保存アセット中央レジストリ。
/// @author  Hasegawa Jin
/// @date    2026-06-13
/// @note 未保存の値と保存処理を所有し、選択変更後も Save All / 終了時確認へ接続する。
#pragma once
#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

struct DirtyAsset {
    std::string path;                 ///< @note 絶対パス。
    std::string displayPath;
    std::string typeLabel;
    std::function<bool()> saveFunc;
    std::any draft;                   ///< @note 型付き shared_ptr の所有者。保存完了または明示破棄まで保持する。
    std::function<void()> discardFunc;
};

/// @note Editor のメインスレッド専用。保存失敗時は値と callback を保持する。
class AssetDirtyRegistry {
public:
    /// @note 既登録の path は現在の保存処理と所有する値へ更新する。
    static void Register(const std::string& absPath,
                         const std::string& displayPath,
                         const std::string& typeLabel,
                         std::function<bool()> saveFunc, std::any draft = {}, std::function<void()> discardFunc = {});

    /// @note Inspector と保存 callback が同じ値を共有し、選択が移っても別アセットの値で上書きしない。
    template<typename T, typename SaveFunc>
    static void RegisterDraft(const std::string& absPath, const std::string& displayPath,
                              const std::string& typeLabel, std::shared_ptr<T> draft,
                              SaveFunc saveFunc, std::function<void()> discardFunc = {})
    {
        Register(absPath, displayPath, typeLabel, [draft, saveFunc = std::move(saveFunc)] {
            return draft && saveFunc(*draft);
        }, draft, std::move(discardFunc));
    }

    /// @return 未保存の同型の値。保存完了・明示破棄・異なる型なら nullptr。
    template<typename T>
    static std::shared_ptr<T> FindDraft(const std::string& absPath)
    {
        for (const auto& entry : s_dirty) {
            if (entry.path != absPath) continue;
            if (const auto* draft = std::any_cast<std::shared_ptr<T>>(&entry.draft)) return *draft;
        }
        return {};
    }

    /// @note 保存完了または対象の明示破棄時に値と callback を解放する。
    static void MarkClean(const std::string& absPath);

    static bool IsDirty(const std::string& absPath);
    static bool HasAny();
    static const std::vector<DirtyAsset>& GetAll();

    /// @return 未登録または保存失敗なら false。失敗時は未保存のまま。
    static bool Save(const std::string& absPath);

    /// @return 保存に失敗した件数。
    static int SaveAll();

    /// @note 表示中の編集値も DiscardEpoch の変更を見てディスクから読み直す。
    static void DiscardAll();
    static std::uint64_t DiscardEpoch();

private:
    static std::vector<DirtyAsset> s_dirty;
    static std::uint64_t s_discardEpoch;
};

} /// @note namespace fbzz::editor
