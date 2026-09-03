/// @file    main.cpp
/// @brief   FBZZTestBench のエントリポイント。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// WHY DX11 を明示するか: このベンチはシェーダーを 1 本も使わず ImGui しか描かない。
///     DX12 経路は実行時に dxcompiler.dll / dxil.dll を要求するため、それが無い環境で
///     «物理を見たいだけ» のツールが起動しなくなる。--renderer=dx12 で上書きはできる。
#include "BenchApp.hpp"

#include <TestKit/Console.hpp>

#include <Engine/Core/Application.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Renderer/RendererBackend.hpp>

#include <cstdio>

int main()
{
    // 起動に失敗したときのログを読ませる。コンソールが無いまま落ちると
    // «何も起きなかった» としか見えない。
    const bool ownsConsole = fbzz::testkit::EnsureConsole();

    auto& app = fbzz::core::Application::Get();

    fbzz::core::Window::Config config;
    config.title  = L"FBZZ Test Bench";
    config.width  = 1600;
    config.height = 900;

    if (!app.Init(config, fbzz::renderer::RendererBackend::DX11)) {
        std::printf("\nApplication::Init に失敗しました。上のログを確認してください。\n");
        if (ownsConsole) fbzz::testkit::WaitForKey();
        return 1;
    }

    fbzz::bench::BenchApp bench;
    app.Run(bench);
    app.Shutdown();

    if (ownsConsole) fbzz::testkit::WaitForKey();
    return 0;
}
