// FBZZ Engine
// sandbox/src/main.cpp
// レンダーテストシーン — マテリアルシェーダー比較
#include <engine/Core/Application.hpp>
#include <engine/Core/Time.hpp>
#include <engine/Input/Input.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Renderer/Camera.hpp>
#include <engine/Renderer/DebugCamera.hpp>
#include <engine/Renderer/DebugDraw.hpp>
#include <engine/Renderer/PrimitiveMesh.hpp>
#include <engine/Renderer/Material.hpp>
#include <engine/Renderer/LightSystem.hpp>
#include <engine/Asset/AssetManager.hpp>
#include <engine/Asset/Model.hpp>
#include <engine/Renderer/ITexture.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/SceneManager.hpp>
#include <engine/Scene/Systems/RenderSystem.hpp>
#include <engine/Scene/Components/MeshRenderer.hpp>
#include <engine/Scene/Components/ParticleEmitter.hpp>
#include <engine/Scene/Components/SkyRenderer.hpp>
#include <physics/World.hpp>
#include <editor/EditorApp.hpp>

using namespace fbzz;

namespace {

std::shared_ptr<renderer::Material> MakeMat(
    renderer::IRenderer& r,
    std::shared_ptr<renderer::IShader> shader,
    math::Vector4 albedo,
    float metallic  = 0.0f,
    float roughness = 0.5f,
    std::shared_ptr<renderer::ITexture> tex       = nullptr,
    std::shared_ptr<renderer::ITexture> normalTex = nullptr)
{
    auto mat = std::make_shared<renderer::Material>();
    mat->shader           = shader;
    mat->params.albedo    = albedo;
    mat->params.metallic  = metallic;
    mat->params.roughness = roughness;
    mat->albedoTexture    = tex;
    mat->normalTexture    = normalTex;
    mat->Init(r);
    return mat;
}

} // namespace

int main()
{
    auto& app = core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ShaderManager::Init(&renderer);
    asset::AssetManager::Init(renderer);

    // ---------------------------------------------------------------- シェーダー
    auto shaderUnlit      = renderer::ShaderManager::Load("assets/shaders/Material/Unlit.hlsl");
    auto shaderLit        = renderer::ShaderManager::Load("assets/shaders/Material/Lit.hlsl");
    auto shaderPhong      = renderer::ShaderManager::Load("assets/shaders/Material/Phong.hlsl");
    auto shaderBlinnPhong = renderer::ShaderManager::Load("assets/shaders/Material/BlinnPhong.hlsl");
    auto shaderPBR        = renderer::ShaderManager::Load("assets/shaders/Material/PBR.hlsl");
    auto shaderToon       = renderer::ShaderManager::Load("assets/shaders/Material/Toon.hlsl");

    // ---------------------------------------------------------------- テクスチャ
    auto albedoTex = asset::AssetManager::Load<renderer::ITexture>("textures/test.jpg");
    auto normalTex = asset::AssetManager::Load<renderer::ITexture>("textures/normal/normal_test.png");

    // ---------------------------------------------------------------- モデル
    auto testModel = asset::AssetManager::Load<asset::Model>("models/test.fbx");

    // ---------------------------------------------------------------- メッシュ
    auto sphereMesh = renderer::PrimitiveMesh::Sphere(renderer, 32);
    auto cubeMesh   = renderer::PrimitiveMesh::Cube(renderer);
    auto planeMesh  = renderer::PrimitiveMesh::Plane(renderer);

    // ---------------------------------------------------------------- ライト (夜間シーン)
    renderer::LightSystem lights;
    // 月光: 暗い青白色、低強度
    lights.SetDirectional({
        { -0.3f, -0.9f, 0.3f },
        0.0f,
        { 0.4f, 0.5f, 0.8f },
        0.15f
    });
    // オレンジ色のランタン (左、シェーダー比較行の前)
    lights.AddPoint({ { -7.0f, 3.0f, 0.0f }, 14.0f, { 1.0f, 0.45f, 0.1f }, 10.0f });
    // 青紫の魔法光 (中央)
    lights.AddPoint({ {  0.0f, 2.5f, 2.5f }, 10.0f, { 0.3f, 0.4f, 1.0f },   8.0f });
    // 金色の暖色光 (PBR メタル行付近)
    lights.AddPoint({ {  5.0f, 3.0f, 8.0f }, 12.0f, { 1.0f, 0.8f, 0.25f },  9.0f });
    // 緑のアクセント光 (右奥)
    lights.AddPoint({ { -4.0f, 4.0f, 8.0f }, 10.0f, { 0.2f, 1.0f, 0.45f },  6.0f });
    // 赤いポイントライト (左奥)
    lights.AddPoint({ {  7.0f, 2.0f, 4.0f }, 10.0f, { 1.0f, 0.15f, 0.1f },  7.0f });
    // SpotLight フィールド順: position, range, direction, innerCos, color, outerCos, intensity
    // スポットライト: モデル展示エリアを真上から照らす (白色)
    lights.AddSpot({
        { 0.0f, 12.0f, 14.0f }, 22.0f,
        { 0.0f, -1.0f, 0.0f },
        0.966f,               // innerCos ~15°
        { 1.0f, 0.95f, 0.85f },
        0.866f,               // outerCos ~30°
        12.0f
    });
    // スポットライト: PBR 球配列を斜め前から照らす (冷白)
    lights.AddSpot({
        { -8.0f, 8.0f, -2.0f }, 20.0f,
        { 0.6f, -0.7f, 0.4f },
        0.940f,               // innerCos ~20°
        { 0.7f, 0.8f, 1.0f },
        0.819f,               // outerCos ~35°
        8.0f
    });

    // ---------------------------------------------------------------- 物理ワールド (ダミー: ボディなし)
    physics::World physWorld;

    // ---------------------------------------------------------------- シーン
    auto& sm = app.GetSceneManager();

    sm.Register("RenderTest", [&]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();
        constexpr float Y = 1.0f;

        // ================================================================
        // 行 1 (Z=0): シェーダー比較
        //   Unlit / Lit / Phong / BlinnPhong / PBR を横に並べる
        // ================================================================
        struct ShaderEntry {
            std::shared_ptr<renderer::IShader> shader;
            math::Vector4 color;
            const char* name;
        };
        ShaderEntry row1[] = {
            { shaderUnlit,      { 1.0f, 0.3f, 0.3f, 1.0f },      "Unlit"      },
            { shaderLit,        { 0.3f, 0.9f, 0.3f, 1.0f },      "Lit"        },
            { shaderPhong,      { 0.3f, 0.5f, 1.0f, 1.0f },      "Phong"      },
            { shaderBlinnPhong, { 1.0f, 0.85f, 0.2f, 1.0f },     "BlinnPhong" },
            { shaderPBR,        { 0.85f, 0.85f, 0.85f, 1.0f },   "PBR"        },
            { shaderToon,       { 0.2f, 0.7f, 1.0f, 1.0f },      "Toon"       },
        };
        constexpr int ROW1_COUNT = 6;
        for (int i = 0; i < ROW1_COUNT; ++i) {
            auto& e = row1[i];
            // BlinnPhong (i=3) と PBR (i=4) のみノーマルマップ対応
            auto nm = (i >= 3 && i <= 4) ? normalTex : nullptr;
            auto& go = s->CreateGameObject(e.name);
            go.transform.localPosition = { (i - 2.5f) * 3.0f, Y, 0.0f };
            go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, e.shader, e.color, 0.0f, 0.3f, albedoTex, nm) });
        }

        // ================================================================
        // 行 2 (Z=4): PBR roughness 0 → 1 (metallic=0, 紫系)
        // ================================================================
        for (int i = 0; i < 5; ++i) {
            auto& go = s->CreateGameObject("PBR_Rough_" + std::to_string(i));
            go.transform.localPosition = { (i - 2) * 3.0f, Y, 4.0f };
            go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, shaderPBR,
                    { 0.7f, 0.2f, 0.9f, 1.0f },
                    0.0f, i * 0.25f, albedoTex, normalTex) });
        }

        // ================================================================
        // 行 3 (Z=8): PBR metallic 0 → 1 (roughness=0.2, 金系)
        // ================================================================
        for (int i = 0; i < 5; ++i) {
            auto& go = s->CreateGameObject("PBR_Metal_" + std::to_string(i));
            go.transform.localPosition = { (i - 2) * 3.0f, Y, 8.0f };
            go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, shaderPBR,
                    { 1.0f, 0.78f, 0.34f, 1.0f },
                    i * 0.25f, 0.2f, albedoTex, normalTex) });
        }

        // ================================================================
        // test.fbx — マテリアル別比較 (Z=14 〜 16)
        //   各シェーダーで同じモデルを並べる
        // ================================================================
        if (testModel && !testModel->meshes.empty()) {
            struct ModelEntry {
                std::shared_ptr<renderer::IShader> shader;
                math::Vector4 color;
                float metallic;
                float roughness;
                const char* name;
            };
            ModelEntry mrow[] = {
                { shaderLit,        { 0.9f, 0.9f, 0.9f, 1.0f }, 0.0f, 0.5f, "M_Lit"        },
                { shaderPhong,      { 0.8f, 0.5f, 0.3f, 1.0f }, 0.0f, 0.4f, "M_Phong"      },
                { shaderBlinnPhong, { 0.3f, 0.6f, 0.9f, 1.0f }, 0.0f, 0.3f, "M_BlinnPhong" },
                { shaderPBR,        { 0.9f, 0.8f, 0.7f, 1.0f }, 0.0f, 0.2f, "M_PBR_Rough"  },
                { shaderPBR,        { 1.0f, 0.78f, 0.3f, 1.0f }, 1.0f, 0.1f, "M_PBR_Metal" },
                { shaderToon,       { 0.4f, 0.8f, 0.5f, 1.0f }, 0.0f, 0.5f, "M_Toon"       },
            };
            constexpr int MROW_COUNT = 6;

            for (int col = 0; col < MROW_COUNT; ++col) {
                auto& entry = mrow[col];
                float xPos  = (col - 2.5f) * 3.0f;

                for (size_t mi = 0; mi < testModel->meshes.size(); ++mi) {
                    std::string name = std::string(entry.name)
                                     + "_m" + std::to_string(mi);
                    auto& go = s->CreateGameObject(name);
                    go.transform.localPosition = { xPos, 0.5f, 14.0f };
                    go.transform.localScale    = { 1.0f, 1.0f, 1.0f };
                    go.AddComponent<scene::MeshRenderer>({ testModel->meshes[mi],
                        MakeMat(renderer, entry.shader,
                                entry.color, entry.metallic, entry.roughness,
                                albedoTex, nullptr) });
                }
            }
        }

        // ================================================================
        // パーティクルエミッター (炎・魔法・火花の 3 種)
        // ================================================================
        struct EmitterDef {
            math::Vector3 pos;
            math::Vector3 vel;
            float         spread;
            math::Vector4 colorStart;
            math::Vector4 colorEnd;
            float         sizeStart;
            float         sizeEnd;
            float         lifetime;
            float         rate;
            const char*   name;
        };
        EmitterDef emDefs[] = {
            // 炎 (暖色、低速、短寿命)
            { { -4.0f, 0.1f, 2.0f }, { 0.0f, 3.5f, 0.0f }, 0.8f,
              { 1.0f, 0.6f, 0.1f, 1.0f }, { 0.9f, 0.1f, 0.0f, 0.0f },
              0.5f, 0.05f, 1.5f, 40.0f, "Emitter_Fire" },
            // 魔法 (青紫、高速上昇、長寿命)
            { {  0.0f, 0.1f, 2.0f }, { 0.0f, 5.0f, 0.0f }, 1.2f,
              { 0.4f, 0.5f, 1.0f, 1.0f }, { 0.2f, 0.0f, 0.8f, 0.0f },
              0.35f, 0.02f, 2.5f, 25.0f, "Emitter_Magic" },
            // 火花 (白金、広角拡散、速い)
            { {  4.0f, 0.1f, 2.0f }, { 0.0f, 6.0f, 0.0f }, 2.5f,
              { 1.0f, 0.95f, 0.6f, 1.0f }, { 1.0f, 0.4f, 0.0f, 0.0f },
              0.25f, 0.02f, 1.2f, 60.0f, "Emitter_Spark" },
        };
        for (auto& ed : emDefs) {
            auto& go = s->CreateGameObject(ed.name);
            go.transform.localPosition = ed.pos;
            scene::ParticleEmitter em;
            em.emitVelocity   = ed.vel;
            em.velocitySpread = ed.spread;
            em.colorStart     = ed.colorStart;
            em.colorEnd       = ed.colorEnd;
            em.sizeStart      = ed.sizeStart;
            em.sizeEnd        = ed.sizeEnd;
            em.lifetime       = ed.lifetime;
            em.emitRate       = ed.rate;
            em.maxParticles   = 300;
            go.AddComponent<scene::ParticleEmitter>(std::move(em));
        }

        // ================================================================
        // スカイドーム (大気散乱)
        // ================================================================
        auto& sky = s->CreateGameObject("Sky");
        sky.AddComponent<scene::SkyRenderer>({
            { 5.8e-3f, 13.5e-3f, 33.1e-3f },  // rayleighScattering (ゲームスケール: 1e-3)
            21.0e-4f,                            // mieScattering
            20.0f,                               // sunIntensity
            0.76f,                               // mieG
        });

        // ================================================================
        // 床 (Lit)
        // ================================================================
        auto& floor = s->CreateGameObject("Floor");
        floor.transform.localPosition = { 0.0f, 0.0f, 4.0f };
        floor.transform.localScale    = { 30.0f, 0.2f, 22.0f };
        floor.AddComponent<scene::MeshRenderer>({ cubeMesh,
            MakeMat(renderer, shaderLit, { 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f, 1.0f, nullptr) });

        return s;
    });

    sm.LoadScene("RenderTest");

    // ---------------------------------------------------------------- エディター
    editor::EditorApp editorApp;
    editorApp.Init(renderer, app.GetWindow());

    editorApp.GetContext().activeScene  = sm.GetActive();
    editorApp.GetContext().lightSystem  = &lights;

    app.GetWindow().SetResizeCallback([&](uint32_t w, uint32_t h) {
        renderer.Resize(w, h);
    });

    // ---------------------------------------------------------------- カメラ
    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 6.0f, -14.0f };
    debugCamera.camera.m_aspect   = 1280.0f / 720.0f;
    debugCamera.LookAt({ 0.0f, 1.0f, 4.0f });

    // ---------------------------------------------------------------- ゲームループ
    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();

        // RT リサイズを先に処理してからシーンを描く (EditorApp::BeginFrame 冒頭で実行)
        editorApp.BeginFrame();

        auto vpRT = editorApp.GetViewportRT();
        debugCamera.camera.m_aspect = vpRT
            ? static_cast<float>(vpRT->GetWidth()) / static_cast<float>(vpRT->GetHeight())
            : 1280.0f / 720.0f;
        debugCamera.Update(dt);
        sm.Update(dt, physWorld);

        renderer.BeginFrame();

        if (auto* activeScene = sm.GetActive())
            scene::RenderSystem(*activeScene, renderer, debugCamera.camera, lights, vpRT);

        renderer::DebugDraw::BeginFrame(renderer, debugCamera.camera.GetViewProjection());
        renderer::DebugDraw::Line(renderer, {0,0,0}, {1,0,0}, {1,0,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,1,0}, {0,1,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,0,1}, {0,0,1,1});
        renderer::DebugDraw::Flush();

        // ---- バックバッファに戻して ImGui を描く ----
        renderer.SetRenderTarget(nullptr);
        renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });

        editorApp.GetContext().activeScene = sm.GetActive();
        editorApp.RenderPanels(editorApp.GetContext());
        editorApp.EndFrame(renderer);

        renderer.EndFrame();
    }

    editorApp.Shutdown();
    asset::AssetManager::UnloadAll();
    renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
