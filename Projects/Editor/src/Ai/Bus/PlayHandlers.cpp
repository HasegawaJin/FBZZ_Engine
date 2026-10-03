/// @file    PlayHandlers.cpp
/// @brief   Play 制御・入力注入・ビューポートの撮影とカメラ (play.* / input.* / viewport.*)。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Ai/OperatorBridge.hpp>
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

/// @brief Game View の実 RT と同じアスペクト比・カメラ解決規則で、意味付き投影用 Camera を再構成する。
/// @note Scene View カメラや独自の CameraComponent 選択を使うと、描画画像と pixel 座標が一致しない。
bool BuildGameViewportCamera(editor::EditorContext& ctx,
                             renderer::ResourceHandle<renderer::RenderTargetTag> rt,
                             renderer::Camera& camera)
{
    if (ctx.activeScene == nullptr || ctx.editorCamera == nullptr || ctx.resources == nullptr || !rt.IsValid()) {
        return false;
    }
    const auto* renderTarget = ctx.resources->Get(rt);
    if (renderTarget == nullptr || renderTarget->GetHeight() == 0) return false;
    const float aspect = static_cast<float>(renderTarget->GetWidth())
        / static_cast<float>(renderTarget->GetHeight());
    camera = scene::ResolveEditorGameCamera(*ctx.activeScene, *ctx.editorCamera, aspect);
    return true;
}

/// @brief PNG に各 GameObject のワールド座標と投影ピクセル座標を添える。
Outcome DoSemanticViewportCapture(editor::EditorContext& ctx,
                                  renderer::ResourceHandle<renderer::RenderTargetTag> rt,
                                  const renderer::Camera& camera,
                                  const char* viewName)
{
    Outcome capture = DoViewportCapture(ctx, rt);
    if (!capture.ok) return capture;
    if (ctx.activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* widthValue = capture.result.Find("width");
    const JsonValue* heightValue = capture.result.Find("height");
    const float width = widthValue != nullptr ? static_cast<float>(widthValue->AsNumber()) : 0.0f;
    const float height = heightValue != nullptr ? static_cast<float>(heightValue->AsNumber()) : 0.0f;
    const math::Matrix4 viewProjection = camera.GetViewProjection();

    JsonValue objects = JsonValue::MakeArray();
    for (GameObject& go : ctx.activeScene->GameObjects()) {
        const math::Vector3 worldPosition = go.transform.worldPosition;
        const math::Vector4 clip = viewProjection * math::Vector4(worldPosition, 1.0f);
        const bool inFront = clip.w > 0.00001f;
        const float ndcX = inFront ? clip.x / clip.w : 0.0f;
        const float ndcY = inFront ? clip.y / clip.w : 0.0f;
        const float ndcZ = inFront ? clip.z / clip.w : 0.0f;
        const bool visible = inFront && ndcX >= -1.0f && ndcX <= 1.0f
            && ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f;

        JsonValue object = JsonValue::MakeObject();
        object.Set("id", JsonValue(go.instanceId));
        object.Set("name", JsonValue(go.name));
        object.Set("worldPosition", VectorToJson(worldPosition));
        object.Set("visible", JsonValue(visible));
        object.Set("depth", JsonValue(ndcZ));
        JsonValue pixel = JsonValue::MakeArray();
        pixel.Push(JsonValue((ndcX * 0.5f + 0.5f) * width));
        pixel.Push(JsonValue((1.0f - (ndcY * 0.5f + 0.5f)) * height));
        object.Set("pixel", std::move(pixel));
        if (GameObject* parent = go.GetParent()) object.Set("parent", JsonValue(parent->instanceId));
        objects.Push(std::move(object));
    }
    capture.result.Set("objects", std::move(objects));
    capture.result.Set("cameraPosition", VectorToJson(camera.m_position));
    capture.result.Set("view", JsonValue(viewName));
    return capture;
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
    const std::string action = StringField(payload, "action");
    if (dryRun) return DryRunPreview(type + ":" + action);
    const char* operatorId = nullptr;
    if (action == "start") {
        if (ctx.scriptReloadBusy) return Outcome::Err("SCRIPT_RELOAD_BUSY", "Script のビルドまたは再読み込み中です");
        if (!ctx.playMode->IsInEditor()) return Outcome::Err("INVALID_PLAY_STATE", "Play は Editor 状態からのみ開始できます");
        operatorId = "play.start";
    } else if (action == "stop") {
        if (ctx.playMode->IsInEditor()) return Outcome::Err("INVALID_PLAY_STATE", "Play Mode は開始されていません");
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
    result.Set("playState", JsonValue(PlayStateName(*ctx.playMode)));
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

/// @brief Scene / Game ビューの RT を PNG で返す。隠れているビューも数フレーム描かせる。
Outcome DoViewportCaptureQuery(BusCall& call)
{
    editor::EditorContext& ctx = call.ctx;
    const JsonValue& payload = call.payload;
    Outcome outcome = Outcome::Err("UNKNOWN", "未対応の要求です");
    std::string view = StringField(payload, "view");
    if (view.empty()) view = "scene";
    /// @note .vfx はプレファブになり Prefab 編集モードで通常のシーンとして開くため、専用のプレビュー面がなく撮る対象も scene ビューになる。
    if (view != "scene" && view != "game") {
        outcome = Outcome::Err("BAD_ARG", "view は scene か game で指定してください");
    } else {
        /// @note EditorApp は画面に出ているビューポートしか描かない。キャプチャ中は隠れているビューも描き続けさせ、連続キャプチャで古い絵を掴まないための猶予にする。
        ctx.aiViewportRenderUntilFrame = Time::frameCount + 8;
        const auto target = view == "game" ? call.state.gameViewportRT : call.state.sceneViewportRT;
        outcome = DoViewportCapture(ctx, target);
        if (outcome.ok) outcome.result.Set("view", JsonValue(view));
    }
    return outcome;
}

Outcome DoViewportSemanticQuery(BusCall& call)
{
    editor::EditorContext& ctx = call.ctx;
    const JsonValue& payload = call.payload;
    Outcome outcome = Outcome::Err("UNKNOWN", "未対応の要求です");
    std::string view = StringField(payload, "view");
    if (view.empty()) view = "scene";
    /// @note viewport.capture と同じ理由で、隠れているビューも描き続けさせる。
    ctx.aiViewportRenderUntilFrame = Time::frameCount + 8;
    if (view != "scene" && view != "game") {
        outcome = Outcome::Err("BAD_ARG", "view は scene または game で指定してください");
    } else if (view == "scene" && ctx.editorCamera == nullptr) {
        outcome = Outcome::Err("NO_CAMERA", "Scene Viewカメラが未設定です");
    } else if (view == "scene") {
        outcome = DoSemanticViewportCapture(ctx,
            call.state.sceneViewportRT, *ctx.editorCamera, "scene");
    } else if (ctx.activeScene == nullptr) {
        outcome = Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    } else {
        renderer::Camera gameCamera;
        if (!BuildGameViewportCamera(ctx, call.state.gameViewportRT, gameCamera)) {
            outcome = Outcome::Err("NO_CAMERA", "Game ViewカメラまたはRenderTargetが未設定です");
        } else {
            outcome = DoSemanticViewportCapture(ctx, call.state.gameViewportRT, gameCamera, "game");
        }
    }
    return outcome;
}
} /// @note namespace

void RegisterPlayHandlers(BusHandlerTable& table)
{
    table.AddQuery("viewport.capture", DoViewportCaptureQuery);
    table.AddQuery("viewport.semantic", DoViewportSemanticQuery);

    table.AddCommand("input.inject", [](BusCall& call) { return DoInputInject(call.ctx, call.payload, call.dryRun); });
    table.AddCommand("play.control", [](BusCall& call) { return DoPlayControl(call.ctx, call.payload, call.dryRun); });
    table.AddCommand("viewport.camera", [](BusCall& call) { return DoViewportCamera(call.ctx, call.payload, call.dryRun); });
}

} /// @note namespace fbzz::editor::ai::bus
