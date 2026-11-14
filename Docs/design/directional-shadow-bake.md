<!-- @file    directional-shadow-bake.md -->
<!-- @brief   方向ライトの時刻別自動影ベイクと実行時の再利用契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-07 -->

# 方向ライトの自動影ベイク

編集時の `render.directional_shadow_mode` は `mode` に `realtime`、`auto_bake`、`cached_far_cascade` を受け取り、選択中の方向ライトか `node` で指定したライトを変更する。`distance` は影の最大到達距離 [m]。0 は描画設定の距離を使い、省略時は現在の距離を使う。設定は一件の Undo で戻せる。

`LightComponent` の `Shadow Mode` は `Realtime` と `Auto Bake` を持つ。既存シーンの既定は `Realtime`。`Auto Bake` では昼と夕方のライト方向を記録し、エディター内のPlay前に必要な静的深度を準備する。未指定の方向は対象ライトに結び付いたSequenceから昼の開始値と日没側の高度12度を探す。該当するトラックがなければ、昼は現在の方向、夕方は同じ方位の高度12度を使う。光の向き、空と水面は既存のTransformとSequenceを正本にする。ベイクは照明の値を書き換えない。

静的深度はワールド空間に固定した方向ライトの投影で作る。投影範囲はシーンの保守的な境界から求め、カメラ外や隣接区画から範囲内に落ちる影も候補に含める。最遠カスケードの投影範囲がベイク内に収まるビューでは、静的深度をそのカスケードの座標へ変換して復元する。全カスケードが従来の投影と解像度を使い、動的キャスターは従来どおり毎フレーム描く。影アトラスを共用するので地形、水面、体積光は同じ深度を参照できる。

静的深度へ入れるのは、形状と姿勢の変更を追跡できる地形と、`MeshRenderer` の `Bake Static Shadow` を有効にした適格な不透明メッシュ。ベイク元は1テクセルあたり1 mを目安に最大4096²まで解像度を上げる。上限で1 mを超えるシーンでは地形をベイクし、メッシュの影は元の解像度で描く。スキニング、風変形、インスタンス群、ビューで形状が変わるLODはリアルタイムで扱う。太陽が動いている間、未ベイク、データや設定の更新、範囲不足、資源エラーでは通常のカスケードで描く。

深度は対応する投影と合わせて公開する。GPU上の色バッファ経由で保存可能な深度へ変換し、実行時は `SV_Depth` でアトラスに復元する。D3D12の深度リソースの部分コピーに依存しない。生成物の同一性はシーン、ライト方向、対応するキャスターと影設定で検証する。[D3D12 深度コピーの制約](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion) · [Unreal Engine の静的・動的シャドウキャッシュ](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine#caching)

葉のキャスターは、`Auto Bake` の方向ライトでは風で変形する前の姿勢を使ってリアルタイムに深度を描く。方向ライトの3枚目以降のカスケードでも静止形状を使い、アルファカットとLODの輪郭を保持する。`Realtime` の手前2枚と点・スポットライトの影では風の変形を反映する。[葉の配置と描画](instanced-mesh.md) · [Unreal Engine の遠景の頂点変形](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine#disabling-mesh-deformation-at-a-distance)

Play要求は編集シーンに対してベイクを準備し、生成中はクローンの開始を待つ。編集シーンを閉じたときやPlay要求を取り消したときは開始を中止する。`Library/DirectionalShadowBake` へ保存した生成物は次のエディター内Playで読み込める。ベイクの範囲はシーン単位で、焼いた地形の細部は固定範囲と元画像の解像度に依存する。計測は初回ベイクと定常描画を分け、昼と夕方の各4視点で `Realtime` と `Auto Bake` を比較する。

## 遠景カスケードの実行時キャッシュ

CSM の最遠カスケードの末尾 15% で、太陽のリアルタイム遮蔽を連続的に減衰する。通常 PCF、受光面 PCF と PCSS に同じ重みを使う。雲影はワールド空間の遮蔽として維持する。ローカルライトは影距離の末尾 20% で影の強度を下げ、影がゼロになった光源は影アトラスの候補から外す。距離は光源の到達球の表面から測る。

方向ライトの `Cached Far Cascade` (`cached_far_cascade`) は、Play中にカスケードが2枚以上あり、深度を矩形でクリアできる場合に使える。ビューごとの深度専用アトラスに静的キャスターを保持する。4カスケードでは中距離の静的深度を4フレーム、最遠を8フレームごとに更新し、更新の位相を2フレームずらす。2〜3カスケードでは最遠を8フレームごとに更新する。毎フレーム、静的深度を同じ矩形と投影で通常の影アトラスへ転写し、動的キャスターを現在の姿勢で重ねる。手前のカスケードは通常のリアルタイム描画を続ける。Spot / Point は既存の影アトラスを使う。編集ビューや資源を確保できない場合は通常のカスケードを使う。

キャッシュ対象の投影に12.5%の余白を確保し、カメラ移動・回転や太陽の方向が許容範囲を外れたときは更新を早める。静的深度の対象は、CPUでLODを決める高さ場と、`Bake Static Shadow` が有効で形状の版が分かる不透明メッシュ。直前フレームから移動したメッシュ、スキニング、風や切り抜きのある形状、インスタンス、GPU間接選択の地形は毎フレーム描く。各タイルの地形パッチをライトの視錐台で絞り、そのタイルに提出するパッチのLODをキャッシュの判定に使う。地形の形状と姿勢、静的メッシュの姿勢・材質・バッファの版も照合する。[Unreal Engine のカスケード深度再利用](https://dev.epicgames.com/documentation/en-us/unreal-engine/directional-lights-in-unreal-engine#reusing-cascade-depths-between-frames) · [CRYENGINE Cached Shadows](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21267738)

インスタンスのライト視点Hi-Zは、GPU選別の候補があり、先に深度を描いたタイルを囲む範囲から作る。未準備の配置は候補に残し、既存の常駐境界を使える場合は、ライトの視錐台と描画距離から必要なタイルを絞る。

Playtest の `profiler.snapshot` は、静的深度を作り直したカスケードを `staticShadowUpdateMask`、当該フレームの合成に使った範囲を `staticShadowReadyMask` に返す。動くカメラ・太陽・キャスターを比較するときは、同じシーケンス時刻のフレームを照合し、更新のあるフレームの p95 も記録する。

半径3のPCFは、タイル内部で線形比較サンプラーに隣接テクセルの重みを渡し、7×7のボックスフィルタを16回の比較サンプルで求める。タイル端では従来の個別クランプを使う。 [Ignacio Castaño の比較サンプラーによる分離フィルタ](https://www.ludicon.com/castano/blog/articles/shadow-mapping-summary-part-1/)
