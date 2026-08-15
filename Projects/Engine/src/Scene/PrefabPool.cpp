// FBZZ Engine
// PrefabPool.cpp | fbzz::scene
// プレファブインスタンスの使い回し実装
#include <Engine/Scene/PrefabPool.hpp>

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Core/Logger.hpp>

#include <unordered_map>
#include <vector>

namespace fbzz::scene {
namespace {

// prefabAssetPath → 待機中インスタンス。Scene ごとに独立させる。
// WHY Scene* で分けるか: エディタは編集用 Scene とプレイ用 Scene を同時に持ち得る。
//     待機列を共有すると、片方の Scene の EntityID をもう片方で引いてしまう。
using Buckets = std::unordered_map<std::string, std::vector<EntityID>>;

std::unordered_map<const Scene*, Buckets>& Pools()
{
    static std::unordered_map<const Scene*, Buckets> s_pools;
    return s_pools;
}

// GO とその子孫すべての Script へ通知する。
// WHY 子孫も回すか: 弾のプレファブは「ルート = 挙動スクリプト、子 = エフェクト/コライダー」
//     という構成が普通で、リセットが必要な状態は子側にもある。
void NotifyScripts(Scene& scene, GameObject& gameObject, bool spawned)
{
    if (auto* sc = gameObject.GetComponent<ScriptComponent>()) {
        for (auto& entry : sc->scripts) {
            Script* script = entry.script.get();
            if (!script) continue;
            // 再利用時は OnAwake / OnStart が再発火しないため、コンテキストだけ張り直す。
            script->SetContext(&scene, &gameObject);
            if (spawned)
                script->OnSpawn();
            else
                script->OnDespawn();
        }
    }

    const int childCount = gameObject.GetChildCount();
    for (int i = 0; i < childCount; ++i) {
        if (GameObject* child = gameObject.GetChild(i))
            NotifyScripts(scene, *child, spawned);
    }
}

// プレファブから 1 体作り、ルート GameObject を返す。
GameObject* InstantiateRoot(Scene& scene, const std::string& prefabPath)
{
    std::vector<EntityID> roots;
    if (!Script::InvokePrefabInstantiate(scene, prefabPath, roots) || roots.empty()) {
        FBZZ_LOG_WARN("PrefabPool: instantiate failed -> %s", prefabPath.c_str());
        return nullptr;
    }
    return scene.GetGameObject(roots.front());
}

} // namespace

GameObject* PrefabPool::Spawn(Scene& scene,
                              const std::string& prefabPath,
                              math::Vector3 position,
                              math::Quaternion rotation)
{
    if (prefabPath.empty()) return nullptr;

    GameObject* instance = nullptr;

    auto& bucket = Pools()[&scene][prefabPath];
    while (!bucket.empty()) {
        const EntityID id = bucket.back();
        bucket.pop_back();
        // 待機中に別経路で破棄されていることがあるため、必ず生存確認してから使う。
        if (GameObject* pooled = scene.GetGameObject(id)) {
            instance = pooled;
            break;
        }
    }

    if (!instance) {
        instance = InstantiateRoot(scene, prefabPath);
        if (!instance) return nullptr;
        // Instantiate 経由なら prefabAssetPath は書かれているはずだが、
        // Despawn の鍵になる値なので念のため保証しておく。
        if (instance->prefabAssetPath.empty())
            instance->prefabAssetPath = prefabPath;
    }

    instance->transform.position = position;
    instance->transform.rotation = rotation;
    instance->SetActive(true);
    NotifyScripts(scene, *instance, /*spawned=*/true);
    return instance;
}

bool PrefabPool::Despawn(Scene& scene, GameObject& gameObject)
{
    const std::string prefabPath = gameObject.prefabAssetPath;
    if (prefabPath.empty()) return false;

    NotifyScripts(scene, gameObject, /*spawned=*/false);
    gameObject.SetActive(false);

    auto& bucket = Pools()[&scene][prefabPath];
    // 二重 Despawn で同じ実体が 2 回配られるのを防ぐ。
    const EntityID id = gameObject.GetID();
    for (const EntityID pooled : bucket)
        if (pooled == id) return true;

    bucket.push_back(id);
    return true;
}

int PrefabPool::Prewarm(Scene& scene, const std::string& prefabPath, int count)
{
    if (prefabPath.empty() || count <= 0) return 0;

    int created = 0;
    for (int i = 0; i < count; ++i) {
        GameObject* instance = InstantiateRoot(scene, prefabPath);
        if (!instance) break;
        if (instance->prefabAssetPath.empty())
            instance->prefabAssetPath = prefabPath;
        // OnSpawn を通さずに直接待機列へ入れる (まだ「出していない」ため)。
        instance->SetActive(false);
        Pools()[&scene][prefabPath].push_back(instance->GetID());
        ++created;
    }
    return created;
}

size_t PrefabPool::AvailableCount(const Scene& scene, const std::string& prefabPath)
{
    const auto sceneIt = Pools().find(&scene);
    if (sceneIt == Pools().end()) return 0;
    const auto bucketIt = sceneIt->second.find(prefabPath);
    return bucketIt == sceneIt->second.end() ? 0 : bucketIt->second.size();
}

void PrefabPool::Clear(const Scene& scene)
{
    Pools().erase(&scene);
}

void PrefabPool::ClearAll()
{
    Pools().clear();
}

} // namespace fbzz::scene
