<!-- @file    skinned-crowds.md -->
<!-- @brief   Independent skeletal animation sharing, compute batching and pose LOD contracts. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->
# スキンドメッシュの多数個体

同一モデルの多数個体は独立した再生時計・状態・骨姿勢・変形出力を持つ。キー補間と四骨の重み付き変形は既存式を維持する。

## 共有と計算

Model と Skeleton、入力頂点は既存キャッシュを使う。Animator のロード済みクリップは AssetManager の世代付きハンドルを参照し、キー列を個体ごとに複製しない。アセット失効後の参照は空クリップへ縮退し、通常の世代検知で再ロードする。手動で組んだ runtime clips は従来の値配列を維持する。

個体の行列をビュー単位の一括パレットへ収め、三列の DynamicBufferPool から借りる。同一入力メッシュの要求を二次元 Dispatch にまとめる。Y が個体、X が頂点群。出力は既存 Renderer 所有のバッファを維持し、行ごとの UAV 添字で書く。RHI は間接書込先をすべて検証し、状態遷移・GPU 内容版・UAV バリア・GPU fence による寿命を通常の出力と同様に扱う。未対応機器・プログラム・確保失敗は従来の個別 Dispatch に戻る。

## 距離による姿勢更新

更新 LOD は既定で無効。全有効 CameraComponent のうち最も近いカメラとの距離を使う。カメラが無い場合は毎フレーム評価する。遠景は姿勢の評価頻度だけを下げ、状態時計・BlendTree の位相・遷移・イベントの更新を毎フレーム続ける。近距離への復帰時はそのフレームの再生時刻を評価する。保持した姿勢の bounds と前姿勢を維持し、変形・色・影・マスク・速度は同じ公開姿勢を使う。

Property/Material/Morph Track を含むクリップ、RootMotion、外部ポーズ、追加レイヤー、IK・SpringBone・MotionWarp のある個体は毎フレーム評価へ戻す。ボーン Transform を毎フレーム読むソケットや独自スクリプトのある個体は LOD を有効にしない。Editor の Scene View を含む作者のプレビューは LOD を無効にして比較する。

## 描画

通常の自動 InstanceBatcher を拡張し、同じ元メッシュ・インデックス・材質・パイプライン・すべての束縛が一致する変形済み個体を束ねる。インスタンス行に world、normal と変形済み頂点の raw SRV 添字を持たせる。VS は SV_VertexID で既存 60 byte 頂点を読み、同じ共通シェーダー本体を実行する。未対応 shader、非 bindless、LOD dither、透過・特殊なパスは従来描画へ戻る。Velocity は前姿勢の二重スキニングを維持し、バッチ対象から外す。

カリングは既存ビュー・光源ごとの CPU 判定を維持する。変形バッチは resident/static 用 GPU compaction の行形式を使わない。通常 draw と同じ順序で連続する個体だけを束ねる。

## 検証

100/1000 体、アニメーション ON/OFF の同条件測定で CPU 完了フレーム、CPU 内訳、GPU パス、実 Dispatch/Draw、パレット転送、追跡資源量を記録する。GPU 準備時間がタイムスタンプ対象外の場合は未取得と記載する。スキニングの Dispatch・頂点・パレット転送統計は、当該エンジンフレームで実際に記録した準備処理を全ビューで共有する。同じ出力を再利用する後続ビューは重複計上しない。数値 GPU readback は異なるパレットの先頭・末尾を検証し、共有出力による位相同期を防ぐ。LOD の遠近復帰、資産更新、容量変更、失効ハンドル、材質差と描画変種のフォールバックを回帰する。
