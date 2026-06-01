# Script API リファレンス

`Script` クラス (`Engine/Scene/Script.hpp`) は Unity の `MonoBehaviour` に相当するユーザースクリプトの基底クラス。

---

## Quick Start — スクリプトの書き方

### 1. ファイルの置き場所

`Projects/Sandbox/src/Scripts/` 以下に `.hpp` を置く。  
Sandbox スクリプトは engine 層からインクルードされない末端ヘッダなので、`using namespace` が許可されている。

### 2. 宣言テンプレート

```cpp
// Projects/Sandbox/src/Scripts/MyScript.hpp
#pragma once
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using namespace fbzz::util;

namespace sandbox {

class MyScript : public Script {
public:
    // GetScript<T>() の型判別に使う。クラス名と一致させること。
    static constexpr const char* TYPE_NAME = "MyScript";
    const char* GetTypeName() const override { return TYPE_NAME; }

    // ── Inspector / Serializer に公開するフィールド ──────────────────────────
    // Reflect() に列挙したフィールドは Editor の Inspector で編集でき、
    // .fbzz シーンファイルにシリアライズされる。
    float speed = 5.0f;
    bool  loop  = true;

    void Reflect(IReflector& r) override {
        r.Field("Speed", speed);
        r.Field("Loop",  loop);
    }

    // ── ライフサイクル ────────────────────────────────────────────────────────
    void OnStart() override {
        // 初回 Update 直前に 1 度だけ呼ばれる。Component の取得・初期化はここで行う。
    }

    void OnUpdate(float dt) override {
        // 毎フレーム呼ばれる。dt は今フレームの経過秒。
    }

    void OnCollisionEnter(const CollisionInfo& info) override {
        // info.other  : 衝突相手の GameObject*
        // info.contactNormal : self から見た接触法線
    }

private:
    // メンバ変数は m_ プレフィックス + lowerCamelCase
    float m_elapsed = 0.0f;
};

} // namespace sandbox
```

### 3. `using namespace` 許可範囲

| 名前空間 | 内容 |
|---|---|
| `fbzz::scene` | `Script`, `GameObject`, `Transform`, `CollisionInfo` など |
| `fbzz::math` | `Vector2/3/4`, `Quaternion`, `Matrix4` |
| `fbzz::input` | `KeyCode` |
| `fbzz::util` | `Mathf`, `Color`, `Random` |

> **注意**: engine 側の `.hpp` (ScriptProxy/*.hpp 等) では `using namespace` 禁止。Sandbox スクリプト専用の規則。

---

## 主要型チートシート

### `math::Vector3`

```cpp
// 定数
Vector3::ZERO     // (0, 0, 0)
Vector3::ONE      // (1, 1, 1)
Vector3::UP       // (0, 1, 0)
Vector3::RIGHT    // (1, 0, 0)
Vector3::FORWARD  // (0, 0, 1)  — DirectX 左手系、+Z が奥

// 構築
Vector3 v(1.0f, 2.0f, 3.0f);
Vector3 v{ 1.f, 2.f, 3.f };

// 演算
v + w;  v - w;  v * scalar;  v / scalar;  -v;
v += w; v -= w;

// メソッド
v.Length();       // 長さ
v.LengthSq();     // 長さの二乗 (比較用、sqrt なし)
v.Normalized();   // 正規化した新しい Vector3 (v 自体は変わらない)

// 静的メソッド
Vector3::Dot(a, b);         // 内積
Vector3::Cross(a, b);       // 外積
Vector3::Lerp(a, b, t);     // 線形補間 (t = 0.0〜1.0)
```

### `math::Quaternion`

```cpp
// 構築
Quaternion::Identity()                          // 無回転
Quaternion::FromAxisAngle(Vector3::UP, rad)     // 軸と角度 (ラジアン)
Quaternion::FromEuler(Vector3(x, y, z))         // オイラー角 (ラジアン)
Quaternion::LookRotation(forward)               // forward 方向を向く回転

// 演算
q1 * q2       // 回転の合成
q * Vector3   // ベクトルを回転

// メソッド
q.Normalized()
q.Conjugate()
q.Inverse()

// 補間
Quaternion::Slerp(a, b, t)   // 球面線形補間
Quaternion::Lerp(a, b, t)    // NLerp (等角速度でないが高速)
```

### `util::Color`

```cpp
// プリセット
Color::Red, Color::Green, Color::Blue
Color::White, Color::Black, Color::Yellow
Color::Cyan, Color::Magenta, Color::Gray, Color::Clear  // Clear はα=0

// 構築
Color c(r, g, b);         // a = 1.0f
Color c(r, g, b, a);      // 各成分 0.0f 〜 1.0f
Color::FromHex("#FF4400");
Color::FromHex("#FF440080");  // α付き
Color::FromHSV(hue, sat, val);  // h: 0〜360, s/v: 0〜1

// 操作
Color::Lerp(a, b, t)
c.WithAlpha(0.5f)    // α だけ変えた複製
c * 2.0f             // 明るさ乗算 (HDR)
c.ToVector4()        // シェーダーに渡すとき
```

### `util::Mathf`

```cpp
Mathf::Lerp(a, b, t)
Mathf::Clamp(v, lo, hi)
Mathf::Clamp01(v)
Mathf::SmoothStep(edge0, edge1, x)
Mathf::SmoothDamp(current, target, velocity/*ref*/, smoothTime, dt)
Mathf::MoveTowards(current, target, maxDelta)
Mathf::PingPong(t, length)
Mathf::DeltaAngle(current, target)
Mathf::Approximately(a, b)

Mathf::PI, Mathf::DEG2RAD, Mathf::RAD2DEG, Mathf::EPSILON
```

### `input::KeyCode`

```cpp
// アルファベット
KeyCode::A 〜 KeyCode::Z

// 数字
KeyCode::KEY_0 〜 KeyCode::KEY_9

// 特殊キー
KeyCode::ESCAPE, KeyCode::SPACE, KeyCode::ENTER
KeyCode::BACKSPACE, KeyCode::TAB
KeyCode::SHIFT, KeyCode::CTRL, KeyCode::ALT

// 方向キー
KeyCode::LEFT, KeyCode::RIGHT, KeyCode::UP, KeyCode::DOWN

// ファンクションキー
KeyCode::F1 〜 KeyCode::F12
```

---

## `Transform` フィールド早見表

`transform->` で直接アクセスする。

```cpp
// ── 書き込み (ローカル空間) ─────────────────────────────────────────────────
transform->localPosition = Vector3(0, 1, 0);
transform->localRotation = Quaternion::FromAxisAngle(Vector3::UP, ToRad(90.f));
transform->localScale    = Vector3::ONE * 2.f;

// ── 読み取り (ワールド空間: TransformSystem が毎フレーム更新) ─────────────────
Vector3    pos   = transform->position;    // ワールド位置
Quaternion rot   = transform->rotation;   // ワールド回転
Vector3    scale = transform->worldScale; // ワールドスケール

// ── 方向ベクトル (ワールド空間) ─────────────────────────────────────────────
Vector3 fwd   = transform->Forward();  // +Z
Vector3 up    = transform->Up();       // +Y
Vector3 right = transform->Right();   // +X

// ── 操作メソッド ────────────────────────────────────────────────────────────
transform->Translate(delta);                   // ローカル空間移動
transform->Translate(delta, true);             // ワールド空間移動
transform->Rotate(Vector3(0, 90, 0));          // オイラー角 (度) でローカル回転
transform->LookAt(targetWorldPos);            // 任意ワールド座標へ向く
Matrix4 mat = transform->GetWorldMatrix();     // TRS 行列
```

> **書き込みは `local` 系のみ**。`position` / `rotation` は TransformSystem / PhysicsSystem が上書きするため直接書いても次フレームで消える。

---

## `CollisionInfo` フィールド早見表

`OnCollisionEnter / Stay / Exit` および `OnTriggerEnter / Stay / Exit` の引数。

```cpp
void OnCollisionEnter(const CollisionInfo& info) override {
    info.self;           // GameObject* — このスクリプトを持つ GO
    info.other;          // GameObject* — 衝突相手の GO
    info.selfCollider;   // const ColliderComponent* — 自側コライダー
    info.otherCollider;  // const ColliderComponent* — 相手側コライダー
    info.contactNormal;  // Vector3 — self から見た接触法線 (地面判定などに使う)
    info.contactPoint;   // Vector3 — 接触点のワールド座標
    info.contactDepth;   // float   — 貫通深度
}
```

---

## 設計方針: Proxy パターンによるヘッダ分割

ショートハンド API の増加による `Script.hpp` の肥大化を防ぐため、カテゴリごとに **Proxy 構造体** に分割する。  
各 Proxy は `Script*` を持ち、該当 Component / System へ転送する。  
ユーザーは `Script.hpp` のみ include すればよく、呼び出し時は `physics.AddForce()` のようにカテゴリが自明になる。

```cpp
// Script.hpp が全 Proxy をメンバとして保持する
class Script {
public:
    ScriptTransformProxy  transform;   // 旧 transform* はこの中に移動
    ScriptInputProxy      input;
    ScriptPhysicsProxy    physics;
    ScriptAudioProxy      audio;
    ScriptLightProxy      light;
    ScriptCameraProxy     camera;
    ScriptMaterialProxy   material;
    ScriptParticleProxy   particle;
    ScriptUIProxy         ui;
    ScriptSceneProxy      scene;
    ScriptDebugProxy      debug;
    // ...
};
```

```cpp
// ユーザースクリプトの記述例
void OnUpdate(float dt) override {
    if (input.GetKeyDown(KeyCode::Space))
        physics.AddImpulse(math::Vector3::UP * 5.f);

    material.SetFloat("_Dissolve", m_dissolve);
    audio.Play("jump.wav");
    debug.DrawSphere(transform->position, 0.5f, util::Color::Red);
}
```

### ファイル構成

```
Engine/Scene/
├── Script.hpp                      ← ユーザーはこれだけ include
├── ScriptCore.hpp                  ← ライフサイクル・context・基本メンバ (Script.hpp が include)
└── ScriptProxy/
    ├── ScriptTransformProxy.hpp
    ├── ScriptInputProxy.hpp
    ├── ScriptPhysicsProxy.hpp
    ├── ScriptAudioProxy.hpp
    ├── ScriptLightProxy.hpp
    ├── ScriptCameraProxy.hpp
    ├── ScriptMaterialProxy.hpp
    ├── ScriptParticleProxy.hpp
    ├── ScriptUIProxy.hpp
    ├── ScriptSceneProxy.hpp
    └── ScriptDebugProxy.hpp
```

---

## ライフサイクル

`ScriptCore.hpp` に定義。`Script.hpp` 経由で公開される。

| メソッド | タイミング | 備考 |
|---|---|---|
| `OnAwake()` | `AddComponent` / シーンロード直後、`enabled` 問わず1度 | 他 Script の `OnStart` より先に走る初期化に使う |
| `OnStart()` | 初回 `OnUpdate` の直前に1度 | ✅ 実装済み |
| `OnEnable()` | `enabled` が `false → true` になった瞬間 | |
| `OnDisable()` | `enabled` が `true → false` になった瞬間 | |
| `OnUpdate(float dt)` | 毎フレーム (PhysicsSystem 前) | ✅ 実装済み |
| `OnFixedUpdate(float dt)` | 物理レート固定更新 (PhysicsSystem と同周期) | dt は固定タイムステップ |
| `OnLateUpdate(float dt)` | 毎フレーム (PhysicsSystem 後) | ✅ 実装済み |
| `OnPreRender()` | カメラの描画パス実行直前 | |
| `OnPostRender()` | カメラの描画パス実行直後 | |
| `OnSetupRenderPasses(RenderGraph&, RenderPassContext&)` | RenderSystem が RenderGraph を構築するとき | ✅ 実装済み |
| `OnCollisionEnter(CollisionInfo)` | 衝突開始フレーム | ✅ 実装済み |
| `OnCollisionStay(CollisionInfo)` | 衝突継続中の毎フレーム | ✅ 実装済み |
| `OnCollisionExit(CollisionInfo)` | 衝突終了フレーム | ✅ 実装済み |
| `OnTriggerEnter(CollisionInfo)` | トリガー侵入フレーム | ✅ 実装済み |
| `OnTriggerStay(CollisionInfo)` | トリガー滞在中の毎フレーム | ✅ 実装済み |
| `OnTriggerExit(CollisionInfo)` | トリガー退出フレーム | ✅ 実装済み |
| `OnDestroy()` | GameObject 破棄時 | ✅ 実装済み |

---

## メンバ変数 (ScriptSystem が毎フレーム注入)

`Script` 直接メンバ。Proxy 経由ではなくそのまま参照する。

| 変数 | 型 | 説明 |
|---|---|---|
| `enabled` | `bool` | false のとき Update 系・Collision 系が呼ばれない | ✅ 実装済み |
| `m_deltaTime` | `float` | 今フレームの経過秒 |
| `m_time` | `float` | アプリ起動からの累積秒 |
| `m_frameCount` | `uint64_t` | フレームカウンタ |
| `m_unscaledDeltaTime` | `float` | タイムスケールを無視した経過秒 |

---

## タイマー / 遅延呼び出し

`Script` 直接メンバ。コルーチン禁止の制約下で「N秒後」「繰り返し」を実現する。  
内部は `ScriptSystem` にタイマーキューを持ち、`OnDestroy` で自動キャンセル。

| API | 説明 |
|---|---|
| `Invoke(std::function<void()> fn, float delay)` | `delay` 秒後に `fn` を1度実行 |
| `InvokeRepeating(std::function<void()> fn, float delay, float interval)` | `delay` 秒後から `interval` 秒ごとに繰り返し実行 |
| `FrameDelay(uint32_t n, std::function<void()> fn)` | `n` フレーム後に `fn` を1度実行 |
| `CancelInvoke()` | 自スクリプトの全タイマーをキャンセル |

---

## イベント / メッセージ

`Script` 直接メンバ。型付きイベントを Scene スコープで送受信する軽量バス。  
購読は `OnDestroy` で自動解除される。

| API | 説明 |
|---|---|
| `Emit<T>(const T& data)` | 型 `T` のイベントをシーン全体に送出 |
| `On<T>(std::function<void(const T&)> callback)` | 型 `T` のイベントを購読 |

---

## リフレクション

`Script` 直接メンバ。

| API | 説明 |
|---|---|
| `Reflect(IReflector&)` | Inspector / Serializer からフィールドを列挙 | ✅ 実装済み |
| `GetTypeName()` | 型名文字列を返す (GetScript<T> の型判別に使用) | ✅ 実装済み |

---

## `transform` — ScriptTransformProxy

`ScriptProxy/ScriptTransformProxy.hpp`

自 GO の `Transform` への直接アクセスと、よく使う操作の短縮形。  
旧 `Script::transform*` はこの Proxy に統合する。

```cpp
transform->position   // Transform フィールドへの直接アクセスは従来通り
transform.SetPosition(v)
transform.Translate(v)
```

| API | 説明 |
|---|---|
| `transform->position / rotation / scale` | Transform フィールドへの直接アクセス | ✅ 実装済み (旧 `Script::transform`) |
| `transform.SetPosition(v)` | `transform->position = v` |
| `transform.Translate(v)` | ローカル空間での相対移動 |
| `transform.Rotate(axis, deg)` | 指定軸まわりに相対回転 |
| `transform.LookAt(target)` | target 方向に向く (Y軸アップ固定) |
| `transform.DistanceTo(const GameObject& other)` | 他 GO までの距離 |
| `transform.DirectionTo(const GameObject& other)` | 他 GO への正規化方向ベクトル |

---

## `input` — ScriptInputProxy

`ScriptProxy/ScriptInputProxy.hpp`

`fbzz::input::Input` への転送。`#include <Engine/Input/Input.hpp>` 不要で使える。

| API | 説明 |
|---|---|
| `input.GetKey(KeyCode)` | キー押下中 |
| `input.GetKeyDown(KeyCode)` | キーを押した瞬間 |
| `input.GetKeyUp(KeyCode)` | キーを離した瞬間 |
| `input.GetAxis(std::string_view name)` | 仮想軸の値 (-1.0 〜 1.0)。例: `"Horizontal"`, `"Vertical"` |
| `input.GetMouseDelta()` | 今フレームのマウス移動量 (`Vector2`) |
| `input.GetMousePosition()` | スクリーン座標でのマウス位置 (`Vector2`) |
| `input.GetMouseScrollDelta()` | マウスホイール量 |
| `input.MouseButton(int btn)` | マウスボタン押下中 |
| `input.MouseButtonDown(int btn)` | マウスボタンを押した瞬間 |
| `input.MouseButtonUp(int btn)` | マウスボタンを離した瞬間 |

---

## `physics` — ScriptPhysicsProxy

`ScriptProxy/ScriptPhysicsProxy.hpp`

RigidBody 操作は自 GO の `RigidBodyComponent` へ転送。RigidBody が無ければ何もしない。  
Raycast / Overlap 系は `physics::World` へ転送。

| API | 説明 |
|---|---|
| `physics.AddForce(v)` | 力を加える |
| `physics.AddImpulse(v)` | 撃力を加える |
| `physics.SetVelocity(v)` | 速度を直接セット |
| `physics.GetVelocity()` | 現在の速度を取得 |
| `physics.AddTorque(v)` | トルクを加える |
| `physics.Raycast(origin, dir, dist, RaycastHit& hit)` | 最近の 1 件を返す。戻り値は bool |
| `physics.RaycastAll(origin, dir, dist)` | 全ヒットを距離昇順で返す |
| `physics.SphereCast(origin, radius, dir, dist, RaycastHit& hit)` | 球形スイープ。最近の 1 件を返す |
| `physics.OverlapSphere(center, radius)` | 球と重なる全コライダーを返す |

**`RaycastHit` フィールド:**

```cpp
physics.Raycast(origin, dir, 100.f, hit);
hit.point;    // Vector3 — ヒット点 (ワールド空間)
hit.normal;   // Vector3 — ヒット面の外向き法線
hit.distance; // float   — origin からの距離
hit.collider; // const Collider* — ヒットしたコライダー
hit.body;     // RigidBody*      — 紐づく剛体 (static コライダーなら nullptr)
```

対応形状: Sphere / AABB / OBB / Capsule / TriangleMesh (BVH) / ConvexHull

---

## `audio` — ScriptAudioProxy

`ScriptProxy/ScriptAudioProxy.hpp`

自 GO の `AudioSourceComponent` への転送。AudioSource が無ければ何もしない。

| API | 説明 |
|---|---|
| `audio.Play(std::string_view clipPath)` | クリップを差し替えて再生 |
| `audio.Stop()` | 再生停止 |
| `audio.Pause()` | 一時停止 |
| `audio.SetVolume(float v)` | 音量 (0.0 〜 1.0) |
| `audio.SetLoop(bool loop)` | ループ切り替え |

---

## `light` — ScriptLightProxy

`ScriptProxy/ScriptLightProxy.hpp`

自 GO の `LightComponent` への転送。Light が無ければ何もしない。

| API | 説明 |
|---|---|
| `light.SetColor(const math::Vector3& color)` | 光の色 |
| `light.SetIntensity(float intensity)` | 光の強度 |
| `light.SetRange(float range)` | 有効距離 (Point / Spot のみ) |
| `light.SetEnabled(bool enabled)` | ライトの有効/無効 |

---

## `camera` — ScriptCameraProxy

`ScriptProxy/ScriptCameraProxy.hpp`

自 GO の `CameraComponent` への転送。Camera が無ければ何もしない。

| API | 説明 |
|---|---|
| `camera.SetAsMain()` | 自 GO のカメラを isMain = true にセット |
| `camera.SetFOV(float fovY)` | 垂直 FOV (度) |
| `camera.SetNearFar(float nearZ, float farZ)` | クリッピング距離 |
| `camera.WorldToScreenPoint(const math::Vector3& worldPos)` | ワールド座標 → スクリーン座標 |
| `camera.ScreenToWorldPoint(const math::Vector3& screenPos)` | スクリーン座標 → ワールド座標 |

---

## `material` — ScriptMaterialProxy

`ScriptProxy/ScriptMaterialProxy.hpp`

自 GO の `MaterialComponent` + `ShaderDescriptor` を組み合わせた短縮形。  
内部で `GetShaderDescriptor` を呼ぶため、Script 側は desc を意識しなくてよい。

| API | 説明 |
|---|---|
| `material.SetFloat(std::string_view param, float v)` | シェーダーパラメータを名前で設定 |
| `material.SetInt(std::string_view param, int v)` | 〃 |
| `material.SetVector3(std::string_view param, const math::Vector3& v)` | 〃 |
| `material.SetVector4(std::string_view param, const math::Vector4& v)` | 〃 |
| `material.SetTexture(std::string_view slot, std::string_view texPath)` | テクスチャスロットを差し替え |
| `material.QueueRenderPass(UserRenderPassDesc)` | カスタム描画パスを登録 | ✅ 実装済み (旧 `Script::QueueRenderPass`) |
| `material.GetShaderDescriptor(std::string_view path)` | ShaderDescriptor を取得 | ✅ 実装済み (旧 `Script::GetShaderDescriptor`) |

---

## `particle` — ScriptParticleProxy

`ScriptProxy/ScriptParticleProxy.hpp`

自 GO の `ParticleEmitter` への転送。

| API | 説明 |
|---|---|
| `particle.SetEmitRate(float rate)` | 毎秒の発生数 |
| `particle.SetEnabled(bool enabled)` | エミッターの有効/無効 |
| `particle.Clear()` | 現在の全パーティクルを即破棄 |

---

## `ui` — ScriptUIProxy

`ScriptProxy/ScriptUIProxy.hpp`

自 GO の UI 系 Component への転送。対応 Component が無ければ何もしない。

| API | 説明 |
|---|---|
| `ui.SetButtonInteractable(bool v)` | `UIButton::isInteractable` を設定 |
| `ui.SetImageColor(const math::Vector4& color)` | `UIImage::color` を設定 |
| `ui.SetText(std::string_view text)` | `UIText::text` を設定 |
| `ui.SetCanvasSortOrder(int order)` | `UICanvas::sortOrder` を設定 |

---

## `scene` — ScriptSceneProxy

`ScriptProxy/ScriptSceneProxy.hpp`

GameObject 検索・生成・シーン遷移をまとめた Proxy。  
`SceneManager*` を `SetContext` で受け取り、`m_sceneManager` として保持する。

| API | 説明 |
|---|---|
| `scene.Find(std::string_view name)` | 名前で GO を検索 | ✅ 実装済み (旧 `Script::Find`) |
| `scene.FindWithTag(std::string_view tag)` | タグで GO を検索 | ✅ 実装済み (旧 `Script::FindWithTag`) |
| `scene.FindObjectOfType<T>()` | Component 型で最初の GO を検索 |
| `scene.FindObjectsOfType<T>()` | Component 型で全 GO を取得 |
| `scene.GetGameObject(EntityID)` | EntityID から GO を取得 | ✅ 実装済み (旧 `Script::GetGameObject`) |
| `scene.GetMainCameraObject()` | メインカメラ GO を返す | ✅ 実装済み (旧 `Script::GetMainCameraObject`) |
| `scene.Create(std::string_view name)` | GO を生成して返す | ✅ 実装済み (旧 `Script::CreateGameObject`) |
| `scene.Destroy(GameObject& go, float delay)` | GO を破棄 | ✅ 実装済み (旧 `Script::Destroy`) |
| `scene.GetScript<T>()` | 自 GO のスクリプトを取得 |
| `scene.GetScript<T>(GameObject& go)` | 任意 GO のスクリプトを取得 |
| `scene.GetComponent<T>()` | 自 GO の Component を取得 | ✅ 実装済み (旧 `Script::GetComponent`) |
| `scene.GetOrAddComponent<T>()` | 無ければ追加して返す |
| `scene.RequireComponent<T>()` | 無ければ assert |
| `scene.isActiveAndEnabled()` | `go.IsActive() && enabled` の合成プロパティ |
| `scene.LoadScene(std::string_view name)` | 登録済みシーンへ遷移。次フレーム冒頭で切り替わる |
| `scene.GetSceneName()` | 現在のシーン名を返す |
| `scene.GetTerrainHeightAt(const math::Vector3& worldPos)` | ワールド座標でのテレイン高さ |
| `scene.GetTerrainNormalAt(const math::Vector3& worldPos)` | 〃 法線ベクトル |
| `scene.GetWaterSurfaceHeight(const math::Vector3& worldPos, float time)` | ワールド座標での水面高さ (Gerstner 波込み) |

> **実装メモ**: シーン遷移は `SceneManager::LoadScene` を呼ぶだけでよく、Script 側でフレーム管理は不要。  
> Terrain / Water は `FindObjectOfType` でキャッシュし、2回目以降は検索を省く。

---

## `animator` — (Proxy 化対象)

現在は `Script` 直接メンバとして実装済み。Proxy 化リファクタ時に `ScriptAnimatorProxy` へ移行する。

| API | 説明 |
|---|---|
| `animator.SetFloat(name, v)` | | ✅ 実装済み (旧 `Script::SetAnimatorFloat`) |
| `animator.SetInt(name, v)` | | ✅ 実装済み (旧 `Script::SetAnimatorInt`) |
| `animator.SetBool(name, v)` | | ✅ 実装済み (旧 `Script::SetAnimatorBool`) |
| `animator.SetTrigger(name)` | | ✅ 実装済み (旧 `Script::SetAnimatorTrigger`) |
| `animator.IsInState(name)` | | ✅ 実装済み (旧 `Script::IsAnimatorInState`) |

---

## `debug` — ScriptDebugProxy

`ScriptProxy/ScriptDebugProxy.hpp`

開発ビルドのみ有効。`FBZZ_DEBUG` 未定義時は呼び出しが空になる。

| API | 説明 |
|---|---|
| `debug.Log(msg)` | INFO レベルで出力 |
| `debug.LogWarning(msg)` | WARN レベルで出力 |
| `debug.LogError(msg)` | ERROR レベルで出力 |
| `debug.DrawLine(a, b, color, duration)` | ワイヤーライン |
| `debug.DrawSphere(center, radius, color, duration)` | ワイヤー球 |
| `debug.DrawBox(center, halfExtents, color, duration)` | ワイヤーボックス |
| `debug.DrawRay(origin, dir, color, duration)` | 方向付きレイ |

---

## `postprocess` — ScriptPostProcessProxy

`ScriptProxy/ScriptPostProcessProxy.hpp`

`renderer::PostProcessSettings` への動的変更。  
Script が破棄されると設定は自動的にクリアされる。

| API | 説明 |
|---|---|
| `postprocess.Get()` | 現在の PostProcessSettings への参照 | ✅ 実装済み (旧 `Script::GetRuntimePostProcessSettings`) |
| `postprocess.Set(settings)` | PostProcessSettings を上書き | ✅ 実装済み (旧 `Script::SetRuntimePostProcessSettings`) |
| `postprocess.Clear()` | Script 由来の設定をリセット | ✅ 実装済み (旧 `Script::ClearRuntimePostProcessSettings`) |
