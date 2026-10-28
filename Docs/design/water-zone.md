<!-- @file    water-zone.md -->
<!-- @brief   接続水域を一つの数値メッシュ・波・描画・浮力へまとめる初版の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

Water Zoneは接続先のWaterを持つGameObjectへ追加するopt-inのComponentとする。そのWaterは暗黙の親水域であり、membersへ湖・川・矩形Waterを参照と優先順位で登録する。各Waterは一つの有効Zoneだけに所属できる。既存の制御点・材質・波設定は保持する。初版のZone全体では親の材質・波・波紋・流れの場・浮力係数を共有し、各水域は輪郭・基準高さ・水深・基流を提供する。水域ごとの別材質や別波スペクトルの遷移、ビュー別ZoneタイルLOD、Ocean岸線は次の段階とする。[Epic Water Meshing System](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine)の共通メッシュとZoneの責務を参照する。

WaterZoneSnapshotはScene/RHIの参照を持たず、WaterSplineSnapshotと同じ数値頂点・三角形・BVHで有限水域の和集合を表す。入力は最大32水域と32河口。重なりは高いpriority、同値なら親・membersの登録順を優先する。輪郭を三角形化した入力から、既に採用した領域を差し引いて一度だけ面を生成する。共有辺の交点を挿入してT字接続を分割し、共通メッシュを細分化した後に各頂点の属性を一度だけ解決する。湖の凹形・分離した水域・和集合にできる穴を保つ。和集合そのものの頂点・三角形・演算予算を超えた場合はoutを変更せず失敗する。内部の細分化だけが予算に達した場合は最後の完全な共通メッシュとqualityLimitedを返し、水域の一部だけを欠落させない。

riverMouthsの各項目はmembersの川、接続する始点/終点、world mのblendLengthを指定する。接続先はZoneの親水域とし、端点がその変位前輪郭内にあることを要する。変換した端点と内向き接線を使い、河口付近の高さ・水深・基流をsmoothstepで親へつなぐ。明示した河口の範囲では川と親の補間が両者の通常priorityに優先し、それ以外は通常の優先順位を使う。接続境界では共通頂点から同じ値を使う。一つの川の両端も接続できるが、重なって競合する接続範囲は診断して拒否する。高さ場は鉛直に一価なので、優先順位は水柱の上下を表すものではない。重なり内の制御値は共通メッシュの頂点で選び、CPUとGPUで同じ重心座標補間を使う。

Zoneの準備は元のSplineを先に用意し、元形状・各Transform・参照世代・priority・河口設定・親の水深・目標セル幅の署名が変わったときだけ和集合を生成する。成功した親WaterはzoneSnapshotを持ち、登録メンバーはzoneOwnerで親へ解決される。membersが空ならZoneは動作させない。Zoneを持つ別オブジェクトをmembersへ登録する入れ子は初版では扱わない。無効な入力や競合は診断を出し、そのZoneは成立させず、元のWaterの動作を維持する。所属・有効状態は毎回確認し、非数値の失敗をキャッシュして再有効化を妨げない。Zone無効化・削除・参照変更時には前の所属を解除する。ランタイムsnapshot・所属・診断状態は保存しない。

描画・物理・水中判定・着水の収集は所属メンバーを除外し、親を一度だけ処理する。親の有効形状はzoneSnapshotへ切り替え、制御点の正本やsurfaceShapeを書き換えない。親の水深上限を生成頂点へ適用し、各地点の水深・基流は共通三角形から得る。ZoneのGPU資源は親の既存Scene/entity世代キャッシュが単独所有し、別メンバーへ所有ハンドルを複製しない。Scene・親の破棄入口でまとめて解放する。

Scene保存は共通ReflectionでEntity参照をinstanceIdへ変換する。Edit/Play複製は参照を再マップし、新しいSceneの準備で所属を作り直す。公開の既存水面高さAPIは指定したWaterが所属する親を解決し、その場でSceneの幾何準備を更新して同じZone水面を返す。Local高さAPIの入力座標と返す基準Yは、呼び出したメンバーのTransformを維持する。メンバーからのAddRipple/Splashは親の波紋へ届く。メンバーの設定APIは元のauthoring値を編集し、Zone無効時に再び有効になる。

使い方は接続先の海・湖を選び、WaterとWater Zoneを追加する。membersの各waterへ接続するWaterをドラッグする。川を接続するときはriverMouthsにも同じ川を指定し、atStartとblendLengthを選ぶ。端点を親水域の中へ置く。statusで有効・無効と原因を確認する。共通水面を細かくしたいときはZoneのmeshCellSizeを使う。Water単体のRectangle格子設定はZoneの共有メッシュへは適用しない。

受入境界は和集合面積・重複被覆なし・穴と共有辺・priority・端点接続・高さ/深さ/流速補間・二重所属の拒否・無効化後の復帰・Scene参照保存と複製・一つの描画入力/浮力Volumeである。波評価と固定密度メッシュの差は従来のLOD近似として扱い、Zoneにより物理の波を描画密度へ依存させない。

2026-10-07に実装と独立レビューを完了し、数値形状・保存/Play複製・描画資源・共有水深・API座標と波紋・PhysicsSystemの単独浮力の回帰を追加した。2026-10-08に残存コンパイラの競合が解消し、Release構成のビルド検査を完了した。検出したInspectorのFieldオーバーロード隠蔽、テストの実行コンテキストの一時値、Transform同期宣言のinclude漏れ、GPUテストのVector4添字とfixtureリソース参照を修正した。EngineAutoはエラー・警告0でコンパイル・リンク済み ([ログ](../../build/agent/build-20261008-074939-60180.log))。EditorLauncherとGraphicsAuto/GraphicsStandaloneもエラー・警告0でビルド済み ([ログ](../../build/agent/build-20261008-075546-40156.log))。Shaderのビルド対象は最新状態である。テスト本体の実行・見た目の確認は未実施であり、ビルド成功を動作回帰の合格として扱わない。
