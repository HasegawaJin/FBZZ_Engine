# ParticleSystem 設計 — 全体概要

## 目的

CPU で粒子の生成・更新・描画を行う汎用パーティクルシステム。
炎・煙・爆発・魔法エフェクトなど「短命な粒子の集合」を `ParticleEmitter` コンポーネントで表現する。

現在の実装は軽量なビルボード粒子を優先しており、Unity の ParticleSystem 相当の全機能ではなく、
ゲームプレイや環境演出で必要になる最小構成から段階的に拡張する方針とする。

---

## 現在の実装状況

| 機能 | 状態 | 実装箇所 |
|---|---|---|
| ポイントエミッター（1 点から放出） | 実装済み | `ParticlePass.cpp` |
| `emitRate` / `maxParticles` による連続放出 | 実装済み | `ParticleEmitter.hpp` / `ParticlePass.cpp` |
| `emitAccum` によるサブフレーム放出数の蓄積 | 実装済み・要改善 | `ParticleEmitter::emitAccum` |
| `colorStart` → `colorEnd` 線形補間 | 実装済み | `LerpVec4()` |
| `sizeStart` → `sizeEnd` 線形補間 | 実装済み | `ParticlePass.cpp` |
| 重力 | 実装済み・要改善 | `ParticlePass.cpp` 内の `5.0f` 固定値 |
| CPU 頂点生成 + VS ビルボード展開 | 実装済み | `ParticleVertex` / `Particle.hlsl` |
| 加算合成 + Depth Read | 実装済み | `Particle.hlsl` コメント / `particlePSO` |
| Inspector 編集 | 実装済み | `InspectorEffects.cpp` |
| Scene 保存 / 読み込み | 実装済み | `SceneSerializer.cpp` |
| Script API | 実装済み・薄い | `ScriptParticleProxy`（`SetEmitRate` / `SetEnabled` / `Clear`） |

---

## 現在の制約

| 制約 | 影響 |
|---|---|
| 放出形状がポイント固定 | 球状爆発、円錐噴射、箱範囲の埃などを作れない |
| 重力が `5.0f` 固定 | 炎は上昇、火花は落下、煙はゆっくり漂う、という調整ができない |
| `std::rand()` 使用 | エミッター単位のシード、再現性、分布制御がない |
| テクスチャ未対応 | `Particle.hlsl` はソフト円形フェードのみで、煙・炎・魔法模様を表現しにくい |
| 深度ソートなし | 半透明粒子の重なり順がカメラ位置によって破綻しやすい |
| 回転なし | 粒子が常に同じ向きのため、火花・葉・破片表現が単調になる |
| 再生状態なし | `duration` / `loop` / `startDelay` / `Play()` / `Stop()` がなく、ワンショット制御ができない |
| バーストなし | 爆発・ヒット・着水など、瞬間的な N 粒子放出を表現しにくい |

---

## アーキテクチャ

```
│ Editor Layer                                                 │
│  InspectorEffects                                            │
│    ・ParticleEmitter の永続パラメータを編集                  │
│    ・ランタイム粒子配列 particles は直接編集しない           │
               ↓ Reflect / SceneSerializer
│ Scene Layer                                                  │
│  GameObject + ParticleEmitter                                │
│    ・enabled                                                 │
│    ・emitPosition / emitVelocity / velocitySpread            │
│    ・colorStart / colorEnd                                   │
│    ・sizeStart / sizeEnd                                     │
│    ・lifetime / emitRate / maxParticles                      │
│    ・particles: std::vector<Particle>（ランタイム状態）       │
│    ・emitAccum: float（ランタイム状態）                       │
               ↓ ExecuteParticlePass が Scene をイテレート
│ System Layer                                                 │
│  ParticlePass                                                │
│    1. emitRate * dt を emitAccum に蓄積                      │
│    2. emitAccum >= 1.0 かつ maxParticles 未満なら粒子生成    │
│    3. age >= lifetime の粒子を削除                           │
│    4. 位置・速度・色・サイズを CPU 更新                      │
│    5. ParticleVertex × 4 を構築（1 粒子 = 1 クワッド）        │
│    6. particleVB を毎フレーム Update                         │
│    7. IRenderer 経由で DrawCall を Submit                    │
```

`ParticleEmitter` はコンポーネントとして編集可能な設定値とランタイム状態を同じ構造体に持つ。
これは実装を単純に保つための現在の選択だが、`particles` と `emitAccum` は保存対象ではない。
シーン保存ではエミッター設定だけを永続化し、再生時に粒子状態を再生成する。

---

## 粒子データ構造

```cpp
struct Particle {
    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector4 color;
    float         size;
    float         age;
};
```

現在の頂点レイアウトは `Particle.hlsl::ParticleVSIn` と一致させる。

```cpp
struct ParticleVertex {
    float center[3];  // POSITION   12 bytes
    float uv[2];      // TEXCOORD0   8 bytes
    float color[4];   // COLOR       16 bytes
    float size;       // TEXCOORD1    4 bytes
};                    // 40 bytes
```

`Particle` はシミュレーション状態、`ParticleVertex` は描画転送用データ。
今後 `rotation` やスプライトシート UV を追加する場合は、両方の構造体と HLSL 入力レイアウトを同時に更新する。

---

## 永続化対象

SceneSerializer は次の `ParticleEmitter` フィールドを保存 / 読み込みする。

| フィールド | 用途 |
|---|---|
| `enabled` | エミッターの有効状態 |
| `emitPosition` | GameObject 位置からの放出オフセット |
| `emitVelocity` | 初速の基準値 |
| `velocitySpread` | XZ 方向に加えるランダム幅 |
| `colorStart` / `colorEnd` | 寿命に沿った色フェード |
| `sizeStart` / `sizeEnd` | 寿命に沿ったサイズ変化 |
| `lifetime` | 粒子 1 個の寿命 |
| `emitRate` | 1 秒あたりの放出数 |
| `maxParticles` | エミッター単位の最大粒子数 |

保存しないランタイム状態:

| フィールド | 理由 |
|---|---|
| `particles` | 実行時に毎フレーム変化するため、保存すると再生開始状態が不安定になる |
| `emitAccum` | フレームレート差を吸収する内部蓄積値であり、シーン設定ではない |

---

## フレーム描画順序における位置づけ

```
  1. ShadowPass
  2. GBuffer / Forward 不透明描画
  3. Sky / Water / Trail / MeshTrail などの半透明描画
  4. ParticlePass（半透明・加算合成）
  5. PostProcessSystem
```

粒子は加算合成を前提にしているため、通常のアルファブレンドより順序破綻は目立ちにくい。
ただし煙や半透明テクスチャへ拡張する場合は、カメラ距離による back-to-front ソートが必要になる。

---

## 不足している機能・改善点

### 重要度：高

#### 重力係数の公開

現在は `ParticlePass.cpp` 内で `it->velocity.y -= 5.0f * dt;` と固定している。
エフェクトごとに上昇・落下・無重力を選べないため、`ParticleEmitter` に `gravity` または `gravityScale` を追加する。

**修正範囲:**

| ファイル | 修正内容 |
|---|---|
| `ParticleEmitter.hpp` | フィールド追加 + `Reflect()` 対応 |
| `ParticlePass.cpp` | 固定値をエミッター設定へ置換 |
| `InspectorEffects.cpp` | Inspector 操作用 DragFloat 追加 |
| `SceneSerializer.cpp` | 保存 / 読み込み対応 |

#### `emitAccum` の上限到達時の溢れ対策

`maxParticles` 到達中も `emitAccum` が増え続けると、粒子が寿命で減った瞬間に大量放出される。
上限到達中は `emitAccum` を一定範囲へ clamp するか、放出できなかった分を破棄する。

#### バースト放出

爆発・ヒット・着水などは `emitRate` だけでは作りにくい。
`Burst(int count)` を Script API として追加し、内部では既存の粒子生成処理を再利用する。

#### 再生状態の追加

現在は `enabled` が更新・描画・放出の全停止を兼ねている。
ワンショットエフェクトには `playing` / `duration` / `loop` / `startDelay` を追加し、
`enabled` はコンポーネント有効状態、`playing` はエフェクト再生状態として分ける。

### 重要度：中

#### テクスチャ対応

`Particle.hlsl` は UV を受け取るがテクスチャサンプリングはしていない。
`texturePath` と `ResourceHandle<TextureTag>` を `ParticleEmitter` に追加し、`DrawCall::textures[0]` にバインドする。

#### 深度ソート

煙や通常アルファブレンドへ拡張する場合は、粒子をカメラ距離で back-to-front に並べる。
加算合成のみなら優先度は下がるが、テクスチャ対応と同時に検討する。

#### 回転（スピン）

`Particle` に `rotation` / `angularVelocity` を追加し、HLSL 側で corner ベクトルを回転する。
火花・葉・破片などの見た目を単調にしないための拡張。

#### 乱数の差し替え

`std::rand()` はグローバル状態で再現性が低い。
既存のランダムユーティリティがある場合はエミッター単位の seed を持たせ、再生結果を制御できるようにする。

### 重要度：低

| 項目 | 概要 |
|---|---|
| エミッター形状 | Sphere / Cone / Box など、放出位置と初速方向の分布を増やす |
| over-lifetime カーブ | 速度・色・サイズを線形補間からカーブ制御へ拡張する |
| スプライトシート | 炎や煙の連番テクスチャを寿命に沿って切り替える |
| コリジョン | 地面・コライダーとの反射 / 消滅 |
| サブエミッター | 粒子の誕生・死亡時に子エミッターを発火する |
| GPU パーティクル | Compute Shader 更新で大量粒子に対応する |

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| 1 | `gravity` 公開 + `emitAccum` 溢れ修正 + `std::rand()` 差し替え | Inspector / Scene 保存で重力が保持され、上限到達後に瞬間大量放出しない |
| 2 | `Burst()` / `Play()` / `Stop()` Script API + `playing` / `duration` / `loop` / `startDelay` | スクリプトからワンショット爆発とループエフェクトを制御できる |
| 3 | テクスチャ対応 + `texturePath` 保存 / 読み込み | 炎・煙・魔法テクスチャを粒子へ貼れる |
| 4 | 深度ソート + 通常アルファブレンド対応 | 煙などの半透明粒子がカメラ距離順に描画される |
| 5 | `rotation` / `angularVelocity` | 粒子が寿命中にスピンする |
| 6 | Sphere / Cone / Box エミッター形状 | 形状を選ぶだけで放出パターンを変更できる |
| 7 | スプライトシート / over-lifetime カーブ | 表現力を Unity 風のモジュール構成へ近づける |
| 8 | GPU パーティクル | 1 万粒子以上でも CPU 更新コストを抑えられる |

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
    SceneSerializer.cpp              ← ParticleEmitter の保存 / 読み込み
    ScriptProxies.cpp                ← ScriptParticleProxy 実装
    Systems/
      RenderPasses/
        ParticlePass.cpp             ← 更新・描画ロジック
        GeometryPasses.hpp           ← ParticleVertex / ExecuteParticlePass 宣言

Projects/Editor/
  src/Panels/Inspector/
    InspectorEffects.cpp             ← ParticleEmitter Inspector

Assets/Shaders/
  Material/Effects/
    Particle.hlsl                    ← VS（ビルボード展開）+ PS（ソフト円形フェード）

Docs/System/Particle/
  overview.md                        ← このファイル
```

---

## 隣接ドキュメント

- [../MeshTrail/overview.md](../MeshTrail/overview.md) — 同じ半透明描画系のメッシュ残像システム
- [../Environment/overview.md](../Environment/overview.md) — WaterComponent から水しぶき用途で ParticleEmitter を利用
- [../Rendering/overview.md](../Rendering/overview.md) — RenderSystem / RenderPass 全体の設計
