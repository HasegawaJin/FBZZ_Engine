/// @file    AssetDirtyRegistry.cpp
/// @brief   未保存アセット中央レジストリの実装。
/// @author  Hasegawa Jin
/// @date    2026-06-13
#include <Editor/Asset/Storage/AssetDirtyRegistry.hpp>
#include <algorithm>

namespace fbzz::editor {

std::vector<DirtyAsset> AssetDirtyRegistry::s_dirty;
std::uint64_t AssetDirtyRegistry::s_discardEpoch = 0;
std::uint64_t AssetDirtyRegistry::s_nextRevision = 0;

void AssetDirtyRegistry::Register(const std::string& absPath,
                                   const std::string& displayPath,
                                   const std::string& typeLabel,
                                   std::function<bool()> saveFunc, std::any draft, std::function<void()> discardFunc)
{
    const auto revision = ++s_nextRevision;
    for (auto& entry : s_dirty) {
        if (entry.path == absPath) {
            // saveFunc だけ更新 (最新の in-memory 状態をキャプチャしたクロージャで上書き)
            entry.saveFunc    = std::move(saveFunc);
            entry.displayPath = displayPath;
            entry.typeLabel = typeLabel;
            entry.draft = std::move(draft);
            entry.discardFunc = std::move(discardFunc);
            entry.revision = revision;
            return;
        }
    }
    s_dirty.push_back({ absPath, displayPath, typeLabel, std::move(saveFunc), std::move(draft),
                        std::move(discardFunc), revision });
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
        if (d.path == absPath) {
            return true;
        }
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
    const std::string path = absPath;
    // saveFunc をコピーしてから呼ぶ: 中で Register が呼ばれると s_dirty が再確保され、イテレータ (と entry への参照) が無効化されうる。
    std::function<bool()> saveFunc;
    std::uint64_t revision = 0;
    for (const auto& entry : s_dirty) {
        if (entry.path != path) {
            continue;
        }
        saveFunc = entry.saveFunc;
        revision = entry.revision;
        break;
    }
    if (!saveFunc) {
        return false;
    }
    if (!saveFunc()) {
        return false;
    }
    // ローカルの保存コールバックが登録を更新・解除しても、新しい版を取り除かない。
    std::erase_if(s_dirty, [&](const DirtyAsset& entry) {
        return entry.path == path && entry.revision == revision;
    });
    return true;
}

int AssetDirtyRegistry::SaveAll()
{
    int failed = 0;
    // 保存側が自分の登録を解除しても、残りのパスを安定してたどる。
    std::vector<std::string> paths;
    paths.reserve(s_dirty.size());
    for (const auto& entry : s_dirty) {
        paths.push_back(entry.path);
    }
    for (const auto& path : paths)
        if (IsDirty(path) && !Save(path)) {
            ++failed;
        }
    return failed;
}

void AssetDirtyRegistry::DiscardAll()
{
    auto discarded = std::move(s_dirty);
    s_dirty.clear();
    ++s_discardEpoch;
    for (auto& entry : discarded)
        if (entry.discardFunc) {
            entry.discardFunc();
        }
}

std::uint64_t AssetDirtyRegistry::DiscardEpoch()
{
    return s_discardEpoch;
}

} // namespace fbzz::editor
