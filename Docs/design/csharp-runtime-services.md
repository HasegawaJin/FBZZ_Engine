<!-- @file    csharp-runtime-services.md -->
<!-- @brief   C#の実行サービス、所有権、転送と失敗時の公開条件。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-05 -->
# C# runtime services

2026-10-05。C# の基本サービスを既存 native 実装へ接続する契約。公開型・署名・callback 権限は SDK catalog と XML に投影する。Engine API は同期 callback の現在のメインスレッドに限り、constructor / OnValidate は許可しない。Connect は read と所有要求の登録だけを許す。

## Application と Scene

`Script.App.IsEditor` は native Application の Editor host 状態を読む。Edit / Play と独立し、Editor の Play 中も true、Standalone は false。`IsDevelopmentBuild` は native host の assert 有効構成（Debug / Development）で true、Release で false。どちらも通常の同期 read callback から取得し、constructor / OnValidate には権限を与えない。ゲーム DLL 自体のコンパイル条件とは区別する。

ゲームの条件付きコンパイルは共通 starter の `Fbzz.ScriptBuild.props` と AgentBuild の同じ props を使う。`FbzzBuildConfiguration` は Debug / Development / Release のいずれかで、直接ビルドと IDE では `Configuration` が既定、AgentBuild は native 側の選択構成を明示する。SDK の参照先を選ぶ `FbzzSdkConfiguration` とは独立。Debug は `FBZZ_DEBUG;FBZZ_DEVELOPMENT`、Development は `FBZZ_DEVELOPMENT`、Release は `FBZZ_RELEASE`、現行 Windows ターゲットは `FBZZ_WINDOWS` を定義する。SDK 標準の DEBUG / TRACE は .NET の Configuration に従い、エンジンの条件判定には FBZZ 接頭のシンボルを使う。

独自シンボルは csproj の共通 props import より前、または import 済みの `Fbzz.Sdk.props` で `FbzzDefineConstants` にセミコロン区切りで指定する。設定はディスク上の project / props を正本にして IDE・Editor・配布ビルドへ共有する。FBZZ 接頭はエンジン予約。Editor で生成した同じ DLL を Standalone でも使うため `FBZZ_EDITOR` は定義せず、host 分岐は `App.IsEditor` を使う。

既存ゲームは SDK の共通 starter から `Fbzz.ScriptBuild.props` を csproj の隣へ追加し、`Fbzz.Sdk.props` の後で import すると IDE も同じ定義になる。AgentBuild は既存 csproj にも共通 props を適用する。project / props の内容変更は既存の入力 hash・Editor 監視に含まれ、DefineConstants の変更は Roslyn の増分コンパイル入力になる。コンパイル失敗時は候補世代を公開せず、旧世代を保持する。

Scene の Entity と Component は必要な分だけ動的に拡張し、制作上の固定個体数上限を設けない。caller buffer の余りは保持し、検索は実結果を全件返す。生成・query の件数と転送 byte 数は CLR/ABI の数値表現内で検証する。pool は負の確保数を拒否し、全個体の生成失敗では既存 Scene と出力を変更しない。保存方式と参照安定性は [Scene の動的保存](scene-capacity.md) に従う。

world position batch は件数で打ち切らない。全入力を内部 packet で段階転送し、全 target・lifetime・変換結果を事前検証してから一度だけ commit する。転送・検証が失敗した場合は Scene と caller 出力を未変更にする。physics hit は caller buffer の Required / Written 契約で全件を返し、内部 packet size は結果件数の上限ではない。packet の容量制限を API の総件数制限として扱わない。

`Script.App` は現在の Play / Edit と実 window の幅・高さ [pixel] を読む。Game View の viewport サイズとは区別する。window が未作成なら 0。`Quit()` は Play 中だけ受け付ける終了要求。Standalone は既存 Application の終了 flag、Editor は現在の host / Scene lifetime の要求を次の安全な frame 境界で Play 停止へ変換する。callback の途中に Scene を破棄せず、Editor 全体を閉じない。Stop / Shutdown / destructor は非所有 hook を解除する。

`Script.Scene.TryLoadRegistered(name)` は実 SceneManager の登録名を指定する。未登録なら false、manager が接続されていなければ診断。成功は queue の受付を表し、load 完了ではない。同じ callback の残りは現在の Scene、次 frame の SceneManager 更新で切り替える。factory 失敗は旧 Scene を保持する。Stop は保留要求を既存 ReleaseOwnedScene 経由で解除する。`RegisteredName` は実 ActiveSceneName を読み、外部 Scene を bind したときは空。空値を返す未実装 native GetSceneName proxy は転送しない。

## Cursor

`Script.Cursor.Push(lockMode, visible, priority)` は Script 個体・コード世代・Scene lifetime が所有する要求を返す。優先順位は既存 stack と同じで、同値は後から積んだ要求が勝つ。要求の Set は順序を変えない。Release / Dispose は現在の同期 callback で行う。finalizer から Engine を呼ばない。disable / fault / destroy / requirements block / reload / Scene 変更は native owner cleanup が要求を解除する。ClearRequests 後の handle は失効し、別要求に再接続しない。IsActive は保持中かを表し、優先順位の勝者かとは別。

read は実効の Visible / LockMode / Shape。作者へ Editor 専用 Reset / Suppressed 操作を公開しない。基底への直接書込みは初期 slice に含めず、所有要求で復元を保証する。要求 object は差し替え状態の保存対象にせず、新しい Connect / lifecycle で再構築する。

## Light と ParticleSystem

`Light` は実 LightComponent、`ParticleSystem` は実 ParticleEmitter の birth token を保持する。Get / Add / Remove / 必須 Component / private 参照再接続は同じ型対応を使い、native kind 27 / 28 を canonical NativeComponentKind.COUNT に基づく pending bitset と birth table で扱う。除去予約・再追加・Scene 失効は既存 exact lifetime 契約に従う。

Light の Color は線形 RGB、Intensity は native LightComponent の type-relative authoring 単位で、lux / lumen を名乗らない。Directional は放射照度、Point / Spot / Sphere / Tube は 1m 基準の強さ、Area は面の輝度。Range は [m]、SpotAngles は [degree]。値は有限・非負、角度は 0 <= inner <= outer <= 180。角度の組は全件検証後に変更する。Light は ExecuteAlways の見た目構築に使えるため、live Edit callback でも read / write を許す。

ParticleSystem の Play(restart=true) / Stop(clear=false) / Burst(count) / Clear は実 Emitter メソッドへ接続する。Clear は粒子・累積・GPU clear 要求を初期化するが playing は変えない。Stop は発生を止め、clear=false の既存粒子を残す。Burst は非負で、pending count の加算 overflow を変更前に拒否する。Enabled と IsPlaying は別。ParticleCount は CPU 生存粒子数で GPU simulation は 0。EmissionRate は [particles/s]、Lifetime は [s]、PlaybackTime は [s]。変更は Play の同期 callback のみ、read は live Edit callback でも可能。未存在 / pending row は取得不能、失効 facade の操作は診断して replacement を変更しない。

## Material

Play の書き込み可能テクスチャは [RuntimeTexture](csharp-runtime-textures.md) を使う。`CreateR8` / `CreateRGBA8`、同期 `UpdateRegion`、`Material.TrySetRuntimeTexture` は作成元 Script が所有し、共有アセットや保存内容を変更しない。

`GameObject.GetComponent<Material>()` は実 MaterialComponent の kind 29 と birth token を持つ。pending removal は即座に不在、再追加は別 token で、保持済み facade が replacement を操作しない。pending bitset・制御・保存 birth table は共通 NativeComponentKind.COUNT を正本とし、番号だけを追加して mask を越えない。

slot は 0 始まりの既存 submesh slot 番号を各呼び出しに渡す。範囲外は診断し、slot 0 への fallback、Component の自動追加、slot 配列の自動拡張をしない。slot 自体の寿命を保持する wrapper は公開しない。`SlotCount`、Component の `Enabled`、slot の表示状態、型付き float/int/uint/bool/Vector2/3/4/Color の override を扱う。Color は linear RGBA、他の数値・vector の単位と意味は実 shader property の宣言に従う。finite 値・reflection の型を検証し、欠損・未取得 reflection・型不一致は TrySet=false、描画値は未変更。TryGet は .NET の out 形式で、false 時は out を型の既定値へ設定する。ClearOverride は共有値へ戻し、ClearAllOverrides は指定 slot の数値・texture・描画 override を消す。

共有 .mat の割当は MaterialAssetRef、texture override は TextureAssetRef の実 GUID を解決し、対象の型とロード成功を確認してから変更する。無効・欠損・別型は false、旧割当と override は保持する。空の MaterialAssetRef は明示的な割当解除。違う path への成功した共有 Material 変更だけが該当 slot の override を消去し、同じ割当への再設定では維持する。共有アセットの内容を runtime に書き換えない。Get は既存の登録 GUID を返し、参照取得のためだけに metadata を新規発行しない。read は Connect と通常同期 callback、write は live Edit/Play callback、DrawGizmos は read のみ。

Matrix4 は行優先16要素の shader 値で、Matrix の row/column-major GPU 配置は native reflection が処理する。TryGetValues/TrySetValues は double の数値列を用い、整数の全32bitと配列・任意形状の行列を保持する。全値を reflection の型・要素数・範囲で検査し、write は未公開候補から一度だけ変更する。内部 packet は64 KiB以内だが数値列全体の上限ではない。caller Span が短ければ Required と Written=0 を返して出力を変更せず、件数だけの照会では native 値列の double 変換を行わない。成功時も余りの領域は保持する。途中の read 失敗は caller 出力へ部分結果を公開しない。全値の read/write は件数に比例する転送と候補用メモリを必要とする。

BlendMode は Opaque/Alpha/Additive/Premultiplied、DoubleSided と RenderQueue は指定 slot の runtime override。未 override の read は実共有アセットを解決して返し、未割当は native の標準値、割当済みアセットのロード失敗は診断する。slot0 への代理読込みをしない。

## 独立 RandomStream

RandomStream は Engine callback を要求しない値型で、一つの uint64 状態を持つ。値コピーは独立した同じ位置の乱数列。zero seed/state と default(struct) は固定非ゼロ値へ正規化し、native と SDK で xorshift64* の状態遷移と乗算を揃える。暗号用途には使わない。State は明示した再開位置で、Script 設定 codec への RandomStream 型の保存を自動で追加する契約ではない。

Value は上位24bitによる [0,1)、int Range は両端を含む [min,max] で、INT32_MIN と INT32_MAX を含む全範囲を支持する。差と最終加算は int64 で行う。実出力域が 1..UINT64_MAX なので、`limit=UINT64_MAX-UINT64_MAX%span` を超える word を棄却し、`(word-1)%span` へ写す。実周期を等しい件数に分割し、単純 modulo の偏りを残さない。[rejection sampling の根拠](https://www.pcg-random.org/posts/bounded-rands.html)と状態遷移の根拠は実装の @see にも記載する。

float Range は有限端点の [min,max)、double 中間値で差の overflow を避け、float の丸めで max になったら直前の表現値を返す。同値端点はその値を返して状態を進めない。逆順・非有限端点は SDK で例外、native で数学契約違反。Chance は有限な [0,1] を要求し、0/1 も一回状態を進める。単位円・球の生成は既存 native と同じ draw 手順で、三角関数の末尾 bit までの環境非依存一致は保証しない。固定 vector と強制した棄却 seed で両実装を検証し、実行時間の改善とは扱わない。

## Render

`Script.Render` は Application に登録された Play 専用 `RenderSettings` copy を読む・変更する。Edit の設定や Editor の viewport を変更しない。copy が無い場合は `IsAvailable == false` とし、他の設定 query は値を捏造せず失敗する。設定 getters は readonly draw callback からも使えるが、全 Render 設定の read/write は Play 専用。`RenderWidth` / `RenderHeight` は実 game viewport と runtime scale から解決した内部解像度 [pixel] を返す。

Brightness は 0.1..4、BloomScale は 0..2、RenderScale は 0.5..2 に native が clamp する。Quality は LOW/MEDIUM/HIGH/ULTRA。ShadowResolution は正の `int` を exact integer として送り、native が 512..8192 に clamp する。ShadowCascades も exact signed integer を送り、native が 1..4 に clamp するため、int32 の端値で float 変換による境界越えを起こさない。SSR、GTAO、SSAO、TAA、FXAA、Bloom、MotionBlur、VolumetricLight、ContactShadow は個別の実 render-pass preference で、ある pass を別の pass へ代用しない。

## Display

`Script.Display` の read-only query は Edit / Play の同期 callback と readonly draw で、実 host の client size、monitor modes、window mode、VSync を読む。未作成 window や未接続 renderer は既定値を生成せず query を失敗させる。`CanChangeWindow` は Editor host では false。Editor window の変更、ゲーム用 render viewport の代用、要求値を実際の window 状態として返すことはしない。

`TrySetFullscreen`、`TrySetResolution`、`TrySetVSync` は Play の同期 callback からのみ受け付ける。resolution は正の pixel 寸法を要求する。Editor または host が要求を受理できない場合は false。受理は frame 境界で適用する intent の受付で、即時変更の完了ではない。要求は現在の Scene lifetime と結び、Scene 終了・Play 停止・ProjectRuntime shutdown 時には破棄するため、古い Scene の要求が後の Project に適用されない。

## Joint

`GameObject.GetComponent<Joint>()` は native `JointComponent` kind 34 の birth token を持つ facade。query getters は Play の同期 callback と readonly draw で使え、mutation は `FixedUpdate` のみ。Joint row と owner の RigidBody が必要で、接続先は同じ Scene の有効な RigidBody を持つ別 GameObject に限る。pending removal は即時に不在として扱う。無効な target、self-connect、非有限値、失効した facade は row や物理 World を変更せず失敗する。

Fixed / Distance / Rope / Spring / Hinge / Slider の接続、limits、motor、distance、Break は既存 `JointComponent` と `physics::World` を変更する。Hinge/Slider 軸は world-space direction、local anchors は各 body の local-space [m]。Chain は順序付き `GameObject` recipe を保存する。全 target の Scene・birth・RigidBody・重複を検証し、全 chain candidate を用意してから一度だけ公開する。64 KiB の内部 transfer packet は chain の件数上限ではない。途中 packet・target 解決・owner lifetime fence の失敗では以前の recipe を保持する。物理 World から partner body が消えたときは constraint を解放し、古い EntityID を持つ recipe は再接続しない。

## Save と Config

Script.Save と Script.Config は実 Application の別 SaveStore に接続する。進行データの path/Load/Clear で環境設定を消さない。全操作は Play の同期 main-thread callback。Connect ではメモリの read だけ、Load/Save を含む変更は live callback。DrawGizmos と authoring candidate callback では使わない。既定の path は実行ファイル隣の Saves/save0.toml と Config/settings.toml、相対 path も実行ファイル基準で、path 設定は暗黙に Load しない。

公開範囲は bool/int/float/string/Vector2/3/4 の scalar 値、型違い・欠損時の明示した既定値、Has/Remove/Clear、Path/IsDirty、明示 Save/Load。setter はメモリだけを更新する。Save は成功後に dirty を解除する。Load は真に欠損した file だけを初回状態として空の clean 値に置換し、権限・I/O・parse 失敗は false で既存値と dirty を保つ。Play 停止や Scene 差し替えで永続データの変更を Undo しない。構造化した CLR object を native の IScriptSerializable pointer として渡す API は提供しない。

大きな string の read は同期 callback 内で SaveStore の既存文字列を借用し、返信の encoding までだけ保持する。借用 view を managed 側へ公開しない。GetString の fallback は呼出元に保持し、wire へ送らず、欠損・型違いのとき同じ文字列を返す。既存の空文字列は欠損と区別する。固定 packet のページで UTF8 の途中文字を分断しても decoder が全文を復元する。全読み出しには文字列の長さに比例した転送・復号と最終 managed string の確保が必要である。

SetString は通常の一回の同期呼出しとして、少量値は既存 packet、大きな値は内部の begin/stage/commit/abort を使う。packet の容量を値全体の対応上限にしない。全量の strict UTF8 検証と byte 数取得を公開前に行い、不正な UTF16 は長さによらず旧値・dirty を変更せず拒否する。埋込み NUL は値として保存する。受信は exact Script 個体・世代・Scene/Entity・同期 call・store/key・revision/path が所有し、byte offset と宣言した全量を照合する。部分値は生の store へ公開せず、完了後に所有 std::string を一度だけ移す。順序違い・重複・UTF8 不正・途中終了・競合は旧値を変更しない。外部変更で更新された dirty を巻き戻さず、失敗が新たに dirty を変えない。

現実装の受信は最終値の std::string 容量を保持するため、その長さと旧値に比例したメモリが必要であり、退避 file を使ったものとは説明しない。同時受信 handle は32件、単一 chunk は64 KiB以内の資源契約。未 commit 値は明示 abort、同期 call 終了、owner 解放、世代退役で回収する。file 保存は同一 parent の排他的に作った一意な temp を完全に書いて flush/close 後に置き換える。失敗は旧 disk と既存 dirty を保持し、自分が作った exact temp だけを回収する。既存の予測可能な .tmp を上書き・削除しない。file reparse point は明示拒否し、電源断の耐久性を flush/replace の実保証以上に主張しない。

Write<T>/TryRead<T> は portable field codec に対応する値を、実 TOML store の型付き envelope として扱う。formatVersion=1、assembly名と完全型名、宣言 schema の SHA256、JSON 値を保持する。GameObject、Component、ComponentRef は入れ子・配列内も拒否し、Material/Texture/DataAsset の型付き GUID は保持する。Scene の GUID を暗黙に現在の物体へ再接続しない。constructor/getter/callback は保存・復元中に実行しない。短い値も大きい値も不正 Unicode は宣言 schema 違反として拒否する。

受信の revision/path fence と commit は文字列保存と同じで、構文・型・schema・版の不一致、重複 key、非有限数、途中終了は旧値と dirty を保つ。配列/List/string 全体に packet/node/byte 数の固定上限は設けず、実表現・メモリ・深さの契約を守る。TryRead の false は out を既定値にし、渡した defaults は未変更。成功時も defaults を受動的に複製した別の値を返す。schema が変わった保存値には明示 migration が必要で、FieldKey の旧名だけで schema の不一致を迂回しない。公開 API は CLR object を native IScriptSerializable pointer として渡さない。
