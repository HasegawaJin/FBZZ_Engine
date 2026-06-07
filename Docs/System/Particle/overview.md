# ParticleSystem 設計 — 全体概要

## 目的

CPU で粒子の生成・更新・描画を行う汎用パーティクルシステム。
炎・煙・爆発・魔法エフェクトなど「短命な粒子の集合」を `ParticleEmitter` コンポーネントで表現する。

---

## 現在の実装状況

| 機能 | 状態 |
|---|---|
| ポイントエミッター（1 点から放出） | ✅ 実装済 |
| `emitRate` / `maxParticles` | ✅ 実装済 |
| `colorStart → colorEnd` 線形補間（CPU） | ✅ 実装済 |
| `sizeStart → sizeEnd` 線形補間（CPU） | ✅ 実装済 |
| 重力（`5.0f` ハードコード） | ✅ 実装済・要改善 |
| CPU ビルボード描画（VS でスクリーン展開） | ✅ 実装済 |
| `ScriptParticleProxy`（SetEmitRate / SetEnabled / Clear） | ✅ 実装済 |

---

## 不足している機能

### 重要度：高

| 機能 | 説明 | 該当箇所 |
|---|---|---|
| **重力係数の公開** | `5.0f` がコード中にハードコードされており調整不可 | `ParticlePass.cpp:70`、`Reflect()` に `gravity` フィールド追加が必要 |
| **エミッター形状** | 現在はポイントのみ。スフィア・コーン・ボックス形状での放出がない | `ParticleEmitter.hpp` に `EmitterShape` 列挙が必要 |
| **パーティクルの回転（スピン）** | `Particle` 構造体に `rotation` / `angularVelocity` がなく、ビルボードが常に正面固定 | `Particle` 構造体拡張 |
| **テクスチャ・スプライトシート** | テクスチャ割り当てなし。炎・煙・魔法など見た目がフラットカラーのみ | シェーダー側の UV サンプリング + `textureHandle` フィールド追加 |
| **バースト放出** | 特定タイミングに N 粒子を一括放出するモードがない（爆発・ヒットエフェクト等） | `BurstEntry`（time, count）リストを `ParticleEmitter` に追加 |
| **ループ / 持続時間制御** | `duration` / `loop` フラグがなく常に無限放出 | `duration`・`loop` フィールドと経過時間追跡が必要 |
| **深度ソート不在** | アルファブレンド描画で粒子の描画順が未ソートのため半透明が正しく合成されない | `ParticlePass` に Camera 距離でのソートが必要 |

### 重要度：中

| 機能 | 説明 |
|---|---|
| **速度の over-lifetime 制御** | 初速以降の速度変化がなく、軌跡に抑揚が付けられない |
| **乱数の品質** | `std::rand()` を使用。エミッターごとのシードなし、再現性もない。`Util/Random.hpp` が既存なので差し替え可 |
| **emitAccum の溢れ問題** | `maxParticles` 到達後も `emitAccum` が加算され続け、上限解除時に粒子が瞬間大量放出する |
| **Script API の薄さ** | `SetEmitRate` / `SetEnabled` / `Clear` のみ。`Burst()` / `Play()` / `Stop()` がない |
| **開始遅延（startDelay）** | エフェクト再生開始から放出まで遅らせる手段がない |

### 重要度：低（将来対応）

| 機能 | 説明 |
|---|---|
| **GPU パーティクル** | 現在 CPU 更新のため数千粒子で重くなる。Compute Shader による並列更新 |
| **ノイズ / タービュランス** | 速度に乱流を加える Curl Noise フィールド |
| **コリジョン** | 地面・コライダーとの衝突反射 |
| **レンダラーモード** | ビルボードのみ。ストレッチドビルボード・水平ビルボード・メッシュパーティクル |
| **サブエミッター** | 粒子の誕生・死亡時にトリガーする子エミッター |

---

## アーキテクチャ

```
│ Editor Layer                                                │
│  ParticleTool（未実装）                                     │
│    ↓ ParticleEmitter パラメータを変更                       │
               ↓ reads & writes
│ Scene Layer                                                 │
│  GameObject + ParticleEmitter                               │
│    ・emitRate / maxParticles / lifetime                     │
│    ・emitPosition / emitVelocity / velocitySpread           │
│    ・colorStart / colorEnd（CPU lerp）                      │
│    ・sizeStart / sizeEnd（CPU lerp）                        │
│    ・particles: std::vector<Particle>                       │
│    ・emitAccum: float（サブフレーム蓄積）                   │
               ↓ ExecuteParticlePass が Scene をイテレート
│ System Layer                                                │
│  ParticlePass（ExecuteParticlePass）                        │
│    ・emitAccum ≥ 1.0 → 粒子生成（ランダム速度）            │
│    ・age ≥ lifetime → 粒子削除                             │
│    ・重力（5.0f 固定）+ 位置・色・サイズ更新               │
│    ・CPU で ParticleVertex × 4 を構築（1 粒子 = 1 クワッド）│
│    ・VB を毎フレーム resources.Update() で転送              │
│    ・TRANSPARENT_LAYER で DrawCall 発行                     │
│                  ↓ ResourceManager 経由                    │
│  IRenderer（DX11 具体実装に非依存）                         │
```

---

## 粒子データ構造

```cpp
struct Particle {
    Vector3 position;
    Vector3 velocity;
    Vector4 color;
    float   size;
    float   age;       // 現在経過時間
    // 不足: float rotation, angularVelocity
};
```

頂点レイアウト（現在）：

```
center(12) + uv(8) + color(16) + size(4) = 40 bytes/vertex × 4 vertex/particle
```

テクスチャ対応後（予定）：

```
center(12) + uv(8) + color(16) + size(4) + rotation(4) = 44 bytes/vertex
```

---

## フレーム描画順序における位置づけ

```
  1. RenderSystem（不透明メッシュ）
  2. TerrainRenderSystem
  3. WaterRenderSystem（半透明）
  4. TrailRenderSystem（半透明）
  5. ParticlePass（半透明）← 最後尾。深度ソート後に描画が理想
  6. PostProcessSystem
```

深度ソートは ParticlePass 内で各エミッターの粒子を Camera 方向距離でソートして解決する。

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| **1** | `gravity` フィールド公開 + `emitAccum` 溢れ修正 + `std::rand()` → `Random` 差し替え | Inspector で重力を変更できる。maxParticles 到達時に瞬間放出しない |
| **2** | `duration` / `loop` / `startDelay` フィールド追加 + `Play()` / `Stop()` + `Burst()` Script API | 有限時間のワンショットエフェクトが作れる。スクリプトから爆発が出せる |
| **3** | パーティクル回転（`rotation` / `angularVelocity`）+ 深度ソート | ビルボードがスピンし、半透明のZ順が正しくなる |
| **4** | テクスチャ対応（`textureHandle`）+ スプライトシート UV アニメーション | 炎・煙テクスチャが貼れる |
| **5** | エミッター形状（Sphere / Cone / Box）| 形状を選ぶだけで放出パターンが変わる |
| **6** | `ParticleTool`（エディタ UI）+ `SceneSerializer` 対応 | エディタでリアルタイム調整・シーン保存が通る |
| **7** | GPU パーティクル（Compute Shader 更新） | 1 万粒子でも CPU 負荷ゼロ |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/
    Scene/
      Components/
        ParticleEmitter.hpp          ← Particle + ParticleEmitter 定義
      ScriptProxy/
        ScriptParticleProxy.hpp      ← Script API
      Systems/
        RenderPassContext.hpp        ← particleShader / particlePSO / particleVB / particleIB
  src/Scene/
    Systems/
      RenderPasses/
        ParticlePass.cpp             ← 更新・描画ロジック

Assets/Shaders/
  Rendering/
    Particle.hlsl                    ← VS（ビルボード展開）+ PS（カラー / テクスチャ）

Docs/System/Particle/
  overview.md                        ← このファイル
```

---

## 隣接ドキュメント

- [../Trail/overview.md](../Trail/overview.md) — 同じ半透明パスを使う先行実装（マイタージョイント・深度順の参考）
- [../Water/overview.md](../Water/overview.md) — 半透明描画パスの構成例
