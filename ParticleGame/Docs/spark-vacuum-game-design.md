# Spark Vacuum ゲーム設計

## コンセプト

プレイヤーがステージを走り回り、携帯バキュームの吸引フィールドで浮遊するSpark群を回収する60秒のスコアアタック。

## 操作

- `WASD`: 移動
- `Shift`: BODYを消費するParticle Trail Slash
- `Space`: ジャンプ
- `E`長押し: Sparkと敵弾を渦巻き吸収
- 左クリック長押し→離す: Particle Lance
- `Q`: BODY最大付近でSupernova
- マウス: カメラ操作

## スコアルール

- Spark 1個の基礎点は100点。
- 3秒以内に次のSparkを回収するとコンボが1ずつ増える。
- 獲得点は `基礎点 × 現在コンボ`。
- 全Spark回収または制限時間終了でResultへ遷移する。

## 実装方針

- Sparkは`ParticleEmitter`を持つGameObjectとして配置する。
- PlayerのSkinnedMeshは描画せず、`Player.fbx`の全スキン頂点から生成した約6,000粒の
  MeshSurface Particleだけを表示する。
- Animatorの現在のボーン行列を4ウェイトで頂点へ適用し、歩行・ジャンプ中もParticle形状を
  SkinnedAnimationへ追従させる。
- Player Particleは短寿命GPUシミュレーションで毎フレーム人型を再構成し、移動残像を抑える。
- Playerの身体Particle残量をHP・射撃弾薬・ダッシュ燃料として共用する。
- 左クリックの長押し時間と吸収した敵弾でParticle Lanceを強化し、最大時は標的を貫通する。
- Particle標的は赤いParticle弾を撃ち返し、被弾時は身体Particleが周囲へ散る。
- E吸収は敵弾の軌道をPlayerへ曲げ、吸収成功時にBODY回復・加点・次のLance強化を行う。
- ShiftダッシュのSkinned Particle残像には攻撃判定を持たせる。
- BODY最大付近でQを押すと、敵弾消去と全標的攻撃を行うSupernovaを発動する。
- Sparkは捕捉範囲を広めに取り、距離依存加速と接線速度を合成した螺旋軌道で回収する。
- Spark回収で身体Particleを回復し、残量低下に合わせて人型のParticle密度も薄くする。
- 回収Sparkはすべて`Assets/Textures/Particles/spark_01.png`を使用する。
- Playerの`ParticleForceField`をE入力中だけAttractとして有効化し、見た目の粒子を吸引する。
- Spark本体も同じ吸引半径と方向で移動させ、見た目と回収判定を一致させる。
- スコア状態は`SparkVacuumGame`の静的セッション値でResultシーンへ引き継ぐ。
- HUDは`ScriptUIProxy`、シーン遷移は`ScriptSceneProxy`経由で行う。

## ビジュアル演出

- PlayerへCore Aura・Vacuum Vortex・Lance Charge・Supernova・Dash Slash・Reconstructionの6層を重ね、
  BODY残量と操作状態をParticleの密度・色・発光として直接見せる。
- 吸収開始時はSkinned MeshSurface Particleへ自身のAttract力場と下向き重力を短時間適用し、
  輪郭を崩した後にFBXの現在姿勢へ高密度再構築する。再構築時は外周の光点も身体へ落下する。
- Core AuraのPoint LightはBODY残量と被弾に追従し、身体Particleそのものを光源として扱う。
- Bloom・露出・彩度・Vignette・色収差・Screen Fadeを実行時に連動させ、吸収、溜め、被弾、
  Supernovaを画面全体の反応として強調する。
- 自機弾、敵弾、散乱粒子は用途別のParticlesテクスチャと寒色／暖色で即座に識別できるようにする。
- Particle標的はHPに応じて色を変え、位相をずらした脈動で静止中も生命感を出す。

## 拡張候補

- 金色Sparkの追加、時間延長、危険なRepulseフィールド。
- 回収順ミッションとコンボ中の色・音・ポストエフェクト強化。
- ステージ別ベストスコアのDataAsset保存。
