#pragma once

#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabRef.hpp>
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class BulletComponent : public Script {
public:
    static constexpr const char* TYPE_NAME = "BulletComponent";
    const char* GetTypeName() const override { return TYPE_NAME; }

    float       speed          = 20.0f;
    float       lifetime       = 3.0f;
    float       effectLifetime = 3.0f;
    PrefabRef   effectPrefab;
    std::string ignoreTag      = "Player";
    Vector3     direction;  // BulletShooterComponent が Instantiate 後に設定する

    void Reflect(IReflector& r) override
    {
        r.Field("Speed",            speed);
        r.Field("Lifetime",         lifetime);
        r.Field("Effect Lifetime",  effectLifetime);
        r.Field("Effect Prefab",    effectPrefab);
        r.Field("Ignore Tag",       ignoreTag);
    }

    void OnCollisionEnter(const CollisionInfo& info) override
    {
        // WHY: PhysicsSystem コールバック内で Instantiate を呼ぶとシーン破壊が起きる。
        //      フラグと座標だけ記録して OnUpdate で実際の処理を行う。
        if (!m_alive || ShouldIgnore(info.other)) return;
        m_alive = false;
        m_hitPoint = info.contactPoint;
    }

    void OnTriggerEnter(const CollisionInfo& info) override
    {
        if (!m_alive || ShouldIgnore(info.other)) return;
        m_alive = false;
        m_hitPoint = transform ? transform->position : Vector3{};
    }

    void OnUpdate(float dt) override
    {
        if (!transform) return;

        // 衝突フラグが立っていたら ScriptSystem コンテキストで安全に処理する
        // WHY: DestroySelf() は遅延実行のため m_alive=false 後も複数フレーム OnUpdate が
        //      来る。Die() で m_effectSpawned を立て、エフェクトの二重生成を防ぐ。
        if (!m_alive) {
            Die(m_hitPoint);
            return;
        }

        const Vector3 pos = transform->position;
        const Vector3 dir = (direction.LengthSq() > 0.0001f) ? direction
                                                              : transform->Forward();

        // Raycast で前方の衝突を検出（高速移動によるトンネル防止）
        RaycastHit hit;
        if (physics.Raycast(pos, dir, speed * dt, hit)) {
            if (!ShouldIgnore(hit.gameObject)) {
                Die(hit.point);
                return;
            }
        }

        const Vector3 nextPos = pos + dir * (speed * dt);

        // 地形高さフォールバック: 地形 MeshCollider を Raycast が通らない場合の補完
        const float terrainH = scene.GetTerrainHeightAt(nextPos);
        if (nextPos.y <= terrainH) {
            Die({ nextPos.x, terrainH, nextPos.z });
            return;
        }

        transform->localPosition = nextPos;

        m_remaining -= dt;
        if (m_remaining <= 0.0f)
            Die(nextPos);
    }

private:
    bool    m_alive         = true;
    bool    m_effectSpawned = false;
    float   m_remaining     = 0.0f;
    Vector3 m_hitPoint;

    void OnStart() override { m_remaining = lifetime; }

    bool ShouldIgnore(const GameObject* go) const
    {
        if (!go) return false;
        if (go == scene.Self()) return true;
        if (!ignoreTag.empty() && go->tag == ignoreTag) return true;
        return false;
    }

    // 着弾処理: 破棄キュー登録 → エフェクト生成 (1回のみ)
    // WHY: scene.Instantiate() は内部でシーンを再構築する可能性があり、
    //      呼び出し後に script->m_gameObject がダングリングポインタになる場合がある。
    //      DestroySelf() を先に呼び EntityID をキューへ退避してから SpawnEffect() を呼ぶ。
    void Die(const Vector3& point)
    {
        m_alive    = false;
        m_hitPoint = point;
        scene.DestroySelf();
        if (!m_effectSpawned) {
            SpawnEffect(m_hitPoint);
            m_effectSpawned = true;
        }
    }

    void SpawnEffect(const Vector3& point)
    {
        if (effectPrefab.path.empty()) return;
        auto* go = scene.Instantiate(effectPrefab);
        if (!go) return;
        go->transform.localPosition = point;
        go->AddComponent<LifetimeComponent>({effectLifetime, true});
    }
};

} // namespace sandbox
