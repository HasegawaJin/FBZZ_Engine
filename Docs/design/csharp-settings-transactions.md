<!-- @file    csharp-settings-transactions.md -->
<!-- @brief   C# 設定の編集・検証・Play 調整を同じ transaction で扱う契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-05 -->

# C# 設定 transaction

Inspector、複数選択、AI、DataAsset の設定編集を一つの schema と transaction に集約する。既存の経路との互換性より、値を失わず予測できる編集を優先する。この文書は実装の受入条件であり、記載だけでは実装済みを意味しない。

## 値と対象

- schema は保存キー、型、既定値、null 許可、範囲、列挙候補、表示属性、参照先型を持つ。表示名を保存キーに使わない。
- 編集 snapshot は設定値と RFC 6901 path ごとの実体参照 token を持つ。GUID は保存用、token は現在の Scene・Entity・Script 個体の照合用とする。
- 配列の移動・削除は token にも同じ操作を適用する。同じ GUID が複数あっても位置から推測し直さない。明示的な再接続以外で、失効参照を同じ GUID の別個体へ復活させない。
- transaction は対象の個体 ID、schema 世代、変更前 revision と変更 path を指定する。別操作や reload が対象を変更した場合は競合を返し、上書きしない。
- Scene の保存アドレスは owner GUID と `savedScriptId`、DataAsset は asset GUID とする。Script の型名・列挙順や実行時 `inspectionId` を永続識別子に使わない。同じ所有者内の ID 重複を拒否し、保存・再読込・Play 復元では ID を保持する。
- native Component の保存アドレスは owner GUID と一意な Component 型とする。実行時参照には追加時に割り当てた寿命 token も持ち、削除後に同じ型を再追加しても以前の参照を復活させない。`GetComponent` の明示的な再取得で接続する。Transform の寿命は GameObject と同じとする。

## 配列の容量と大きな設定

- 配列・List の総件数と確保方針はスクリプト側が決める。4096 件、32768 要素、JSON 1 MiB といった表示・転送上の都合を、設定値の保存・編集・再読込の対応上限にしない。
- 高頻度 API は呼び出し側が事前確保したバッファへ書き込み、必要容量と実際の件数を返す。容量不足を黙って切り捨てず、既存の全件処理では出力を未変更に保つ。ページ取得は位置・件数・残りの有無を明示する。
- Inspector の描画件数、通信の一チャンク、保持中 snapshot の常駐メモリはそれぞれ独立した容量とする。設定全体の大きさとは分け、実際の資源不足や明示された容量不足は必要量と回復方法を診断する。
- 大きな観測は Host 側で path と範囲を指定して読む。全配列を一度 JSON 化してから一部を返す処理にしない。整合した複数ページには不変の観測 token を使い、取得後の値変更がページへ混ざらないようにする。
- Inspector の通常表示は可視範囲だけをその要求時点で読み、ページ間の同時点性を保証しない。配列を展開するだけで全体の snapshot・hash・退避を作らない。編集開始時の revision と候補の取得は表示から分離し、確定時に再照合する。
- 大きな snapshot と履歴は必要に応じてチャンク化・退避する。退避領域は要求単位で所有し、破棄・期限切れ・世代退役時に自身の領域だけを回収する。値の複製や検証に必要な作業は件数に比例するため、件数に依存しない処理時間とは扱わない。
- 大きな編集も未確定の領域で受信・検証し、全対象の照合が通ってから公開する。通信失敗や容量不足で途中までの値を生個体へ残さない。変更した葉・範囲を履歴の単位にし、不要な全配列の保持を避ける。
- 循環参照など保存形式として未対応の構造は path 付きで診断する。循環検出と配列件数の制限を同じものにしない。

通常の設定表示は named `READ_SETTINGS_PAGE` へ path/start/count を渡す。応答の `sample=request` は要求時点の生値であり、ページ間の同時点性は保証しない。子 object/collection は deferred な path と件数だけを返し、展開されたページと scalar だけを直接読む。巨大 string は UTF-16 code-unit 範囲を返し、surrogate pair の境界を保つ。schema と軽量 Host counter は cache できるが、callback の直接 field 代入を counter が検出するとは扱わない。

`OPEN_SETTINGS_EDIT` は編集開始時だけ exact 個体と全設定 revision を固定する。`STAGE_SETTINGS_EDIT` は offset、chunk SHA-256、Base64 bytes、final を持つ packet を要求所有の記憶域へ受信し、同一 chunk の再送は既存 bytes と照合する。小入力はメモリへ、大入力は所有する temporary spool へ保存する。最終 packet 前には候補 callback も live 代入も行わない。`PREPARE_SETTINGS_EDIT` は完結した JSON を一度だけ候補へ適用し、既存の共通 prepare/commit/Undo に接続する。破損・欠落・競合は公開前に拒否し、`CLOSE_SETTINGS_EDIT`、個体削除・世代退役で資源を回収する。保持される session 件数と packet 容量は明示的な資源制約であり、設定全体の件数上限ではない。

## 検証と確定

通常の Configure preview / apply 応答は全候補値を再掲せず、対象数・変更 path 数・最初の review page を返す。byte 予算は返される review JSON / Op data payload に対する合計であり、全 target、change、参照 token と metadata を含める。外側の Operator / MCP transport envelope の byte 数はこの予算に含めない。省略された値と対象には deferred 案内、総数、次の offset を返し、保存値自体を切り捨てない。

Editor 内部の prepared plan も全 before/after JSON を複製しない。Host の `GET_PREPARED_REVIEW` の `plan` は exact 個体・revision・変更件数だけをページで返し、`validate` は初回公開前の全設定 revision、Undo / Redo 前の変更 leaf hash を読み取り専用で照合する。実際の commit は全対象の個体寿命・参照・値を再検査してから一括公開する。候補と完全な変更 snapshot の所有者は Host に閉じ、Native の Scene・個体・有効状態の検査も維持する。

Inspector の live 文字列ページは UTF-16 の `start` / `returned` / `nextStart` を使い、Host が返した継続位置を履歴に保持する。要素数や64文字固定のページ番号から継続位置を推測しない。文字列の部分置換は取得した範囲に結び付け、複数範囲は後ろから適用する。検証失敗後の修正では元の revision を保持し、全対象が変わっていない場合だけ新しい編集 session を開く。競合した下書きは明示的な破棄・再取得まで勝手に付け替えない。

Script と DataAsset の Inspector は同じ継続位置の履歴を使い、最後のページでは「次へ」を無効にする。短いページや surrogate pair によって返却件数が変わっても、前後のページと部分置換の添字は実際に取得した範囲を保持する。DataAsset の保存失敗・選択変更では下書きを保持し、再試行と Undo / Redo は元のファイルの正確な bytes を基準にする。

Script の表示キャッシュは未確定入力と区別する。64件は入力を持たない表示キャッシュの目安であり、未確定の設定・参照・Play保持選択・旧世代の下書きは件数超過で消去しない。非表示の編集 session は解放して入力と元 revision だけを保持し、編集再開時は同じ対象・revision を再検査して開く。Host の32 session枠は保持できる入力の件数上限ではない。

再読み込みで managed 世代・schema が変わった未確定入力は、元の対象・世代・schema・値・patch として保持し、現行設定と分けて確認・コピー・明示破棄できるようにする。旧 widget の編集状態を終了させ、旧入力を新しい世代へ自動適用しない。適用済み設定のディスク保存や、停止時の Edit / Play 復元とは別の下書き保持である。

`script.configure.review` は保持中の plan と `options.mode=targets|changes|value` を指定する読み取り専用 query である。targets は targetOffset/targetCount、changes は targetIndex/changeOffset/changeCount、value は targetIndex/changeIndex/side/path/start/count を受ける。詳細は既存の before/after 不変な changed subtree から読む。大きな文字列の詳細は UTF8 byte 境界の continuation を使い、全体を JSON 化してから page へ切り取らない。discard・世代変更・Undo 値の解放で review は失効する。

候補の検証・保存・commit・Undo/Redo は全値を保持する。Host の候補構築・全設定 revision の照合・公開値の復元に要る費用は、処理対象の設定全体に比例する。Native が保持する prepared plan は上記の metadata に限定するため、外部応答のページ化と、候補作成そのものの費用削減を同じものとして扱わない。

1. 全対象の現在値を複製した候補へ変更を適用する。
2. 初回追加だけ候補の `Reset` を実行し、明示された設定を適用する。既存値の編集や Undo で Reset しない。
3. 候補の `OnValidate` を一度実行する。既存 callback 契約どおり Engine API は使えず、設定の補正・相互検証だけを行う。生個体を変更しない。
4. 補正後の全設定を schema で検証する。範囲、有限値、参照型、階層と循環の有無を同じ validator で確認する。配列の総件数を表示・転送用の上限で拒否しない。
5. 全対象が成功した場合だけ、現在値と revision を再照合して同一メインスレッドで確定する。callback は確定中に再実行しない。

例外・検証失敗・競合があれば、設定、Undo、dirty 状態を変更しない。C# の static 変更や外部 I/O は候補複製では巻き戻せないため、Reset / OnValidate で禁止する契約と診断を用意する。callback の失敗は対象・フィールド・元の C# 位置を含む診断として返す。

Undo / Redo は補正前の入力でなく、確定した変更前後の snapshot を保存する。補正によって入力外のフィールドが変わった場合も同じ Undo に含める。Undo / Redo で OnValidate を再実行しない。複数対象の編集は全件成功か全件無変更とする。

Undo / Redo が競合した場合は履歴位置も進めず、対象と理由を返す。検査できない旧式の複合コマンドへ transaction を包まない。適用要求の再送判定用 receipt は軽量な記録とし、破棄済み Undo の値グラフを保持しない。未確定 preview は明示破棄でき、容量不足を解消できる経路を持つ。

Script 追加の Undo は通常の後始末を行って個体を解放する。Redo は新しい実行時個体を通常の constructor で作り、保存した補正済み設定を復元する。Reset / OnValidate は再実行しない。破棄した個体を保持して再利用せず、private な実行状態や旧寿命の参照を復活させない。

既存個体の検証候補は constructor を実行せず、設定だけを独立した値へ複製する。OnValidate は設定と定数、純粋な計算にだけ依存し、設定でない instance field の初期化値や実行時 cache を使わない。直接の禁止操作を analyzer で診断し、任意の .NET コードを sandbox 化できるとは扱わない。

## Edit と Play

Play は Edit Scene とその ProjectRuntime・Physics World を保持したまま、別 Scene と別 ProjectRuntime の clone を実行する。通常の Stop では Edit を deserialize し直さず、元の GameObject/Component/Script 個体と Undo 履歴を再び有効にする。Edit 側の event 配信は parked flag で停止し、active/enabled 設定や購読自体は変更しない。clone 取得は Edit の OnBeforeSerialize と保存 cache の更新を行わず、必要な保存 callback は detached clone だけで実行・検証する。開始の失敗は元の編集状態を維持する。

この方式は Play 中に Scene/runtime の追加メモリを使う。native DLL の旧 vtable を参照する Edit 個体が保持されるため、native DLL の再読込は Stop 後まで延期する。managed Edit generation は lease と元個体の所有によって保持し、Play の新 generation と候補 generation を別に扱う。Stop は元 generation の metadata/catalog を戻し、新しいコードを Edit 個体へ自動的に適用しない。最新コードへの移行は別の検証済み操作として示し、不一致の managed Undo を黙って消去したり履歴位置だけ進めたりしない。

最初の C# Play も保持した元 assembly を別の collectible AssemblyLoadContext へロードし、game の static 状態を Edit generation と共有しない。取得順は passive baseline → Edit generation retain → fresh Play generation select → clone 構築・検証とし、失敗時は候補個体を破棄してから元 generation を再選択する。native DLL の static 状態、SDK 内の host サービス、任意の外部 I/O は process 単位のままであり、プロセス全体の domain 隔離を保証するものではない。

- Edit の確定は Scene / Prefab または DataAsset を dirty にし、通常の保存と Undo へ入る。
- Play / Pause の設定編集は実行中個体にだけ適用する。Scene の保存用 snapshot と Edit の Undo 履歴を変更しない。Play 用の履歴は Stop で破棄する。
- C# Script のヘッダーには属性・追加フィールドなしで共通の有効・無効チェックを表示する。`script.set_enabled` は exact 個体だけを変更し、Edit では保存と Undo、Play / Pause では一時履歴へ記録する。通知は次 Tick の通常 lifecycle から行い、fault / 必須設定による停止をチェックだけで解除しない。有効状態は Stop 後の採用対象に含めない。
- Inspector の通常表示は設定を中心にし、スクリプト固有の操作と Play 設定の保持を折り畳む。保持の選択チェックは保持欄を開いたときだけ表示する。可視ページは毎フレーム現在値を読み、手動再取得のボタンや説明を常設しない。未適用下書きと停止診断は保持する。
- 有効チェックは単体・複数選択で `script.enabled_status` の混在状態・切り替え可否・理由を共有する。混在状態のクリックは全対象を有効にし、`script.set_enabled` の対象一覧は全件検査後に一括公開する。Undo は変更した個体だけを一回の履歴で復元し、競合時は全件を未変更に保つ。操作不可のチェックは無効表示にし、理由をホバーで示す。
- 画面外の設定は実測高さを使ってレイアウトだけを進め、値の取得を省く。幅・文字サイズ・保持欄の変更で高さを再測定する。編集中の項目、フォーカス移動先、開いたピッカーは描画を保ち、非表示の下書きや参照を破棄しない。長い診断と旧下書きの JSON は詳細を開くまで描画・生成しない。
- 一時設定用 UndoStack と未確定 preview は Play clone の破棄より先に解放する。Edit の dirty 正本は保持した `editScene` のままで、一時設定の Publish / Undo はその dirty を変更しない。編集対応範囲は既存 managed Script の Setting と共通の有効状態とし、native Inspector 編集・汎用 mutation / legacy transaction を一時履歴へ混ぜない。
- 「編集時の値へ採用」は変更 path と対象を表示する明示操作とする。Play 開始時の保存対象へ適用する候補を保持し、Stop 復元後に再検証して Edit の一つの Undo として確定する。
- Play 中に生成した個体、開始時に存在しない Script、失効した参照、schema 不一致は採用対象にできない。採用失敗でも Stop 自体を妨げない。
- Play 中の C# reload は現在の一時設定と保持状態を候補世代へ移す。採用要求は安定した保存 ID と型キーで再照合し、曖昧なら診断する。

## DataAsset と複数選択

DataAsset の型と既定値は managed catalog から取得する。ファイル編集も同じ schema、候補検証、Undo を使い、書込は一時ファイルから確定する。型名だけで不明な native factory を呼ばない。参照は GUID と宣言型で検証する。

既存 DataAsset の読み込みと型探索では constructor を実行しない。新規作成を明示した場合に constructor と Reset を一度実行し、補正後の全設定を保存する。schema の合成既定値は任意の field initializer を再現するものではない。型変更で保存キーが不足した場合は合成既定値と検証結果を移行候補として表示する。Scene とファイルの公開先を混在させた transaction は受理しない。

ファイル一件の atomic replace と複数ファイルの atomic commit を区別する。復旧用 journal を持たない間は、一度の transaction で変更する DataAsset を一件に制限する。

複数選択は共通の型・設定キーを表示する。異なる値は mixed と表示し、操作した path だけを各対象へ適用する。選択した複数個体の既存の異なる設定や参照を、代表一個体の値で埋めない。

`script.configure.preview` は `domain: "edit" | "play"` と明示した `targets` を受ける。各対象は `ownerGuid`、`savedScriptId`、必要なら transient `scriptId`、変更する `fields` / RFC 6901 `patches`、`expectedRevision` を持つ。同型が同じ owner に複数ある場合も保存 ID で区別し、重複個体・世代変更・個体置換は公開前に拒否する。既存の `objects` と共通設定の指定はこの batch の簡略形として扱う。

Inspector の混在比較は可視 path/page の live 値だけを読む。全設定 revision の走査は編集開始時に各対象で一度行う。一つの primary edit session が batch の chunk upload を保持し、他の対象は exact revision だけを保持できるため、選択件数ぶんの開いた upload handle を必要としない。最終 prepare / commit は全対象を比較・検証し、いずれかの失敗時には一件も公開しない。代表の未編集 sibling 値は payload に含めない。

補正後の差分と Undo の競合判定も leaf path 単位とし、同じ構造内の無関係な編集を保持する。配列の件数変更・並べ替えは配列全体を一つの変更として扱い、要素の実体参照 token を同時に保存する。

## 受入検証

補正による別フィールド変更と Undo / Redo、候補 callback 失敗の全件無変更、競合拒否、同一 GUID を含むリスト移動、Play 一時編集と Stop 復元、明示採用と Undo、型を改名した DataAsset の保存・再読込、mixed 値の一括編集を確認する。UI と AI の同じ入力が同じ結果になることを検証する。
