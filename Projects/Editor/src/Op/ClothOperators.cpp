/// @file    ClothOperators.cpp
/// @brief   静的・スキンメッシュの布アセット書き出しと固定点編集。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <charconv>
#include <sstream>

namespace fbzz::editor {
void RegisterClothOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "cloth.export_mesh";
    op.label = "Export Selected Mesh as Cloth";
    op.category = "Cloth";
    op.desc = "選択した静的またはスキンメッシュを Cloth に保存する。スキンはボーン名・逆バインド・4 ウェイトも転送する。";
    op.caution = "頂点対応は 1:1。スキンの submesh はローカルスロット番号。使用時は Cloth の skinTarget に元の Renderer を指定する。";
    op.kind = OpKind::Action;
    OpParam submesh;
    submesh.name = "submesh"; submesh.type = OpParamType::Int; submesh.required = false; submesh.defaultValue = 0;
    op.params = {submesh};
    op.poll = [](const OpContext& context, const OpArgs& args) {
        auto* go = context.ctx.GetSelectedGO();
        if (!go || (context.ctx.playMode && !context.ctx.playMode->IsInEditor()) || context.ctx.projectRoot.empty()
            || go->GetComponent<scene::ClothComponent>()) return false;
        if (const auto* skin = go->GetComponent<scene::SkinnedMeshRenderer>())
            return skin->model && skin->model->skeleton && args.GetInt("submesh", 0) >= 0
                && skin->SubmeshMesh(static_cast<size_t>(args.GetInt("submesh", 0)));
        const auto* renderer = go->GetComponent<scene::MeshRenderer>();
        return renderer && renderer->mesh && !renderer->mesh->isSkinned;
    };
    op.exec = [](OpContext& context, const OpArgs& args) {
        auto* go = context.ctx.GetSelectedGO();
        auto* meshRenderer = go ? go->GetComponent<scene::MeshRenderer>() : nullptr;
        auto* skin = go ? go->GetComponent<scene::SkinnedMeshRenderer>() : nullptr;
        asset::ClothAsset cloth;
        bool created = false;
        if (skin) {
            const int slot = args.GetInt("submesh", 0);
            const auto* mesh = slot >= 0 ? skin->SubmeshMesh(static_cast<size_t>(slot)) : nullptr;
            if (!mesh || !skin->model || !skin->model->skeleton)
                return OpResult::Err("NO_MESH", "有効なスキンメッシュの submesh を指定してください。");
            created = asset::CreateSkinnedClothAsset(*mesh, *skin->model->skeleton, {}, cloth);
        } else if (meshRenderer && meshRenderer->mesh) {
            created = asset::CreateClothAsset(*meshRenderer->mesh, {}, cloth);
        } else return OpResult::Err("NO_MESH", "メッシュを選択してください。");
        if (!created)
            return OpResult::Err("INVALID_MESH", "形状・ボーン名・バインド行列・ウェイトを確認してください。モーフは転送しません。");
        const auto path = util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / "Assets" / "Cloth"
            / (asset::AssetDatabase::GenerateGuid() + ".cloth");
        const std::string filename = util::FileSystem::PathToUtf8(path);
        if (!asset::SaveClothAssetToFile(filename, cloth)) return OpResult::Err("WRITE_FAILED", "布アセットを保存できませんでした。");
        const std::string guid = asset::AssetDatabase::GuidFromPath(filename);
        if (guid.empty()) return OpResult::Err("META_FAILED", "布は保存済みですが .meta を作成できませんでした: " + filename);
        context.ctx.requestAssetBrowserRefresh = true;
        context.ctx.requestRevealAssetPath = filename;
        context.ctx.requestRevealAssetSelect = true;
        OpResult result;
        result.message = "Cloth saved: guid:" + guid + " (" + filename + ")";
        return result;
    };
    registry.Register(std::move(op));

    EditorOperator toggle;
    toggle.id = "cloth.paint_mode";
    toggle.label = "Paint Cloth Pins";
    toggle.category = "Cloth";
    toggle.desc = "選択した布の固定点ブラシを切り替える。Shift で解除、奥の頂点も対象。";
    toggle.kind = OpKind::Action;
    toggle.poll = [](const OpContext& c, const OpArgs&) {
        auto* go = c.ctx.GetSelectedGO();
        return c.ctx.clothPinPainting || (go && go->GetComponent<scene::ClothComponent>()
            && (!c.ctx.playMode || c.ctx.playMode->IsInEditor()));
    };
    toggle.checked = [](const OpContext& c, const OpArgs&) { return c.ctx.clothPinPainting; };
    toggle.exec = [](OpContext& c, const OpArgs&) {
        c.ctx.clothPinPainting = !c.ctx.clothPinPainting;
        return OpResult::Ok();
    };
    registry.Register(std::move(toggle));

    EditorOperator paint;
    paint.id = "cloth.paint_pins";
    paint.label = "Apply Cloth Pin Stroke";
    paint.category = "Cloth";
    paint.desc = "空白区切りの物理質点番号を固定・解除する。1 ストロークを 1 件の Undo にする。";
    paint.kind = OpKind::Mutation;
    paint.undoLabel = "Paint Cloth Pins";
    OpParam node;
    node.name = "node"; node.type = OpParamType::NodeId; node.required = false;
    OpParam particles;
    particles.name = "particles"; particles.type = OpParamType::String;
    OpParam pin;
    pin.name = "pin"; pin.type = OpParamType::Bool; pin.required = false; pin.defaultValue = true;
    paint.params = {node, particles, pin};
    paint.poll = [](const OpContext& c, const OpArgs&) {
        return c.ctx.activeScene && (!c.ctx.playMode || c.ctx.playMode->IsInEditor());
    };
    paint.exec = [](OpContext& c, const OpArgs& args) {
        auto* go = args.GetString("node").empty() ? c.ctx.GetSelectedGO() : c.ctx.activeScene->FindByGuid(args.GetString("node"));
        auto* cloth = go ? go->GetComponent<scene::ClothComponent>() : nullptr;
        if (!cloth || !cloth->enabled || !cloth->runtime.initialized || cloth->runtime.failed)
            return OpResult::Err("NOT_READY", "描画の初期化が完了した有効な布を選択してください。");
        if (cloth->runtime.appliedAsset != cloth->clothAssetPath
            || (cloth->clothAssetPath.empty() && (cloth->runtime.shape[2] != static_cast<float>(cloth->segments)
                || (!cloth->overridePins && cloth->runtime.shape[4] != static_cast<float>((cloth->pinTop ? 1 : 0) | (cloth->pinLeft ? 2 : 0))))))
            return OpResult::Err("NOT_READY", "形状変更後の再初期化を待ってください。");
        const size_t count = cloth->runtime.restLocal.size();
        std::vector<bool> selected(count, false), fixed(count, false);
        std::istringstream input(args.GetString("particles"));
        std::string token;
        bool any = false;
        while (input >> token) {
            int id = -1;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), id);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || id < 0 || static_cast<size_t>(id) >= count)
                return OpResult::Err("BAD_PARTICLE", "質点番号が不正です。変更は適用されていません。");
            selected[id] = true;
            any = true;
        }
        const bool beforeOverride = cloth->overridePins;
        const auto before = cloth->pinnedParticles;
        if (beforeOverride) {
            for (int id : before) {
                if (id < 0 || static_cast<size_t>(id) >= count) return OpResult::Err("BAD_PINS", "既存の固定点番号を修正してください。");
                fixed[id] = true;
            }
        } else {
            for (uint32_t id : cloth->runtime.pins) {
                if (id >= count) return OpResult::Err("BAD_PINS", "布の再初期化を待ってください。");
                fixed[id] = true;
            }
        }
        const auto original = fixed;
        for (size_t i = 0; i < count; ++i) if (selected[i]) fixed[i] = args.GetBool("pin", true);
        if (!any || fixed == original) { OpResult result; result.noChange = true; return result; }
        std::vector<int> after;
        for (size_t i = 0; i < count; ++i) if (fixed[i]) after.push_back(static_cast<int>(i));
        EditorContext* context = &c.ctx;
        const std::string guid = go->instanceId;
        auto* owner = c.ctx.activeScene;
        const auto apply = [context, owner, guid](bool overridePins, const std::vector<int>& values) {
            if (context->activeScene != owner) return;
            auto* target = owner->FindByGuid(guid);
            auto* component = target ? target->GetComponent<scene::ClothComponent>() : nullptr;
            if (!component) return;
            component->overridePins = overridePins;
            component->pinnedParticles = values;
            if (context->markSceneDirty) context->markSceneDirty();
        };
        apply(true, after);
        OpResult result;
        result.command = std::make_unique<LambdaCommand>("Paint Cloth Pins",
            [apply, after] { apply(true, after); }, [apply, beforeOverride, before] { apply(beforeOverride, before); });
        return result;
    };
    registry.Register(std::move(paint));
}
}
