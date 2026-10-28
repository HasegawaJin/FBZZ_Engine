/// @file    SequenceCurveTests.cpp
/// @brief   Timeline float curves preserve runtime sampling, persistence, and undoable UI edits.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/SequencePanel.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include "../../../Editor/src/Panels/SequenceCurveEditing.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace fbzz::tests {
namespace {

asset::SequenceAsset MakeCurve(asset::AnimInterp interp = asset::AnimInterp::Cubic)
{
    asset::SequenceAsset sequence;
    sequence.duration = 2.0;
    asset::SequenceTrack track;
    track.type = asset::SequenceTrackType::Property;
    track.name = "Intensity";
    track.binding = "$self";
    track.property.valueType = asset::AnimValueType::Float;
    track.property.interp = interp;
    track.property.floatKeys = { { 0.0, 0.0f, 0.0f, 4.0f }, { 2.0, 2.0f, 0.0f, 0.0f } };
    sequence.tracks.push_back(std::move(track));
    return sequence;
}

} /// @note namespace

class SequenceCurveTest : public testkit::EditorFixture {};

TEST_F(SequenceCurveTest, CubicTangentsAreDerivativesPerSecondAndEndpointsClamp)
{
    const auto sequence = MakeCurve();
    const auto& keys = sequence.tracks[0].property.floatKeys;

    EXPECT_NEAR(asset::SampleFloatKeys(keys, 0.5, asset::AnimInterp::Cubic), 1.4375f, 1.0e-6f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, -1.0, asset::AnimInterp::Cubic), 0.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 2.0, asset::AnimInterp::Cubic), 2.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 3.0, asset::AnimInterp::Cubic), 2.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 0.5, asset::AnimInterp::Linear), 0.5f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 0.5, asset::AnimInterp::Step), 0.0f);
}

TEST_F(SequenceCurveTest, ReversedScrubbingHasNoFrameHistoryAndNaNQueryIsSafe)
{
    const auto keys = MakeCurve().tracks[0].property.floatKeys;
    const float forward = asset::SampleFloatKeys(keys, 0.5, asset::AnimInterp::Cubic);

    (void)asset::SampleFloatKeys(keys, 1.5, asset::AnimInterp::Cubic);

    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 0.5, asset::AnimInterp::Cubic), forward);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, std::numeric_limits<double>::quiet_NaN(),
                                         asset::AnimInterp::Cubic), 0.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys({}, 1.0, asset::AnimInterp::Cubic), 0.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, std::numeric_limits<double>::infinity(),
                                         asset::AnimInterp::Cubic), 2.0f);
}

TEST_F(SequenceCurveTest, DuplicateTimesKeepLegacyEndpointAndInteriorSamplingRules)
{
    const std::vector<asset::FloatKey> keys = {
        { 0.0, 1.0f }, { 0.0, 2.0f }, { 1.0, 3.0f }, { 1.0, 4.0f }, { 2.0, 8.0f }
    };

    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 0.0, asset::AnimInterp::Linear), 1.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 1.0, asset::AnimInterp::Step), 4.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 1.5, asset::AnimInterp::Linear), 6.0f);
}

TEST_F(SequenceCurveTest, MovingAcrossKeysAndEqualTimesKeepsTheSameKeySelected)
{
    auto keys = MakeCurve().tracks[0].property.floatKeys;
    int selected = editor::detail::AddFloatCurveKey(keys, 1.0, 7.0f);
    keys[static_cast<std::size_t>(selected)].outTangent = 13.0f;

    ASSERT_TRUE(editor::detail::MoveFloatCurveKey(keys, selected, 2.0, 9.0f));

    ASSERT_EQ(selected, 1);
    EXPECT_FLOAT_EQ(keys[static_cast<std::size_t>(selected)].outTangent, 13.0f);
    EXPECT_FLOAT_EQ(keys[2].value, 2.0f);
    EXPECT_FALSE(editor::detail::MoveFloatCurveKey(keys, selected,
        std::numeric_limits<double>::quiet_NaN(), 4.0f));
    EXPECT_EQ(selected, 1);
    EXPECT_FLOAT_EQ(keys[1].value, 9.0f);
}

TEST_F(SequenceCurveTest, LinearTangentPresetMatchesALinearRampWithNonUnitDuration)
{
    auto keys = MakeCurve().tracks[0].property.floatKeys;

    editor::detail::SetFloatCurveLinearTangents(keys, 0);
    editor::detail::SetFloatCurveLinearTangents(keys, 1);

    EXPECT_FLOAT_EQ(keys[0].outTangent, 1.0f);
    EXPECT_FLOAT_EQ(keys[1].inTangent, 1.0f);
    EXPECT_NEAR(asset::SampleFloatKeys(keys, 0.5, asset::AnimInterp::Cubic), 0.5f, 1.0e-6f);
}

TEST_F(SequenceCurveTest, SaveLoadRetainsTangentsAndAllLegacyInterpolationModes)
{
    const auto path = File("curve.sequence").generic_string();
    for (const auto interp : { asset::AnimInterp::Step, asset::AnimInterp::Linear, asset::AnimInterp::Cubic }) {
        const auto source = MakeCurve(interp);
        ASSERT_TRUE(asset::SaveSequenceAsset(path, source));
        asset::SequenceAsset loaded;

        ASSERT_TRUE(asset::LoadSequenceAsset(path, loaded));

        ASSERT_EQ(loaded.tracks.size(), 1u);
        const auto& track = loaded.tracks[0].property;
        ASSERT_EQ(track.floatKeys.size(), 2u);
        EXPECT_EQ(track.interp, interp);
        EXPECT_FLOAT_EQ(track.floatKeys[0].outTangent, 4.0f);
        EXPECT_FLOAT_EQ(asset::SampleFloatKeys(track.floatKeys, 0.5, interp),
            asset::SampleFloatKeys(source.tracks[0].property.floatKeys, 0.5, interp));
    }
}

TEST_F(SequenceCurveTest, LegacyMissingTangentsLoadFlatAndInvalidKeysNeverReachSampling)
{
    const auto path = File("legacy.sequence").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(path, R"(version = 1
[sequence]
duration = 2.0
[[tracks]]
type = "Property"
valueType = "Float"
interp = "Cubic"
floatKeys = [{time=2.0,value=2.0},{time=nan,value=4.0},{time=0.0,value=0.0},{time=1.0,value=inf}]
)"));
    asset::SequenceAsset loaded;

    ASSERT_TRUE(asset::LoadSequenceAsset(path, loaded));

    ASSERT_EQ(loaded.tracks[0].property.floatKeys.size(), 2u);
    const auto& keys = loaded.tracks[0].property.floatKeys;
    EXPECT_DOUBLE_EQ(keys[0].time, 0.0);
    EXPECT_FLOAT_EQ(keys[0].outTangent, 0.0f);
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(keys, 1.0, asset::AnimInterp::Cubic), 1.0f);
}

TEST_F(SequenceCurveTest, InvalidSavePreservesTheLastGoodFile)
{
    const auto path = File("safe.sequence").generic_string();
    auto source = MakeCurve();
    ASSERT_TRUE(asset::SaveSequenceAsset(path, source));
    std::string before;
    ASSERT_TRUE(util::FileSystem::ReadText(path, before));
    source.tracks[0].property.floatKeys[0].outTangent = std::numeric_limits<float>::infinity();

    EXPECT_FALSE(asset::SaveSequenceAsset(path, source));

    std::string after;
    ASSERT_TRUE(util::FileSystem::ReadText(path, after));
    EXPECT_EQ(before, after);
}

/// @note CPU-only ImGui frames invoke public OnRender and observe saved assets; no OS windows or GPU.
class SequenceCurveUiTest : public testkit::EditorFixture {
protected:
    editor::SequencePanel m_panel;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;

    void SetUp() override
    {
        EditorFixture::SetUp();
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1400, 1100 };
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Context().projectRoot = ProjectRoot().generic_string();
    }

    void TearDown() override
    {
        editor::AssetDirtyRegistry::DiscardAll();
        m_panel.OnShutdown();
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }

    ImGuiWindow* PanelWindow() const { return ImGui::FindWindowByID(ImHashStr("###Sequence")); }

    ImGuiWindow* Child(const char* label) const
    {
        auto* parent = PanelWindow();
        if (!parent) return nullptr;
        for (auto* window : parent->DC.ChildWindows)
            if (std::strstr(window->Name, label)) return window;
        return nullptr;
    }

    void Frame(const char* button = nullptr, const char* child = nullptr)
    {
        ImGui::NewFrame();
        if (button) {
            auto* window = child ? Child(child) : PanelWindow();
            ASSERT_NE(window, nullptr);
            const auto id = ImHashStr(button, 0, window->ID);
            GImGui->NavActivateId = id;
            GImGui->NavActivateDownId = id;
            GImGui->NavActivatePressedId = id;
            GImGui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1380, 1080 });
        m_panel.OnRender(Context());
        ImGui::Render();
    }

    void SelectTrack()
    {
        auto* timeline = Child("##SequenceTimeline");
        ASSERT_NE(timeline, nullptr);
        const auto origin = timeline->DC.CursorStartPos;
        ImGui::GetIO().AddMousePosEvent(origin.x + 20, origin.y + 31);
        Frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Frame();
    }

    asset::SequenceAsset Saved()
    {
        Frame("Save");
        asset::SequenceAsset result;
        EXPECT_TRUE(asset::LoadSequenceAsset(File("ui.sequence").generic_string(), result));
        return result;
    }
};

TEST_F(SequenceCurveUiTest, AddDeleteUndoRedoAndReloadUseTheActualPanelAndSavedRuntimeCurve)
{
    const auto path = File("ui.sequence").generic_string();
    ASSERT_TRUE(asset::SaveSequenceAsset(path, MakeCurve()));
    m_panel.RequestOpen(path);
    Frame();
    Frame();
    SelectTrack();

    Frame("Add at playhead", "##SequenceInspector");
    ASSERT_EQ(Saved().tracks[0].property.floatKeys.size(), 3u);
    Frame("Undo");
    ASSERT_EQ(Saved().tracks[0].property.floatKeys.size(), 2u);
    Frame("Redo");
    ASSERT_EQ(Saved().tracks[0].property.floatKeys.size(), 3u);
    SelectTrack();
    Frame("Add at playhead", "##SequenceInspector");
    Frame("Delete key", "##SequenceInspector");
    ASSERT_EQ(Saved().tracks[0].property.floatKeys.size(), 3u);
    Frame("Undo");
    ASSERT_EQ(Saved().tracks[0].property.floatKeys.size(), 4u);
    Frame("Redo");
    const auto final = Saved();
    ASSERT_EQ(final.tracks[0].property.floatKeys.size(), 3u);
    Frame("Reload");
    const auto reloaded = Saved();
    EXPECT_FLOAT_EQ(asset::SampleFloatKeys(final.tracks[0].property.floatKeys, 0.5, asset::AnimInterp::Cubic),
        asset::SampleFloatKeys(reloaded.tracks[0].property.floatKeys, 0.5, asset::AnimInterp::Cubic));
}

} /// @note namespace fbzz::tests
