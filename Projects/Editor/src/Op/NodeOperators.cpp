/// @file    NodeOperators.cpp
/// @brief   GameObject そのものを対象にする Operator (リネーム・生成)。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// 生成の実体は ObjectCreation.hpp。Hierarchy・メインメニュー・ホットキー・パレット・
/// AI (editor.op.invoke) はここに登録した Operator を呼ぶ。AI バスの preset.create /
/// node.create は dryRun と transaction のため同じコマンド生成関数を直接使う。
/// @see Docs/design/editor-operator-model.md
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ObjectCreation.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

scene::GameObject* ResolveNode(const OpContext& c, const std::string& nodeId)
{
    if (c.ctx.activeScene == nullptr) return nullptr;
    if (nodeId.empty()) return c.ctx.GetSelectedGO();
    return c.ctx.activeScene->FindByGuid(nodeId);
}

void RegisterRenameOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id        = "node.rename";
    op.label     = "Rename Node";
    op.category  = "Edit";
    op.desc      = "GameObject の名前を変更する。node を省略すると選択中のものが対象。"
                   "Hierarchy のインライン編集と同じ実体を通る。";
    op.kind      = OpKind::Mutation;
    op.undoLabel = "Rename GameObject";

    OpParam nodeParam;
    nodeParam.name     = "node";
    nodeParam.type     = OpParamType::NodeId;
    nodeParam.desc     = "対象の GameObject。省略すると選択中のもの";
    nodeParam.required = false;

    OpParam nameParam;
    nameParam.name = "name";
    nameParam.type = OpParamType::String;
    nameParam.desc = "新しい名前 (空は不可)";
    op.params = { nodeParam, nameParam };

    /// @note 名前が同じかは exec で見る。引数なしのメニュー問い合わせで常に無効化されないため。
    op.poll = [](const OpContext& c, const OpArgs& args) {
        return ResolveNode(c, args.GetString("node")) != nullptr;
    };

    op.exec = [](OpContext& c, const OpArgs& args) -> OpResult {
        scene::GameObject* go = ResolveNode(c, args.GetString("node"));
        if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

        const std::string newName = args.GetString("name");
        if (newName.empty()) return OpResult::Err("BAD_ARG", "name が空です");

        OpResult result;
        result.command = MakeRenameNodeCommand(c.ctx, go->GetID(), newName,
                                               "Rename GameObject", /*applyNow=*/true);
        if (!result.command) {
            result.noChange = true;
            result.message  = "名前は変わりませんでした";
        }
        return result;
    };

    registry.Register(std::move(op));
}

/// @name 生成

/// @brief 生成系 Operator に共通の引数 (parent / name / position / placeInView)。
std::vector<OpParam> CreateCommonParams()
{
    OpParam parent;
    parent.name     = "parent";
    parent.type     = OpParamType::NodeId;
    parent.desc     = "親の GameObject。省略するとルート。Prefab インスタンスの下へ足した子は override として追跡されない";
    parent.required = false;

    OpParam name;
    name.name     = "name";
    name.type     = OpParamType::String;
    name.desc     = "名前。省略すると既定名を兄弟の中で一意化する (\"Cube (1)\")";
    name.required = false;

    OpParam position;
    position.name     = "position";
    position.type     = OpParamType::Vec3;
    position.desc     = "ローカル座標。省略時、子は親の原点、ルートは placeInView に従う";
    position.required = false;

    OpParam placeInView;
    placeInView.name         = "placeInView";
    placeInView.type         = OpParamType::Bool;
    placeInView.desc         = "ルートに置くとき Scene View の注視点へ置く (既定 true)。false なら原点基準";
    placeInView.required     = false;
    placeInView.defaultValue = true;

    return { parent, name, position, placeInView };
}

CreateObjectRequest ReadCreateRequest(const OpArgs& args, CreateObjectSource source, std::string key)
{
    CreateObjectRequest request;
    request.source      = source;
    request.key         = std::move(key);
    request.parentGuid  = args.GetString("parent");
    request.name        = args.GetString("name");
    request.hasPosition = args.Has("position");
    request.position    = args.GetVec3("position");
    request.placeInView = args.GetBool("placeInView", true);
    return request;
}

/// @note 生成物の種類 (preset / path / script) の誤りは exec で理由付きのエラーにする。
///       poll で弾くと NOT_AVAILABLE にしかならず、AI から直し方が読めない。
bool PollCreate(const OpContext& c, const OpArgs& args)
{
    if (c.ctx.activeScene == nullptr) return false;
    const std::string parent = args.GetString("parent");
    return parent.empty() || c.ctx.activeScene->FindByGuid(parent) != nullptr;
}

OpResult ExecCreate(OpContext& c, const CreateObjectRequest& request)
{
    std::string code;
    std::string message;
    if (!ValidateCreateObjectRequest(c.ctx, request, code, message))
        return OpResult::Err(std::move(code), std::move(message));

    auto createdIds = std::make_shared<std::vector<std::string>>();
    OpResult result;
    result.command = MakeCreateObjectCommand(
        c.ctx, request, CreateObjectLabel(request), /*applyNow=*/true,
        [createdIds](const std::vector<std::string>& ids) { *createdIds = ids; });
    if (!result.command)
        return OpResult::Err("CREATE_FAILED", "生成に失敗しました: " + request.key);

    OpData ids = OpData::MakeArray();
    for (const std::string& id : *createdIds) ids.Push(id);
    if (!createdIds->empty()) result.data.Set("id", createdIds->front());
    result.data.Set("ids", std::move(ids));

    const scene::GameObject* parent = request.parentGuid.empty()
        ? nullptr : c.ctx.activeScene->FindByGuid(request.parentGuid);
    if (IsInsidePrefabInstance(parent)) result.message = PrefabInstanceChildWarning();
    return result;
}

EditorOperator MakeCreateOperatorBase(const char* id, const char* label, const char* desc)
{
    EditorOperator op;
    op.id        = id;
    op.label     = label;
    op.category  = "GameObject";
    op.desc      = desc;
    op.caution   = PrefabInstanceChildWarning();
    op.kind      = OpKind::Mutation;
    op.undoLabel = "Create <name>";
    op.poll      = &PollCreate;
    return op;
}

void RegisterCreateOperators(OperatorRegistry& registry)
{
    {
        EditorOperator op = MakeCreateOperatorBase(
            "node.create_empty", "Create Empty",
            "コンポーネントを持たない GameObject を作って選択する (プリセット empty と同じ実体)。");
        op.undoLabel = "Create Empty";
        op.params    = CreateCommonParams();
        op.exec = [](OpContext& c, const OpArgs& args) {
            return ExecCreate(c, ReadCreateRequest(args, CreateObjectSource::Preset, "empty"));
        };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op = MakeCreateOperatorBase(
            "node.create_preset", "Create From Preset",
            "Create メニューのプリセット (preset.catalog) から GameObject を作って選択する。");
        OpParam preset;
        preset.name = "preset";
        preset.type = OpParamType::String;
        preset.desc = "プリセット id (\"3d.cube\" 等)";
        for (const ObjectPreset& entry : ObjectPresetCatalog()) preset.enumValues.emplace_back(entry.id);
        op.params = { preset };
        for (OpParam& common : CreateCommonParams()) op.params.push_back(std::move(common));
        op.exec = [](OpContext& c, const OpArgs& args) {
            return ExecCreate(c, ReadCreateRequest(args, CreateObjectSource::Preset, args.GetString("preset")));
        };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op = MakeCreateOperatorBase(
            "prefab.instantiate", "Instantiate Prefab",
            ".prefab / .vfx をシーンへ実体化して選択する。");
        OpParam path;
        path.name = "path";
        path.type = OpParamType::String;
        path.desc = ".prefab / .vfx のパス (projectRoot 相対か絶対)";
        op.params = { path };
        for (OpParam& common : CreateCommonParams()) op.params.push_back(std::move(common));
        op.exec = [](OpContext& c, const OpArgs& args) {
            return ExecCreate(c, ReadCreateRequest(args, CreateObjectSource::Prefab, args.GetString("path")));
        };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op = MakeCreateOperatorBase(
            "node.create_script_object", "Create Script Object",
            "スクリプトと、それが FBZZ_REQUIRE_COMPONENT で要求するコンポーネント一式を持つ GameObject を作る。");
        OpParam script;
        script.name = "script";
        script.type = OpParamType::String;
        script.desc = "スクリプト型名 (\"EnemyComponent\" 等)";
        op.params = { script };
        for (OpParam& common : CreateCommonParams()) op.params.push_back(std::move(common));
        op.exec = [](OpContext& c, const OpArgs& args) {
            return ExecCreate(c, ReadCreateRequest(args, CreateObjectSource::Script, args.GetString("script")));
        };
        registry.Register(std::move(op));
    }
}

} // namespace

void RegisterNodeOperators(OperatorRegistry& registry)
{
    RegisterRenameOperator(registry);
    RegisterCreateOperators(registry);
}

} // namespace fbzz::editor
