/// @file    AssetDirtyRegistry.cpp
/// @brief   未保存アセット中央レジストリの実装。
/// @author  Hasegawa Jin
/// @date    2026-06-13
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <algorithm>

namespace fbzz::editor {

std::vector<DirtyAsset> AssetDirtyRegistry::s_dirty;

void AssetDirtyRegistry::Register(const std::string& absPath,
                                   const std::string& displayPath,
                                   const std::string& typeLabel,
                                   std::function<bool()> saveFunc)
{
    for (auto& entry : s_dirty) {
        if (entry.path == absPath) {
            // saveFunc だけ更新 (最新の in-memory 状態をキャプチャしたクロージャで上書き)
            entry.saveFunc    = std::move(saveFunc);
            entry.displayPath = displayPath;
            return;
        }
    }
    s_dirty.push_back({ absPath, displayPath, typeLabel, std::move(saveFunc) });
}

void AssetDirtyRegistry::MarkClean(const std::string& absPath)
{
    s_dirty.erase(std::remove_if(s_dirty.begin(), s_dirty.end(),
        [&absPath](const DirtyAsset& d) { return d.path == absPath; }),
        s_dirty.end());
}

bool AssetDirtyRegistry::IsDirty(const std::string& absPath)
{
    for (const auto& d : s_dirty)
        if (d.path == absPath) return true;
    return false;
}

bool AssetDirtyRegistry::HasAny()
{
    return !s_dirty.empty();
}

const std::vector<DirtyAsset>& AssetDirtyRegistry::GetAll()
{
    return s_dirty;
}

bool AssetDirtyRegistry::Save(const std::string& absPath)
{
    // saveFunc をコピーしてから呼ぶ。
    // WHY: saveFunc の中で Register が呼ばれると s_dirty が再確保され、
    //      イテレータ (と entry への参照) が実行中に無効化されうる。
    std::function<bool()> saveFunc;
    for (const auto& entry : s_dirty) {
        if (entry.path != absPath) continue;
        saveFunc = entry.saveFunc;
        break;
    }
    if (!saveFunc) return false;
    if (!saveFunc()) return false;
    MarkClean(absPath);
    return true;
}

int AssetDirtyRegistry::SaveAll()
{
    int failed = 0;
    // 保存しながら clean 扱いにする (逆順ループで safe erase)
    std::vector<std::string> saved;
    for (auto& entry : s_dirty) {
        if (entry.saveFunc && entry.saveFunc())
            saved.push_back(entry.path);
        else
            ++failed;
    }
    for (const auto& p : saved)
        MarkClean(p);
    return failed;
}

void AssetDirtyRegistry::DiscardAll()
{
    s_dirty.clear();
}

} // namespace fbzz::editor
