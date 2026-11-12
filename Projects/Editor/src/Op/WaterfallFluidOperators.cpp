/// @file    WaterfallFluidOperators.cpp
/// @brief   Bake a selected waterfall on a worker and publish the validated result with Undo.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/WaterfallFluidBake.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/WaterfallFluidClip.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/WaterfallFluid.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <chrono>
#include <filesystem>
#include <memory>

namespace fbzz::editor {
namespace {
struct Progress {
    std::atomic<bool> cancel = false;
    std::atomic<int> percent = 0;
};
struct BakeJob {
    std::shared_ptr<Progress> progress;
    std::future<std::string> result;
    std::filesystem::path staged;
    std::string project;
    std::string node;
    std::string originalClip;
    std::string assetName;
    const scene::Scene* scene = nullptr;
    std::uint64_t generation = 0;
    std::uint64_t lifetime = 0;
    std::uint64_t stamp = 0;
    std::string status;
};
BakeJob g_bake;

bool CanAuthor(const EditorContext& ctx)
{
    return ctx.activeScene && !ctx.projectRoot.empty() && (!ctx.playMode || ctx.playMode->IsInEditor());
}

scene::GameObject* ResolveNode(const EditorContext& ctx, const OpArgs& args)
{
    const auto node = args.GetString("node");
    return node.empty() ? ctx.GetSelectedGO() : ctx.activeScene->FindByGuid(node);
}

void RemoveStaged()
{
    std::error_code ignored;
    std::filesystem::remove(g_bake.staged, ignored);
}
} /// @note namespace

std::string WaterfallFluidBakeStatus(std::string_view node)
{
    if (g_bake.node != node) return {};
    if (g_bake.result.valid())
        return g_bake.progress->cancel.load() ? "Canceling 3D fluid bake..."
            : "Baking 3D fluid: " + std::to_string(g_bake.progress->percent.load()) + "%";
    return g_bake.status;
}

void PollWaterfallFluidBake(EditorContext& ctx)
{
    if (!g_bake.result.valid()) return;
    const bool sameScene = CanAuthor(ctx) && ctx.activeScene == g_bake.scene && ctx.projectRoot == g_bake.project
        && ctx.activeScene->GetRenderSceneGeneration() == g_bake.generation;
    if (!sameScene) g_bake.progress->cancel.store(true);
    if (g_bake.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    g_bake.status = g_bake.result.get();
    if (g_bake.progress->cancel.load() || !g_bake.status.empty()) {
        if (g_bake.status.empty()) g_bake.status = "Bake canceled.";
        RemoveStaged();
        return;
    }
    auto* owner = ctx.activeScene->FindByGuid(g_bake.node);
    auto* waterfall = owner ? owner->GetComponent<scene::WaterfallComponent>() : nullptr;
    asset::WaterfallFluidBakeInput current;
    std::string error;
    scene::FlushWorldTransforms(*ctx.activeScene);
    if (!waterfall || ctx.activeScene->GetComponentLifetime<scene::WaterfallComponent>(owner->GetID()) != g_bake.lifetime
        || waterfall->fluidClip != g_bake.originalClip
        || !scene::CaptureWaterfallFluidBake(*ctx.activeScene, *owner, current, error) || current.stamp != g_bake.stamp) {
        g_bake.status = "Waterfall inputs changed during the bake. Bake again to use the new settings.";
        RemoveStaged();
        return;
    }
    const std::string relative = "Assets/Generated/Waterfalls/" + g_bake.assetName;
    const auto output = util::FileSystem::PathFromUtf8(ctx.projectRoot) / relative;
    if (!util::FileSystem::EnsureDirectory(output.parent_path())) {
        g_bake.status = "Could not create Assets/Generated/Waterfalls.";
        RemoveStaged();
        return;
    }
    std::error_code moveError;
    std::filesystem::rename(g_bake.staged, output, moveError);
    if (moveError) {
        g_bake.status = "Could not publish the baked 3D fluid asset.";
        RemoveStaged();
        return;
    }
    const auto guid = asset::AssetDatabase::GuidFromPath(util::FileSystem::PathToUtf8(output));
    if (guid.empty()) {
        g_bake.status = "Could not register the baked 3D fluid asset.";
        return;
    }
    const std::string next = "guid:" + guid + "|" + relative;
    const auto assign = [context = &ctx, generation = g_bake.generation, node = g_bake.node,
        lifetime = g_bake.lifetime](const std::string& value, std::string& reason) {
        auto* world = context->activeScene;
        auto* object = world && world->GetRenderSceneGeneration() == generation ? world->FindByGuid(node) : nullptr;
        auto* component = object ? object->GetComponent<scene::WaterfallComponent>() : nullptr;
        if (!component || world->GetComponentLifetime<scene::WaterfallComponent>(object->GetID()) != lifetime) {
            reason = "The waterfall is no longer available.";
            return false;
        }
        component->fluidClip = value;
        if (context->markSceneDirty) context->markSceneDirty();
        return true;
    };
    const auto previous = waterfall->fluidClip;
    if (ctx.undoStack) {
        auto command = std::make_unique<CheckedLambdaCommand>("Assign 3D Waterfall Fluid",
            [assign, next](std::string& reason) { return assign(next, reason); },
            [assign, previous](std::string& reason) { return assign(previous, reason); });
        if (!ctx.undoStack->Execute(std::move(command))) {
            g_bake.status = "The asset was saved. Assign it in Fluid Clip.";
            return;
        }
    } else {
        static_cast<void>(assign(next, error));
    }
    g_bake.status = "3D fluid baked and assigned.";
}

void RegisterWaterfallFluidOperators(OperatorRegistry& registry)
{
    for (const std::string action : {"bake", "cancel", "status"}) {
        EditorOperator op;
        op.id = "waterfall.fluid." + action;
        op.label = action == "bake" ? "Bake Waterfall 3D Fluid" : "Waterfall Fluid " + action;
        op.category = "Terrain & Water";
        op.desc = "Bake, cancel or inspect the selected waterfall's local liquid surface.";
        op.kind = action == "status" ? OpKind::Query : OpKind::Action;
        OpParam node;
        node.name = "node";
        node.required = false;
        op.params.push_back(std::move(node));
        op.poll = [action](const OpContext& context, const OpArgs& args) {
            if (!context.ctx.activeScene || (action != "status" && !CanAuthor(context.ctx))) return false;
            auto* owner = ResolveNode(context.ctx, args);
            if (!owner || !owner->GetComponent<scene::WaterfallComponent>()) return false;
            if (action == "status") return true;
            if (action == "cancel") return g_bake.result.valid() && g_bake.node == owner->instanceId;
            return !g_bake.result.valid();
        };
        op.exec = [action](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            if (!ctx.activeScene || (action != "status" && !CanAuthor(ctx)))
                return OpResult::Err("NO_EDIT_SCENE", "Open the edit scene before baking.");
            auto* owner = ResolveNode(ctx, args);
            auto* waterfall = owner ? owner->GetComponent<scene::WaterfallComponent>() : nullptr;
            if (!waterfall) return OpResult::Err("NO_WATERFALL", "Select a Waterfall component.");
            if (action == "status") {
                OpData data = OpData::MakeObject();
                data.Set("bake", OpData(WaterfallFluidBakeStatus(owner->instanceId)));
                data.Set("playback", OpData(waterfall->fluidStatus));
                data.Set("clip", OpData(waterfall->fluidClip));
                return OpResult::Data(std::move(data));
            }
            if (action == "cancel") {
                if (g_bake.result.valid() && g_bake.node == owner->instanceId) g_bake.progress->cancel.store(true);
                return OpResult{};
            }
            if (g_bake.result.valid()) return OpResult::Err("WATERFALL_BUSY", "A waterfall bake is running.");

            asset::WaterfallFluidBakeInput input;
            std::string error;
            scene::FlushWorldTransforms(*ctx.activeScene);
            if (!scene::CaptureWaterfallFluidBake(*ctx.activeScene, *owner, input, error)) {
                g_bake.node = owner->instanceId;
                g_bake.status = error;
                return OpResult::Err("WATERFALL_INPUT", error);
            }
            BakeJob job;
            job.progress = std::make_shared<Progress>();
            job.assetName = "Waterfall-" + asset::AssetDatabase::GenerateGuid() + ".wfall";
            const auto stage = util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Library" / "WaterfallBake";
            if (!util::FileSystem::EnsureDirectory(stage))
                return OpResult::Err("WATERFALL_STAGE", "Could not create the waterfall bake staging directory.");
            job.staged = stage / job.assetName;
            job.project = ctx.projectRoot;
            job.node = owner->instanceId;
            job.scene = ctx.activeScene;
            job.generation = ctx.activeScene->GetRenderSceneGeneration();
            job.lifetime = ctx.activeScene->GetComponentLifetime<scene::WaterfallComponent>(owner->GetID());
            job.originalClip = waterfall->fluidClip;
            job.stamp = input.stamp;
            job.result = TaskSystem::Submit([input = std::move(input), path = util::FileSystem::PathToUtf8(job.staged),
                progress = job.progress] {
                return asset::BakeWaterfallFluidClip(input, path, progress->cancel, progress->percent);
            });
            g_bake = std::move(job);
            return OpResult{};
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
