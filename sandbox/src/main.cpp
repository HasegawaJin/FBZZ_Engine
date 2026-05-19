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

    // ---------------------------------------------------------------- ライト (シーン別 3 セット)
    // Scene 1-3: PostProcessShowcase — 斜め太陽光 + Bloom 用強白光
    renderer::LightSystem lightsShowcase;
    lightsShowcase.SetDirectional({ { 0.55f, -0.45f, 0.35f }, 0.0f, { 1.0f, 0.92f, 0.76f }, 3.5f });
    lightsShowcase.AddPoint({ { 0.0f, 9.0f, 2.0f }, 22.0f, { 1.0f, 0.97f, 0.90f }, 35.0f });
    lightsShowcase.AddPoint({ { 5.5f, 4.0f, 4.5f }, 10.0f, { 1.0f, 0.40f, 0.10f }, 15.0f });

    // Scene 4: MultiLight — 暗いアンビエント + 色とりどりのポイント/スポット
    renderer::LightSystem lightsMulti;
    lightsMulti.SetDirectional({ { 0.0f, -1.0f, 0.0f }, 0.0f, { 0.15f, 0.15f, 0.2f }, 0.08f });
    lightsMulti.AddPoint({ { -5.0f, 2.5f, 2.0f },  10.0f, { 1.0f, 0.15f, 0.10f }, 20.0f }); // 赤
    lightsMulti.AddPoint({ {  5.0f, 2.5f, 2.0f },  10.0f, { 0.20f, 0.40f, 1.0f }, 20.0f }); // 青
    lightsMulti.AddPoint({ {  0.0f, 2.5f, 6.5f },  10.0f, { 0.20f, 1.00f, 0.3f }, 18.0f }); // 緑
    lightsMulti.AddPoint({ { -4.0f, 2.5f, 5.0f },   9.0f, { 1.0f, 0.60f, 0.1f }, 15.0f }); // オレンジ
    lightsMulti.AddPoint({ {  4.0f, 2.5f,-1.0f },   9.0f, { 0.85f, 0.2f, 1.0f }, 15.0f }); // 紫
    lightsMulti.AddSpot({  // 真上から白スポット
        { 0.0f, 10.0f, 3.0f }, 18.0f,
        { 0.0f, -1.0f, 0.0f }, 0.97f,
        { 1.0f,  1.0f, 1.0f }, 0.87f, 25.0f
    });
    lightsMulti.AddSpot({  // 左横から冷白スポット
        { -9.0f, 6.0f, 1.0f }, 18.0f,
        {  0.75f, -0.55f, 0.36f }, 0.96f,
        { 0.65f, 0.80f, 1.0f }, 0.85f, 12.0f
    });

    // Scene 5: Skydome — 水平線近くの夕日
    renderer::LightSystem lightsSkydome;
    lightsSkydome.SetDirectional({ { 0.97f, -0.12f, 0.2f }, 0.0f, { 1.0f, 0.65f, 0.30f }, 2.5f });

    renderer::LightSystem* pLights = &lightsShowcase;

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

    // ---------------------------------------------------------------- PostProcess ショーケースシーン
    // Shadow / Bloom / FXAA の撮影に特化したシーン
    sm.Register("PostProcessShowcase", [&]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();

        // ===== フロア: 影受け面 =====
        auto& floor = s->CreateGameObject("Floor");
        floor.transform.localPosition = { 0.0f, -0.1f, 5.0f };
        floor.transform.localScale    = { 22.0f, 0.2f, 22.0f };
        floor.AddComponent<scene::MeshRenderer>({ cubeMesh,
            MakeMat(renderer, shaderLit, { 0.65f, 0.63f, 0.60f, 1.0f }, 0.0f, 0.9f) });

        // ===== 柱 5 本: 長い影をフロアに落とす =====
        for (int i = 0; i < 5; ++i) {
            auto& col = s->CreateGameObject("Column_" + std::to_string(i));
            col.transform.localPosition = { (i - 2) * 3.8f, 3.0f, 8.0f };
            col.transform.localScale    = { 0.8f, 6.0f, 0.8f };
            col.AddComponent<scene::MeshRenderer>({ cubeMesh,
                MakeMat(renderer, shaderLit, { 0.88f, 0.85f, 0.80f, 1.0f }, 0.0f, 0.7f) });
        }

        // ===== Bloom 用: 高 metallic PBR 球 (金・銀・銅) =====
        struct SphereEntry { math::Vector4 color; float roughness; const char* name; };
        SphereEntry spheres[] = {
            { { 1.00f, 0.78f, 0.34f, 1.0f }, 0.04f, "Sphere_Gold"   },
            { { 0.95f, 0.95f, 0.95f, 1.0f }, 0.03f, "Sphere_Silver" },
            { { 0.72f, 0.45f, 0.20f, 1.0f }, 0.07f, "Sphere_Copper" },
        };
        for (int i = 0; i < 3; ++i) {
            auto& go = s->CreateGameObject(spheres[i].name);
            go.transform.localPosition = { (i - 1) * 3.5f, 1.0f, 1.5f };
            go.transform.localScale    = { 1.3f, 1.3f, 1.3f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, shaderPBR, spheres[i].color, 1.0f, spheres[i].roughness) });
        }

        // ===== Bloom 追加ソース: 炎パーティクル =====
        {
            auto& go = s->CreateGameObject("Fire");
            go.transform.localPosition = { 5.5f, 0.1f, 4.5f };
            scene::ParticleEmitter em;
            em.emitVelocity   = { 0.0f, 4.0f, 0.0f };
            em.velocitySpread = 0.7f;
            em.colorStart     = { 1.0f, 0.60f, 0.1f, 1.0f };
            em.colorEnd       = { 0.9f, 0.10f, 0.0f, 0.0f };
            em.sizeStart      = 0.6f;
            em.sizeEnd        = 0.05f;
            em.lifetime       = 1.5f;
            em.emitRate       = 45.0f;
            em.maxParticles   = 300;
            go.AddComponent<scene::ParticleEmitter>(std::move(em));
        }

        // ===== スカイドーム =====
        {
            auto& sky = s->CreateGameObject("Sky");
            sky.AddComponent<scene::SkyRenderer>({
                { 5.8e-3f, 13.5e-3f, 33.1e-3f },
                21.0e-4f, 20.0f, 0.76f,
            });
        }

        return s;
    });

    // ---------------------------------------------------------------- Scene 4: MultiLight
    sm.Register("MultiLight", [&]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();

        // フロア: 暗めで光の反射が見やすい
        auto& floor = s->CreateGameObject("Floor");
        floor.transform.localPosition = { 0.0f, -0.1f, 3.0f };
        floor.transform.localScale    = { 20.0f, 0.2f, 20.0f };
        floor.AddComponent<scene::MeshRenderer>({ cubeMesh,
            MakeMat(renderer, shaderPBR, { 0.12f, 0.12f, 0.14f, 1.0f }, 0.05f, 0.5f) });

        // 4x3 グリッドの PBR 球 (material バリエーション)
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                auto& go = s->CreateGameObject("Sphere_" + std::to_string(row * 4 + col));
                go.transform.localPosition = { (col - 1.5f) * 3.0f, 1.0f, row * 3.0f };
                go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
                go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                    MakeMat(renderer, shaderPBR,
                        { 0.9f, 0.9f, 0.9f, 1.0f },
                        static_cast<float>(col) / 3.0f,   // metallic 0→1
                        static_cast<float>(row) / 2.0f)   // roughness 0→1
                });
            }
        }

        return s;
    });

    // ---------------------------------------------------------------- Scene 5: Skydome
    sm.Register("Skydome", [&]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();

        // 広い地面
        auto& ground = s->CreateGameObject("Ground");
        ground.transform.localPosition = { 0.0f, -0.1f, 10.0f };
        ground.transform.localScale    = { 60.0f, 0.2f, 60.0f };
        ground.AddComponent<scene::MeshRenderer>({ cubeMesh,
            MakeMat(renderer, shaderLit, { 0.22f, 0.20f, 0.16f, 1.0f }, 0.0f, 0.9f) });

        // 水平線にシルエットの台地 3 本
        float mesaX[] = { -14.0f, 0.0f, 10.0f };
        float mesaW[] = {   8.0f, 6.0f,  7.0f };
        float mesaH[] = {   5.0f, 3.5f,  4.5f };
        for (int i = 0; i < 3; ++i) {
            auto& mesa = s->CreateGameObject("Mesa_" + std::to_string(i));
            mesa.transform.localPosition = { mesaX[i], mesaH[i] * 0.5f, 28.0f };
            mesa.transform.localScale    = { mesaW[i], mesaH[i], 4.0f };
            mesa.AddComponent<scene::MeshRenderer>({ cubeMesh,
                MakeMat(renderer, shaderLit, { 0.10f, 0.09f, 0.08f, 1.0f }, 0.0f, 0.9f) });
        }

        // スカイドーム (夕焼けパラメーター: Mie 散乱を増やして霞感)
        auto& sky = s->CreateGameObject("Sky");
        sky.AddComponent<scene::SkyRenderer>({
            { 5.8e-3f, 13.5e-3f, 33.1e-3f },
            58.0e-4f,   // mieScattering 大きめ → 霞んだ夕焼け
            22.0f,
            0.82f,
        });

        return s;
    });

    sm.LoadScene("MultiLight");

    // ---------------------------------------------------------------- エディター
    editor::EditorApp editorApp;
    editorApp.Init(renderer, app.GetWindow());

    editorApp.GetContext().activeScene  = sm.GetActive();
    editorApp.GetContext().lightSystem  = pLights;

    app.GetWindow().SetResizeCallback([&](uint32_t w, uint32_t h) {
        renderer.Resize(w, h);
    });

    // ---------------------------------------------------------------- カメラ (シーン別プリセット)
    struct CamPreset { math::Vector3 pos; math::Vector3 target; };
    CamPreset camPresets[] = {
        { { -5.0f,  7.0f, -10.0f }, {  0.0f, 1.5f,  5.0f } }, // PostProcessShowcase
        { {  0.0f,  5.0f,  -9.0f }, {  0.0f, 1.5f,  3.0f } }, // MultiLight
        { {  0.0f,  3.0f,  -3.0f }, {  0.0f, 5.0f, 20.0f } }, // Skydome
    };
    int activeCam = 1;  // MultiLight から開始

    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = camPresets[activeCam].pos;
    debugCamera.camera.m_aspect   = 1280.0f / 720.0f;
    debugCamera.LookAt(camPresets[activeCam].target);

    // ---------------------------------------------------------------- ゲームループ
    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();

        // F1〜F3 でシーン / ライト / カメラを切り替え
        struct SceneSwitch { const char* name; renderer::LightSystem* lights; int cam; };
        SceneSwitch switches[] = {
            { "PostProcessShowcase", &lightsShowcase, 0 },
            { "MultiLight",          &lightsMulti,    1 },
            { "Skydome",             &lightsSkydome,  2 },
        };
        int switchIdx = -1;
        if (input::Input::KeyDown(input::KeyCode::F1)) switchIdx = 0;
        if (input::Input::KeyDown(input::KeyCode::F2)) switchIdx = 1;
        if (input::Input::KeyDown(input::KeyCode::F3)) switchIdx = 2;
        if (switchIdx >= 0) {
            sm.LoadScene(switches[switchIdx].name);
            pLights   = switches[switchIdx].lights;
            activeCam = switches[switchIdx].cam;
            debugCamera.camera.m_position = camPresets[activeCam].pos;
            debugCamera.LookAt(camPresets[activeCam].target);
            editorApp.GetContext().activeScene = sm.GetActive();
            editorApp.GetContext().lightSystem = pLights;
        }

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
            scene::RenderSystem(*activeScene, renderer, debugCamera.camera, *pLights, vpRT, &editorApp.GetContext().renderSettings);

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
