/// @file    EditorMenuCatalogTests.cpp
/// @brief   View 分類で隠したパネルや追加の Asset Browser の到達性を失わない契約。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <TestKit/TestKit.hpp>
#include <Editor/Util/EditorMenuCatalog.hpp>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class MenuTestPanel final : public editor::IPanel {
public:
    MenuTestPanel(std::string name, bool listed = true, const char* category = nullptr)
        : m_name(std::move(name)), m_listed(listed), m_category(category) {}
    const char* GetWindowName() const override { return m_name.c_str(); }
    bool ShowInViewMenu() const override { return m_listed; }
    const char* GetMenuCategory() const override { return m_category; }
protected:
    void OnRenderContent(editor::EditorContext&) override {}
private:
    std::string m_name;
    bool m_listed;
    const char* m_category;
};

} /// @note namespace

class EditorMenuCatalogTest : public testkit::Fixture {};

TEST_F(EditorMenuCatalogTest, ListsEveryEligibleInstanceExactlyOnceRegardlessOfVisibilityOrRegistrationOrder)
{
    std::vector<std::unique_ptr<editor::IPanel>> panels;
    for (const char* name : {"Analysis", "Asset Browser 2", "Scene", "Animation Graph", "Console",
                             "Asset Browser", "Future Editor", "Asset Browser 3", "Inspector"})
        panels.push_back(std::make_unique<MenuTestPanel>(name));
    panels[1]->visible = false;
    panels[3]->visible = false;
    panels.push_back(std::make_unique<MenuTestPanel>("Build Settings", false));

    const auto groups = editor::BuildPanelMenu(panels);
    std::set<editor::IPanel*> seen;
    for (const auto& group : groups) {
        EXPECT_FALSE(group.entries.empty());
        for (const auto& entry : group.entries) {
            EXPECT_TRUE(seen.insert(entry.panel).second);
        }
    }
    ASSERT_EQ(seen.size(), panels.size() - 1);
    for (std::size_t i = 0; i + 1 < panels.size(); ++i) EXPECT_TRUE(seen.contains(panels[i].get()));
    EXPECT_FALSE(seen.contains(panels.back().get()));
    EXPECT_FALSE(panels[1]->visible);
    EXPECT_FALSE(panels[3]->visible);
    EXPECT_STREQ(panels[0]->GetWindowName(), "Analysis");
}

TEST_F(EditorMenuCatalogTest, PlacesCommonPanelsAtTheRootAndKeepsAdditionalBrowsersDistinct)
{
    std::vector<std::unique_ptr<editor::IPanel>> panels;
    for (const char* name : {"Asset Browser 2", "Console", "Inspector", "Game", "Scene Hierarchy",
                             "Scene", "Asset Browser", "UI"})
        panels.push_back(std::make_unique<MenuTestPanel>(name));
    const auto groups = editor::BuildPanelMenu(panels);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_TRUE(groups.front().category.empty());
    const char* expected[] = {"Scene", "Game", "UI", "Scene Hierarchy", "Inspector", "Asset Browser", "Console"};
    ASSERT_EQ(groups.front().entries.size(), std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i)
        EXPECT_STREQ(groups.front().entries[i].panel->GetWindowName(), expected[i]);
    EXPECT_EQ(groups.back().category, "Assets");
    ASSERT_EQ(groups.back().entries.size(), 1u);
    EXPECT_STREQ(groups.back().entries.front().panel->GetWindowName(), "Asset Browser 2");
}

TEST_F(EditorMenuCatalogTest, PreservesDeclaredCategoriesForNewPanelsAndOmitsEmptyGroups)
{
    std::vector<std::unique_ptr<editor::IPanel>> panels;
    EXPECT_TRUE(editor::BuildPanelMenu(panels).empty());
    panels.push_back(std::make_unique<MenuTestPanel>("Future Tool", true, "Custom Tools"));
    panels.push_back(std::make_unique<MenuTestPanel>("Uncategorized Tool"));
    const auto groups = editor::BuildPanelMenu(panels);
    ASSERT_EQ(groups.size(), 2u);
    std::set<std::string> categories;
    for (const auto& group : groups) categories.insert(group.category);
    EXPECT_TRUE(categories.contains("Custom Tools"));
    EXPECT_TRUE(categories.contains("Other Panels"));
}

} /// @note namespace fbzz::tests
