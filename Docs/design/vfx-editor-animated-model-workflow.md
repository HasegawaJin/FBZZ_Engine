# VFX Editor Animated Model Workflow

## 方針

VFX Editorへモデルとアニメーションを持ち込み、制作時のPreview Actorとゲーム実行時のAnimatedMeshノードを同じAnimator実装で再生する。

保存されるAnimatedMeshノードではAnimator Controllerを必須にする。ControllerがState、Layer、Mask、Event、Clip依存を確定するため、VFX Editorとゲームで異なる一時設定が再生される事故を防げる。

FBXまたは`.anim`をPreviewへ直接ドロップした場合だけ、一時Controller相当の単一Clip再生を許可する。グラフへAnimatedMeshノードとして保存する際は`.animctrl`または`.animcontroller`を割り当てる。

## データの分離

- Preview Actor
  - VFX Editorセッションだけに存在する制作補助モデル。
  - `.vfx`へ描画ノードとして保存しない。
  - Model、Controller、Clip、再生時刻、Bone選択をEditor設定へ保存する。
- AnimatedMesh Node
  - ゲームで生成されるVFX Graphノード。
  - Model、Animator Controller、Material、開始State、Socketを保持する。
  - VFX Graph時間からAnimator時間を決定論的にシークする。

## 実装チェックリスト

- [x] ✅ Asset BrowserからFBX／`.anim`をPreview Actorへドラッグ
- [x] ✅ Preview ActorのModel／Clip／Controller解決
- [x] ✅ Preview用Animator操作パネル
- [x] ✅ TimelineへAnimation Trackを表示
- [x] ✅ TimelineスクラブとAnimatorの決定論的同期
- [x] ✅ Bone Picker
- [x] ✅ Socket Attachment
- [x] ✅ Controller必須AnimatedMeshノード
- [x] ✅ AnimatedMeshのゲーム実行時生成
- [x] ✅ `OnAnimationEvent`リンクトリガー
- [x] ✅ Event名によるリンクフィルター
- [x] ✅ `OnTrigger`リンクと`ScriptVFXProxy::Trigger`
- [x] ✅ ScriptからPlay／Pause／Stop／Speed／Float／Int／Bool／Vector／Color／Asset操作
- [x] ✅ `VFXRef` SerializeFieldと`ScriptVFXProxy::SetGraph`
- [x] ✅ Model／Controller／Clip／Material Dependency保存
- [x] ✅ AnimatedMeshランタイムPooling
- [x] ✅ Overdraw表示でモデルを含める／除外する設定
- [x] ✅ Preview設定の永続化
- [x] ✅ 静的差分検証
- [ ] Visual Studio 2022全体ビルド

## 決定論

AnimatedMeshのAnimatorをフレーム差分だけで進めず、`graph.playTime - node.startTime`から毎フレーム再生時刻を決める。Restart、Timeline scrub、AI Preview、ゲーム実行で同じ時刻に同じPoseとAnimation Eventが得られることを優先する。

Root Motionは既定で無効にする。VFXノードの配置はGraph TransformとSocketが所有し、Animation Root Motionが二重に位置を動かさないようにする。

## Pooling

モデル、Bone階層、Animator、MaterialComponentを含む生成物をController／Model／Materialの組み合わせで再利用する。非アクティブ化時にAnimator時間、Event履歴、Morph Weight、Material override、Root Motion差分を初期化する。
