// FBZZ Engine
// VolumeOverride.cpp | fbzz::asset
// VolumeOverride の型レジストリ実装。
#include <Engine/Asset/VolumeOverride.hpp>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace fbzz::asset {

const char* ToString(VolumeOverrideCategory category)
{
    switch (category) {
    case VolumeOverrideCategory::Exposure:         return "Exposure";
    case VolumeOverrideCategory::AntiAliasing:     return "Anti-Aliasing";
    case VolumeOverrideCategory::AmbientOcclusion: return "Ambient Occlusion";
    case VolumeOverrideCategory::Color:            return "Color";
    case VolumeOverrideCategory::Lens:             return "Lens";
    case VolumeOverrideCategory::Atmosphere:       return "Atmosphere";
    case VolumeOverrideCategory::Shadowing:        return "Shadowing & Reflection";
    case VolumeOverrideCategory::Stylize:          return "Stylize";
    case VolumeOverrideCategory::Custom:           return "Custom";
    }
    return "Other";
}

namespace {

struct RegistryData {
    std::unordered_map<std::string, VolumeOverrideFactory::Factory> factories;
    std::vector<VolumeOverrideFactory::Entry> entries;
    // entries はメニュー描画のたびに並べ直したくないので、登録が増えたときだけ整列する。
    bool sorted = false;
};

RegistryData& Registry()
{
    static RegistryData registry;
    return registry;
}

} // namespace

bool VolumeOverrideFactory::Register(const std::string& typeName, Factory factory)
{
    if (typeName.empty() || !factory) return false;

    auto& registry = Registry();
    // メタ情報 (表示名・カテゴリ) は仮実体を 1 つ作って引く。
    // WHY: Register<T>() のテンプレート側で T::DISPLAY_NAME のような静的定数を
    //      要求すると、派生ごとに定数と仮想関数の二重定義になり食い違いうる。
    //      仮想関数を唯一の情報源にしておけば、そのずれが起きない。
    const std::unique_ptr<VolumeOverride> probe = factory();
    if (!probe) return false;

    registry.factories[typeName] = std::move(factory);
    registry.entries.push_back(Entry{
        typeName, probe->GetDisplayName(), probe->GetCategory(), probe->AllowsMultiple() });
    registry.sorted = false;
    return true;
}

std::unique_ptr<VolumeOverride> VolumeOverrideFactory::Create(const std::string& typeName)
{
    auto& registry = Registry();
    auto it = registry.factories.find(typeName);
    if (it == registry.factories.end()) return nullptr;
    return it->second();
}

const std::vector<VolumeOverrideFactory::Entry>& VolumeOverrideFactory::RegisteredEntries()
{
    auto& registry = Registry();
    if (!registry.sorted) {
        // カテゴリ順 → 表示名順。静的初期化の順序は不定なので、
        // 登録順のままだとビルドのたびにメニューの並びが変わってしまう。
        std::sort(registry.entries.begin(), registry.entries.end(),
            [](const Entry& lhs, const Entry& rhs) {
                if (lhs.category != rhs.category)
                    return static_cast<int>(lhs.category) < static_cast<int>(rhs.category);
                return lhs.displayName < rhs.displayName;
            });
        registry.sorted = true;
    }
    return registry.entries;
}

} // namespace fbzz::asset
