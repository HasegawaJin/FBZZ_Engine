# Terrain System 設計 — エディタツール (TerrainTool)

---

## 目的

エディタ上でマウス操作により地形の高さ彫刻（Sculpt）とテクスチャ塗布（Paint）を行う。
変更結果は `TerrainComponent.heightData` / `splatData` に書き戻し、
`heightDirty` / `splatDirty` を立てて `TerrainRenderSystem` にメッシュ再構築を通知する。

---

## クラス設計

```cpp
// Projects/Editor/src/Tools/TerrainTool.hpp
namespace fbzz::editor {

class TerrainTool {
public:
    // ブラシモード
    enum class Mode {
        Sculpt, // 高さ彫刻
        Paint,  // スプラットマップ塗布
    };

    // Sculpt サブモード
    enum class SculptMode {
        Raise,   // 高さを上げる
        Lower,   // 高さを下げる
        Smooth,  // 周囲と平滑化
        Flatten, // 基準高さに揃える
        Stamp,   // スタンプ（押し付け）
    };

    // ブラシのフォールオフ形状
    enum class FalloffType {
        Linear,     // 距離に比例して線形減衰
        Smooth,     // smoothstep（端が滑らか）
        Gaussian,   // ガウス曲線（自然な盛り上がり）
    };

    struct BrushSettings {
        float       radius      = 5.0f;               // ブラシ半径（ワールド単位）
        float       strength    = 0.05f;              // 1 フレームあたりの最大変化量 [0,1]
        FalloffType falloff     = FalloffType::Smooth;
    };

    // 毎フレーム呼ぶ（マウス入力処理・地形編集）
    void Update(fbzz::scene::Scene& scene,
                fbzz::renderer::IRenderer& renderer,
                fbzz::ResourceManager& resources,
                const fbzz::renderer::Camera& camera,
                float dt);

    // ImGui パネルを描画（エディタウィンドウ内から呼ぶ）
    void OnEditorGUI();

private:
    Mode         m_mode       = Mode::Sculpt;
    SculptMode   m_sculpt     = SculptMode::Raise;
    BrushSettings m_brush;

    // Paint モード: 現在選択中のレイヤーインデックス [0,3]
    uint32_t     m_paintLayer = 0;
    // Flatten モード: 最初にクリックした高さを基準値として保持
    float        m_flattenTarget = 0.0f;
    bool         m_flattenLocked = false;

    // レイキャスト結果
    bool                       m_isHovering = false;
    fbzz::math::Vector3        m_hitPoint;            // ヒット位置（ワールド座標）
    fbzz::scene::GameObject*   m_hitTerrain = nullptr;

    // 内部ヘルパー
    bool RaycastTerrain(fbzz::scene::Scene& scene,
                        const fbzz::renderer::Camera& camera,
                        const fbzz::math::Vector2& mouseNDC,
                        fbzz::math::Vector3& outHitPoint,
                        fbzz::scene::GameObject*& outGO) const;

    void ApplySculpt(fbzz::scene::TerrainComponent& terrain,
                     const fbzz::math::Vector3& hitLocalPos, float dt) const;

    void ApplyPaint(fbzz::scene::TerrainComponent& terrain,
                    const fbzz::math::Vector3& hitLocalPos, float dt) const;

    float ComputeWeight(float dist) const; // フォールオフ計算
};

} // namespace fbzz::editor
```

---

## レイキャスト — 地形ヒット判定

マウス位置から地形への交差を求める。DDA（グリッドトラバーサル）方式を採用する。

### 手順

```
1. マウス NDC 座標 → カメラレイ (origin, direction) を生成
   ray.origin    = camera.position
   ray.direction = Unproject(mouseNDC, camera.viewProj)

2. 地形の AABB（地形全体の境界ボックス）と交差判定
   → ミスなら早期リターン

3. 地形のグリッドに沿って DDA でレイを進める
   各グリッドセル (x, z) でレイの高さ y_ray と
   地形高さ h(x, z) を比較し、交差を検出する

4. 交差セルが見つかったら二分探法（8 回）で精度を高める

5. ヒット位置をローカル座標に変換して返す
```

#### 二分探法スケッチ

```cpp
// lo, hi はレイ上のパラメータ t (lo 側が地面より上、hi 側が地面より下)
for (int i = 0; i < 8; ++i) {
    float mid = (lo + hi) * 0.5f;
    Vector3 p  = ray.origin + ray.direction * mid;
    float   hg = terrain.GetHeightAt(p.x, p.z); // ローカル座標
    if (p.y > hg) lo = mid;
    else          hi = mid;
}
hitPoint = ray.origin + ray.direction * ((lo + hi) * 0.5f);
```

---

## フォールオフ計算

ブラシ中心からの距離 `d` と半径 `r` に対してウェイト `w ∈ [0, 1]` を返す。

| FalloffType | 計算式 |
|---|---|
| `Linear` | `w = max(0, 1 - d / r)` |
| `Smooth` | `w = 1 - smoothstep(0, r, d)` ただし `smoothstep(e0,e1,x) = t²(3-2t), t = (x-e0)/(e1-e0)` |
| `Gaussian` | `w = exp(-3 * (d/r)²)` （`d > r` で打ち切り） |

```cpp
float TerrainTool::ComputeWeight(float dist) const {
    float r = m_brush.radius;
    if (dist >= r) return 0.0f;
    float t = dist / r;
    switch (m_brush.falloff) {
        case FalloffType::Linear:   return 1.0f - t;
        case FalloffType::Smooth:   return 1.0f - t * t * (3.0f - 2.0f * t);
        case FalloffType::Gaussian: return std::exp(-3.0f * t * t);
    }
    return 0.0f;
}
```

---

## 高さ彫刻アルゴリズム (Sculpt)

ブラシ半径内のすべての頂点に対して実行する。

```cpp
void TerrainTool::ApplySculpt(TerrainComponent& terrain,
                               const Vector3& hitLocalPos, float dt) const
{
    int cx = int(hitLocalPos.x / terrain.cellSize);
    int cz = int(hitLocalPos.z / terrain.cellSize);
    int ri = int(m_brush.radius / terrain.cellSize) + 1;

    for (int z = cz - ri; z <= cz + ri; ++z) {
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= int(terrain.columns)) continue;
            if (z < 0 || z >= int(terrain.rows))    continue;

            float wx = x * terrain.cellSize;
            float wz = z * terrain.cellSize;
            float dist = std::sqrt((wx - hitLocalPos.x) * (wx - hitLocalPos.x)
                                 + (wz - hitLocalPos.z) * (wz - hitLocalPos.z));
            float w = ComputeWeight(dist); // [0,1]
            if (w <= 0.0f) continue;

            float& h = terrain.heightData[z * terrain.columns + x];

            switch (m_sculpt) {
                case SculptMode::Raise:
                    h = std::clamp(h + m_brush.strength * w * dt, 0.0f, 1.0f);
                    break;
                case SculptMode::Lower:
                    h = std::clamp(h - m_brush.strength * w * dt, 0.0f, 1.0f);
                    break;
                case SculptMode::Flatten:
                    // 最初のクリックで基準高さを固定する（m_flattenTarget）
                    h = fbzz::math::Lerp(h, m_flattenTarget / terrain.maxHeight,
                                         m_brush.strength * w * dt);
                    break;
                case SculptMode::Smooth:
                    // 上下左右 4 近傍の平均に近づける
                    {
                        float avg = SampleAvg4(terrain, x, z);
                        h = fbzz::math::Lerp(h, avg, m_brush.strength * w * dt);
                    }
                    break;
                case SculptMode::Stamp:
                    // ブラシ中心が最高点になるように最大値を取る
                    {
                        float stamp = w; // [0,1]（中心 = 1）
                        h = std::max(h, stamp);
                    }
                    break;
            }
        }
    }
    terrain.heightDirty = true;
}
```

#### SampleAvg4 スケッチ

```cpp
float SampleAvg4(const TerrainComponent& t, int x, int z) {
    auto h = [&](int xi, int zi) {
        xi = std::clamp(xi, 0, int(t.columns) - 1);
        zi = std::clamp(zi, 0, int(t.rows)    - 1);
        return t.heightData[zi * t.columns + xi];
    };
    return (h(x-1,z) + h(x+1,z) + h(x,z-1) + h(x,z+1)) * 0.25f;
}
```

---

## テクスチャペイントアルゴリズム (Paint)

スプラットマップの選択レイヤーのウェイトを上げ、他レイヤーを按分して下げる。

```cpp
void TerrainTool::ApplyPaint(TerrainComponent& terrain,
                              const Vector3& hitLocalPos, float dt) const
{
    int cx = int(hitLocalPos.x / terrain.cellSize);
    int cz = int(hitLocalPos.z / terrain.cellSize);
    int ri = int(m_brush.radius / terrain.cellSize) + 1;

    for (int z = cz - ri; z <= cz + ri; ++z) {
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= int(terrain.columns)) continue;
            if (z < 0 || z >= int(terrain.rows))    continue;

            float wx = x * terrain.cellSize;
            float wz = z * terrain.cellSize;
            float dist = std::sqrt((wx - hitLocalPos.x) * (wx - hitLocalPos.x)
                                 + (wz - hitLocalPos.z) * (wz - hitLocalPos.z));
            float w = ComputeWeight(dist);
            if (w <= 0.0f) continue;

            int base = (z * terrain.columns + x) * 4;
            // 現在の float ウェイト
            float weights[4];
            for (int i = 0; i < 4; ++i)
                weights[i] = terrain.splatData[base + i] / 255.0f;

            // 選択レイヤーを上げる
            float delta = m_brush.strength * w * dt;
            weights[m_paintLayer] = std::min(1.0f, weights[m_paintLayer] + delta);

            // 合計が 1 を超えないよう他レイヤーを按分して下げる
            float excess = 0.0f;
            for (int i = 0; i < 4; ++i)
                excess += weights[i];
            excess -= 1.0f;

            if (excess > 0.0f) {
                float otherTotal = 0.0f;
                for (int i = 0; i < 4; ++i)
                    if (i != int(m_paintLayer)) otherTotal += weights[i];
                if (otherTotal > 1e-4f) {
                    float scale = (otherTotal - excess) / otherTotal;
                    for (int i = 0; i < 4; ++i)
                        if (i != int(m_paintLayer))
                            weights[i] = std::max(0.0f, weights[i] * scale);
                }
            }

            // uint8 に書き戻す
            for (int i = 0; i < 4; ++i)
                terrain.splatData[base + i] = uint8_t(weights[i] * 255.0f + 0.5f);
        }
    }
    terrain.splatDirty = true;
}
```

---

## ImGui UI 設計

`OnEditorGUI()` の想定レイアウト:

```
[Terrain Tool]
  Mode: [Sculpt ▼] / [Paint ▼]

  --- Sculpt モード ---
  Brush: [Raise][Lower][Smooth][Flatten][Stamp]
  Radius:   [==●======] 5.0
  Strength: [=●========] 0.05
  Falloff:  [Smooth ▼]

  --- Paint モード ---
  Layer: [0: Grass ▼]
  Radius:   [==●======] 5.0
  Strength: [=●========] 0.05
  Falloff:  [Smooth ▼]

  --- ブラシプレビュー ---
  (ビューポートに円形ブラシを投影表示)
```

ブラシ円プレビューはデバッグ描画（`DebugDraw` / `DrawCircle`）でビューポートに重ねる。
ヒット位置 `m_hitPoint` を中心に `m_brush.radius` 半径の円を描く。

---

## Flatten モードの基準高さ固定

マウスボタンを押した瞬間にその位置の高さを `m_flattenTarget` に記録し、
ドラッグ中は変更しない。ボタンを離したらリセット。

```cpp
// Update() 内
if (IsMouseButtonDown(MouseButton::Left)) {
    if (!m_flattenLocked && m_sculpt == SculptMode::Flatten) {
        m_flattenTarget = terrain->GetHeightAt(hitLocal.x, hitLocal.z);
        m_flattenLocked = true;
    }
    ApplySculpt(*terrain, hitLocal, dt);
}
if (IsMouseButtonReleased(MouseButton::Left)) {
    m_flattenLocked = false;
}
```

---

## Stamp モードの拡張（将来）

Stamp は単純な `w` マスクではなく、外部ハイトマップテクスチャをブラシとして使える。
`stampTexture` を `TerrainTool` に持たせ、UV を `(d / r * 0.5 + 0.5)` でサンプリングする。

---

## エラーハンドリング方針

| 状況 | 対応 |
|---|---|
| レイキャストで地形ヒットなし | 編集処理をスキップ（UI は表示維持） |
| `hitTerrain` が `TerrainComponent` を持たない | `assert`（ヒット判定ロジックのバグ） |
| `m_paintLayer >= layers.size()` | クランプして最終レイヤーに丸める |
| `splatData` が空 | `ApplyPaint` 冒頭で初期化（全 R=255） |

---

## 今後の拡張

| 優先度 | 内容 |
|---|---|
| 高 | Undo / Redo（コマンドパターンで heightData スナップショット保存） |
| 高 | スプラットマップの解像度をハイトマップと分離（splatResolutionX/Z フィールド追加） |
| 中 | Stamp ブラシ用テクスチャ読み込み UI |
| 中 | Foliage モード（草・木のインスタンス配置ブラシ） |
| 低 | ブラシプリセット保存・読み込み |
| 低 | マルチ地形の同時編集（複数 TerrainComponent に跨るブラシ） |
