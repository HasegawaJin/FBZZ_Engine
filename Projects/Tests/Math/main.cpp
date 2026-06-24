// FBZZ Engine
// Tests/Math/main.cpp
// Math モジュール単体テスト: Vector3 / Quaternion / Matrix4 / MathUtils / Plane / Ray / Frustum
#include <cstdio>
#include <cmath>
#include <vector>
#include <memory>

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Plane.hpp>
#include <Math/Ray.hpp>
#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>

#include "../TestHelper.hpp"

using namespace fbzz::math;

// ─── Vector3 ──────────────────────────────────────────────────────────────────

static void TestMath_Vector3()
{
    std::printf("\n=== Math: Vector3 ===\n");

    // 基本算術
    {
        const Vector3 a{ 1.0f, 2.0f, 3.0f };
        const Vector3 b{ 4.0f, 5.0f, 6.0f };

        const Vector3 sum  = a + b;
        checkV(std::abs(sum.x-5.f)<1e-5f && std::abs(sum.y-7.f)<1e-5f && std::abs(sum.z-9.f)<1e-5f,
               "Vector3: (1,2,3)+(4,5,6)==(5,7,9)", sum.x, sum.y, sum.z, "(5,7,9)");

        const Vector3 diff = b - a;
        checkV(std::abs(diff.x-3.f)<1e-5f && std::abs(diff.y-3.f)<1e-5f && std::abs(diff.z-3.f)<1e-5f,
               "Vector3: (4,5,6)-(1,2,3)==(3,3,3)", diff.x, diff.y, diff.z, "(3,3,3)");

        const Vector3 neg = -a;
        checkV(std::abs(neg.x+1.f)<1e-5f && std::abs(neg.y+2.f)<1e-5f && std::abs(neg.z+3.f)<1e-5f,
               "Vector3: -(1,2,3)==(-1,-2,-3)", neg.x, neg.y, neg.z, "(-1,-2,-3)");

        const Vector3 scaled  = a * 3.0f;
        const Vector3 divided = b / 2.0f;
        checkV(std::abs(scaled.x-3.f)<1e-5f && std::abs(scaled.y-6.f)<1e-5f && std::abs(scaled.z-9.f)<1e-5f,
               "Vector3: (1,2,3)*3==(3,6,9)", scaled.x, scaled.y, scaled.z, "(3,6,9)");
        checkV(std::abs(divided.x-2.f)<1e-5f && std::abs(divided.y-2.5f)<1e-5f && std::abs(divided.z-3.f)<1e-5f,
               "Vector3: (4,5,6)/2==(2,2.5,3)", divided.x, divided.y, divided.z, "(2,2.5,3)");
    }

    // 長さ・正規化
    {
        const Vector3 v{ 3.0f, 4.0f, 0.0f };
        checkF(std::abs(v.LengthSq()-25.0f)<1e-5f, "Vector3: LengthSq(3,4,0)==25", v.LengthSq(), "==25");
        checkF(std::abs(v.Length() - 5.0f)<1e-5f,  "Vector3: Length(3,4,0)==5",    v.Length(),   "==5");

        const Vector3 n = v.Normalized();
        checkF(std::abs(n.Length()-1.0f)<1e-5f, "Vector3: Normalized は単位長", n.Length(), "==1");
        checkF(std::abs(n.x-0.6f)<1e-5f,        "Vector3: Normalized.x==0.6",  n.x, "==0.6");
        checkF(std::abs(n.y-0.8f)<1e-5f,        "Vector3: Normalized.y==0.8",  n.y, "==0.8");
    }

    // 内積
    {
        checkF(std::abs(Vector3::Dot(Vector3::UP, Vector3::RIGHT))<1e-5f,
               "Vector3: Dot(UP,RIGHT)==0 (直交)", Vector3::Dot(Vector3::UP, Vector3::RIGHT), "==0");
        checkF(std::abs(Vector3::Dot(Vector3::UP, Vector3::UP)-1.0f)<1e-5f,
               "Vector3: Dot(UP,UP)==1 (同方向)", Vector3::Dot(Vector3::UP, Vector3::UP), "==1");

        const float d = Vector3::Dot({1.0f,2.0f,3.0f}, {4.0f,5.0f,6.0f}); // 4+10+18=32
        checkF(std::abs(d-32.0f)<1e-5f, "Vector3: Dot((1,2,3),(4,5,6))==32", d, "==32");
    }

    // 外積: RIGHT(1,0,0)×UP(0,1,0)=(0*0-0*1, 0*0-1*0, 1*1-0*0)=(0,0,1)=FORWARD
    {
        const Vector3 c = Vector3::Cross(Vector3::RIGHT, Vector3::UP);
        checkF(std::abs(c.x)<1e-5f && std::abs(c.y)<1e-5f,
               "Vector3: Cross(RIGHT,UP) x,y==0", c.x, "==0");
        checkF(std::abs(c.z-1.0f)<1e-5f, "Vector3: Cross(RIGHT,UP).z==1 (FORWARD)", c.z, "==1");

        // 反交換律: a×b == -(b×a)
        const Vector3 ba = Vector3::Cross(Vector3::UP, Vector3::RIGHT);
        checkF(std::abs(ba.z+1.0f)<1e-5f, "Vector3: Cross(UP,RIGHT).z==-1 (反交換律)", ba.z, "==-1");
    }

    // Lerp
    {
        const Vector3 L = Vector3::Lerp(Vector3::ZERO, {10.0f,20.0f,30.0f}, 0.5f);
        checkF(std::abs(L.x-5.0f)<1e-5f,  "Vector3: Lerp(0→10, 0.5)==5",  L.x, "==5");
        checkF(std::abs(L.y-10.0f)<1e-5f, "Vector3: Lerp(0→20, 0.5)==10", L.y, "==10");
        checkF(std::abs(L.z-15.0f)<1e-5f, "Vector3: Lerp(0→30, 0.5)==15", L.z, "==15");
    }

    // 静的定数
    {
        check(Vector3::ZERO == Vector3{0.0f,0.0f,0.0f}, "Vector3: ZERO==(0,0,0)");
        check(std::abs(Vector3::UP.y-1.0f)<1e-5f,       "Vector3: UP.y==1");
        check(std::abs(Vector3::RIGHT.x-1.0f)<1e-5f,    "Vector3: RIGHT.x==1");
        check(std::abs(Vector3::FORWARD.z-1.0f)<1e-5f,  "Vector3: FORWARD.z==1");
    }
}

// ─── Quaternion ───────────────────────────────────────────────────────────────

static void TestMath_Quaternion()
{
    std::printf("\n=== Math: Quaternion ===\n");

    // Identity は回転なし
    {
        const Vector3 r = Quaternion::Identity() * Vector3::UP;
        checkF(std::abs(r.x)<1e-4f && std::abs(r.y-1.f)<1e-4f && std::abs(r.z)<1e-4f,
               "Quaternion: Identity * UP == UP", r.y, "y==1");
    }

    // 0° 回転でベクトル不変
    {
        const Quaternion q0 = Quaternion::FromAxisAngle(Vector3::UP, 0.0f);
        const Vector3 r = q0 * Vector3::RIGHT;
        checkF(std::abs(r.x-1.f)<1e-4f, "Quaternion: 0° 回転でベクトル不変", r.x, "x==1");
    }

    // 180° 回転: Z 軸周り → RIGHT.x == -1
    {
        const Quaternion q180 = Quaternion::FromAxisAngle(Vector3::FORWARD, PI);
        const Vector3 r = q180 * Vector3::RIGHT;
        checkF(std::abs(r.x+1.0f)<1e-4f, "Quaternion: Z軸180° → RIGHT.x≈-1", r.x, "≈-1");
        checkF(std::abs(r.y)<1e-4f,       "Quaternion: Z軸180° → y≈0",        r.y, "≈0");
        checkF(std::abs(r.z)<1e-4f,       "Quaternion: Z軸180° → z≈0",        r.z, "≈0");
    }

    // 360° 回転でベクトル復元
    {
        const Quaternion q360 = Quaternion::FromAxisAngle(Vector3::UP, TWO_PI);
        const Vector3 r = q360 * Vector3::RIGHT;
        checkF(std::abs(r.x-1.f)<1e-4f, "Quaternion: 360° 回転でベクトル復元", r.x, "≈1");
    }

    // 回転は長さを保つ
    {
        const Vector3 v{ 1.0f, 2.0f, 3.0f };
        const Quaternion q = Quaternion::FromAxisAngle(Vector3::UP, PI * 0.37f);
        const float rLen = (q * v).Length();
        checkF(std::abs(rLen-v.Length())<1e-4f, "Quaternion: 回転は長さを保つ", rLen, "==origLen");
    }

    // q * q^-1 = Identity (元のベクトルに戻る)
    {
        const Quaternion q  = Quaternion::FromAxisAngle({1.0f,1.0f,0.0f}, PI * 0.4f);
        const Vector3 v{ 1.0f, 0.5f, -0.5f };
        const Vector3 r = q * (q.Inverse() * v);
        checkF(std::abs(r.x-v.x)<1e-4f && std::abs(r.y-v.y)<1e-4f && std::abs(r.z-v.z)<1e-4f,
               "Quaternion: q*(q^-1*v)==v", r.x, "x==orig");
    }

    // 合成: 同軸の 90° × 2 = 180° → RIGHT.x ≈ -1
    {
        const Quaternion q90  = Quaternion::FromAxisAngle(Vector3::FORWARD, PI * 0.5f);
        const Quaternion q180 = q90 * q90;
        const Vector3 r = q180 * Vector3::RIGHT;
        checkF(std::abs(r.x+1.f)<2e-4f, "Quaternion: 90°×2合成=180° → RIGHT.x≈-1", r.x, "≈-1");
    }

    // Slerp: t=0 は a、t=1 は b に一致 (長さ保持で確認)
    {
        const Quaternion a  = Quaternion::Identity();
        const Quaternion b  = Quaternion::FromAxisAngle(Vector3::UP, PI);
        const Quaternion sa = Quaternion::Slerp(a, b, 0.0f);
        const Quaternion sb = Quaternion::Slerp(a, b, 1.0f);

        const Vector3 ra = sa * Vector3::RIGHT;
        checkF(std::abs(ra.x-1.f)<1e-4f, "Quaternion: Slerp(t=0)==a → RIGHT保持", ra.x, "x≈1");

        const float lenB = (sb * Vector3::RIGHT).Length();
        checkF(std::abs(lenB-1.f)<1e-4f, "Quaternion: Slerp(t=1)==b 長さ保持", lenB, "≈1");
    }

    // FromEuler: (0, 0, 180°) は Z 軸 180° 回転と等価
    {
        const Quaternion qe = Quaternion::FromEuler({ 0.0f, 0.0f, PI });
        const Vector3 r = qe * Vector3::RIGHT;
        checkF(std::abs(r.x+1.f)<1e-4f, "Quaternion: FromEuler(0,0,π) → RIGHT.x≈-1", r.x, "≈-1");
    }
}

// ─── Matrix4 ──────────────────────────────────────────────────────────────────

static void TestMath_Matrix4()
{
    std::printf("\n=== Math: Matrix4 ===\n");

    // Identity の対角成分 = 1
    {
        const Matrix4 id = Matrix4::Identity();
        checkF(std::abs(id.m[0][0]-1.f)<1e-5f && std::abs(id.m[1][1]-1.f)<1e-5f &&
               std::abs(id.m[2][2]-1.f)<1e-5f && std::abs(id.m[3][3]-1.f)<1e-5f,
               "Matrix4: Identity 対角==1", id.m[0][0], "==1");

        // Identity * v == v
        const Vector4 v{ 1.0f, 2.0f, 3.0f, 1.0f };
        const Vector4 r = id * v;
        checkF(std::abs(r.x-1.f)<1e-5f && std::abs(r.y-2.f)<1e-5f && std::abs(r.z-3.f)<1e-5f,
               "Matrix4: Identity * v == v", r.x, "x==1");
    }

    // 平行移動: Translate(1,2,3) * (0,0,0,1) = (1,2,3,1)
    {
        const Matrix4 T = Matrix4::Translate({ 1.0f, 2.0f, 3.0f });
        const Vector4 r = T * Vector4{ 0.0f, 0.0f, 0.0f, 1.0f };
        checkF(std::abs(r.x-1.f)<1e-5f, "Matrix4: Translate x==1", r.x, "==1");
        checkF(std::abs(r.y-2.f)<1e-5f, "Matrix4: Translate y==2", r.y, "==2");
        checkF(std::abs(r.z-3.f)<1e-5f, "Matrix4: Translate z==3", r.z, "==3");
        checkF(std::abs(r.w-1.f)<1e-5f, "Matrix4: Translate w 不変==1", r.w, "==1");
    }

    // スケール: Scale(2,3,4) * (1,1,1,1) = (2,3,4,1)
    {
        const Matrix4 S = Matrix4::Scale({ 2.0f, 3.0f, 4.0f });
        const Vector4 r = S * Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
        checkF(std::abs(r.x-2.f)<1e-5f, "Matrix4: Scale x==2", r.x, "==2");
        checkF(std::abs(r.y-3.f)<1e-5f, "Matrix4: Scale y==3", r.y, "==3");
        checkF(std::abs(r.z-4.f)<1e-5f, "Matrix4: Scale z==4", r.z, "==4");
    }

    // TRS(T=(1,0,0), R=Identity, S=(2,2,2)) * (1,0,0,1) = (3,0,0,1)
    {
        const Matrix4 M = Matrix4::TRS(
            { 1.0f, 0.0f, 0.0f },
            Quaternion::Identity(),
            { 2.0f, 2.0f, 2.0f });
        const Vector4 r = M * Vector4{ 1.0f, 0.0f, 0.0f, 1.0f };
        checkF(std::abs(r.x-3.f)<1e-5f, "Matrix4: TRS(T=(1,0,0),S=2) * (1,0,0,1).x==3", r.x, "==3");
        checkF(std::abs(r.y)<1e-5f,      "Matrix4: TRS result y==0", r.y, "==0");
    }

    // (A^T)^T == A
    {
        const Matrix4 A = Matrix4::TRS(
            { 1.0f, 2.0f, 3.0f },
            Quaternion::FromAxisAngle(Vector3::UP, PI * 0.25f),
            { 1.0f, 1.0f, 1.0f });
        const Matrix4 ATT = Matrix4::Transpose(Matrix4::Transpose(A));
        bool eq = true;
        for (int i = 0; i < 4 && eq; ++i)
            for (int j = 0; j < 4 && eq; ++j)
                if (std::abs(ATT.m[i][j] - A.m[i][j]) > 1e-5f) eq = false;
        check(eq, "Matrix4: (A^T)^T == A");
    }

    // M * M^-1 ≈ Identity
    {
        const Matrix4 M = Matrix4::TRS(
            { 3.0f, 1.0f, -2.0f },
            Quaternion::FromAxisAngle(Vector3::RIGHT, PI * 0.3f),
            { 1.5f, 2.0f, 0.5f });
        const Matrix4 Id = M * Matrix4::Inverse(M);
        checkF(std::abs(Id.m[0][0]-1.f)<1e-4f, "Matrix4: M*M^-1 [0][0]≈1", Id.m[0][0], "≈1");
        checkF(std::abs(Id.m[1][1]-1.f)<1e-4f, "Matrix4: M*M^-1 [1][1]≈1", Id.m[1][1], "≈1");
        checkF(std::abs(Id.m[2][2]-1.f)<1e-4f, "Matrix4: M*M^-1 [2][2]≈1", Id.m[2][2], "≈1");
        checkF(std::abs(Id.m[0][1])<1e-4f,     "Matrix4: M*M^-1 [0][1]≈0", Id.m[0][1], "≈0");
    }
}

// ─── MathUtils ────────────────────────────────────────────────────────────────

static void TestMath_MathUtils()
{
    std::printf("\n=== Math: MathUtils ===\n");

    // Clamp
    {
        checkF(std::abs(Clamp(5.0f, 0.0f, 10.0f)-5.0f)<1e-6f,   "MathUtils: Clamp(5,0,10)==5",  Clamp(5.0f,0.0f,10.0f),  "==5");
        checkF(std::abs(Clamp(-1.0f, 0.0f, 10.0f))<1e-6f,        "MathUtils: Clamp(-1,0,10)==0", Clamp(-1.0f,0.0f,10.0f), "==0");
        checkF(std::abs(Clamp(20.0f, 0.0f, 10.0f)-10.0f)<1e-6f,  "MathUtils: Clamp(20,0,10)==10",Clamp(20.0f,0.0f,10.0f), "==10");
    }

    // Lerp
    {
        checkF(std::abs(Lerp(0.0f, 10.0f, 0.5f)-5.0f)<1e-6f,  "MathUtils: Lerp(0,10,0.5)==5",  Lerp(0.0f,10.0f,0.5f), "==5");
        checkF(std::abs(Lerp(0.0f, 10.0f, 0.0f))<1e-6f,       "MathUtils: Lerp(0,10,0)==0",    Lerp(0.0f,10.0f,0.0f), "==0");
        checkF(std::abs(Lerp(0.0f, 10.0f, 1.0f)-10.0f)<1e-6f, "MathUtils: Lerp(0,10,1)==10",   Lerp(0.0f,10.0f,1.0f), "==10");
    }

    // InverseLerp
    {
        checkF(std::abs(InverseLerp(0.0f, 10.0f, 5.0f)-0.5f)<1e-6f,
               "MathUtils: InverseLerp(0,10,5)==0.5", InverseLerp(0.0f,10.0f,5.0f), "==0.5");
        checkF(std::abs(InverseLerp(0.0f, 10.0f, 0.0f))<1e-6f,
               "MathUtils: InverseLerp(0,10,0)==0",   InverseLerp(0.0f,10.0f,0.0f), "==0");
    }

    // Remap
    {
        const float r = Remap(5.0f, 0.0f, 10.0f, 100.0f, 200.0f);
        checkF(std::abs(r-150.0f)<1e-5f, "MathUtils: Remap(5, 0→10, 100→200)==150", r, "==150");
    }

    // NearlyEqual / NearlyZero
    {
        check(NearlyEqual(1.0f, 1.0f + 1e-7f), "MathUtils: NearlyEqual(1, 1+1e-7)");
        check(!NearlyEqual(1.0f, 2.0f),         "MathUtils: !NearlyEqual(1, 2)");
        check(NearlyZero(1e-7f),                "MathUtils: NearlyZero(1e-7)");
        check(!NearlyZero(0.1f),                "MathUtils: !NearlyZero(0.1)");
    }

    // 角度変換
    {
        checkF(std::abs(ToRad(180.0f)-PI)<1e-5f,     "MathUtils: ToRad(180)==PI",  ToRad(180.0f), "==PI");
        checkF(std::abs(ToDeg(PI)-180.0f)<1e-4f,     "MathUtils: ToDeg(PI)==180",  ToDeg(PI),     "==180");
        checkF(std::abs(ToRad(360.0f)-TWO_PI)<1e-5f, "MathUtils: ToRad(360)==2PI", ToRad(360.0f), "==2PI");
    }

    // Min / Max / Abs / Sign
    {
        checkF(std::abs(Min(3.0f, 7.0f)-3.0f)<1e-6f, "MathUtils: Min(3,7)==3",   Min(3.0f,7.0f), "==3");
        checkF(std::abs(Max(3.0f, 7.0f)-7.0f)<1e-6f, "MathUtils: Max(3,7)==7",   Max(3.0f,7.0f), "==7");
        checkF(std::abs(Abs(-5.0f)-5.0f)<1e-6f,       "MathUtils: Abs(-5)==5",    Abs(-5.0f),     "==5");
        checkF(std::abs(Sign(-3.0f)+1.0f)<1e-6f,      "MathUtils: Sign(-3)==-1",  Sign(-3.0f),    "==-1");
        checkF(std::abs(Sign( 3.0f)-1.0f)<1e-6f,      "MathUtils: Sign(+3)==1",   Sign( 3.0f),    "==1");
    }
}

// ─── Plane ────────────────────────────────────────────────────────────────────

static void TestMath_Plane()
{
    std::printf("\n=== Math: Plane ===\n");

    // Y=0 水平面: Plane(normal=UP, d=0) → dot(UP,p)+0=0 → y=0
    {
        const Plane floor(Vector3::UP, 0.0f);

        const float above = floor.SignedDistanceTo({ 0.0f,  5.0f, 0.0f });
        const float below = floor.SignedDistanceTo({ 0.0f, -3.0f, 0.0f });
        checkF(std::abs(above-5.0f)<1e-5f, "Plane: 上方 y=5 の符号付き距離==5",   above, "==5");
        checkF(std::abs(below+3.0f)<1e-5f, "Plane: 下方 y=-3 の符号付き距離==-3", below, "==-3");

        check( floor.IsOnPositiveSide({ 0.0f,  1.0f, 0.0f }), "Plane: (0,1,0) は Y=0 の正側");
        check(!floor.IsOnPositiveSide({ 0.0f, -1.0f, 0.0f }), "Plane: (0,-1,0) は Y=0 の負側");
    }

    // Normalized: 法線が単位ベクトル、符号の向きが保たれる
    {
        const Plane unnorm({ 0.0f, 2.0f, 0.0f }, 4.0f);
        const Plane norm = unnorm.Normalized();
        checkF(std::abs(norm.normal.Length()-1.0f)<1e-5f,
               "Plane: Normalized() 法線は単位ベクトル", norm.normal.Length(), "==1");
        const float d1 = unnorm.SignedDistanceTo({ 0.0f, 0.0f, 0.0f });
        const float d2 = norm.SignedDistanceTo({ 0.0f, 0.0f, 0.0f });
        check((d1 >= 0.0f) == (d2 >= 0.0f), "Plane: Normalized() は符号の向きを保つ");
    }

    // FromPoints: y=1 の平面
    {
        const Plane p = Plane::FromPoints(
            { 0.0f, 1.0f, 0.0f },
            { 1.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 1.0f });
        checkF(std::abs(p.normal.y) > 0.9f,
               "Plane: FromPoints(y=1面) 法線は Y 方向", std::abs(p.normal.y), ">0.9");
        const float d = p.SignedDistanceTo({ 5.0f, 1.0f, 3.0f });
        checkF(std::abs(d)<1e-4f, "Plane: FromPoints 任意の y=1 点の距離≈0", d, "≈0");
    }

    // FromNormalAndPoint
    {
        const Plane p = Plane::FromNormalAndPoint(Vector3::UP, { 0.0f, 3.0f, 0.0f });
        checkF(std::abs(p.SignedDistanceTo({ 0.0f, 3.0f, 0.0f }))<1e-4f,
               "Plane: FromNormalAndPoint → 構成点の距離≈0",
               p.SignedDistanceTo({ 0.0f, 3.0f, 0.0f }), "≈0");
        check(p.IsOnPositiveSide({ 0.0f, 5.0f, 0.0f }),
              "Plane: FromNormalAndPoint → 法線側 (y>3) が正側");
    }
}

// ─── Ray ──────────────────────────────────────────────────────────────────────

static void TestMath_Ray()
{
    std::printf("\n=== Math: Ray ===\n");

    // At(t) = origin + t * direction
    {
        const Ray ray({ 0.0f, 0.0f, 0.0f }, Vector3::UP);
        const Vector3 p = ray.At(5.0f);
        checkF(std::abs(p.y-5.0f)<1e-5f, "Ray: At(5) y==5", p.y, "==5");
        checkF(std::abs(p.x)<1e-5f && std::abs(p.z)<1e-5f, "Ray: At(5) x,z==0", p.x, "==0");
    }

    // IntersectPlane: 上方向レイが y=5 平面に当たる → t==5
    {
        const Ray   ray({ 0.0f, 0.0f, 0.0f }, Vector3::UP);
        const Plane plane(Vector3::UP, -5.0f);
        float t = 0.0f;
        const bool hit = ray.IntersectPlane(plane, t);
        check(hit, "Ray: IntersectPlane hit==true");
        checkF(std::abs(t-5.0f)<1e-4f, "Ray: IntersectPlane t==5", t, "==5");
    }

    // IntersectPlane: 平行レイ (RIGHT 方向、水平面) → 交差しない
    {
        const Ray   ray({ 0.0f, 0.0f, 0.0f }, Vector3::RIGHT);
        const Plane plane(Vector3::UP, 0.0f);
        float t = 0.0f;
        check(!ray.IntersectPlane(plane, t), "Ray: 平行レイと水平面は交差しない");
    }

    // IntersectSphere: 原点から +X へのレイが (5,0,0) 半径 1 の球に当たる → t≈4
    {
        const Ray ray({ 0.0f, 0.0f, 0.0f }, Vector3::RIGHT);
        float t = 0.0f;
        const bool hit = ray.IntersectSphere({ 5.0f, 0.0f, 0.0f }, 1.0f, t);
        check(hit, "Ray: IntersectSphere hit==true");
        checkF(std::abs(t-4.0f)<1e-4f, "Ray: IntersectSphere t≈4 (手前の表面)", t, "≈4");
    }

    // IntersectSphere: レイが球と完全に離れている → hit==false
    {
        const Ray ray({ 0.0f, 10.0f, 0.0f }, Vector3::RIGHT);
        float t = 0.0f;
        check(!ray.IntersectSphere({ 5.0f, 0.0f, 0.0f }, 1.0f, t),
              "Ray: y=10 のレイは y=0 の球に当たらない");
    }

    // IntersectTriangle (Möller–Trumbore): 原点から +Z レイが Z=2 の三角形に当たる → t==2
    {
        const Ray ray({ 0.0f, 0.0f, 0.0f }, Vector3::FORWARD);
        float t = 0.0f;
        const bool hit = ray.IntersectTriangle(
            { -1.0f, -1.0f, 2.0f },
            {  1.0f, -1.0f, 2.0f },
            {  0.0f,  1.0f, 2.0f },
            t);
        check(hit, "Ray: IntersectTriangle hit==true");
        checkF(std::abs(t-2.0f)<1e-4f, "Ray: IntersectTriangle t==2", t, "==2");
    }

    // IntersectTriangle: レイが三角形を外れる → hit==false
    {
        const Ray ray({ 0.0f, 0.0f, 0.0f }, Vector3::FORWARD);
        float t = 0.0f;
        check(!ray.IntersectTriangle(
                { 5.0f, 5.0f, 2.0f },
                { 6.0f, 5.0f, 2.0f },
                { 5.5f, 6.0f, 2.0f }, t),
              "Ray: IntersectTriangle 外れ → hit==false");
    }
}

// ─── Frustum ──────────────────────────────────────────────────────────────────

static void TestMath_Frustum()
{
    std::printf("\n=== Math: Frustum ===\n");

    // DX 左手系: eye=(0,0,-10) が +Z を向く
    // near=0.1, far=100 → 原点 (z=0) はカメラ前方 10 単位で視野内
    const Matrix4 view = Matrix4::LookAt(
        { 0.0f, 0.0f, -10.0f },
        { 0.0f,  0.0f,   0.0f },
        Vector3::UP);
    const Matrix4 proj = Matrix4::Perspective(
        ToRad(60.0f), 1.0f, 0.1f, 100.0f);
    // 列ベクトル規則 (M*v): combined = proj * view
    const Matrix4 vp = proj * view;
    const Frustum f  = Frustum::FromViewProjection(vp);

    // カメラ前方の原点は視野内
    check(f.Contains({ 0.0f, 0.0f, 0.0f }),
          "Frustum: 原点 (カメラ前方) → Contains==true");

    // カメラの真後ろ → 視野外
    check(!f.Contains({ 0.0f, 0.0f, -200.0f }),
          "Frustum: カメラ真後ろ → Contains==false");

    // far 超過 → 視野外
    check(!f.Contains({ 0.0f, 0.0f, 200.0f }),
          "Frustum: far 超過 → Contains==false");

    // 球: 原点半径 1 は視野内
    check(f.IntersectsSphere({ 0.0f, 0.0f, 0.0f }, 1.0f),
          "Frustum: 原点の球 → IntersectsSphere==true");

    // 球: 真後ろは視野外
    check(!f.IntersectsSphere({ 0.0f, 0.0f, -200.0f }, 0.5f),
          "Frustum: 真後ろの球 → IntersectsSphere==false");

    // AABB: 原点は視野内
    check(f.IntersectsAABB({ 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }),
          "Frustum: 原点 AABB → IntersectsAABB==true");

    // AABB: 真後ろは視野外
    check(!f.IntersectsAABB({ 0.0f, 0.0f, -200.0f }, { 0.5f, 0.5f, 0.5f }),
          "Frustum: 真後ろ AABB → IntersectsAABB==false");
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ Math Tests\n");
    std::printf("===============\n");

    TestMath_Vector3();
    TestMath_Quaternion();
    TestMath_Matrix4();
    TestMath_MathUtils();
    TestMath_Plane();
    TestMath_Ray();
    TestMath_Frustum();

    std::printf("\n===============\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
