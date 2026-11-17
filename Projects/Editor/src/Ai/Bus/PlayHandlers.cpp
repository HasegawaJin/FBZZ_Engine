/// @file    PlayHandlers.cpp
/// @brief   Play 制御・入力注入・ビューポートの撮影とカメラ (play.* / input.* / viewport.*)。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Ai/OperatorBridge.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

/// @brief 指定 View の RT を PNG(base64) にして返す (viewport.capture)。
/// @note 実 RT サイズを width/height に載せる。
Outcome DoViewportCapture(editor::EditorContext& ctx, renderer::ResourceHandle<renderer::RenderTargetTag> rt)
{
    if (ctx.renderer == nullptr || ctx.resources == nullptr) return Outcome::Err("NO_RENDERER", "レンダラーが未初期化です");
    if (!rt.IsValid()) return Outcome::Err("NO_VIEWPORT", "Scene View RT が未生成です");

    std::vector<uint8_t> png;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!ctx.renderer->CaptureRenderTargetToPng(rt, *ctx.resources, png, width, height) || png.empty()) {
        return Outcome::Err("CAPTURE_FAILED", "viewport のキャプチャに失敗しました");
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("mimeType", JsonValue("image/png"));
    result.Set("base64", JsonValue(Base64Encode(png)));
    result.Set("width", JsonValue(static_cast<int>(width)));
    result.Set("height", JsonValue(static_cast<int>(height)));
    return Outcome::Ok(std::move(result));
}

/// @brief PNG に描画入力時点の GameObject 座標と投影ピクセル位置を添える。
void AddSemanticViewportData(JsonValue& result, const RenderCaptureFrame& source)
{
    const float width = static_cast<float>(result.Find("width")->AsNumber());
    const float height = static_cast<float>(result.Find("height")->AsNumber());
    const math::Matrix4 viewProjection = source.camera.GetViewProjection();

    JsonValue objects = JsonValue::MakeArray();
    for (const auto& go : source.objects) {
        const math::Vector3 worldPosition = go.position;
        const math::Vector4 clip = viewProjection * math::Vector4(worldPosition, 1.0f);
        const bool inFront = clip.w > 0.00001f;
        const float ndcX = inFront ? clip.x / clip.w : 0.0f;
        const float ndcY = inFront ? clip.y / clip.w : 0.0f;
        const float ndcZ = inFront ? clip.z / clip.w : 0.0f;
        const bool visible = inFront && ndcX >= -1.0f && ndcX <= 1.0f
            && ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f;

        JsonValue object = JsonValue::MakeObject();
        object.Set("id", JsonValue(go.id));
        object.Set("name", JsonValue(go.name));
        object.Set("worldPosition", VectorToJson(worldPosition));
        object.Set("visible", JsonValue(visible));
        object.Set("depth", JsonValue(ndcZ));
        JsonValue pixel = JsonValue::MakeArray();
        pixel.Push(JsonValue((ndcX * 0.5f + 0.5f) * width));
        pixel.Push(JsonValue((1.0f - (ndcY * 0.5f + 0.5f)) * height));
        object.Set("pixel", std::move(pixel));
        if (!go.parent.empty()) object.Set("parent", JsonValue(go.parent));
        objects.Push(std::move(object));
    }
    result.Set("objects", std::move(objects));
    result.Set("cameraPosition", VectorToJson(source.camera.m_position));
}

/// @brief Play 中のゲームへ仮想入力を注入する (input.inject)。
Outcome DoInputInject(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    if (ctx.playMode == nullptr) return Outcome::Err("NO_PLAY_MODE", "PlayModeController が未設定です");
    const std::string kind = StringField(payload, "kind");
    if (kind != "clear" && ctx.playMode->IsInEditor()) {
        return Outcome::Err("INVALID_PLAY_STATE", "入力注入は Play/Pause 中だけ実行できます");
    }
    if (dryRun) return DryRunPreview("input.inject:" + kind);
    bool applied = false;
    if (kind == "clear") {
        input::Input::ClearInjected();
        applied = true;
    } else if (kind == "key") {
        const int key = VirtualKeyFromName(StringField(payload, "key"));
        const JsonValue* pressed = payload.Find("pressed");
        if (key < 0 || pressed == nullptr || !pressed->IsBool()) return Outcome::Err("BAD_ARG", "有効な key / pressed が必要です");
        applied = input::Input::InjectKey(static_cast<uint32_t>(key), pressed->AsBool());
    } else if (kind == "axis") {
        const std::string axis = StringField(payload, "axis");
        const JsonValue* value = payload.Find("value");
        if (value == nullptr || !value->IsNumber()) return Outcome::Err("BAD_ARG", "axis / value が必要です");
        applied = input::Input::SetVirtualAxis(axis, static_cast<float>(value->AsNumber()));
    } else if (kind == "gamepadAxis") {
        const std::string axis = StringField(payload, "axis");
        const JsonValue* value = payload.Find("value");
        if (value == nullptr || !value->IsNumber()) return Outcome::Err("BAD_ARG", "axis / value が必要です");
        applied = input::Input::SetVirtualAxis(axis, static_cast<float>(value->AsNumber()));
    } else if (kind == "gamepadButton") {
        const std::string buttonName = StringField(payload, "buttonName");
        const JsonValue* pressed = payload.Find("pressed");
        if (pressed == nullptr || !pressed->IsBool()) return Outcome::Err("BAD_ARG", "buttonName / pressed が必要です");
        applied = input::Input::SetVirtualButton(buttonName, pressed->AsBool());
    } else if (kind == "mouseButton") {
        const JsonValue* button = payload.Find("button");
        const JsonValue* pressed = payload.Find("pressed");
        if (button == nullptr || !button->IsNumber() || pressed == nullptr || !pressed->IsBool()) {
            return Outcome::Err("BAD_ARG", "button / pressed が必要です");
        }
        applied = input::Input::InjectMouseButton(button->AsInt(), pressed->AsBool());
    } else if (kind == "mousePosition" || kind == "mouseDelta") {
        const JsonValue* value = payload.Find("value");
        if (value == nullptr || !value->IsArray() || value->AsArray().size() < 2
            || !value->AsArray()[0].IsNumber() || !value->AsArray()[1].IsNumber()) {
            return Outcome::Err("BAD_ARG", "value=[x,y] が必要です");
        }
        const math::Vector2 vector{
            static_cast<float>(value->AsArray()[0].AsNumber()),
            static_cast<float>(value->AsArray()[1].AsNumber())
        };
        if (kind == "mousePosition") input::Input::InjectMousePosition(vector);
        else input::Input::InjectMouseDelta(vector);
        applied = true;
    } else if (kind == "mouseScroll") {
        const JsonValue* value = payload.Find("value");
        if (value == nullptr || !value->IsNumber()) return Outcome::Err("BAD_ARG", "value が必要です");
        input::Input::InjectMouseScroll(static_cast<float>(value->AsNumber()));
        applied = true;
    } else {
        return Outcome::Err("BAD_ARG", "未知の input kind: " + kind);
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("applied", JsonValue(applied));
    result.Set("kind", JsonValue(kind));
    return applied ? Outcome::Ok(std::move(result)) : Outcome::Err("INPUT_REJECTED", "入力値が範囲外です");
}

/// @note Play のスナップショットとランタイム設定の所有権は、UI と共通の operator 経路で更新する。
Outcome DoPlayControl(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    const std::string type = "play.control";
    if (ctx.playMode == nullptr || ctx.activeScene == nullptr) {
        return Outcome::Err("NO_PLAY_MODE", "PlayModeController または Scene が未設定です");
    }
    const bool standaloneActive = ctx.standalonePlay && ctx.standalonePlay->IsActive();
    const std::string action = StringField(payload, "action");
    if (dryRun) return DryRunPreview(type + ":" + action);
    const char* operatorId = nullptr;
    if (action == "start") {
        if (ctx.managedBuildBusy) return Outcome::Err("MANAGED_BUILD_BUSY", "C# のビルド適用中は Play を開始できません");
        if (standaloneActive || !ctx.playMode->IsInEditor()) return Outcome::Err("INVALID_PLAY_STATE", "Play は Editor 状態からのみ開始できます");
        operatorId = "play.start";
    } else if (action == "stop") {
        if (!standaloneActive && ctx.playMode->IsInEditor() && !editor::CanInvokeOperator(ctx, "play.stop"))
            return Outcome::Err("INVALID_PLAY_STATE", "Play Mode は開始されていません");
        operatorId = "play.stop";
    } else if (action == "pause") {
        if (!ctx.playMode->IsPlaying()) return Outcome::Err("INVALID_PLAY_STATE", "一時停止は Playing 状態でのみ実行できます");
        operatorId = "play.pause";
    } else if (action == "resume") {
        if (!ctx.playMode->IsPaused()) return Outcome::Err("INVALID_PLAY_STATE", "再開は Paused 状態でのみ実行できます");
        operatorId = "play.pause";
    } else if (action == "step") {
        if (!ctx.playMode->IsPaused()) return Outcome::Err("INVALID_PLAY_STATE", "ステップは Paused 状態でのみ実行できます");
        operatorId = "play.step";
    } else {
        return Outcome::Err("BAD_ARG", "未知の Play action: " + action);
    }
    JsonValue operatorPayload = JsonValue::MakeObject();
    operatorPayload.Set("id", JsonValue(operatorId));
    Outcome invocation = FromBridge(InvokeOperator(ctx, operatorPayload, false));
    if (!invocation.ok) return invocation;
    JsonValue result = JsonValue::MakeObject();
    result.Set("action", JsonValue(action));
    const bool standaloneNow = ctx.standalonePlay && ctx.standalonePlay->IsActive();
    result.Set("playState", JsonValue(standaloneNow ? "standalone" : PlayStateName(*ctx.playMode)));
    result.Set("executionMode", JsonValue(ctx.playExecutionMode == PlayExecutionMode::STANDALONE ? "standalone" : "inEditor"));
    if (ctx.standalonePlay) result.Set("standaloneDiagnostic", JsonValue(ctx.standalonePlay->Diagnostic()));
    result.Set("restorePending", JsonValue(ctx.playMode->HasPendingRestore()));
    return Outcome::Ok(std::move(result));
}

Outcome DoViewportCamera(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    const std::string type = "viewport.camera";
    if (ctx.editorCamera == nullptr) return Outcome::Err("NO_CAMERA", "Scene View カメラが未設定です");
    math::Vector3 position;
    math::Vector3 lookAt;
    const bool hasPosition = ReadVec3(payload, "position", position);
    bool hasLookAt = ReadVec3(payload, "lookAt", lookAt);
    const std::string targetId = StringField(payload, "targetId");
    if (!targetId.empty()) {
        GameObject* target = ctx.activeScene != nullptr ? ctx.activeScene->FindByGuid(targetId) : nullptr;
        if (target == nullptr) return Outcome::Err("NODE_NOT_FOUND", "targetId が見つかりません: " + targetId);
        lookAt = target->transform.worldPosition;
        hasLookAt = true;
    }
    if (!hasPosition && !hasLookAt) return Outcome::Err("BAD_ARG", "position / lookAt / targetId のいずれかが必要です");
    if (dryRun) return DryRunPreview(type);
    if (hasPosition) ctx.editorCamera->m_position = position;
    if (hasLookAt) ctx.editorCamera->LookAt(lookAt);
    JsonValue result = JsonValue::MakeObject();
    result.Set("position", VectorToJson(ctx.editorCamera->m_position));
    result.Set("forward", VectorToJson(ctx.editorCamera->GetForward()));
    if (hasLookAt) result.Set("lookAt", VectorToJson(lookAt));
    return Outcome::Ok(std::move(result));
}

Outcome ReadViewportFrame(BusCall& call, bool semantic)
{
    auto& ctx = call.ctx;
    std::string view = StringField(call.payload, "view");
    if (view.empty()) view = "scene";
    if (view != "scene" && view != "game") return Outcome::Err("BAD_ARG", "view は scene か game で指定してください");
    const auto* requestedFrame = call.payload.Find("minFrame");
    if (requestedFrame && (!requestedFrame->IsInteger() || requestedFrame->AsInt64() < 0))
        return Outcome::Err("BAD_ARG", "minFrame は非負の整数です");
    const auto minFrame = requestedFrame ? static_cast<std::uint64_t>(requestedFrame->AsInt64()) : 0;
    const bool gameView = view == "game";
    const auto target = gameView ? call.state.gameViewportRT : call.state.sceneViewportRT;
    if (!target.IsValid()) return Outcome::Err("NO_VIEWPORT", "選択したビューの Render Target が未生成です");
    const auto index = gameView ? 1 : 0;
    ctx.aiViewportRenderUntilFrame = Time::frameCount + 8;
    if (semantic) ctx.semanticCaptureUntilFrame[index] = ctx.aiViewportRenderUntilFrame;
    const auto& source = ctx.viewportCaptureFrames[index];
    if (!CanReadCaptureFrame(ctx, source, target, minFrame) || (semantic && !source.hasSemantic))
        return Outcome::Err("CAPTURE_NOT_READY", "CAPTURE_NOT_READY: 指定フレーム以降の描画完了を待っています");
    const auto cameraId = StringField(call.payload, "cameraId");
    if (!cameraId.empty() && cameraId != source.cameraId)
        return Outcome::Err("CAMERA_MISMATCH", "指定したカメラはこのビューの描画に使われていません");

    auto outcome = DoViewportCapture(ctx, target);
    if (!outcome.ok) return outcome;
    outcome.result.Set("view", JsonValue(view));
    if (!semantic && !source.cameraId.empty()) outcome.result.Set("cameraId", JsonValue(source.cameraId));
    outcome.result.Set("provenance", source.Describe<JsonValue>("ViewportOutput", "Output"));
    if (semantic) AddSemanticViewportData(outcome.result, source);
    return outcome;
}
} /// @note namespace

void RegisterPlayHandlers(BusHandlerTable& table)
{
    table.AddQuery("viewport.capture", [](BusCall& call) { return ReadViewportFrame(call, false); });
    table.AddQuery("viewport.semantic", [](BusCall& call) { return ReadViewportFrame(call, true); });

    table.AddCommand("input.inject", [](BusCall& call) { return DoInputInject(call.ctx, call.payload, call.dryRun); });
    table.AddCommand("play.control", [](BusCall& call) { return DoPlayControl(call.ctx, call.payload, call.dryRun); });
    table.AddCommand("viewport.camera", [](BusCall& call) { return DoViewportCamera(call.ctx, call.payload, call.dryRun); });
}

} /// @note namespace fbzz::editor::ai::bus
