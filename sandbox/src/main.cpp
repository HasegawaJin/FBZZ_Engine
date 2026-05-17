#include <engine/Core/Application.hpp>
#include <engine/Core/Time.hpp>
#include <engine/Input/Input.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Renderer/Camera.hpp>
#include <engine/Renderer/DrawCall.hpp>
#include <math/Matrix4.hpp>

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

struct CameraConstants {
    fbzz::math::Matrix4 viewProjection;
    fbzz::math::Vector3 cameraPos;
    float               _pad;
};

struct ObjectConstants {
    fbzz::math::Matrix4 world;
};

struct MaterialConstants {
    float color[4];
};

int main() {
    auto& app = fbzz::core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ShaderManager::Init(&renderer);

    // --- リソース生成 ---
    Vertex vertices[] = {
        {{ 0.0f,  0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.5f, 0.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
    };

    auto vb     = renderer.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(Vertex));
    auto shader = fbzz::renderer::ShaderManager::Load("assets/shaders/Unlit.hlsl");
    auto camCB  = renderer.CreateConstantBuffer(sizeof(CameraConstants));
    auto objCB  = renderer.CreateConstantBuffer(sizeof(ObjectConstants));
    auto matCB  = renderer.CreateConstantBuffer(sizeof(MaterialConstants));

    // カメラ設定
    fbzz::renderer::Camera camera;
    camera.m_position = {0.0f, 0.0f, -3.0f};
    camera.m_aspect   = 1280.0f / 720.0f;

    // マテリアル (オレンジ)
    MaterialConstants mat = {1.0f, 0.5f, 0.1f, 1.0f};
    matCB->Update(&mat, sizeof(mat));

    // ワールド行列 (単位行列)
    ObjectConstants obj;
    obj.world = fbzz::math::Matrix4::Identity();
    objCB->Update(&obj, sizeof(obj));

    // --- ゲームループ ---
    while (app.IsRunning()) {
        fbzz::core::Time::Tick();
        fbzz::input::Input::Update();

        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) {
            app.Quit();
            break;
        }

        // カメラ定数バッファ更新
        CameraConstants camData;
        camData.viewProjection = camera.GetViewProjection();
        camData.cameraPos      = camera.m_position;
        camCB->Update(&camData, sizeof(camData));

        // DrawCall 組み立て
        fbzz::renderer::DrawCall call;
        call.vertexBuffer        = vb;
        call.shader              = shader;
        call.vertexCount         = 3;
        call.constantBuffers[0]  = camCB;
        call.constantBuffers[1]  = objCB;
        call.constantBuffers[2]  = matCB;

        renderer.BeginFrame();
        renderer.Clear({0.10f, 0.15f, 0.25f, 1.0f});
        renderer.Submit(call);
        renderer.EndFrame();
    }

    fbzz::renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
