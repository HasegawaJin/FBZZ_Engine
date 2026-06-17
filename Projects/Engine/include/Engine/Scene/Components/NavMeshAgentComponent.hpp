// FBZZ Engine
// NavMeshAgentComponent.hpp | fbzz::scene
// NavMesh 上を自律移動するエージェントの移動パラメータとパス追従ランタイム状態
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

// オフメッシュリンク通過中の補間状態（NavigationSystem が書き込む）
struct NavMeshLinkTraversal {
    math::Vector3 startPos  = math::Vector3::ZERO;
    math::Vector3 endPos    = math::Vector3::ZERO;
    float         totalTime = 0.3f;
    float         elapsed   = 0.0f;
    bool          active    = false;
};

enum class NavMeshAgentState : uint8_t {
    IDLE,             // 目的地未設定、または到達済み
    MOVING,           // パスに沿って移動中
    TRAVERSING_LINK,  // オフメッシュリンクを通過中（lerp 補間移動）
};

struct NavMeshAgentComponent {
    float radius           = 0.4f;
    float maxSpeed         = 3.5f;
    float acceleration     = 8.0f;
    float angularSpeedDeg  = 360.0f; // 進行方向への回転速度 [deg/s]
    float stoppingDistance = 0.1f;   // 最終ウェイポイントからこの距離以内で到達と判定

    // 衝突回避の優先度。値が大きい Agent ほど他の Agent から押し出されにくい
    // (敵が遮ってもプレイヤー追跡 AI が止まらない、等のケースで使う)。
    int avoidancePriority = 0;

    // NavMeshSurfaceComponent::agentTypeId と一致する Surface を使う（0 = デフォルト）。
    int agentTypeId = 0;

    // true のとき移動後に NavMesh 面の Y 座標へスナップする。
    // 物理・重力と組み合わせる場合は false にして物理側に Y を任せる。
    bool snapToNavMesh = false;

    // false にすると NavigationSystem が GO の位置を書き込まない (アニメーション Root Motion と併用する場合等)。
    bool updatePosition = true;
    // false にすると NavigationSystem が GO の回転を書き込まない。
    bool updateRotation = true;
    // true のとき最終ウェイポイントへ近づくにつれて自動的に速度を落とす（ブレーキング）。
    bool autoBraking = true;

    // 通過できるエリアタイプのビットマスク。-1 = すべて通過可。
    // bit i が立っているとき areaType==i のポリゴンを通過できる。
    int areaMask = -1;

    bool enabled = true;

    // ── ランタイム状態 (NavigationSystem が管理。非永続化) ──────────────────
    bool hasDestination = false;
    // NavigationSystem が毎フレーム更新する現在の速度ベクトル [m/s]。アニメーター連携に使う。
    math::Vector3 velocity = math::Vector3::ZERO;
    math::Vector3 destination = math::Vector3::ZERO;
    std::vector<math::Vector3> path;      // Funnel Algorithm 平滑化済みウェイポイント
    std::vector<bool>          pathLinkFlags; // path[i]==true → オフメッシュリンク起点
    std::vector<float>         pathLinkTimes; // リンク通過時間（link 起点インデックスのみ有効）
    size_t currentWaypoint = 0;
    NavMeshAgentState state = NavMeshAgentState::IDLE;
    float currentSpeed = 0.0f;

    // ── オフメッシュリンク通過状態 ─────────────────────────────────────────
    NavMeshLinkTraversal linkTraversal;

    // true の間、NavigationSystem は移動・回転を止めるがパスは保持する
    // (Unity の NavMeshAgent.isStopped に相当。Resume() で同じパスから再開できる)。
    bool isStopped = false;

    // 直近の SetDestination() 呼び出しに対して到達済みかどうか。
    // OnNavMeshDestinationReached コールバックを実装しない Script でもポーリングで判定できる。
    bool destinationReached = false;

    // 現在位置から最終ウェイポイントまでの残り距離。NavigationSystem が毎フレーム更新する。
    float remainingDistance = 0.0f;

    // ── 追跡対象 (Target Follow) ────────────────────────────────────────────
    // WHY: 敵 AI がプレイヤーを追跡するような「動く目的地」は、毎フレーム SetDestination
    //      し直すと A* + Funnel の再計算コストが大きい。一定間隔・一定移動量ごとにのみ
    //      再パスすることで負荷を抑える (Unity の NavMeshAgent + 自前 Chase ロジック相当)。
    EntityID target = EntityID::INVALID;
    float repathInterval = 0.5f; // target 追跡時の再パス最小間隔 [s]
    float repathTimer    = 0.0f; // ランタイム: 次回再パスまでの残り時間
    math::Vector3 lastTargetPos = math::Vector3::ZERO; // ランタイム: 直前に再パスした時点の target 位置

    // ── 停滞検出 (Stuck Detection) ──────────────────────────────────────────
    // WHY: Avoidance や NavMesh の角で複数 Agent が絡み合うと、パス自体は正しくても
    //      実際には前進できない状態が起こり得る。一定時間進めていなければ
    //      NavigationSystem が自動でパスを再計算し、詰みを防ぐ。
    math::Vector3 lastStuckCheckPos = math::Vector3::ZERO; // ランタイム
    float stuckTimer = 0.0f;  // ランタイム: 直近の有意な前進からの経過時間 [s]
    bool  isStuck    = false; // ランタイム: 直前に停滞を検知して強制再計算したかどうか (デバッグ表示用)

    // パスが計算済みかどうか。Script からパス存在確認に使う。
    bool HasPath() const { return !path.empty(); }

    const char* GetTypeName() const { return "NavMesh Agent"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("radius",           radius);
        r.Field("maxSpeed",         maxSpeed);
        r.Field("acceleration",     acceleration);
        r.Field("angularSpeedDeg",  angularSpeedDeg);
        r.Field("stoppingDistance", stoppingDistance);
        r.Field("avoidancePriority",avoidancePriority);
        r.Field("agentTypeId",      agentTypeId);
        r.Field("snapToNavMesh",    snapToNavMesh);
        r.Field("updatePosition",   updatePosition);
        r.Field("updateRotation",   updateRotation);
        r.Field("autoBraking",      autoBraking);
        r.Field("areaMask",         areaMask);
    }

    // 目的地を設定する。実際のパス計算は NavigationSystem が次フレームで行う。
    // target 追跡中であれば解除する (固定目的地と追跡対象は同時に使えない)。
    void SetDestination(const math::Vector3& worldPos)
    {
        destination        = worldPos;
        hasDestination      = true;
        destinationReached = false;
        isStopped           = false;
        target              = EntityID::INVALID;
        path.clear();
        pathLinkFlags.clear();
        pathLinkTimes.clear();
        linkTraversal.active = false;
        currentWaypoint = 0;
        state           = NavMeshAgentState::MOVING;
    }

    // id の現在位置を目的地として追跡し続ける。NavigationSystem が repathInterval ごと、
    // または target が一定距離動いたタイミングで自動的に再パスする。
    void SetTarget(EntityID id, float interval = 0.5f)
    {
        target         = id;
        repathInterval = interval > 0.05f ? interval : 0.05f;
        repathTimer    = 0.0f; // 次フレームで即座に初回パスを計算させる
    }

    void ClearTarget() { target = EntityID::INVALID; }

    // パスを完全に放棄して停止する (到達済みフラグには触れない)。
    void Stop()
    {
        hasDestination = false;
        target = EntityID::INVALID;
        path.clear();
        pathLinkFlags.clear();
        pathLinkTimes.clear();
        linkTraversal.active = false;
        currentWaypoint = 0;
        currentSpeed = 0.0f;
        remainingDistance = 0.0f;
        isStopped = false;
        state = NavMeshAgentState::IDLE;
    }
};

} // namespace fbzz::scene
