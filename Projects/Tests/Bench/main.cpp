/// @file    main.cpp
/// @brief   FBZZTestBench のエントリポイント。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// @note このベンチはシェーダーを 1 本も使わず ImGui しか描かない。以前は DXC を要求しない
///       DX11 経路を明示していたが、DirectX 11 サポートは v1.0 で終了した。DX12 経路は
///       dxcompiler.dll / dxil.dll を実行時に要求するため、それらが exe の隣に無い環境では
///       起動に失敗する (CMake が FBZZEngine の出力先へ配置する)。
/// @see  Docs/design/dx11-removal.md
#include "BenchApp.hpp"

#include <TestKit/Console.hpp>

#include <Engine/Core/Application.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Renderer/RendererBackend.hpp>

#include <cstdio>

int main()
{
    /// @note 起動に失敗したときのログを読ませる。コンソールが無いまま落ちると
    ///       «何も起きなかった» としか見えない。
    const bool ownsConsole = fbzz::testkit::EnsureConsole();

    auto& app = fbzz::core::Application::Get();

    fbzz::core::Window::Config config;
    config.title  = L"FBZZ Test Bench";
    config.width  = 1600;
    config.height = 900;

    if (!app.Init(config, fbzz::renderer::RendererBackend::DX12)) {
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
