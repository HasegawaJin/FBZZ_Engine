/// @file    BenchApp.cpp
/// @brief   ビジュアル検証ベンチのホスト実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include "BenchApp.hpp"

#include "Scenes/Scenes.hpp"

#include <Engine/Core/Application.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <imgui.h>

#include <filesystem>

namespace fbzz::bench {

namespace {
constexpr float kFixedStep = 1.0f / 120.0f;
/// 1 フレームで進める上限。ブレークポイントで止めた後に一気に飛ぶのを防ぐ。
constexpr int kMaxStepsPerFrame = 16;
constexpr float kFontSize = 17.0f;

/// 日本語グリフを持つフォントを読む。
///
/// WHY 必須か: ImGui のバンドルフォントは ASCII しか持たない。入れないと場面の名前も
///     «見るべきところ» も «???» になり、**そもそも何を見ればいいか分からない画面**に
///     なる。合否を人が決める道具なので、文字が出ないことは機能不全と同じ。
///
/// WHY Editor の EditorTheme を使わないか: ベンチは Editor へ依存させない
///     (Editor は Panel も Command も引き連れてくる)。要るのはフォント 1 枚だけ。
ImFont* LoadJapaneseFont(ImGuiIO& io)
{
    static constexpr const char* kCandidates[] = {
        "C:/Windows/Fonts/YuGothM.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
    };

    // BuildRanges の結果はアトラス生成 (最初の NewFrame) まで生きている必要がある。
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault()); // « » を含む Latin-1
        static const ImWchar kSymbols[] = {
            0x2000, 0x206F, // 約物 (─ に使う 二重ダッシュ・…)
            0x2190, 0x21FF, // 矢印
            0x2500, 0x257F, // 罫線 (─)
            0x25A0, 0x25FF, // 幾何形
            0,
        };
        builder.AddRanges(kSymbols);
        builder.BuildRanges(&ranges);
    }

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;

    for (const char* path : kCandidates) {
        // 存在確認してから渡す。ImGui は読めないファイルで IM_ASSERT する。
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, kFontSize, &cfg, ranges.Data))
            return font;
    }
    return nullptr;
}
} // namespace

bool BenchApp::OnInit()
{
    m_scenes.push_back(MakeXPBDConvergenceScene());
    m_scenes.push_back(MakeXPBDJointChainScene());
    m_scenes.push_back(MakeContactScene());
    m_scenes.push_back(MakeBVHQueryScene());
    m_scenes.push_back(MakeCCDScene());
    m_scenes.push_back(MakeConvexHullScene());
    m_scenes.push_back(MakeRagdollScene());

    m_imguiContext = ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr; // 配置を保存しない。毎回同じ見え方で開く。
    ImGui::StyleColorsDark();

    if (ImFont* font = LoadJapaneseFont(io)) io.FontDefault = font;
    else                                     io.Fonts->AddFontDefault();

    // フォントアトラスはこの中で GPU テクスチャになる。フォントを足すのは必ずこの前。
    core::Application::Get().GetImGuiRenderer().ImGuiInit(
        core::Application::Get().GetWindow().GetHandle());

    SelectScene(0);
    return true;
}

void BenchApp::OnUpdate(float dt)
{
    if (m_scenes.empty()) return;
    BenchScene& scene = *m_scenes[static_cast<size_t>(m_selected)];

    if (m_stepRequested) {
        scene.Simulate(kFixedStep);
        m_stepRequested = false;
        return;
    }
    if (m_paused) return;

    m_accumulator += dt * m_speed;
    int steps = 0;
    while (m_accumulator >= kFixedStep && steps < kMaxStepsPerFrame) {
        scene.Simulate(kFixedStep);
        m_accumulator -= kFixedStep;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame) m_accumulator = 0.0f;
}

void BenchApp::OnLateUpdate(float) {}

void BenchApp::OnRender()
{
    auto& app      = core::Application::Get();
    auto& renderer = app.GetRenderer();

    renderer.BeginFrame();
    if (auto* resources = renderer::ResourceManager::Active())
        renderer.SetRenderTarget({}, *resources);
    renderer.Clear({0.055f, 0.062f, 0.078f, 1.0f});

    app.GetImGuiRenderer().ImGuiNewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    app.GetImGuiRenderer().ImGuiRenderDrawData();

    renderer.EndFrame();
}

void BenchApp::OnShutdown()
{
    // 場面が持つ剛体より先にソルバを畳ませる。破棄順は各場面のデストラクタが持つ。
    m_scenes.clear();

    core::Application::Get().GetImGuiRenderer().ImGuiShutdown();
    if (m_imguiContext) {
        ImGui::DestroyContext(static_cast<ImGuiContext*>(m_imguiContext));
        m_imguiContext = nullptr;
    }
}

void BenchApp::SelectScene(int index)
{
    if (index < 0 || index >= static_cast<int>(m_scenes.size())) return;

    m_selected    = index;
    m_accumulator = 0.0f;

    BenchScene& scene = *m_scenes[static_cast<size_t>(index)];
    scene.Reset();
    scene.ConfigureView(m_view);
}

void BenchApp::DrawUI()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin("##bench", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    DrawSidebar();
    ImGui::SameLine();
    DrawViewportPanel();

    ImGui::End();
}

void BenchApp::DrawSidebar()
{
    ImGui::BeginChild("##sidebar", {430.0f, 0.0f}, ImGuiChildFlags_Borders);

    ImGui::TextUnformatted("ビジュアル検証ベンチ");
    ImGui::TextDisabled("合否はコードではなく目で判断する");

    ImGui::SeparatorText("場面");
    for (int i = 0; i < static_cast<int>(m_scenes.size()); ++i) {
        if (ImGui::Selectable(m_scenes[static_cast<size_t>(i)]->Name(), i == m_selected))
            SelectScene(i);
    }

    ImGui::SeparatorText("再生");
    if (ImGui::Button(m_paused ? "再生" : "一時停止", {110.0f, 0.0f})) m_paused = !m_paused;
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_paused);
    if (ImGui::Button("1 ステップ", {110.0f, 0.0f})) m_stepRequested = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("リセット", {100.0f, 0.0f})) SelectScene(m_selected);
    ImGui::SliderFloat("速度", &m_speed, 0.05f, 3.0f, "x%.2f");

    if (!m_scenes.empty()) {
        BenchScene& scene = *m_scenes[static_cast<size_t>(m_selected)];

        ImGui::SeparatorText("見るべきところ");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(scene.WhatToLookFor());
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText("設定");
        scene.DrawControls();
    }

    ImGui::EndChild();
}

void BenchApp::DrawViewportPanel()
{
    m_view.Begin("##viewport", {0.0f, 0.0f});
    m_view.HandlePanZoom();
    if (!m_scenes.empty()) m_scenes[static_cast<size_t>(m_selected)]->Draw(m_view);
    m_view.End();
}

} // namespace fbzz::bench
