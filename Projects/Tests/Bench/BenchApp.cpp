/// @file    BenchApp.cpp
/// @brief   ビジュアル検証ベンチのホスト実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include "BenchApp.hpp"

#include "Scenes/Scenes.hpp"

#include <Engine/Core/Application.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

// imgui_impl_win32.h では #if 0 で隠されているため手動で前方宣言する
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace fbzz::bench {

namespace {
/// WHY 1/120 ではないか: 場面側 (XPBD) が自前で substep を刻むので、外側まで倍の頻度で
///     回す必要が無い。倍の刻みは倍の負荷になり、重い場面 (ラグドール) では
///     «1 フレームぶんの実時間を、1 フレームでは進めきれない» 側へ倒れる。
constexpr float kFixedStep = 1.0f / 60.0f;
/// 1 フレームで進める上限。
///
/// WHY 小さいか: ここが大きいと «遅い → 溜まる → もっと刻む → もっと遅い» の循環に入り、
///     1 フレームに数百 ms 掛けて画面が固まる。上限を 2 にすると、負荷が刻みを
///     追い越したとき絵は «スロー再生» になるだけで、操作は最後まで効く。
///     どれだけ遅れているかは m_droppedFrames が申告する。
constexpr int kMaxStepsPerFrame = 2;
/// 1 フレームに取り込む実時間の上限 [s]。ブレークポイントで止めた後に一気に飛ぶのを防ぐ。
constexpr float kMaxFrameDelta = 0.1f;
/// 刻みに «あと少し» 足りないときも進めてしまう猶予 [s]。
///
/// WHY 要るか: 表示が 60Hz で刻みも 1/60 だと、実測 dt のわずかな揺れで «0 回 → 2 回» が
///     交互に来る。物理は正しいのに絵だけがガタつき、«物理が不安定» と読み違える。
constexpr float kStepSnap = 0.0004f;
/// 表示用のならし係数 (1 フレームぶんの重み)。
constexpr float kSmoothing = 0.1f;
constexpr float kFontSize = 17.0f;

constexpr ImVec4 kOkColor    {0.48f, 0.90f, 0.55f, 1.0f};
constexpr ImVec4 kWarnColor  {1.00f, 0.72f, 0.36f, 1.0f};
constexpr ImVec4 kErrorColor {1.00f, 0.36f, 0.41f, 1.0f};

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

    // WHY 必須か: ImGui は Win32 のメッセージを自分では拾わない。ここを繋がないと
    //     マウスもキーも一切届かず、«描画はされるがボタンが押せない» 画面になる。
    //     ビューポートのパン・ズームも同じ経路なので、丸ごと死ぬ。
    core::Application::Get().GetWindow().SetWndProcHook(
        [](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> bool {
            return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0;
        });

    SelectScene(0);
    return true;
}

void BenchApp::OnUpdate(float dt)
{
    m_frameMsAvg += (dt * 1000.0f - m_frameMsAvg) * kSmoothing;

    m_stepsThisFrame = 0;
    if (m_scenes.empty()) return;
    BenchScene& scene = *m_scenes[static_cast<size_t>(m_selected)];

    const auto begin = std::chrono::steady_clock::now();

    if (m_stepRequested) {
        scene.Simulate(kFixedStep);
        m_stepsThisFrame = 1;
        m_stepRequested  = false;
    } else if (!m_paused) {
        m_accumulator += (std::min)(dt, kMaxFrameDelta) * m_speed;
        while (m_accumulator >= kFixedStep - kStepSnap &&
               m_stepsThisFrame < kMaxStepsPerFrame) {
            scene.Simulate(kFixedStep);
            m_accumulator -= kFixedStep;
            ++m_stepsThisFrame;
        }
        // 消化しきれなかったぶんは捨てる。持ち越すと次のフレームも上限に張り付き、
        // 一度遅れたら二度と追いつけない。
        if (m_accumulator >= kFixedStep - kStepSnap) {
            m_accumulator = 0.0f;
            ++m_droppedFrames;
        }
    }

    const float simMs =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
    m_simMsAvg += (simMs - m_simMsAvg) * kSmoothing;
}

/// WHY 描画中ではなくここで検査するか: DrawControls はスライダーを触った時点で場面を
///     組み直す。描画の途中で検査すると «同じフレームの中で設定前と設定後が混ざった状態» を
///     見ることになり、触った瞬間だけ偽の異常が出る。
void BenchApp::OnLateUpdate(float)
{
    if (m_scenes.empty()) return;
    BenchScene& scene = *m_scenes[static_cast<size_t>(m_selected)];

    // 検査も描画も Present が作った «このフレームの最終状態» だけを見る。
    scene.Present();

    m_anomalies.BeginFrame();
    scene.DetectAnomalies(m_anomalies);
    m_anomalies.EndFrame();
}

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

    // ImGui を畳む前にフックを外す。残したまま context を壊すと、終了処理中に来た
    // 1 通のメッセージが破棄済みの context を触りに行く。
    core::Application::Get().GetWindow().SetWndProcHook(nullptr);

    core::Application::Get().GetImGuiRenderer().ImGuiShutdown();
    if (m_imguiContext) {
        ImGui::DestroyContext(static_cast<ImGuiContext*>(m_imguiContext));
        m_imguiContext = nullptr;
    }
}

void BenchApp::SelectScene(int index)
{
    if (index < 0 || index >= static_cast<int>(m_scenes.size())) return;

    m_selected       = index;
    m_accumulator    = 0.0f;
    m_droppedFrames  = 0;
    // 前の場面の履歴を持ち越さない。«この場面で何回出たか» が読めなくなる。
    m_anomalies.BeginFrame();
    m_anomalies.ClearHistory();

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

    DrawPerformance();

    if (!m_scenes.empty()) {
        BenchScene& scene = *m_scenes[static_cast<size_t>(m_selected)];

        DrawAnomalies();

        ImGui::SeparatorText("見るべきところ");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(scene.WhatToLookFor());
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText("設定");
        scene.DrawControls();
    }

    ImGui::EndChild();
}

/// WHY 出すか: «重い» は目で見ても «物理が変» と区別が付かない。数字が無いと、
///     刻みが追いついていないだけの絵を «挙動がおかしい» と読んでしまう。
void BenchApp::DrawPerformance()
{
    ImGui::SeparatorText("性能");

    const float fps = m_frameMsAvg > 0.0f ? 1000.0f / m_frameMsAvg : 0.0f;
    const ImVec4 fpsColor = fps >= 50.0f ? kOkColor : (fps >= 25.0f ? kWarnColor : kErrorColor);
    ImGui::TextColored(fpsColor, "%.0f FPS  (%.2f ms/frame)", fps, m_frameMsAvg);

    // 場面の負荷と «描画も含めた» 負荷の差が、物理以外に掛かっている分。
    ImGui::Text("物理 %.2f ms / %d 刻み", m_simMsAvg, m_stepsThisFrame);

    if (m_droppedFrames > 0) {
        ImGui::TextColored(kWarnColor, "刻みが追いつかず %d フレーム分を捨てた",
                           m_droppedFrames);
        ImGui::SameLine();
        if (ImGui::SmallButton("消す##dropped")) m_droppedFrames = 0;
        ImGui::TextDisabled("絵は実時間より遅い。負荷 (サブステップ・剛体数) を下げて比べる");
    }
}

/// 場面が自分で申告した異常を出す。«目で見て気づけないもの» はここにしか出ない。
void BenchApp::DrawAnomalies()
{
    ImGui::SeparatorText("異常検知");

    if (m_anomalies.Clean()) {
        ImGui::TextColored(kOkColor, "検出なし");
    } else {
        ImGui::PushTextWrapPos(0.0f);
        for (const Anomaly& anomaly : m_anomalies.Current()) {
            ImGui::TextColored(anomaly.severity == Severity::Error ? kErrorColor : kWarnColor,
                               "%s", anomaly.message.c_str());
        }
        ImGui::PopTextWrapPos();
        if (m_anomalies.Suppressed() > 0)
            ImGui::TextDisabled("ほか %d 件", m_anomalies.Suppressed());
    }

    // 1 フレームだけ出て消えた異常はここにしか残らない。放置して回している間の分も拾う。
    if (m_anomalies.Frames() > 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("この場面で %d フレーム検出 / 最初: %s", m_anomalies.Frames(),
                            m_anomalies.FirstSeen().c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("履歴を消す")) m_anomalies.ClearHistory();
    }
}

void BenchApp::DrawViewportPanel()
{
    m_view.Begin("##viewport", {0.0f, 0.0f});
    m_view.HandlePanZoom();
    if (!m_scenes.empty()) m_scenes[static_cast<size_t>(m_selected)]->Draw(m_view);
    m_view.End();
}

} // namespace fbzz::bench
