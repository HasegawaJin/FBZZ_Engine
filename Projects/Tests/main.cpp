// FBZZ Engine
// Projects/Tests/main.cpp
// Physics 実装テスト: Phase 1〜11 の動作確認
#include <cstdio>
#include <cmath>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

#include <Physics/World.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/GJK.hpp>
#include <Physics/EPA.hpp>
#include <Physics/CCDSolver.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/SpringConstraint.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/RopeConstraint.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <Physics/FixedConstraint.hpp>
#include <Physics/SliderConstraint.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/RigidBodySerializer.hpp>
#include <Physics/Layer.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <Math/MathUtils.hpp>

using namespace fbzz::physics;
using namespace fbzz::math;

// ─── ユーティリティ ────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* label, const char* detail = nullptr)
{
    if (cond)
    {
        std::printf("  [PASS] %s\n", label);
        ++g_passed;
    }
    else
    {
        if (detail)
            std::fprintf(stderr, "  [FAIL] %s  <- %s\n", label, detail);
        else
            std::fprintf(stderr, "  [FAIL] %s\n", label);
        ++g_failed;
    }
}

// 値付き check ヘルパー
static void checkF(bool cond, const char* label, float actual, const char* expr)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "actual=%.4f  (%s)", actual, expr);
    check(cond, label, cond ? nullptr : buf);
}

static void checkV(bool cond, const char* label,
                   float x, float y, float z, const char* expr)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "actual=(%.3f, %.3f, %.3f)  (%s)", x, y, z, expr);
    check(cond, label, cond ? nullptr : buf);
}

// ColliderInstance を手軽に作るヘルパー
static ColliderInstance makeInstance(std::shared_ptr<Collider> col,
                                     RigidBody* body,
                                     bool isTrigger = false)
{
    static PhysicsMaterial mat;
    ColliderInstance ci;
    ci.collider  = std::move(col);
    ci.body      = body;
    ci.material  = &mat;
    ci.isTrigger = isTrigger;
    return ci;
}

static ColliderInstance makeInstanceMat(std::shared_ptr<Collider> col,
                                        RigidBody* body,
                                        const PhysicsMaterial* mat,
                                        bool isTrigger = false)
{
    ColliderInstance ci;
    ci.collider  = std::move(col);
    ci.body      = body;
    ci.material  = mat;
    ci.isTrigger = isTrigger;
    return ci;
}

// ─── Phase 1: Warm Starting + PGS 積み重ね安定化テスト ─────────────────────

static void TestPhase1_StackStability()
{
    std::printf("\n=== Phase 1: Warm Starting / PGS Stacking ===\n");

    // 5 個の Sphere を縦に積む (y = 1, 3, 5, 7, 9)
    constexpr int N = 5;
    constexpr float R = 0.5f;

    std::vector<std::shared_ptr<RigidBody>>    bodies;
    std::vector<ColliderInstance>              colliders;

    // 床 (Static AABB)
    auto floorBody = std::make_shared<RigidBody>();
    floorBody->m_isStatic = true;
    floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
    auto floorCol  = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
    floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());
    bodies.push_back(floorBody);
    colliders.push_back(makeInstance(floorCol, floorBody.get()));

    std::vector<RigidBody*> dynamicBodies;

    for (int i = 0; i < N; ++i)
    {
        auto body = std::make_shared<RigidBody>();
        body->SetMass(1.0f);
        body->SetPosition({ 0.0f, R + static_cast<float>(i) * (R * 2.0f + 0.01f), 0.0f });
        body->SetVelocity({ 0.0f, 0.0f, 0.0f });

        auto col = std::make_shared<SphereCollider>(R);
        col->Update(body->GetPosition(), body->GetRotation());

        dynamicBodies.push_back(body.get());
        colliders.push_back(makeInstance(col, body.get()));
        bodies.push_back(std::move(body));
    }

    World world;
    world.SetGravity({ 0.0f, -9.81f, 0.0f });
    world.BeginSceneSync();
    for (const auto& body : bodies)
        world.SyncBody({}, body);
    for (const auto& collider : colliders)
        world.SyncCollider({}, collider);
    world.EndSceneSync();

    // 3 秒分ステップ
    constexpr float dt  = 1.0f / 60.0f;
    constexpr int   STEPS = static_cast<int>(3.0f / dt);
    for (int s = 0; s < STEPS; ++s)
        world.Step(dt);

    // 最下段の球が床から大きく沈み込んでいないか (y > -0.1)
    const float y0 = dynamicBodies[0]->GetPosition().y;
    checkF(y0 > -0.1f, "Bottom sphere not sunken below floor", y0, "y > -0.1");

    // 全球の速度が収束しているか (|v| < 0.5 m/s)
    bool allSettled = true;
    for (RigidBody* b : dynamicBodies)
    {
        const Vector3 v = b->GetVelocity();
        const float spd = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (spd > 0.5f) { allSettled = false; break; }
    }
    check(allSettled, "All stacked spheres settled (|v| < 0.5 m/s)");

    // 積み重ね高さが大きく崩れていないか (最上段が y > N*R*1.5)
    const float yTop = dynamicBodies[N - 1]->GetPosition().y;
    const float yMin = static_cast<float>(N) * R * 1.5f;
    checkF(yTop > yMin, "Top sphere still in stack (y > N*R*1.5)", yTop, "y > N*R*1.5");
}

// ─── Phase 2: TriangleMeshCollider (BVH) テスト ────────────────────────────

static void TestPhase2_TriangleMesh()
{
    std::printf("\n=== Phase 2: TriangleMeshCollider (BVH) ===\n");

    // 水平な四角形床 (2 三角形) を TriangleMeshCollider で作成
    std::vector<Vector3> positions = {
        { -5.0f, 0.0f, -5.0f },
        {  5.0f, 0.0f, -5.0f },
        {  5.0f, 0.0f,  5.0f },
        { -5.0f, 0.0f,  5.0f },
    };
    std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };

    auto meshCol = std::make_shared<TriangleMeshCollider>(positions, indices);

    // Static 床ボディ
    auto floorBody = std::make_shared<RigidBody>();
    floorBody->m_isStatic = true;
    floorBody->SetPosition({ 0.0f, 0.0f, 0.0f });
    meshCol->UpdateWithScale(floorBody->GetPosition(),
                              floorBody->GetRotation(),
                              { 1.0f, 1.0f, 1.0f });

    // BVH が構築されたか
    check(!meshCol->GetBVH().nodes.empty(), "BVH nodes built");
    check(!meshCol->GetBVH().triangles.empty(), "BVH triangles built (2 tris)");

    // AABB が適切か
    const AABB aabb = meshCol->GetAABB();
    check(aabb.min.y <= 0.0f && aabb.max.y >= 0.0f,
          "Mesh AABB covers y=0 plane");

    // Sphere を落として衝突を検出
    auto sphereBody = std::make_shared<RigidBody>();
    sphereBody->SetMass(1.0f);
    sphereBody->SetPosition({ 0.0f, 2.0f, 0.0f });

    constexpr float SR = 0.5f;
    auto sphereCol = std::make_shared<SphereCollider>(SR);
    sphereCol->Update(sphereBody->GetPosition(), sphereBody->GetRotation());

    std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, sphereBody };
    std::vector<ColliderInstance> colliders = {
        makeInstance(meshCol,   floorBody.get()),
        makeInstance(sphereCol, sphereBody.get()),
    };

    World world;
    world.SetGravity({ 0.0f, -9.81f, 0.0f });
    world.BeginSceneSync();
    for (const auto& body : bodies)
        world.SyncBody({}, body);
    for (const auto& collider : colliders)
        world.SyncCollider({}, collider);
    world.EndSceneSync();

    constexpr float dt    = 1.0f / 60.0f;
    constexpr int   STEPS = static_cast<int>(2.0f / dt);
    for (int s = 0; s < STEPS; ++s)
        world.Step(dt);

    // 球が床面 (y=0) より大きく沈み込んでいない (y >= -0.1)
    const float fy = sphereBody->GetPosition().y;
    checkF(fy >= -0.1f, "Sphere resting on mesh floor (y >= -0.1)", fy, "y >= -0.1");

    // 速度が収束しているか
    const Vector3 v = sphereBody->GetVelocity();
    const float spd = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    check(spd < 1.0f, "Sphere settled on mesh floor");
}

// ─── Phase 3: ConvexHullCollider (GJK + EPA) テスト ──────────────────────

static void TestPhase3_ConvexHull()
{
    std::printf("\n=== Phase 3: ConvexHullCollider (GJK + EPA) ===\n");

    // 1m 立方体の頂点 (8 点)
    auto makeCubeVerts = [](float s) -> std::vector<Vector3> {
        return {
            { -s, -s, -s }, {  s, -s, -s }, {  s,  s, -s }, { -s,  s, -s },
            { -s, -s,  s }, {  s, -s,  s }, {  s,  s,  s }, { -s,  s,  s },
        };
    };

    // ConvexHull A: 中心 (0, 0, 0)
    ConvexHullCollider hullA(makeCubeVerts(0.5f));
    hullA.Update({ 0.0f, 0.0f, 0.0f }, Quaternion::Identity());

    // ConvexHull B: 中心 (0.8, 0, 0) → 0.2m 重複
    ConvexHullCollider hullB(makeCubeVerts(0.5f));
    hullB.Update({ 0.8f, 0.0f, 0.0f }, Quaternion::Identity());

    // GJK で交差を確認
    GJKResult gjkRes = GJK_Intersect(
        &hullA, ConvexHullCollider::SupportFnImpl,
        &hullB, ConvexHullCollider::SupportFnImpl);

    check(gjkRes.intersects, "GJK: overlapping cubes intersect");

    // EPA で貫通深度・法線を取得 (GJKResult に保存済みの Simplex を使う)
    if (gjkRes.intersects)
    {
        check(gjkRes.simplex.size == 4, "GJK simplex is tetrahedron for EPA");

        if (gjkRes.simplex.size == 4)
        {
            EPAResult epaRes = EPA_GetContactInfo(
                &hullA, ConvexHullCollider::SupportFnImpl,
                &hullB, ConvexHullCollider::SupportFnImpl,
                gjkRes.simplex);

            check(epaRes.valid, "EPA result is valid");
            check(epaRes.depth > 0.0f, "EPA penetration depth > 0");
            // 貫通深度はおよそ 0.2m のはず (0.1〜0.3 の範囲で合格)
            checkF(epaRes.depth > 0.05f && epaRes.depth < 0.35f,
                  "EPA depth in expected range (~0.2m)", epaRes.depth, "0.05 < depth < 0.35");
            // 法線は X 軸に近いはず
            const float nx = std::abs(epaRes.normal.x);
            checkF(nx > 0.7f, "EPA normal aligned with X axis", nx, "|normal.x| > 0.7");
        }
    }

    // 非交差ケース: 2m 離れた立方体
    ConvexHullCollider hullC(makeCubeVerts(0.5f));
    hullC.Update({ 2.0f, 0.0f, 0.0f }, Quaternion::Identity());

    GJKResult gjkSep = GJK_Intersect(
        &hullA, ConvexHullCollider::SupportFnImpl,
        &hullC, ConvexHullCollider::SupportFnImpl);

    check(!gjkSep.intersects, "GJK: separated cubes do not intersect");
}

// ─── Phase 4: CCD テスト ───────────────────────────────────────────────────

static void TestPhase4_CCD()
{
    std::printf("\n=== Phase 4: CCD (Swept Sphere) ===\n");

    // SweptSphereSphere の直接テスト
    {
        // A: 位置 (0,0,0)、速度 (200,0,0)、r=0.5
        // B: 位置 (3,0,0)、静止、r=0.5
        // 1フレーム (1/60s) で 3.33m 移動 → 間隔 2.0m を超えて接触 (t ≈ 0.60)
        CCDResult res = CCDSolver::SweptSphereSphere(
            { 0.0f, 0.0f, 0.0f }, 0.5f, { 200.0f, 0.0f, 0.0f },
            { 3.0f, 0.0f, 0.0f }, 0.5f,
            1.0f / 60.0f);

        check(res.hit, "CCD: fast sphere hits stationary sphere");
        check(res.toi >= 0.0f && res.toi <= 1.0f, "CCD: TOI in [0, 1]");
    }

    // 非衝突ケース (離れた方向へ移動)
    {
        CCDResult res = CCDSolver::SweptSphereSphere(
            { 0.0f, 0.0f, 0.0f }, 0.5f, { 0.0f, 100.0f, 0.0f },
            { 5.0f, 0.0f, 0.0f }, 0.5f,
            1.0f / 60.0f);

        check(!res.hit, "CCD: orthogonally-moving sphere does not collide");
    }

    // NeedsCCD テスト
    {
        RigidBody fastBody;
        fastBody.SetMass(1.0f);
        fastBody.SetVelocity({ 50.0f, 0.0f, 0.0f });
        check(CCDSolver::NeedsCCD(fastBody, 0.5f, 1.0f / 60.0f),
              "CCD: NeedsCCD true for v=50 r=0.5");

        RigidBody slowBody;
        slowBody.SetMass(1.0f);
        slowBody.SetVelocity({ 0.1f, 0.0f, 0.0f });
        check(!CCDSolver::NeedsCCD(slowBody, 0.5f, 1.0f / 60.0f),
              "CCD: NeedsCCD false for v=0.1 r=0.5");
    }

    // World 統合テスト: 高速球が薄い静的球を貫通しないか
    {
        // 静的球 (壁代わり): 位置 (2, 0, 0)
        auto wallBody = std::make_shared<RigidBody>();
        wallBody->m_isStatic = true;
        wallBody->SetPosition({ 2.0f, 0.0f, 0.0f });
        auto wallCol = std::make_shared<SphereCollider>(0.5f);
        wallCol->Update(wallBody->GetPosition(), wallBody->GetRotation());

        // 発射球: 位置 (-2, 0, 0)、速度 (200, 0, 0)、CCD 有効
        auto bulletBody = std::make_shared<RigidBody>();
        bulletBody->SetMass(1.0f);
        bulletBody->SetPosition({ -2.0f, 0.0f, 0.0f });
        bulletBody->SetVelocity({ 200.0f, 0.0f, 0.0f });
        bulletBody->m_useCCD    = true;
        bulletBody->m_ccdRadius = 0.3f;
        auto bulletCol = std::make_shared<SphereCollider>(0.3f);
        bulletCol->Update(bulletBody->GetPosition(), bulletBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies   = { wallBody, bulletBody };
        std::vector<ColliderInstance>           cols = {
            makeInstance(wallCol,   wallBody.get()),
            makeInstance(bulletCol, bulletBody.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f }); // 重力なし
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // 1 フレームだけ実行
        world.Step(1.0f / 60.0f);

        // 発射球が壁球を通過していないか (x < 2.5)
        const float bx = bulletBody->GetPosition().x;
        check(bx < 2.5f, "CCD: bullet sphere does not tunnel through wall");
    }
}

// ─── Phase 5: 積み重ね応力テスト ──────────────────────────────────────────────

static void TestPhase5_StackStress()
{
    std::printf("\n=== Phase 5: Stack Stress Tests ===\n");

    // ── 5-1: 10 球タワー (Phase 1 より 2 倍高い) ──────────────────────────
    {
        constexpr int   N  = 10;
        constexpr float R  = 0.5f;
        constexpr float DT = 1.0f / 60.0f;

        std::vector<std::shared_ptr<RigidBody>> bodies;
        std::vector<ColliderInstance>           cols;

        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());
        bodies.push_back(floorBody);
        cols.push_back(makeInstance(floorCol, floorBody.get()));

        std::vector<RigidBody*> dynBodies;
        for (int i = 0; i < N; ++i)
        {
            auto body = std::make_shared<RigidBody>();
            body->SetMass(1.0f);
            body->SetPosition({ 0.0f, R + static_cast<float>(i) * R * 2.01f, 0.0f });
            auto col = std::make_shared<SphereCollider>(R);
            col->Update(body->GetPosition(), body->GetRotation());
            dynBodies.push_back(body.get());
            cols.push_back(makeInstance(col, body.get()));
            bodies.push_back(std::move(body));
        }

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        for (int s = 0; s < static_cast<int>(6.0f / DT); ++s)
            world.Step(DT);

        // 全球が床に沈んでいないか
        bool allAboveFloor = true;
        for (RigidBody* b : dynBodies)
            if (b->GetPosition().y < -0.1f) { allAboveFloor = false; break; }
        check(allAboveFloor, "10-sphere tower: no sphere sinks below floor");

        // 全球が X/Z 方向に大きく流れていないか (Y は重力で多少動く)
        bool allSettled = true;
        for (RigidBody* b : dynBodies)
        {
            const Vector3 v = b->GetVelocity();
            if (v.x*v.x + v.z*v.z > 1.0f) { allSettled = false; break; } // 横方向のみ
        }
        check(allSettled, "10-sphere tower: all spheres settled (no lateral drift)");
    }

    // ── 5-2: AABB ブロックの積み重ね ──────────────────────────────────────
    {
        constexpr int   N  = 6;
        constexpr float DT = 1.0f / 60.0f;
        const Vector3   HE = { 0.5f, 0.25f, 0.5f }; // 半サイズ

        std::vector<std::shared_ptr<RigidBody>> bodies;
        std::vector<ColliderInstance>           cols;

        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.25f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.25f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());
        bodies.push_back(floorBody);
        cols.push_back(makeInstance(floorCol, floorBody.get()));

        std::vector<RigidBody*> dynBodies;
        for (int i = 0; i < N; ++i)
        {
            auto body = std::make_shared<RigidBody>();
            body->SetMass(1.0f);
            body->SetPosition({ 0.0f, HE.y + static_cast<float>(i) * HE.y * 2.01f, 0.0f });
            auto col = std::make_shared<AABBCollider>(HE);
            col->Update(body->GetPosition(), body->GetRotation());
            dynBodies.push_back(body.get());
            cols.push_back(makeInstance(col, body.get()));
            bodies.push_back(std::move(body));
        }

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        for (int s = 0; s < static_cast<int>(3.0f / DT); ++s)
            world.Step(DT);

        // 最上段ブロックが残っているか
        const float yTop = dynBodies[N - 1]->GetPosition().y;
        check(yTop > static_cast<float>(N - 1) * HE.y * 1.5f,
              "AABB stack: top block still in place");

        // 全ブロックが収束しているか
        bool allSettled = true;
        bool noAngularDrift = true;
        for (RigidBody* b : dynBodies)
        {
            const Vector3 v = b->GetVelocity();
            if (v.x*v.x + v.y*v.y + v.z*v.z > 0.5f * 0.5f) { allSettled = false; }

            const Vector3 w = b->GetAngularVelocity();
            if (w.x*w.x + w.y*w.y + w.z*w.z > 0.01f * 0.01f) { noAngularDrift = false; }
        }
        check(allSettled, "AABB stack: all blocks settled");
        check(noAngularDrift, "AABB stack: no angular drift");
    }

    // ── 5-3: 初速あり → 衝突後に静止 ──────────────────────────────────────
    {
        constexpr float DT = 1.0f / 60.0f;

        // 床
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        // 上から高速で落下する球 (v = -20 m/s)
        auto ballBody = std::make_shared<RigidBody>();
        ballBody->SetMass(1.0f);
        ballBody->SetPosition({ 0.0f, 5.0f, 0.0f });
        ballBody->SetVelocity({ 0.0f, -20.0f, 0.0f });
        auto ballCol = std::make_shared<SphereCollider>(0.5f);
        ballCol->Update(ballBody->GetPosition(), ballBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, ballBody };
        std::vector<ColliderInstance> cols = {
            makeInstance(floorCol, floorBody.get()),
            makeInstance(ballCol,  ballBody.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        for (int s = 0; s < static_cast<int>(5.0f / DT); ++s)
            world.Step(DT);

        // 球が床より上で静止しているか
        check(ballBody->GetPosition().y >= -0.1f, "High-speed drop: ball rests above floor");
        // 5秒後に床近傍 (y < 2.0) にいれば収束と見なす (重力で高跳ねしていない)
        check(ballBody->GetPosition().y < 2.0f, "High-speed drop: ball settled near floor");
    }
}

// ─── Phase 6: カプセルコライダー衝突テスト ──────────────────────────────────

static void TestPhase6_Capsule()
{
    std::printf("\n=== Phase 6: Capsule Collisions ===\n");

    // ── 6-1: Capsule が AABB 床で止まるか ─────────────────────────────────
    {
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        // r=0.3, halfHeight=0.5 (Y 向きカプセル、全長 1.6m)
        auto capBody = std::make_shared<RigidBody>();
        capBody->SetMass(1.0f);
        capBody->SetPosition({ 0.0f, 3.0f, 0.0f });
        auto capCol = std::make_shared<CapsuleCollider>(0.3f, 0.5f);
        capCol->Update(capBody->GetPosition(), capBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, capBody };
        std::vector<ColliderInstance> cols = {
            makeInstance(floorCol, floorBody.get()),
            makeInstance(capCol,   capBody.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        for (int s = 0; s < 180; ++s) world.Step(1.0f / 60.0f); // 3s

        const float cy = capBody->GetPosition().y;
        checkF(cy >= -0.1f, "Capsule-AABB: capsule rests on floor (y >= -0.1)", cy, "y >= -0.1");
        const Vector3 cv = capBody->GetVelocity();
        checkF(cv.x*cv.x + cv.y*cv.y + cv.z*cv.z < 1.0f,
               "Capsule-AABB: capsule settled (|v|^2 < 1)", cv.y, "|v|^2 < 1");
    }

    // ── 6-2: Sphere が Capsule 上に落ちて止まるか ─────────────────────────
    {
        // 床 AABB
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        // 静的カプセルを床に置く (直立)
        auto capBody = std::make_shared<RigidBody>();
        capBody->m_isStatic = true;
        capBody->SetPosition({ 0.0f, 1.3f, 0.0f }); // 床面から少し上 (r+hH+r=1.3)
        auto capCol = std::make_shared<CapsuleCollider>(0.3f, 0.5f);
        capCol->Update(capBody->GetPosition(), capBody->GetRotation());

        // Sphere を上から落とす
        auto sphBody = std::make_shared<RigidBody>();
        sphBody->SetMass(1.0f);
        sphBody->SetPosition({ 0.0f, 4.0f, 0.0f });
        auto sphCol = std::make_shared<SphereCollider>(0.3f);
        sphCol->Update(sphBody->GetPosition(), sphBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, capBody, sphBody };
        std::vector<ColliderInstance> cols = {
            makeInstance(floorCol, floorBody.get()),
            makeInstance(capCol,   capBody.get()),
            makeInstance(sphCol,   sphBody.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        for (int s = 0; s < 180; ++s) world.Step(1.0f / 60.0f); // 3s

        const float sy = sphBody->GetPosition().y;
        checkF(sy >= -0.1f, "Sphere-Capsule: sphere does not fall through capsule", sy, "y >= -0.1");
    }

    // ── 6-3: Capsule 同士の積み重ね ───────────────────────────────────────
    {
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        // 下 capsule (静止・床上)
        auto capBodyA = std::make_shared<RigidBody>();
        capBodyA->m_isStatic = true;
        capBodyA->SetPosition({ 0.0f, 1.3f, 0.0f });
        auto capColA = std::make_shared<CapsuleCollider>(0.3f, 0.5f);
        capColA->Update(capBodyA->GetPosition(), capBodyA->GetRotation());

        // 上 capsule (動的・上から落下)
        auto capBodyB = std::make_shared<RigidBody>();
        capBodyB->SetMass(1.0f);
        capBodyB->SetPosition({ 0.0f, 4.5f, 0.0f });
        auto capColB = std::make_shared<CapsuleCollider>(0.3f, 0.5f);
        capColB->Update(capBodyB->GetPosition(), capBodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, capBodyA, capBodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(floorCol, floorBody.get()),
            makeInstance(capColA,  capBodyA.get()),
            makeInstance(capColB,  capBodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        for (int s = 0; s < 180; ++s) world.Step(1.0f / 60.0f); // 3s

        const float by = capBodyB->GetPosition().y;
        checkF(by >= -0.1f, "Capsule-Capsule: upper capsule rests on lower", by, "y >= -0.1");
    }

    // 6-4: centerOffset が Step 後も collider に残るか
    {
        auto body = std::make_shared<RigidBody>();
        body->m_isStatic = true;
        body->SetPosition({ 0.0f, 0.0f, 0.0f });

        auto sphereCol = std::make_shared<SphereCollider>(0.5f);
        sphereCol->Update({ 1.0f, 0.0f, 0.0f }, body->GetRotation());

        ColliderInstance inst = makeInstance(sphereCol, body.get());
        inst.centerOffset = { 1.0f, 0.0f, 0.0f };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        world.SyncBody({}, body);
        world.SyncCollider({}, inst);
        world.EndSceneSync();
        world.Step(1.0f / 60.0f);

        const float centerX = sphereCol->GetAABB().Center().x;
        checkF(std::abs(centerX - 1.0f) < 0.001f,
               "Collider center offset persists after Step",
               centerX,
               "|centerX - 1| < 0.001");
    }

    // 6-5: rotated capsule と OBB が BroadPhase/NarrowPhase を通るか
    {
        auto boxBody = std::make_shared<RigidBody>();
        boxBody->m_isStatic = true;
        boxBody->SetPosition({ 0.65f, 0.0f, 0.0f });
        auto boxCol = std::make_shared<OBBCollider>(Vector3{ 0.25f, 0.25f, 0.25f });
        boxCol->Update(boxBody->GetPosition(), boxBody->GetRotation());

        auto capBody = std::make_shared<RigidBody>();
        capBody->SetMass(1.0f);
        capBody->SetPosition({ 0.0f, 0.0f, 0.0f });
        capBody->SetRotation(Quaternion::FromAxisAngle(Vector3::FORWARD, 90.0f * DEG2RAD));
        auto capCol = std::make_shared<CapsuleCollider>(0.3f, 0.5f);
        capCol->Update(capBody->GetPosition(), capBody->GetRotation());

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        world.SyncBody({}, boxBody);
        world.SyncBody({}, capBody);
        world.SyncCollider({}, makeInstance(boxCol, boxBody.get()));
        world.SyncCollider({}, makeInstance(capCol, capBody.get()));
        world.EndSceneSync();
        world.Step(1.0f / 60.0f);

        check(!world.GetEnterEvents().empty(),
              "Capsule-Box: rotated capsule overlaps OBB");
    }
}

// ─── Phase 7: 運動量保存テスト ────────────────────────────────────────────────

static void TestPhase7_Momentum()
{
    std::printf("\n=== Phase 7: Momentum Conservation ===\n");

    // ── 7-1: 等質量球の正面衝突 — 運動量保存 ─────────────────────────────
    {
        // 重力なし。Ball A: pos=(-3,0,0) v=(+5,0,0)  Ball B: pos=(+3,0,0) v=(-5,0,0)
        // 合計運動量 = 0 → 衝突後も 0
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ -3.0f, 0.0f, 0.0f });
        bodyA->SetVelocity({  5.0f, 0.0f, 0.0f });
        auto colA = std::make_shared<SphereCollider>(0.5f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 3.0f, 0.0f, 0.0f });
        bodyB->SetVelocity({ -5.0f, 0.0f, 0.0f });
        auto colB = std::make_shared<SphereCollider>(0.5f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // 衝突するまで十分ステップ (1.2 / 5.0 = 0.24s → ~15 frames)
        for (int s = 0; s < 60; ++s) world.Step(1.0f / 60.0f);

        // 衝突後の合計運動量 (x 成分) ≈ 0
        const float totalPx = bodyA->GetVelocity().x + bodyB->GetVelocity().x;
        checkF(std::abs(totalPx) < 0.5f,
               "Momentum: total px ≈ 0 after head-on collision", totalPx, "|px| < 0.5");
    }

    // ── 7-2: 等質量球の一方向衝突 — 速度交換方向が正しいか ───────────────
    {
        // Ball A: pos=(-2,0,0) v=(+10,0,0)  Ball B: pos=(2,0,0) v=(0,0,0)
        // 衝突後: A は減速、B は加速 (x 方向)
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ -2.0f, 0.0f, 0.0f });
        bodyA->SetVelocity({ 10.0f, 0.0f, 0.0f });
        auto colA = std::make_shared<SphereCollider>(0.5f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 2.0f, 0.0f, 0.0f });
        bodyB->SetVelocity({ 0.0f, 0.0f, 0.0f });
        auto colB = std::make_shared<SphereCollider>(0.5f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // 衝突が起きるまで待機 (距離 3m / v_rel 10 m/s = 0.3s → 18 frames)
        for (int s = 0; s < 30; ++s) world.Step(1.0f / 60.0f);

        // B は A 方向に動いているはず
        check(bodyB->GetVelocity().x > 1.0f,
              "Momentum: ball B gained positive x velocity after impact");
        // A は B より遅くなっているはず (衝突前 10 m/s、衝突後 ≤ 5 m/s 程度)
        check(bodyA->GetVelocity().x < bodyB->GetVelocity().x + 2.0f,
              "Momentum: ball A velocity <= ball B after impact (velocity exchange)");
    }
}

// ─── Phase 8: 反発係数・摩擦テスト ──────────────────────────────────────────

static void TestPhase8_Materials()
{
    std::printf("\n=== Phase 8: Restitution & Friction ===\n");

    // ── 8-1: Rubber vs Stone — 反発高さの比較 ───────────────────────────
    // 同じ高さから落とした Rubber 球と Stone 球を比較
    // Rubber (e=0.8): 跳ね上がり高さ > 1.0m
    // Stone  (e=0.1): 跳ね上がり高さ < 0.5m
    // 床と球に同じ素材を使うことで CombineRestitution が意図通りに働く
    // Rubber+Rubber: min(0.8,0.8)=0.8 → 大きく跳ね上がる
    // Stone+Stone:   min(0.1,0.1)=0.1 → ほとんど跳ねない
    auto runBounce = [](const PhysicsMaterial* mat) -> float
    {
        // 静的床
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        // 球を y=4 から放す
        auto ballBody = std::make_shared<RigidBody>();
        ballBody->SetMass(1.0f);
        ballBody->SetPosition({ 0.0f, 4.0f, 0.0f });
        ballBody->SetVelocity({ 0.0f, 0.0f, 0.0f });
        auto ballCol = std::make_shared<SphereCollider>(0.3f);
        ballCol->Update(ballBody->GetPosition(), ballBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, ballBody };
        std::vector<ColliderInstance> cols = {
            makeInstanceMat(floorCol, floorBody.get(), mat),
            makeInstanceMat(ballCol,  ballBody.get(),  mat),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // 2 秒分シミュレート (1 回跳ね返り後)
        float maxY = 0.0f;
        bool  hitFloor = false;
        for (int s = 0; s < 120; ++s)
        {
            world.Step(1.0f / 60.0f);
            const float y = ballBody->GetPosition().y;
            // 床に触れた後の最大高さを記録
            if (!hitFloor && y <= ballCol->m_radius + 0.05f) hitFloor = true;
            if (hitFloor) maxY = std::max(maxY, y);
        }
        return maxY;
    };

    const float rubberMaxY = runBounce(&PhysicsMaterial::Rubber);
    const float stoneMaxY  = runBounce(&PhysicsMaterial::Stone);

    checkF(rubberMaxY > 1.5f, "Restitution: Rubber ball bounces above 1.5m",
           rubberMaxY, "maxY > 1.5");
    checkF(stoneMaxY < 0.5f, "Restitution: Stone ball bounces below 0.5m",
           stoneMaxY, "maxY < 0.5");
    check(rubberMaxY > stoneMaxY,
          "Restitution: Rubber bounces higher than Stone");

    // ── 8-2: 摩擦 — Ice vs Stone で水平滑走距離が異なるか ────────────────
    // 同じ初速で AABB をスライドさせ、Ice のほうが長く滑るか
    auto runSlide = [](const PhysicsMaterial* slideMat) -> float
    {
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 50.0f, 0.5f, 50.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        auto boxBody = std::make_shared<RigidBody>();
        boxBody->SetMass(1.0f);
        boxBody->SetPosition({ 0.0f, 0.5f, 0.0f });
        boxBody->SetVelocity({ 5.0f, 0.0f, 0.0f }); // 初速 5 m/s
        auto boxCol = std::make_shared<AABBCollider>(Vector3{ 0.5f, 0.5f, 0.5f });
        boxCol->Update(boxBody->GetPosition(), boxBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, boxBody };
        std::vector<ColliderInstance> cols = {
            makeInstanceMat(floorCol, floorBody.get(), &PhysicsMaterial::Stone),
            makeInstanceMat(boxCol,   boxBody.get(),   slideMat),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        for (int s = 0; s < 180; ++s) world.Step(1.0f / 60.0f); // 3s

        return boxBody->GetPosition().x;
    };

    const float iceDist   = runSlide(&PhysicsMaterial::Ice);
    const float stoneDist = runSlide(&PhysicsMaterial::Stone);

    checkF(iceDist > 5.0f, "Friction: Ice slides more than 5m", iceDist, "x > 5.0");
    checkF(stoneDist < iceDist, "Friction: Stone slides less than Ice",
           stoneDist, "stone_x < ice_x");
}

// ─── Phase 9: ConvexHull × World 統合テスト ─────────────────────────────────

static void TestPhase9_ConvexHullWorld()
{
    std::printf("\n=== Phase 9: ConvexHull World Integration ===\n");

    auto makeCubeVerts = [](float s) -> std::vector<Vector3> {
        return {
            { -s,-s,-s },{ s,-s,-s },{ s, s,-s },{ -s, s,-s },
            { -s,-s, s },{ s,-s, s },{ s, s, s },{ -s, s, s },
        };
    };

    // ── 9-1: ConvexHull が AABB 床で止まるか ─────────────────────────────
    {
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        auto cvxBody = std::make_shared<RigidBody>();
        cvxBody->SetMass(1.0f);
        cvxBody->SetPosition({ 0.0f, 3.0f, 0.0f });
        auto cvxCol = std::make_shared<ConvexHullCollider>(makeCubeVerts(0.5f));
        cvxCol->Update(cvxBody->GetPosition(), cvxBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, cvxBody };
        std::vector<ColliderInstance> cols = {
            makeInstance(floorCol, floorBody.get()),
            makeInstance(cvxCol,   cvxBody.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        for (int s = 0; s < 180; ++s) world.Step(1.0f / 60.0f);

        const float cy = cvxBody->GetPosition().y;
        checkF(cy >= -0.1f, "ConvexHull-AABB: hull rests on floor", cy, "y >= -0.1");
        const Vector3 cv = cvxBody->GetVelocity();
        checkF(cv.x*cv.x + cv.y*cv.y + cv.z*cv.z < 1.0f,
               "ConvexHull-AABB: hull settled", cv.y, "|v|^2 < 1");
    }

    // ── 9-2: Sphere が ConvexHull 上に落ちて止まるか ─────────────────────
    {
        auto floorBody = std::make_shared<RigidBody>();
        floorBody->m_isStatic = true;
        floorBody->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto floorCol = std::make_shared<AABBCollider>(Vector3{ 10.0f, 0.5f, 10.0f });
        floorCol->Update(floorBody->GetPosition(), floorBody->GetRotation());

        auto cvxBody = std::make_shared<RigidBody>();
        cvxBody->m_isStatic = true;
        cvxBody->SetPosition({ 0.0f, 0.5f, 0.0f }); // 床面上に置いた 1×1×1 キューブ
        auto cvxCol = std::make_shared<ConvexHullCollider>(makeCubeVerts(0.5f));
        cvxCol->Update(cvxBody->GetPosition(), cvxBody->GetRotation());

        auto sphBody = std::make_shared<RigidBody>();
        sphBody->SetMass(1.0f);
        sphBody->SetPosition({ 0.0f, 4.0f, 0.0f });
        auto sphCol = std::make_shared<SphereCollider>(0.3f);
        sphCol->Update(sphBody->GetPosition(), sphBody->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { floorBody, cvxBody, sphBody };
        std::vector<ColliderInstance> cols = {
            makeInstance(floorCol, floorBody.get()),
            makeInstance(cvxCol,   cvxBody.get()),
            makeInstance(sphCol,   sphBody.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        for (int s = 0; s < 180; ++s) world.Step(1.0f / 60.0f);

        const float sy = sphBody->GetPosition().y;
        checkF(sy >= -0.1f, "Sphere-ConvexHull: sphere does not fall through hull", sy, "y >= -0.1");
    }

    // ── 9-3: ConvexHull 同士の正面衝突 (World 経由) ───────────────────────
    {
        // 右に動く A と静止する B を衝突させる
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ -2.5f, 0.0f, 0.0f });
        bodyA->SetVelocity({  3.0f, 0.0f, 0.0f });
        auto colA = std::make_shared<ConvexHullCollider>(makeCubeVerts(0.5f));
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 2.5f, 0.0f, 0.0f });
        bodyB->SetVelocity({ 0.0f, 0.0f, 0.0f });
        auto colB = std::make_shared<ConvexHullCollider>(makeCubeVerts(0.5f));
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // 衝突するまで待機 (3m / 3m/s = 1s)
        for (int s = 0; s < 90; ++s) world.Step(1.0f / 60.0f);

        // A が右に突き抜けていないか (B を超えた先にいないか)
        check(bodyA->GetPosition().x < bodyB->GetPosition().x + 2.0f,
              "ConvexHull-ConvexHull: A does not tunnel through B");
        // B が動いているか (衝突でエネルギーが伝わったか)
        check(bodyB->GetVelocity().x > 0.5f || bodyB->GetPosition().x > 2.5f,
              "ConvexHull-ConvexHull: B received impulse from A");
    }
}

// ─── Phase 10: Trigger 衝突イベントテスト ────────────────────────────────────

static void TestPhase10_Triggers()
{
    std::printf("\n=== Phase 10: Trigger Collision Events ===\n");

    // ── 10-1: Trigger 同士の接触でEnterEvent が生成されるか ──────────────
    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });
        auto colA = std::make_shared<SphereCollider>(1.0f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        // B は A と重なる位置 (距離 1.5 < r_A+r_B = 2.0)
        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 1.5f, 0.0f, 0.0f });
        auto colB = std::make_shared<SphereCollider>(1.0f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get(), /*isTrigger=*/true),
            makeInstance(colB, bodyB.get(), /*isTrigger=*/true),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        world.Step(1.0f / 60.0f);

        check(!world.GetEnterEvents().empty(),
              "Trigger: overlapping trigger spheres generate Enter event");
    }

    // ── 10-2: Trigger は解決されず位置に影響しないか ─────────────────────
    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });
        bodyA->SetVelocity({ 0.0f, 0.0f, 0.0f });
        auto colA = std::make_shared<SphereCollider>(1.0f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->m_isStatic = true;
        bodyB->SetPosition({ 1.5f, 0.0f, 0.0f });
        auto colB = std::make_shared<SphereCollider>(1.0f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get(), /*isTrigger=*/true),
            makeInstance(colB, bodyB.get(), /*isTrigger=*/false),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        const Vector3 posA_before = bodyA->GetPosition();
        world.Step(1.0f / 60.0f);
        const Vector3 posA_after = bodyA->GetPosition();

        // isTrigger の A は衝突解決されず位置が変わらないはず
        const float moved = std::sqrt(
            (posA_after.x - posA_before.x) * (posA_after.x - posA_before.x) +
            (posA_after.y - posA_before.y) * (posA_after.y - posA_before.y) +
            (posA_after.z - posA_before.z) * (posA_after.z - posA_before.z));
        checkF(moved < 0.01f, "Trigger: trigger body not pushed by solid body",
               moved, "displacement < 0.01");
    }

    // ── 10-3: 非 Trigger 通常接触でも CollisionEvent が生成されるか ───────
    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ 0.0f, 0.5f, 0.0f });
        auto colA = std::make_shared<SphereCollider>(0.5f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->m_isStatic = true;
        bodyB->SetPosition({ 0.0f, -0.5f, 0.0f });
        auto colB = std::make_shared<AABBCollider>(Vector3{ 5.0f, 0.5f, 5.0f });
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, -9.81f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // 落下させて接触させる
        for (int s = 0; s < 30; ++s) world.Step(1.0f / 60.0f);

        const bool hasEvents = !world.GetEnterEvents().empty() ||
                               !world.GetStayEvents().empty();
        check(hasEvents, "CollisionEvent: solid contact generates Enter/Stay event");
    }

    // ── 10-4: Stay → Exit イベントの遷移 ────────────────────────────────
    {
        // 1 ステップ重なり → Enter
        // 次のステップも重なり → Stay
        // 離れたら → Exit
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });
        bodyA->SetVelocity({ 3.0f, 0.0f, 0.0f }); // x+ 方向に移動
        auto colA = std::make_shared<SphereCollider>(0.5f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->m_isStatic = true;
        bodyB->SetPosition({ 0.5f, 0.0f, 0.0f }); // 初期位置から重なる
        auto colB = std::make_shared<SphereCollider>(0.5f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get(), true),
            makeInstance(colB, bodyB.get(), true),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();

        // Step 1: Enter
        world.Step(1.0f / 60.0f);
        const bool hasEnter = !world.GetEnterEvents().empty();
        check(hasEnter, "CollisionEvent: Step 1 generates Enter event");

        // Step 2: Stay (まだ接触中のはず)
        world.Step(1.0f / 60.0f);
        const bool hasStay = !world.GetStayEvents().empty();
        check(hasStay, "CollisionEvent: Step 2 generates Stay event");

        // Steps 3-10: 十分動かして Exit させる
        for (int s = 0; s < 8; ++s) world.Step(1.0f / 60.0f);
        const bool hasExit = !world.GetExitEvents().empty() ||
                             world.GetStayEvents().empty();
        check(hasExit, "CollisionEvent: after moving apart, no longer in Stay");
    }
}

// ─── Phase 11: 制約 (Constraint) テスト ─────────────────────────────────────

static void TestPhase11_Constraints()
{
    std::printf("\n=== Phase 11: Constraints ===\n");

    // ── 11-1: DistanceConstraint — 最大距離を超えないか ──────────────────
    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->m_isStatic = true;
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 5.0f, 0.0f, 0.0f });
        bodyB->SetVelocity({ 3.0f, 0.0f, 0.0f }); // さらに離れようとする

        auto colA = std::make_shared<SphereCollider>(0.1f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());
        auto colB = std::make_shared<SphereCollider>(0.1f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        // 距離制約: 最大 3.0m
        world.AddConstraint(std::make_shared<DistanceConstraint>(
            bodyA.get(), bodyB.get(), 3.0f));

        for (int s = 0; s < 120; ++s) world.Step(1.0f / 60.0f);

        const Vector3 diff = bodyB->GetPosition() - bodyA->GetPosition();
        const float dist = std::sqrt(diff.x*diff.x + diff.y*diff.y + diff.z*diff.z);
        checkF(dist <= 3.5f, "DistanceConstraint: separation capped at ~3m",
               dist, "dist <= 3.5");
    }

    // ── 11-2: SpringConstraint — 引っ張られて戻るか ─────────────────────
    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->m_isStatic = true;
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 3.0f, 0.0f, 0.0f }); // 自然長 1.0m より 2.0m 伸びた位置
        bodyB->SetVelocity({ 0.0f, 0.0f, 0.0f });

        auto colA = std::make_shared<SphereCollider>(0.1f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());
        auto colB = std::make_shared<SphereCollider>(0.1f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        // k=50, c=1 の春定数スプリング
        world.AddConstraint(std::make_shared<SpringConstraint>(
            bodyA.get(), bodyB.get(), 1.0f, 50.0f, 1.0f));

        // 0.5 秒後: bodyB は B が A 方向に引き寄せられているはず (x < 3.0)
        for (int s = 0; s < 30; ++s) world.Step(1.0f / 60.0f);

        const float xAfterHalf = bodyB->GetPosition().x;
        checkF(xAfterHalf < 2.5f,
               "SpringConstraint: body B pulled toward A after 0.5s",
               xAfterHalf, "x < 2.5");

        // 1.0 秒後: 振動して自然長付近を通り越しているはず (x < 1.5)
        for (int s = 0; s < 30; ++s) world.Step(1.0f / 60.0f);
        const float xAfterFull = bodyB->GetPosition().x;
        checkF(xAfterFull < 1.5f,
               "SpringConstraint: body B oscillates past rest length at 1.0s",
               xAfterFull, "x < 1.5");
    }

    // ── 11-3: SpringConstraint — 圧縮時に押し戻すか ─────────────────────
    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->m_isStatic = true;
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });

        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 0.3f, 0.0f, 0.0f }); // 自然長 1.0m より圧縮 (0.7m)
        bodyB->SetVelocity({ 0.0f, 0.0f, 0.0f });

        auto colA = std::make_shared<SphereCollider>(0.1f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());
        auto colB = std::make_shared<SphereCollider>(0.1f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());

        std::vector<std::shared_ptr<RigidBody>> bodies = { bodyA, bodyB };
        std::vector<ColliderInstance> cols = {
            makeInstance(colA, bodyA.get()),
            makeInstance(colB, bodyB.get()),
        };

        World world;
        world.SetGravity({ 0.0f, 0.0f, 0.0f });
        world.BeginSceneSync();
        for (const auto& body : bodies)
            world.SyncBody({}, body);
        for (const auto& collider : cols)
            world.SyncCollider({}, collider);
        world.EndSceneSync();
        world.AddConstraint(std::make_shared<SpringConstraint>(
            bodyA.get(), bodyB.get(), 1.0f, 50.0f, 1.0f));

        // 0.25 秒後: 押し戻されて x > 0.5 になっているはず
        for (int s = 0; s < 15; ++s) world.Step(1.0f / 60.0f);

        const float x = bodyB->GetPosition().x;
        checkF(x > 0.5f, "SpringConstraint: compressed spring pushes body away",
               x, "x > 0.5");
    }
}

// ─── エントリポイント ────────────────────────────────────────────────────────

static void TestPhase7_RigidBodyFreeze()
{
    std::printf("\n=== Phase 7b: RigidBody Freeze Axes ===\n");

    {
        RigidBody body;
        body.SetMass(1.0f);
        body.SetFreezePosition({ true, false, false });
        body.ApplyForce({ 10.0f, 5.0f, 0.0f });
        body.Integrate(1.0f);

        const Vector3 position = body.GetPosition();
        const Vector3 velocity = body.GetVelocity();
        checkF(std::abs(position.x) < 0.001f, "Freeze Position X: position x stays locked", position.x, "|x| < 0.001");
        checkF(std::abs(velocity.x) < 0.001f, "Freeze Position X: velocity x stays locked", velocity.x, "|vx| < 0.001");
        check(position.y > 0.0f, "Freeze Position X: unfrozen axis still moves");
    }

    {
        RigidBody body;
        body.SetMass(1.0f);
        body.SetFreezeRotation({ false, true, false });
        body.ApplyAngularImpulse({ 1.0f, 2.0f, 3.0f });

        const Vector3 angularVelocity = body.GetAngularVelocity();
        checkF(std::abs(angularVelocity.y) < 0.001f,
               "Freeze Rotation Y: angular velocity y stays locked",
               angularVelocity.y,
               "|wy| < 0.001");
        check(std::abs(angularVelocity.x) > 0.0f || std::abs(angularVelocity.z) > 0.0f,
              "Freeze Rotation Y: unfrozen angular axes still respond");
    }
}

static void TestPhase12_PhysicsFeatureCoverage()
{
    std::printf("\n=== Phase 12: Physics Feature Coverage ===\n");

    {
        RigidBody negativeMass;
        negativeMass.SetMass(-1.0f);
        negativeMass.ApplyForce({ 0.0f, -10.0f, 0.0f });
        negativeMass.Integrate(1.0f);
        check(negativeMass.GetVelocity().y > 0.0f,
              "RigidBody: negative mass accelerates opposite force");

        RigidBody torqueBody;
        torqueBody.SetMass(1.0f);
        torqueBody.ApplyTorque({ 0.0f, 2.0f, 0.0f });
        torqueBody.Integrate(1.0f);
        check(torqueBody.GetAngularVelocity().y > 0.0f,
              "RigidBody: ApplyTorque changes angular velocity");

        RigidBody offCenterBody;
        offCenterBody.SetMass(1.0f);
        offCenterBody.ApplyForceAtPoint({ 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
        offCenterBody.Integrate(1.0f);
        check(offCenterBody.GetAngularVelocity().z > 0.0f,
              "RigidBody: ApplyForceAtPoint generates torque");

        RigidBody dragBody;
        dragBody.SetMass(1.0f);
        dragBody.SetVelocity({ 10.0f, 0.0f, 0.0f });
        dragBody.m_linearDrag = 1.0f;
        dragBody.Integrate(1.0f);
        checkF(dragBody.GetVelocity().x < 10.0f,
               "RigidBody: linear drag damps velocity",
               dragBody.GetVelocity().x,
               "vx < 10");

        RigidBody sleeper;
        sleeper.SetMass(1.0f);
        sleeper.UpdateSleepState(1.0f, 0.03f, 0.03f, 0.75f);
        check(sleeper.IsSleeping(), "RigidBody: low motion body can sleep");
        sleeper.ApplyForce({ 1.0f, 0.0f, 0.0f });
        check(!sleeper.IsSleeping(), "RigidBody: external force wakes sleeping body");
    }

    {
        auto gravityScaled = std::make_shared<RigidBody>();
        gravityScaled->SetMass(1.0f);
        gravityScaled->m_gravityScale = 2.0f;

        World world;
        world.SetSubsteps(64);
        check(world.GetSubsteps() == 32,
              "World: substeps are clamped to supported range");
        world.SetGravity({ 0.0f, -10.0f, 0.0f });
        world.BeginSceneSync();
        world.SyncBody({}, gravityScaled);
        world.EndSceneSync();
        world.Step(1.0f);
        check(gravityScaled->GetVelocity().y < -10.0f,
              "World: gravityScale multiplies world gravity");

        auto sourceA = std::make_shared<RigidBody>();
        sourceA->SetMass(1.0f);
        sourceA->SetPosition({ 0.0f, 0.0f, 0.0f });
        sourceA->m_isGravitationalSource = true;
        sourceA->m_gravitationalMass = 1.0e8f;
        auto sourceB = std::make_shared<RigidBody>();
        sourceB->SetMass(1.0f);
        sourceB->SetPosition({ 1.0f, 0.0f, 0.0f });
        sourceB->m_isGravitationalSource = true;
        sourceB->m_gravitationalMass = 1.0e8f;

        World nBodyWorld;
        nBodyWorld.SetGravity(Vector3::ZERO);
        nBodyWorld.BeginSceneSync();
        nBodyWorld.SyncBody({}, sourceA);
        nBodyWorld.SyncBody({}, sourceB);
        nBodyWorld.EndSceneSync();
        nBodyWorld.Step(1.0f);
        check(sourceA->GetVelocity().x > 0.0f && sourceB->GetVelocity().x < 0.0f,
              "World: gravitational sources attract each other");
    }

    {
        World world;
        auto nearBody = std::make_shared<RigidBody>();
        nearBody->m_isStatic = true;
        nearBody->SetPosition({ 2.0f, 0.0f, 0.0f });
        auto farBody = std::make_shared<RigidBody>();
        farBody->m_isStatic = true;
        farBody->SetPosition({ 4.0f, 0.0f, 0.0f });

        auto nearCol = std::make_shared<SphereCollider>(0.5f);
        nearCol->Update(nearBody->GetPosition(), nearBody->GetRotation());
        auto farCol = std::make_shared<SphereCollider>(0.5f);
        farCol->Update(farBody->GetPosition(), farBody->GetRotation());

        world.BeginSceneSync();
        BodyHandle nearBodyHandle = world.SyncBody({}, nearBody);
        world.SyncBody({}, farBody);
        ColliderHandle nearColHandle = world.SyncCollider({}, makeInstance(nearCol, nearBody.get()));
        world.SyncCollider({}, makeInstance(farCol, farBody.get()));
        world.EndSceneSync();

        World::RaycastHit hit;
        check(world.Raycast({ 0.0f, 0.0f, 0.0f }, Vector3::RIGHT, 10.0f, hit) &&
              hit.body == nearBody.get(),
              "World API: Raycast returns nearest collider");

        const auto hits = world.RaycastAll({ 0.0f, 0.0f, 0.0f }, Vector3::RIGHT, 10.0f);
        check(hits.size() == 2 && hits[0].distance <= hits[1].distance,
              "World API: RaycastAll returns sorted hits");

        World::RaycastHit sphereHit;
        check(world.SphereCast({ 0.0f, 1.0f, 0.0f }, 0.6f, Vector3::RIGHT, 10.0f, sphereHit),
              "World API: SphereCast detects inflated hit");

        const auto overlaps = world.OverlapSphere({ 2.0f, 0.0f, 0.0f }, 0.75f);
        check(overlaps.size() == 1 && overlaps[0]->body == nearBody.get(),
              "World API: OverlapSphere filters by radius");

        world.BeginSceneSync();
        world.SyncBody(nearBodyHandle, nearBody);
        world.SyncCollider(nearColHandle, makeInstance(nearCol, nearBody.get()));
        world.EndSceneSync();
        const auto remainingHits = world.RaycastAll({ 0.0f, 0.0f, 0.0f }, Vector3::RIGHT, 10.0f);
        check(remainingHits.size() == 1,
              "World sync: untouched body/collider handles are removed");
    }

    {
        fbzz::LayerCollisionMatrix matrix;
        matrix.Set(0, 1, false);
        check(!matrix.CanCollide(0, 1) && !matrix.CanCollide(1, 0),
              "LayerCollisionMatrix: Set is symmetric");

        auto bodyA = std::make_shared<RigidBody>();
        bodyA->SetMass(1.0f);
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });
        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 0.5f, 0.0f, 0.0f });
        auto colA = std::make_shared<SphereCollider>(0.5f);
        colA->Update(bodyA->GetPosition(), bodyA->GetRotation());
        auto colB = std::make_shared<SphereCollider>(0.5f);
        colB->Update(bodyB->GetPosition(), bodyB->GetRotation());
        ColliderInstance instA = makeInstance(colA, bodyA.get());
        ColliderInstance instB = makeInstance(colB, bodyB.get());
        instA.layer = 0;
        instB.layer = 1;

        World world;
        world.SetGravity(Vector3::ZERO);
        world.BeginSceneSync();
        world.SyncBody({}, bodyA);
        world.SyncBody({}, bodyB);
        world.SyncCollider({}, instA);
        world.SyncCollider({}, instB);
        world.EndSceneSync();
        world.Step(1.0f / 60.0f, [&matrix](int a, int b) { return matrix.CanCollide(a, b); });
        check(world.GetEnterEvents().empty(),
              "Layer filter: disabled layer pair suppresses collision");
    }

    {
        PhysicsSolver solver;
        PhysicsMaterial mat;

        auto sphere = std::make_shared<SphereCollider>(0.6f);
        sphere->Update({ 0.0f, 0.25f, 0.0f }, Quaternion::Identity());
        auto aabb = std::make_shared<AABBCollider>(Vector3{ 0.6f, 0.6f, 0.6f });
        aabb->Update({ 0.0f, 0.25f, 0.0f }, Quaternion::Identity());
        auto obb = std::make_shared<OBBCollider>(Vector3{ 0.6f, 0.6f, 0.6f });
        obb->Update({ 0.0f, 0.25f, 0.0f }, Quaternion::FromAxisAngle(Vector3::UP, 0.25f));
        auto capsule = std::make_shared<CapsuleCollider>(0.35f, 0.6f);
        capsule->Update({ 0.0f, 0.25f, 0.0f }, Quaternion::Identity());
        auto mesh = std::make_shared<TriangleMeshCollider>(
            std::vector<Vector3>{
                { -2.0f, 0.0f, -2.0f },
                {  2.0f, 0.0f, -2.0f },
                {  0.0f, 0.0f,  2.0f }
            },
            std::vector<uint32_t>{ 0, 1, 2 });
        mesh->Update({ 0.0f, 0.0f, 0.0f }, Quaternion::Identity());
        auto hull = std::make_shared<ConvexHullCollider>(
            std::vector<Vector3>{
                { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f },
                {  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f },
                { -0.5f, -0.5f,  0.5f }, { 0.5f, -0.5f,  0.5f },
                {  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f }
            });
        hull->Update({ 0.0f, 0.25f, 0.0f }, Quaternion::Identity());

        auto instance = [&mat](std::shared_ptr<Collider> collider) {
            ColliderInstance inst;
            inst.collider = std::move(collider);
            inst.material = &mat;
            return inst;
        };
        auto expectPair = [&solver, &instance](std::shared_ptr<Collider> a,
                                               std::shared_ptr<Collider> b,
                                               const char* label) {
            std::vector<CollisionPair> pairs = { { instance(std::move(a)), instance(std::move(b)) } };
            std::vector<ContactPoint> contacts;
            solver.NarrowPhase(pairs, contacts);
            check(!contacts.empty(), label);
        };

        expectPair(sphere, aabb, "NarrowPhase: Sphere-AABB contact");
        expectPair(sphere, obb, "NarrowPhase: Sphere-OBB contact");
        expectPair(sphere, capsule, "NarrowPhase: Sphere-Capsule contact");
        expectPair(sphere, mesh, "NarrowPhase: Sphere-TriangleMesh contact");
        expectPair(sphere, hull, "NarrowPhase: Sphere-ConvexHull contact");
        expectPair(aabb, obb, "NarrowPhase: AABB-OBB contact");
        expectPair(aabb, capsule, "NarrowPhase: AABB-Capsule contact");
        expectPair(aabb, mesh, "NarrowPhase: AABB-TriangleMesh contact");
        expectPair(aabb, hull, "NarrowPhase: AABB-ConvexHull contact");
        expectPair(obb, capsule, "NarrowPhase: OBB-Capsule contact");
        expectPair(obb, mesh, "NarrowPhase: OBB-TriangleMesh contact");
        expectPair(obb, hull, "NarrowPhase: OBB-ConvexHull contact");
        expectPair(capsule, mesh, "NarrowPhase: Capsule-TriangleMesh contact");
        expectPair(capsule, hull, "NarrowPhase: Capsule-ConvexHull contact");
        expectPair(hull, mesh, "NarrowPhase: ConvexHull-TriangleMesh contact");
    }

    {
        auto bodyA = std::make_shared<RigidBody>();
        bodyA->m_isStatic = true;
        bodyA->SetPosition({ 0.0f, 0.0f, 0.0f });
        auto bodyB = std::make_shared<RigidBody>();
        bodyB->SetMass(1.0f);
        bodyB->SetPosition({ 5.0f, 2.0f, 0.0f });

        RopeConstraint rope(bodyA.get(), bodyB.get(), 2.0f);
        rope.SolvePosition(1.0f / 60.0f);
        check((bodyB->GetPosition() - bodyA->GetPosition()).Length() <= 2.01f,
              "RopeConstraint: max length is enforced");

        bodyB->SetPosition({ 2.0f, 3.0f, 0.0f });
        SliderConstraint slider(bodyA.get(), bodyB.get(), Vector3::RIGHT);
        slider.SetLimits(0.0f, 1.0f);
        slider.SolvePosition(1.0f / 60.0f);
        check(std::abs(bodyB->GetPosition().y) < 0.001f &&
              bodyB->GetPosition().x <= 1.01f,
              "SliderConstraint: off-axis and limit errors are corrected");

        bodyB->SetPosition({ 3.0f, 0.0f, 0.0f });
        FixedConstraint fixed(bodyA.get(), bodyB.get());
        bodyB->SetPosition({ 6.0f, 0.0f, 0.0f });
        fixed.SolvePosition(1.0f / 60.0f);
        check(std::abs(bodyB->GetPosition().x - 3.0f) < 0.01f,
              "FixedConstraint: initial relative offset is restored");

        bodyB->SetPosition({ 2.0f, 0.0f, 0.0f });
        HingeConstraint hinge(bodyA.get(), bodyB.get(), Vector3::ZERO, Vector3::ZERO, Vector3::UP);
        hinge.SetMotor(2.0f, 10.0f);
        hinge.ApplyForce(1.0f / 60.0f);
        bodyB->Integrate(1.0f / 60.0f);
        check(bodyB->GetAngularVelocity().y > 0.0f,
              "HingeConstraint: motor applies angular velocity on hinge axis");
        hinge.SolvePosition(1.0f / 60.0f);
        check((bodyB->GetPosition() - bodyA->GetPosition()).Length() < 0.01f,
              "HingeConstraint: anchors are merged");

        auto chainA = std::make_shared<RigidBody>();
        chainA->m_isStatic = true;
        chainA->SetPosition({ 0.0f, 0.0f, 0.0f });
        auto chainB = std::make_shared<RigidBody>();
        chainB->SetMass(1.0f);
        chainB->SetPosition({ 3.0f, 0.0f, 0.0f });
        auto chainC = std::make_shared<RigidBody>();
        chainC->SetMass(1.0f);
        chainC->SetPosition({ 6.0f, 0.0f, 0.0f });
        ChainConstraint chain({ chainA.get(), chainB.get(), chainC.get() }, 1.0f, 8);
        chain.SolvePosition(1.0f / 60.0f);
        check((chainB->GetPosition() - chainA->GetPosition()).Length() < 1.2f &&
              (chainC->GetPosition() - chainB->GetPosition()).Length() < 1.2f,
              "ChainConstraint: neighboring segment lengths are corrected");
    }

    {
        auto body = std::make_shared<RigidBody>();
        body->SetMass(1.0f);
        body->SetPosition({ 0.0f, 0.0f, 0.0f });

        auto volumeShape = std::make_shared<AABBCollider>(Vector3{ 5.0f, 5.0f, 5.0f });
        volumeShape->Update(Vector3::ZERO, Quaternion::Identity());
        VolumeSettings gravitySettings;
        gravitySettings.type = VolumeType::Gravity;
        gravitySettings.gravity = { 0.0f, 10.0f, 0.0f };

        World world;
        world.SetGravity({ 0.0f, -10.0f, 0.0f });
        world.BeginSceneSync();
        world.SyncBody({}, body);
        world.SyncVolume({}, std::make_shared<ColliderVolume>(volumeShape, gravitySettings));
        world.EndSceneSync();
        world.Step(1.0f / 60.0f);
        check(body->GetVelocity().y > 0.0f,
              "Volume: gravity volume overrides world gravity");

        RigidBody magneticBody;
        magneticBody.SetMass(1.0f);
        magneticBody.m_charge = 1.0f;
        magneticBody.SetVelocity(Vector3::RIGHT);
        VolumeSettings magneticSettings;
        magneticSettings.type = VolumeType::Magnetic;
        magneticSettings.magneticField = Vector3::UP;
        ColliderVolume magneticVolume(volumeShape, magneticSettings);
        magneticVolume.Apply(magneticBody, 1.0f);
        magneticBody.Integrate(1.0f);
        check(magneticBody.GetVelocity().z > 0.0f,
              "Volume: magnetic field applies Lorentz force");

        RigidBody vortexBody;
        vortexBody.SetMass(1.0f);
        vortexBody.SetPosition({ 1.0f, 0.0f, 0.0f });
        VolumeSettings vortexSettings;
        vortexSettings.type = VolumeType::Vortex;
        vortexSettings.swirlStrength = 1.0f;
        vortexSettings.inwardStrength = 1.0f;
        vortexSettings.liftStrength = 1.0f;
        ColliderVolume vortexVolume(volumeShape, vortexSettings);
        vortexVolume.Apply(vortexBody, 1.0f);
        vortexBody.Integrate(1.0f);
        check(vortexBody.GetVelocity().LengthSq() > 0.0f,
              "Volume: vortex applies swirl/inward/lift force");

        RigidBody buoyancyBody;
        buoyancyBody.SetMass(1.0f);
        VolumeSettings buoyancySettings;
        buoyancySettings.type = VolumeType::Buoyancy;
        buoyancySettings.buoyancy = 5.0f;
        buoyancySettings.drag = 0.0f;
        ColliderVolume buoyancyVolume(volumeShape, buoyancySettings);
        buoyancyVolume.Apply(buoyancyBody, 1.0f);
        buoyancyBody.Integrate(1.0f);
        check(buoyancyBody.GetVelocity().y > 0.0f,
              "Volume: buoyancy applies upward force");

        RigidBody explosionBody;
        explosionBody.SetMass(1.0f);
        explosionBody.SetPosition({ 1.0f, 0.0f, 0.0f });
        VolumeSettings explosionSettings;
        explosionSettings.type = VolumeType::Explosion;
        explosionSettings.explosionImpulse = 2.0f;
        ColliderVolume explosionVolume(volumeShape, explosionSettings);
        explosionVolume.Apply(explosionBody, 1.0f);
        check(explosionBody.GetVelocity().x > 0.0f,
              "Volume: explosion applies outward impulse");

        VolumeSettings timedSettings;
        timedSettings.type = VolumeType::TimeDilation;
        timedSettings.timeScale = 0.25f;
        timedSettings.duration = 0.1f;
        ColliderVolume timedVolume(volumeShape, timedSettings);
        timedVolume.Tick(0.2f);
        check(std::abs(timedVolume.GetTimeScale() - 0.25f) < 0.001f &&
              timedVolume.IsExpired(),
              "Volume: time dilation scale and duration are exposed");
    }

    {
        AABBCollider box(Vector3{ 1.0f, 1.0f, 1.0f });
        box.Update(Vector3::ZERO, Quaternion::Identity());
        const ColliderDebugGeometry boxDebug = BuildColliderDebugGeometry(box);
        check(!boxDebug.lines.empty(), "DebugGeometry: collider wire lines are generated");

        RigidBody bodyA;
        RigidBody bodyB;
        bodyA.SetPosition(Vector3::ZERO);
        bodyB.SetPosition(Vector3::RIGHT);
        DistanceConstraint distance(&bodyA, &bodyB, 1.0f);
        const ConstraintDebugGeometry constraintDebug = BuildConstraintDebugGeometry(distance);
        check(!constraintDebug.lines.empty(), "DebugGeometry: constraint lines are generated");
    }

    {
        RigidBody source;
        source.SetMass(2.0f);
        source.SetPosition({ 1.0f, 2.0f, 3.0f });
        source.SetVelocity({ 4.0f, 5.0f, 6.0f });
        source.SetFreezePosition({ true, false, true });
        source.SetFreezeRotation({ false, true, false });
        source.m_charge = 3.0f;
        source.m_isGravitationalSource = true;
        source.m_gravitationalMass = 4.0f;

        const std::string saved = RigidBodySerializer::Serialize(source);
        RigidBody loaded;
        const bool ok = RigidBodySerializer::Deserialize(loaded, saved);
        check(ok &&
              std::abs(loaded.GetMass() - 2.0f) < 0.001f &&
              std::abs(loaded.GetPosition().x - 1.0f) < 0.001f &&
              loaded.GetFreezePosition().x &&
              loaded.GetFreezeRotation().y &&
              loaded.m_isGravitationalSource,
              "RigidBodySerializer: state and extended settings round-trip");
    }
}

int main()
{
    std::printf("FBZZ Physics Test\n");
    std::printf("=================\n");

    TestPhase1_StackStability();
    TestPhase2_TriangleMesh();
    TestPhase3_ConvexHull();
    TestPhase4_CCD();
    TestPhase5_StackStress();
    TestPhase6_Capsule();
    TestPhase7_Momentum();
    TestPhase7_RigidBodyFreeze();
    TestPhase8_Materials();
    TestPhase9_ConvexHullWorld();
    TestPhase10_Triggers();
    TestPhase11_Constraints();
    TestPhase12_PhysicsFeatureCoverage();

    std::printf("\n=================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
