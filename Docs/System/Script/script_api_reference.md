# Script API リファレンス (v2)

スクリプト内から使える API の全一覧。

---

## フィールド宣言

```cpp
FBZZ_SCRIPT(ClassName)                                    // TYPE_NAME + Reflect 生成トリガー
FBZZ_GROUP("グループ名")                                   // Inspector の区切り見出し
FBZZ_FIELD(Type, name, default, "表示名")                  // 標準フィールド
FBZZ_FIELD_RANGE(Type, name, default, "表示名", min, max)  // スライダー付き
FBZZ_FIELD_ENUM(Type, name, default, "表示名", "A","B","C")// ドロップダウン
FBZZ_FIELD_READONLY(Type, name, "表示名")                  // 読み取り専用ラベル
```

対応型: `float` `int` `bool` `std::string`  
`Vector2` `Vector3` `Vector4` `Quaternion`  
`EntityRef` `PrefabRef` `KeyCode`

---

## ライフサイクル

```cpp
void OnAwake()                               // AddComponent / シーンロード直後に 1 回
void OnStart()                               // 初回 Update 直前に 1 回
void OnEnable()                              // enabled: false → true
void OnDisable()                             // enabled: true → false
void OnUpdate(float dt)                      // 毎フレーム (Physics 前)
void OnLateUpdate(float dt)                  // 毎フレーム (Physics 後)
void OnDestroy()                             // GameObject 破棄時
void OnDrawGizmos()                          // エディタ描画区間 (gizmo.* を使う)
void OnSetupRenderPasses(RenderGraph&, RenderPassContext&)  // カスタム描画パス登録
void OnCollisionEnter(const CollisionInfo&)
void OnCollisionStay (const CollisionInfo&)
void OnCollisionExit (const CollisionInfo&)
void OnTriggerEnter  (const CollisionInfo&)
void OnTriggerStay   (const CollisionInfo&)
void OnTriggerExit   (const CollisionInfo&)
```

---

## タイミング情報

```cpp
float    deltaTime          // 前フレームからの経過秒 (スケール済み)
float    unscaledDeltaTime  // スケールなし経過秒
float    time               // シーン開始からの累積秒
uint64_t frameCount         // シーン開始からのフレーム数
```

---

## タイマー

```cpp
InvokeHandle Invoke(fn, delay)                      // delay 秒後に 1 回実行
InvokeHandle InvokeRepeating(fn, delay, interval)   // delay 秒後から interval 秒ごとに繰り返す
void         FrameDelay(n, fn)                       // n フレーム後に 1 回実行
void         CancelInvoke()                          // この Script の全タイマーをキャンセル
void         CancelInvoke(InvokeHandle)              // 指定タイマーのみキャンセル
```

---

## transform

すべて `.` でアクセスできる。`->` は不要。  
`position` / `rotation` / `scale` はローカル空間。ワールド空間には `world` プレフィックスを付ける。

```cpp
// ── ローカル空間 (読み書き) — 移動・回転はここに書く ──
Vector3    transform.position      // ローカル座標
Quaternion transform.rotation      // ローカル回転
Vector3    transform.scale         // ローカルスケール

// ── ワールド空間 (主に読み取り) ───────────────────────
Vector3    transform.worldPosition  // ワールド座標
Quaternion transform.worldRotation  // ワールド回転 (読み取り専用)

// ── 算出値 (読み取り専用、worldRotation から計算) ─────
Vector3    transform.forward
Vector3    transform.up
Vector3    transform.right

// ── メソッド ──────────────────────────────────────────
transform.Translate    (v3)
transform.Rotate       (axis, degrees)
transform.LookAt       (target)
float   transform.DistanceTo  (GameObject&)
Vector3 transform.DirectionTo (GameObject&)
```

```cpp
// 使用例
transform.position += transform.forward * speed * dt;  // ローカル移動
auto wp = transform.worldPosition;                      // ワールド座標を読む
transform.LookAt(target.transform.worldPosition);       // 世界座標でターゲット
```

---

## input

```cpp
bool   input.GetKey     (KeyCode)   // 押し続けている
bool   input.GetKeyDown (KeyCode)   // 押した瞬間
bool   input.GetKeyUp   (KeyCode)   // 離した瞬間
bool   input.MouseButton    (int)   // マウスボタン押し続け (0=左 1=右 2=中)
bool   input.MouseButtonDown(int)
bool   input.MouseButtonUp  (int)
float  input.GetAxis         (name) // 軸入力 (-1〜1)
Vector2 input.GetMouseDelta  ()
Vector2 input.GetMousePosition()
float  input.GetMouseScrollDelta()
```

---

## physics

```cpp
void    physics.AddForce         (v3)
void    physics.AddImpulse       (v3)
void    physics.AddForceAtPoint  (force, worldPoint)
void    physics.AddTorque        (v3)
void    physics.SetVelocity      (v3)
Vector3 physics.GetVelocity      ()
void    physics.SetAngularVelocity(v3)
Vector3 physics.GetAngularVelocity()
void    physics.SetMass          (float)
float   physics.GetMass          ()
void    physics.SetStatic        (bool)
void    physics.SetFreezePosition(x, y, z)
void    physics.SetFreezeRotation(x, y, z)

// レイキャスト / 形状クエリ
bool              physics.Raycast    (origin, dir, dist, RaycastHit&)
vector<RaycastHit> physics.RaycastAll(origin, dir, dist)
bool              physics.SphereCast (center, r, dir, dist, RaycastHit&)
vector<GameObject*> physics.OverlapSphere(center, radius)

// RaycastHit のフィールド
GameObject* hit.gameObject
Vector3     hit.point
Vector3     hit.normal
float       hit.distance
```

---

## scene

```cpp
// 検索
GameObject* scene.Find          (name)
GameObject* scene.FindWithTag   (tag)
GameObject* scene.Self          ()
GameObject* scene.GetGameObject (EntityID / EntityRef)
GameObject* scene.GetMainCameraObject()
GameObject* scene.FindObjectOfType <T>()
vector<GO*> scene.FindObjectsOfType<T>()

// 生成 / 破棄
GameObject& scene.Create        (name = "GameObject")
GameObject* scene.Instantiate   (PrefabRef / path)
void        scene.Destroy       (GameObject&, delay = 0)
void        scene.DestroySelf   (delay = 0)

// Component / Script 取得
T*  scene.GetComponent     <T>()          // 自 GameObject
T&  scene.GetOrAddComponent<T>()
T&  scene.RequireComponent <T>()          // 無ければ assert
T*  scene.GetScript        <T>()          // 自 GO の Script
T*  scene.GetScript        <T>(GameObject&)
T*  scene.GetScript        <T>(EntityID)

// シーン操作
void   scene.LoadScene   (name)
string scene.GetSceneName()
bool   scene.IsActiveAndEnabled()

// ワールドクエリ
float   scene.GetTerrainHeightAt (Vector3)
Vector3 scene.GetTerrainNormalAt (Vector3)
float   scene.GetWaterSurfaceHeight(Vector3, time)
```

---

## material

```cpp
MaterialComponent* material.Get()
MaterialComponent* material.Ensure()
bool material.SetMaterial   (path)
bool material.EnsureMaterial(path)
bool material.HasParam      (param)

void material.SetFloat   (param, float)
void material.SetInt     (param, int)
void material.SetVector3 (param, v3)
void material.SetVector4 (param, v4)
void material.SetTexture (slot, texPath)

bool material.SetEnabled    (bool)
bool material.SetBlendMode  (BlendMode)   // Opaque / AlphaBlend / Additive
bool material.SetDoubleSided(bool)
bool material.SetRenderQueue(int)

void material.QueueRenderPass(UserRenderPassDesc)
```

---

## postprocess

```cpp
PostProcessSettings& postprocess.Get()
void                 postprocess.Set  (PostProcessSettings&)
void                 postprocess.Clear()

// カスタムポストプロセス
CustomPostProcessSettings& postprocess.AddCustom   (name, shaderPath, enabled=true)
CustomPostProcessSettings& postprocess.EnsureCustom(name, shaderPath, enabled=true)
CustomPostProcessSettings* postprocess.FindCustom  (name)
bool postprocess.RemoveCustom       (name)
bool postprocess.SetCustomEnabled   (name, bool)
bool postprocess.SetCustomParameter (name, index, float)   // index: 0〜3
bool postprocess.SetCustomParameters(name, x, y, z, w)
```

---

## particle

```cpp
void particle.Play      (restart=true)
void particle.Stop      (clear=false)
void particle.Burst     (count)
void particle.Clear     ()
void particle.SetEnabled(bool)
void particle.SetEmitRate(float)
void particle.SetGravity (v3)
void particle.SetColor   (startV4, endV4)
void particle.SetSize    (start, end)
void particle.SetTexture (path, columns=1, rows=1)
void particle.SetShape        (ParticleEmitterShape)   // Point/Sphere/Cone/Box
void particle.SetBlendMode    (ParticleBlendMode)      // Additive/Alpha
void particle.SetSortMode     (ParticleSortMode)       // None/BackToFront
void particle.SetSimulationMode(ParticleSimulationMode)// Cpu/Gpu
```

---

## trail

```cpp
void trail.SetEnabled  (bool, clearWhenDisabled=true)
void trail.Clear       ()
void trail.SetDuration (seconds)
void trail.SetSampling (interval, minVertexDist)
void trail.SetWidth    (start, end)
void trail.SetWidthEasing(TrailWidthEasing)
void trail.SetColor    (startV4, endV4)
void trail.SetTexture  (path, uvTiling=1, uvScrollSpeed=0)
void trail.SetUVMode   (TrailUVMode)        // Stretch/Tile
void trail.SetAlignment(TrailAlignment)
void trail.SetCameraFacing()                // 剣閃・弾道向け
void trail.SetWorldUp  ()                   // タイヤ跡・地面エフェクト向け
void trail.SetSmoothSubdivisions(int)       // Catmull-Rom 補間数 (0=補間なし)
void trail.SetAttachBone (boneName, offset) // SkinnedMesh のボーン原点追従
void trail.ClearAttachBone()
```

---

## meshTrail

```cpp
void meshTrail.SetEnabled  (bool, clearWhenDisabled=true)
void meshTrail.Clear       ()
void meshTrail.SetDuration (seconds)
void meshTrail.SetSampling (interval, minVertexDist)
void meshTrail.SetMaxSamples(int)
void meshTrail.SetColor    (startV4, endV4)
void meshTrail.SetDoubleSided(bool)
void meshTrail.SetTexture  (path)
void meshTrail.AddExcludedMeshIndex  (index)  // 除外 submesh 指定
void meshTrail.ClearExcludedMeshIndices()
```

---

## animator

```cpp
void animator.SetFloat  (name, float)
void animator.SetInt    (name, int)
void animator.SetBool   (name, bool)
void animator.SetTrigger(name)
bool animator.IsInState (name)
```

---

## camera

```cpp
void    camera.SetAsMain()
void    camera.SetFOV         (fovY)
void    camera.SetAspectRatio (float)
void    camera.SetNearFar     (near, far)
void    camera.SetCullingMask (LayerMask)
Vector3 camera.WorldToScreenPoint(worldPos)
Vector3 camera.ScreenToWorldPoint(screenPos)
```

---

## light

```cpp
void light.SetColor    (v3)
void light.SetType     (int)       // LightType 定数
void light.SetIntensity(float)
void light.SetRange    (float)
void light.SetInnerCone(degrees)   // スポットライト内角
void light.SetOuterCone(degrees)   // スポットライト外角
void light.SetEnabled  (bool)
```

---

## audio

```cpp
void audio.Play     (clipPath)
void audio.Stop     ()
void audio.Pause    ()
void audio.SetVolume(float)
void audio.SetLoop  (bool)
```

---

## debug

```cpp
void debug.Log       (msg)
void debug.LogWarning(msg)
void debug.LogError  (msg)

// 時間付きデバッグ描画 (duration=0 で 1 フレームのみ)
void debug.DrawLine  (a, b, color, duration=0)
void debug.DrawRay   (origin, dir, color, duration=0)
void debug.DrawSphere(center, radius, color, duration=0)
void debug.DrawBox   (center, halfExtents, color, duration=0)
void debug.DrawArrow (from, to, headLength=0.2, headRadius=0.05, color, duration=0)
void debug.DrawCone  (apex, dir, height=1, baseRadius=0.3, color, duration=0)
```

---

## gizmo  ※ OnDrawGizmos() 内のみ有効

```cpp
void gizmo.DrawTransformAxes (position, rotation, size=1)
void gizmo.DrawLine          (from, to, color)
void gizmo.DrawArrow         (from, to, headLength, headRadius, color)
void gizmo.DrawSphere        (center, radius, color)
void gizmo.DrawBox           (center, halfExtents, color)
void gizmo.DrawBox           (center, halfExtents, rotation, color)
void gizmo.DrawSightCone     (position, forward, fovDeg, range, color)
void gizmo.DrawWaypointPath  (span<Vector3>, loop=false, color)
void gizmo.DrawDetectionRange(center, innerR, outerR, innerColor, outerColor)
void gizmo.DrawTargetLine    (from, to, color)
```

---

## GetComponent  ※ 自 GameObject のコンポーネント直取得

```cpp
T* GetComponent<T>()   // 例: GetComponent<RigidBodyComponent>()
```
