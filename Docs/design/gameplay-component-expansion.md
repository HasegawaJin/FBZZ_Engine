# Gameplay Component Expansion

FBZZ Engineの既存GameObject／Component構成を維持しながら、2D描画、汎用Attachment、Constraint、Spline、Camera Rig、UI入力、Audio空間処理をゲーム制作で直接利用できる標準Componentとして追加する。

## 方針

- 単純Componentは`ComponentRegistry`のAutomatic Inspector／Serializerへ登録する。
- 毎フレーム動作する機能は専用Systemへ集約し、Component自身へScene探索や描画処理を持たせない。
- Script DLL境界から操作する機能は内部型を公開せずScriptProxy経由にする。
- 既存の`LightComponent`、Collider群、`AnimatorComponent`、`VFXGraphComponent`と責務が重複する型は増やさない。

## 実装チェックリスト

- [x] ✅ SpriteRendererComponent
- [x] ✅ SortingGroupComponent
- [x] ✅ SocketAttachmentComponent
- [x] ✅ TransformConstraintComponent
- [x] ✅ LineRendererComponent
- [x] ✅ UISlider
- [x] ✅ UIToggle
- [x] ✅ UIScrollView
- [x] ✅ UIMask
- [x] ✅ UIInputField
- [x] ✅ UIEventTrigger
- [x] ✅ AudioReverbZoneComponent
- [x] ✅ AudioOcclusionComponent
- [x] ✅ AudioMixerSendComponent
- [x] ✅ VirtualCameraComponent
- [x] ✅ CameraFollowComponent
- [x] ✅ CameraBlendComponent
- [x] ✅ CameraShakeComponent
- [x] ✅ BillboardComponent
- [x] ✅ ProjectorComponent
- [x] ✅ SplineComponent
- [x] ✅ SplineFollowerComponent
- [x] ✅ ComponentRegistry／Inspector／Scene保存
- [x] ✅ SystemScheduler登録
- [x] ✅ Script API
- [x] ✅ 回帰テスト
- [x] ✅ 静的差分検証
- [ ] Visual Studio 2022全体ビルド
