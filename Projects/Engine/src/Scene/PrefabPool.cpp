/// @file    PrefabPool.cpp
/// @brief   プレファブインスタンスの使い回し実装。
/// @author  Hasegawa Jin
/// @date    2026-08-16
#include <Engine/Scene/PrefabPool.hpp>

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Core/Logger.hpp>

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
namespace {

// prefabAssetPath → 待機中インスタンスと貸し出し中インスタンス。
// WHY Scene* で分けるか: エディタは編集用 Scene とプレイ用 Scene を同時に持ち得る。
//     待機列を共有すると、片方の Scene の EntityID をもう片方で引いてしまう。
struct Bucket {
    // 待機列。末尾から貸し出す (一番最近返ってきたものが一番キャッシュに乗っている)。
    std::vector<EntityID> idle;
    // 貸し出し中。先頭が最古。追い出しはここの先頭から取る。
    std::vector<EntityID> live;
};
using Buckets = std::unordered_map<std::string, Bucket>;

std::unordered_map<const Scene*, Buckets>& Pools()
{
    static std::unordered_map<const Scene*, Buckets> s_pools;
    return s_pools;
}

// 同時数の上限。Scene ではなくプレファブのパスで持つ。
// WHY Scene で分けないか: 上限は «この演出は何発まで重なってよいか» という
//     プレファブ側の性質で、シーンを開き直すたびに設定し直すものではない。
std::unordered_map<std::string, int>& Limits()
{
    static std::unordered_map<std::string, int> s_limits;
    return s_limits;
}

int LimitOf(const std::string& prefabPath)
{
    const auto it = Limits().find(prefabPath);
    return it == Limits().end() ? 0 : it->second;
}

void EraseLive(Bucket& bucket, EntityID id)
{
    for (size_t i = 0; i < bucket.live.size(); ++i) {
        if (bucket.live[i] != id) continue;
        bucket.live.erase(bucket.live.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
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
            if (spawned) {
                script->ExecuteCallback(&Script::OnSpawn, "OnSpawn");
                continue;
            }
            script->ExecuteCallback(&Script::OnDespawn, "OnDespawn");
            // 待機中の «時間で起きる仕事» は前の一生のもの。
            //
            // WHY ここで畳むか: 枠は非アクティブになるだけで Script は生き続け、
            //     ScriptSystem は非アクティブな階層を進めない ─ Invoke もコルーチンも
            //     «破棄» ではなく «一時停止» で残る。畳まないと、次に貸し出された実体で
            //     前の演出の続きが途中から動き出す (モードをまたぐときに
            //     ResetLifecycleState が畳むのと同じ理由)。
            // WHY ResetLifecycleState を使わないか: あちらは購読も畳む。購読は
            //     OnAwake / OnStart で張るのが普通で、再利用では再発火しないため、
            //     外すとその実体は二度とイベントを受け取れなくなる。
            script->CancelInvoke();
            script->StopAllCoroutines();
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
    if (!Script::InstantiatePrefab(scene, prefabPath, roots) || roots.empty()) {
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
    while (!bucket.idle.empty()) {
        const EntityID id = bucket.idle.back();
        bucket.idle.pop_back();
        // 待機中に別経路で破棄されていることがあるため、必ず生存確認してから使う。
        if (GameObject* pooled = scene.GetGameObject(id)) {
            instance = pooled;
            break;
        }
    }

    // 待機列が空で、かつ上限に達しているなら «最古の 1 発» を畳んで奪う。
    // 生存確認で落ちた枠は数に含めない (数えているのは実体ではなく ID なので、
    // ここで詰めないと «居ない枠» が上限を食い続ける)。
    const int limit = LimitOf(prefabPath);
    while (!instance && limit > 0 && static_cast<int>(bucket.live.size()) >= limit) {
        const EntityID oldest = bucket.live.front();
        bucket.live.erase(bucket.live.begin());
        GameObject* victim = scene.GetGameObject(oldest);
        if (!victim) continue;
        // 追い出しも «返却» と同じ手順を通す。OnDespawn を飛ばすと、前の一生の
        // Invoke やコルーチンが次の貸し出し先で動き出す。
        NotifyScripts(scene, *victim, /*spawned=*/false);
        victim->SetActive(false);
        instance = victim;
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
    // bucket への参照はここで取り直す。OnSpawn の中で別のプレファブが Spawn されると
    // バケットの連想配列が rehash され、上で掴んだ参照が無効になる。
    Bucket& liveBucket = Pools()[&scene][prefabPath];
    EraseLive(liveBucket, instance->GetID());
    liveBucket.live.push_back(instance->GetID());
    return instance;
}

bool PrefabPool::Despawn(Scene& scene, GameObject& gameObject)
{
    const std::string prefabPath = gameObject.prefabAssetPath;
    if (prefabPath.empty()) return false;

    NotifyScripts(scene, gameObject, /*spawned=*/false);
    gameObject.SetActive(false);

    auto& bucket = Pools()[&scene][prefabPath];
    const EntityID id = gameObject.GetID();
    EraseLive(bucket, id);
    // 二重 Despawn で同じ実体が 2 回配られるのを防ぐ。
    for (const EntityID pooled : bucket.idle)
        if (pooled == id) return true;

    bucket.idle.push_back(id);
    return true;
}

int PrefabPool::Prewarm(Scene& scene, const std::string& prefabPath, int count)
{
    if (prefabPath.empty() || count <= 0) return 0;

    // 上限を超えて温めても、貸し出された瞬間に追い出される枠が増えるだけ。
    const int limit = LimitOf(prefabPath);
    if (limit > 0) {
        const Bucket& bucket = Pools()[&scene][prefabPath];
        const int held = static_cast<int>(bucket.idle.size() + bucket.live.size());
        count = (std::min)(count, limit - held);
        if (count <= 0) return 0;
    }

    int created = 0;
    for (int i = 0; i < count; ++i) {
        GameObject* instance = InstantiateRoot(scene, prefabPath);
        if (!instance) break;
        if (instance->prefabAssetPath.empty())
            instance->prefabAssetPath = prefabPath;
        // OnSpawn を通さずに直接待機列へ入れる (まだ「出していない」ため)。
        instance->SetActive(false);
        Pools()[&scene][prefabPath].idle.push_back(instance->GetID());
        ++created;
    }
    return created;
}

void PrefabPool::SetLimit(const std::string& prefabPath, int maxLive)
{
    if (prefabPath.empty()) return;
    if (maxLive <= 0) {
        Limits().erase(prefabPath);
        return;
    }
    Limits()[prefabPath] = maxLive;
}

int PrefabPool::GetLimit(const std::string& prefabPath)
{
    return LimitOf(prefabPath);
}

size_t PrefabPool::AvailableCount(const Scene& scene, const std::string& prefabPath)
{
    const auto sceneIt = Pools().find(&scene);
    if (sceneIt == Pools().end()) return 0;
    const auto bucketIt = sceneIt->second.find(prefabPath);
    return bucketIt == sceneIt->second.end() ? 0 : bucketIt->second.idle.size();
}

size_t PrefabPool::LiveCount(const Scene& scene, const std::string& prefabPath)
{
    const auto sceneIt = Pools().find(&scene);
    if (sceneIt == Pools().end()) return 0;
    const auto bucketIt = sceneIt->second.find(prefabPath);
    return bucketIt == sceneIt->second.end() ? 0 : bucketIt->second.live.size();
}

void PrefabPool::Clear(const Scene& scene)
{
    Pools().erase(&scene);
}

void PrefabPool::ClearAll()
{
    Pools().clear();
    Limits().clear();
}

} // namespace fbzz::scene
