// FBZZ Engine
// ScriptObjectFactory.cpp | fbzz::editor
#include <Editor/Util/ScriptObjectFactory.hpp>

// WHY このヘッダを引くか: コンポーネントの「既定値付き追加」(コライダーの自動フィット、
//      RigidBody の既定質量、Animator の随伴 SkinnedMeshRenderer) は
//      AddRegisteredComponent<T> にしか無い。ここで go.AddComponent<T>() を直に呼ぶと、
//      Add Component メニューから付けたものと中身の違うオブジェクトが混ざる。
//      重いヘッダなので、依存はこの 1 TU に閉じ込める。
#include <Panels/Inspector/InspectorCommon.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>

#include <algorithm>

namespace fbzz::editor {

namespace {

// "EnemyComponent" → "Enemy"。スクリプト名の慣習サフィックスは GameObject 名としては冗長。
std::string ObjectNameFromScriptType(const std::string& typeName)
{
    constexpr std::string_view kSuffix = "Component";
    if (typeName.size() > kSuffix.size() && typeName.ends_with(kSuffix))
        return typeName.substr(0, typeName.size() - kSuffix.size());
    return typeName;
}

} // namespace

std::vector<std::string> ScriptObjectTypeNames()
{
    std::vector<std::string> names = scene::ScriptFactory::RegisteredTypeNames();
    std::sort(names.begin(), names.end());
    return names;
}

scene::GameObject* CreateScriptObject(EditorContext& ctx,
                                      const std::string& scriptTypeName,
                                      scene::EntityID parent)
{
    if (!ctx.activeScene || scriptTypeName.empty()) return nullptr;

    // 先に生成を試す。DLL 未ロード時などは登録が空なので、GameObject を作る前に弾く
    // (中身の無いオブジェクトがシーンへ残らないようにする)。
    auto script = scene::ScriptFactory::Create(scriptTypeName);
    if (!script) return nullptr;

    scene::GameObject& go =
        ctx.activeScene->CreateGameObject(ObjectNameFromScriptType(scriptTypeName));

    // 要求コンポーネントを先に付けてからスクリプトを載せる。
    // WHY 順序: コライダーの自動フィットは「その時点で付いているメッシュ」を見るため、
    //      並び自体に依存は無い。ただし OnValidate をスクリプト側で回すのは全部揃った
    //      後にしたいので、コンポーネント → スクリプトの順に固定しておく。
    for (const std::string& typeName : script->RequiredComponents())
        AddRegisteredComponentByName(go, typeName);

    auto* sc = go.GetComponent<scene::ScriptComponent>();
    if (!sc) sc = &go.AddComponent<scene::ScriptComponent>();

    script->SetContext(ctx.activeScene, &go);
    script->Reset();
    script->OnValidate();
    sc->scripts.emplace_back().script = std::move(script);

    if (parent.IsValid()) {
        if (auto* parentGo = ctx.activeScene->GetGameObject(parent))
            go.SetParent(parentGo);
    }

    SelectEntity(ctx, go.GetID());
    return &go;
}

} // namespace fbzz::editor
