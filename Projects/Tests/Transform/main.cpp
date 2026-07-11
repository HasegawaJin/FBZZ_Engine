// FBZZ Engine
// Tests/Transform/main.cpp
// Transform モジュール単体テスト
// fbzz::scene::Transform の位置・回転・スケール演算を検証する。
//
// 設計上の注意:
//   - Forward()/Up()/Right() は worldRotation を読む
//   - Translate / Rotate / LookAt は rotation (ローカル) を更新する
//   - worldRotation は TransformSystem が毎フレーム同期するため、
//     テスト内では手動で worldRotation = rotation と同期して確認する
//   - GetWorldMatrix は worldPosition / worldRotation / worldScale を使う
//   - Vector3::FORWARD = (0,0,1) (DirectX 左手系、+Z が奥)
#include <cstdio>
#include <cmath>

#include <Engine/Scene/Transform.hpp>
#include <Math/MathUtils.hpp>

#include "../TestHelper.hpp"

using namespace fbzz::scene;
using namespace fbzz::math;

// ─── デフォルト値 ─────────────────────────────────────────────────────────────

static void TestTransform_Defaults()
{
    std::printf("\n=== Transform: Defaults ===\n");

    Transform t;

    checkV(t.position.x == 0.0f && t.position.y == 0.0f && t.position.z == 0.0f,
           "Transform: default position is (0,0,0)",
           t.position.x, t.position.y, t.position.z, "== (0,0,0)");

    checkV(t.scale.x == 1.0f && t.scale.y == 1.0f && t.scale.z == 1.0f,
           "Transform: default scale is (1,1,1)",
           t.scale.x, t.scale.y, t.scale.z, "== (1,1,1)");

    // DirectX 左手系: FORWARD = +Z
    const Vector3 fwd = t.Forward();
    checkV(std::abs(fwd.z - 1.0f) < 0.001f,
           "Transform: default forward is (0,0,+1) [DirectX LH +Z]",
           fwd.x, fwd.y, fwd.z, "~= (0,0,1)");

    const Vector3 up = t.Up();
    checkV(std::abs(up.y - 1.0f) < 0.001f,
           "Transform: default up is (0,1,0)",
           up.x, up.y, up.z, "~= (0,1,0)");

    const Vector3 right = t.Right();
    checkV(std::abs(right.x - 1.0f) < 0.001f,
           "Transform: default right is (1,0,0)",
           right.x, right.y, right.z, "~= (1,0,0)");
}

// ─── Translate ────────────────────────────────────────────────────────────────

static void TestTransform_Translate()
{
    std::printf("\n=== Transform: Translate ===\n");

    // ローカル移動: identity rotation → position に delta が加算される
    Transform t;
    t.Translate({ 3.0f, 0.0f, 0.0f });
    checkV(std::abs(t.position.x - 3.0f) < 0.001f,
           "Transform: Translate adds to local position",
           t.position.x, t.position.y, t.position.z, "x ~= 3");

    // 複数回加算
    t.Translate({ 0.0f, 1.0f, 0.0f });
    t.Translate({ 0.0f, 1.0f, 0.0f });
    checkV(std::abs(t.position.y - 2.0f) < 0.001f,
           "Transform: cumulative Translate",
           t.position.x, t.position.y, t.position.z, "y ~= 2");

    // worldSpace=true: identity 親 rotation の場合 position に delta が直接加算される
    // (Translate 実装: parentRot = Identity → position += delta)
    Transform tw;
    tw.worldRotation = Quaternion::Identity();
    tw.rotation      = Quaternion::Identity();
    tw.Translate({ 2.0f, 0.0f, 0.0f }, /*worldSpace=*/true);
    checkV(std::abs(tw.position.x - 2.0f) < 0.001f,
           "Transform: Translate(worldSpace=true) updates local position",
           tw.position.x, tw.position.y, tw.position.z, "x ~= 2");
}

// ─── Rotate ───────────────────────────────────────────────────────────────────

static void TestTransform_Rotate()
{
    std::printf("\n=== Transform: Rotate ===\n");

    Transform t;
    t.worldRotation = Quaternion::Identity();
    t.rotation      = Quaternion::Identity();

    // Y 軸 90 度回転
    t.Rotate({ 0.0f, 90.0f, 0.0f });

    // rotation フィールドが変化した (w が 1 でない = Identity ではない)
    checkF(std::abs(t.rotation.w) < 0.999f,
           "Transform: Rotate(0,90,0) changes rotation quaternion",
           t.rotation.w, "w != 1.0");

    // TransformSystem がいないため worldRotation を手動同期してから Forward を確認
    t.worldRotation = t.rotation;
    const Vector3 fwd = t.Forward();
    // +Z が前方なので Y+90 で +X が前方に来る
    checkF(std::abs(fwd.x - 1.0f) < 0.1f,
           "Transform: after Y+90 rotation, forward ~= +X",
           fwd.x, "~= 1.0");

    // X 軸 90 度回転 → Up が +Z になる
    Transform t2;
    t2.worldRotation = Quaternion::Identity();
    t2.rotation      = Quaternion::Identity();
    t2.Rotate({ 90.0f, 0.0f, 0.0f });
    t2.worldRotation = t2.rotation;
    const Vector3 up = t2.Up();
    checkF(std::abs(up.z - 1.0f) < 0.1f,
           "Transform: after X+90 rotation, up ~= +Z",
           up.z, "~= 1.0");
}

// ─── LookAt ───────────────────────────────────────────────────────────────────

static void TestTransform_LookAt()
{
    std::printf("\n=== Transform: LookAt ===\n");

    // LookAt は rotation を更新する。Forward を確認するため worldRotation を手動同期する。
    Transform t;
    t.worldPosition = { 0.0f, 0.0f, 0.0f };
    t.worldRotation = Quaternion::Identity();
    t.rotation      = Quaternion::Identity();

    t.LookAt({ 1.0f, 0.0f, 0.0f }); // +X 方向を向く
    t.worldRotation = t.rotation;    // 手動同期
    const Vector3 fwd = t.Forward();
    checkF(std::abs(fwd.x - 1.0f) < 0.1f,
           "Transform: LookAt(1,0,0) -> forward ~= +X",
           fwd.x, "~= 1.0");

    // 逆方向 (-X)
    t.worldRotation = Quaternion::Identity();
    t.rotation      = Quaternion::Identity();
    t.LookAt({ -1.0f, 0.0f, 0.0f });
    t.worldRotation = t.rotation;
    const Vector3 fwd2 = t.Forward();
    checkF(std::abs(fwd2.x + 1.0f) < 0.1f,
           "Transform: LookAt(-1,0,0) -> forward ~= -X",
           fwd2.x, "~= -1.0");

    // 上方向 (+Y)
    t.worldRotation = Quaternion::Identity();
    t.rotation      = Quaternion::Identity();
    t.LookAt({ 0.0f, 1.0f, 0.0f });
    t.worldRotation = t.rotation;
    const Vector3 fwd3 = t.Forward();
    checkF(std::abs(fwd3.y - 1.0f) < 0.1f,
           "Transform: LookAt(0,1,0) -> forward ~= +Y",
           fwd3.y, "~= 1.0");
}

// ─── GetWorldMatrix ───────────────────────────────────────────────────────────

static void TestTransform_GetWorldMatrix()
{
    std::printf("\n=== Transform: GetWorldMatrix ===\n");

    // GetWorldMatrix は worldPosition / worldRotation / worldScale を使う。
    // 行優先・列ベクトル形式: 平行移動は m[row][3] 列 (m[0][3]=tx, m[1][3]=ty, m[2][3]=tz)。

    Transform t;
    t.worldPosition = { 2.0f, 3.0f, 4.0f };
    t.worldRotation = Quaternion::Identity();
    t.worldScale    = { 1.0f, 1.0f, 1.0f };

    const Matrix4 mat = t.GetWorldMatrix();

    checkF(std::abs(mat.m[0][3] - 2.0f) < 0.001f,
           "GetWorldMatrix: translation.x == 2", mat.m[0][3], "~= 2");
    checkF(std::abs(mat.m[1][3] - 3.0f) < 0.001f,
           "GetWorldMatrix: translation.y == 3", mat.m[1][3], "~= 3");
    checkF(std::abs(mat.m[2][3] - 4.0f) < 0.001f,
           "GetWorldMatrix: translation.z == 4", mat.m[2][3], "~= 4");

    // worldScale 2 倍で第 0 行の XYZ 長が 2 になる
    t.worldScale = { 2.0f, 2.0f, 2.0f };
    const Matrix4 mat2 = t.GetWorldMatrix();
    const float rowLen = std::sqrt(mat2.m[0][0]*mat2.m[0][0] +
                                   mat2.m[0][1]*mat2.m[0][1] +
                                   mat2.m[0][2]*mat2.m[0][2]);
    checkF(std::abs(rowLen - 2.0f) < 0.001f,
           "GetWorldMatrix: worldScale 2 -> first row XYZ length == 2",
           rowLen, "~= 2");

    // 恒等 Transform → 対角が 1
    Transform id;
    const Matrix4 idMat = id.GetWorldMatrix();
    checkF(std::abs(idMat.m[0][0] - 1.0f) < 0.001f,
           "GetWorldMatrix: identity -> diagonal[0] == 1",
           idMat.m[0][0], "~= 1");
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ Transform Tests\n");
    std::printf("====================\n");

    TestTransform_Defaults();
    TestTransform_Translate();
    TestTransform_Rotate();
    TestTransform_LookAt();
    TestTransform_GetWorldMatrix();

    std::printf("\n====================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
