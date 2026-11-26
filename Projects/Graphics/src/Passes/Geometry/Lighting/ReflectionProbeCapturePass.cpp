/// @file    ReflectionProbeCapturePass.cpp
/// @brief   ReflectionProbe の空のみ／周辺メッシュ込み動的キャプチャと IBL 畳み込み。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @note プローブごとに 6 面を毎フレーム描くとゲーム本体の描画より高価になり得るため、更新間隔と
/// @note 明示リクエストで間引く。昼夜変化だけを追う用途には DynamicSky、室内・配置物の反射には
/// @note DynamicScene を使う。
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include "../RenderScenePassHelpers.hpp"
#include <Graphics/Passes/Geometry/Instancing/InstanceRenderPasses.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Graphics/Scene/RenderProbeInput.hpp>
#include <Graphics/Renderer/Resources/Textures/ITexture.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <vector>
namespace fbzz::renderer {
namespace {
struct CubeFaceBasis { math::Vector3 forward; math::Vector3 up; };
constexpr CubeFaceBasis kCubeFaces[6] = {
    {{ 1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    {{-1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    {{ 0.0f,  1.0f,  0.0f}, { 0.0f, 0.0f, -1.0f}},
    {{ 0.0f, -1.0f,  0.0f}, { 0.0f, 0.0f,  1.0f}},
    {{ 0.0f,  0.0f,  1.0f}, { 0.0f, 1.0f,  0.0f}},
    {{ 0.0f,  0.0f, -1.0f}, { 0.0f, 1.0f,  0.0f}},
};

void RenderSkyFace(RenderPassContext& ctx, const math::Vector3& position, uint32_t face,
                   renderer::ResourceHandle<renderer::RenderTargetTag> target)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const math::Matrix4 projection = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, 0.1f, 500.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(position, position + kCubeFaces[face].forward, kCubeFaces[face].up);
    PerFrameCB frame{};
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = projection * view;
    frame.invViewProjection = math::Matrix4::Inverse(frame.viewProjection);
    frame.cameraPos = position;
    frame.nearZ = 0.1f;
    frame.farZ = 500.0f;
    resources.Update(h.skyCaptureFrameCB, &frame, sizeof(frame));
    ctx.renderer.SetRenderTargetFace(target, face, 0, resources);
    ctx.renderer.ClearDepth();

    renderer::DrawCall skyCall{};
    skyCall.vertexBuffer = h.skyVB;
    skyCall.indexBuffer = h.skyIB;
    skyCall.indexCount = h.skyIndexCount;
    skyCall.shader = h.skyShader;
    skyCall.pipelineState = h.skyPSO;
    skyCall.constantBuffers[cb::CAMERA] = h.skyCaptureFrameCB;
    skyCall.constantBuffers[cb::LIGHT] = h.lightCB;
    skyCall.constantBuffers[cb::POSTPROC] = h.postprocCB;
    skyCall.constantBuffers[cb::ATMOSPHERE] = h.atmosphereCB;
    ctx.renderer.Submit(skyCall, resources);
}

void RenderSceneFace(RenderPassContext& ctx, const RenderReflectionProbeInput& probeObject,
                     const math::Vector3& position, uint32_t face)
{
    // DynamicScene は MeshRenderer の不透明・半透明マテリアルを既存 MaterialComponent 経路で
    // 描く。キャプチャ自身は反射へ混入させない。スキンドメッシュ・粒子・UI は時間依存でコストも
    // 大きいため対象外とし、必要なら更新間隔を短くした専用プローブを配置する。
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const math::Matrix4 projection = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, 0.1f, 500.0f);
    const math::Matrix4 view = math::Matrix4::LookAt(position, position + kCubeFaces[face].forward, kCubeFaces[face].up);
    PerFrameCB frame{};
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = projection * view;
    frame.invViewProjection = math::Matrix4::Inverse(frame.viewProjection);
    frame.cameraPos = position;
    frame.nearZ = 0.1f;
    frame.farZ = 500.0f;
    resources.Update(h.frameCB, &frame, sizeof(frame));

    const auto& input = SceneInput(ctx);
    struct CaptureItem {
        const RenderObject* object;
        const RenderMeshItem* mesh;
        float depth;
    };
    std::vector<CaptureItem> items;
    for (const auto& object : input.Objects()) {
        if (object.skinned || !object.colorEligible || !MatchesView(ctx, object) ||
            (object.sourceIndex == probeObject.sourceIndex && object.sourceGeneration == probeObject.sourceGeneration)) {
            continue;
        }
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input.items[i];
            if (!item.visible || !item.slotVisible || !HasColorGeometry(item)
                || !item.material.valid || !item.material.shader.IsValid()) {
                continue;
            }
            const auto offset = item.boundsCenter - position;
            const auto& forward = kCubeFaces[face].forward;
            items.push_back({ &object, &item, offset.x * forward.x + offset.y * forward.y + offset.z * forward.z });
        }
    }
    // 捕捉面の視線で透明物を奥から並べる。主カメラの並びや Scene の登録順は使わない。
    std::stable_sort(items.begin(), items.end(), [](const CaptureItem& a, const CaptureItem& b) {
        const bool aTransparent = a.mesh->material.capabilities.blend != BlendMode::OPAQUE_BLEND;
        const bool bTransparent = b.mesh->material.capabilities.blend != BlendMode::OPAQUE_BLEND;
        if (aTransparent != bTransparent) {
            return !aTransparent;
        }
        if (a.mesh->material.renderQueue != b.mesh->material.renderQueue) {
            return a.mesh->material.renderQueue < b.mesh->material.renderQueue;
        }
        if (a.depth != b.depth) {
            return aTransparent ? a.depth > b.depth : a.depth < b.depth;
        }
        if (a.object->sourceIndex != b.object->sourceIndex) {
            return a.object->sourceIndex < b.object->sourceIndex;
        }
        if (a.object->sourceGeneration != b.object->sourceGeneration) {
            return a.object->sourceGeneration < b.object->sourceGeneration;
        }
        return a.mesh->materialSlot < b.mesh->materialSlot;
    });
    bool instancesDrawn=false;
    const auto captureInstances=[&] {
        Camera camera;camera.m_position=position;camera.m_fovY=90;camera.m_aspect=1;camera.m_near=0.1f;camera.m_far=500;
        camera.m_rotation=math::Quaternion::LookRotation(kCubeFaces[face].forward,kCubeFaces[face].up);
        ExecuteInstanceCapture(ctx,camera,probeObject.resolution,probeObject.sourceIndex,probeObject.sourceGeneration);
        resources.Update(h.frameCB,&frame,sizeof(frame));
        instancesDrawn=true;
    };
    for (const auto& entry : items) {
        if (!instancesDrawn && entry.mesh->material.capabilities.blend!=BlendMode::OPAQUE_BLEND) {
            captureInstances();
        }
        const auto& object = *entry.object;
        const auto& item = *entry.mesh;
        const auto& material = item.material;
        auto constants = ObjectConstants(object);
        constants.objectParams = {};
        resources.Update(h.objectCB, &constants, sizeof(constants));
        renderer::DrawCall draw{};
        draw.vertexBuffer = item.vertexBuffer;
        draw.indexBuffer = item.indexBuffer;
        draw.indexCount = item.indexCount;
        draw.vertexCount = item.vertexCount;
        draw.shader = material.shader;
        draw.pipelineState = GetOrCreateMaterialPSO(resources, material.capabilities.blend,
            material.doubleSided, material.depthBias, material.depthBiasSlope, false);
        draw.constantBuffers[cb::CAMERA] = h.frameCB;
        draw.constantBuffers[cb::OBJECT] = h.objectCB;
        draw.constantBuffers[cb::MATERIAL] = material.paramsBuffer;
        draw.constantBuffers[cb::LIGHT] = h.lightCB;
        for (size_t i = 0; i < material.textures.size(); ++i) {
            draw.textures[i] = material.textures[i];
        }
        BindProbeCaptureLightingResources(draw, ctx);
        ctx.renderer.Submit(draw, resources);
    }
    if (!instancesDrawn) {
        captureInstances();
    }
}

}
bool PrepareProbeCaptureShadowConstants(RenderPassContext& ctx)
{
    const auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const auto* shadowBuffer = resources.Get(h.reflectionProbeCaptureShadowCB);
    if (!shadowBuffer || shadowBuffer->GetSize() < sizeof(ShadowConstantsCB)) {
        return false;
    }
    const auto* punctualBuffer = resources.Get(h.reflectionProbeCapturePunctualCB);
    if (!punctualBuffer || punctualBuffer->GetSize() < sizeof(PunctualShadowConstantsCB)) {
        return false;
    }

    // ワールド空間の雲影と光源の形状を保持し、メインビューのアトラスを参照する設定を外す。
    auto shadow = MakeShadowConstants(ctx);
    shadow.shadowStrength = 0.0f;
    shadow.cascadeDebugView = 0;
    auto punctual = MakePunctualShadowConstants(ctx);
    punctual.punctualShadowCount = 0;
    punctual.lightCookieCount = 0;
    for (auto& slots : punctual.legacyPunctualSlots) {
        slots.x = -1.0f;
        slots.y = -1.0f;
    }
    for (int i = 0; i < punctual.legacyShapedLightCount; ++i) {
        punctual.legacyShapedLight[i * kLegacyShapedLightStride + 5].z = -1.0f;
    }

    resources.Update(h.reflectionProbeCaptureShadowCB, &shadow, sizeof(shadow));
    resources.Update(h.reflectionProbeCapturePunctualCB, &punctual, sizeof(punctual));
    return true;
}

bool CaptureReflectionProbe(RenderPassContext& ctx, const RenderReflectionProbeInput& input, RenderReflectionProbeResult& output)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    if (input.captureScene && !PrepareProbeCaptureShadowConstants(ctx)) {
        return false;
    }
    resources.Update(h.lightCB, &ctx.lightData, sizeof(ctx.lightData));
    PostProcCB post{}; post.exposure = 1; post.time = ctx.time;
    resources.Update(h.postprocCB, &post, sizeof(post));
    resources.Update(h.atmosphereCB, &input.atmosphere, sizeof(input.atmosphere));
    for (uint32_t face = 0; face < 6; ++face) {
        RenderSkyFace(ctx, input.position, face, input.target);
        if (input.captureScene) {
            RenderSceneFace(ctx, input, input.position, face);
        }
    }
    ctx.renderer.SetRenderTarget({}, resources);
    std::unique_ptr<ITexture> irradiance, prefilter;
    if (!ctx.renderer.BakeSkyLight(input.target, resources, 32, input.resolution, 5, 128, irradiance, prefilter)
        || !irradiance || !prefilter) {
        return false;
    }
    output.irradiance = resources.RegisterTexture(std::move(irradiance));
    output.prefilter = resources.RegisterTexture(std::move(prefilter));
    output.prefilterMipCount = 5;
    output.immutablePublished = resources.Get(output.irradiance) && resources.Get(output.prefilter);
    return output.immutablePublished;
}
}
