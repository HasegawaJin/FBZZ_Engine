# Script 配送の作業領域と地形照会

Script の公開契約は [C# scripting](csharp-scripting.md) を正本とする。更新頻度、順序、個体数、callback の権限、owner / Scene / Script birth の検証を高速化のために緩めない。

## 配送ごとの snapshot

Script / FixedScript / LateScript は、それぞれの呼出し開始時に EntityID と二つの Script 識別子を値として写し、execution order の安定順を作る。Scene の構成や enabled が callback で変わるため、対象一覧をフェーズ間で共有しない。callback を越えて ScriptEntry や Component 配列の参照を保持しない。

各 snapshot は三つの vector を所有する。thread-local な待機領域から空の vector を交換して受け取り、終了時に要素を消して容量だけを戻せる。入れ子の配送では、外側が vector を所有したままなので内側は独立した vector を使う。返却時は各 vector の大きい容量を一組だけ保持する。Scene、GameObject、Script の参照を待機領域へ残さない。

Update の遅延処理、Invoke、Coroutine、OnUpdate は実行した処理の後で対象を引き直す。一度ゲートが対象を拒否した後は、その対象の順番の中で次の callback が実行されないため、同じゲートを繰り返さず次の対象へ進める。後続対象が再有効化した対象は、従来どおり次回の順番から参加する。

## Projected-surface conservative rejection

追加地形の準備済み投影を、元の index 順の連続16三角形で区切る。各ブロックの bounds は同じ geometry byte key と world TRS bit key の準備結果に属する。heightfield の契約通知で入力が変更される可能性があるため、通知後の完全な再照合を維持する。大きな地形、再入、準備不能時の scalar fallback も維持する。

投影の列ベクトルを a=(ax,az), b=(bx,bz)、全係数の最大絶対値を m、既存演算で得る determinant を d とする。既存の有限 float 座標から double で作る差・積・除算は double の overflow / underflow 域へ届かない。採用される barycentric u,v は既存の epsilon 条件から絶対値が2未満である。

|d| >= 1e-6*m*m の投影だけを bounds の対象とする。double 演算誤差を保守的に gamma=2^-46 としても、真の determinant との差は 2*gamma*m*m 以下、各分子の差は 2*gamma*m*P 以下（P は投影原点からの差の最大絶対値）。2x2 の逆変換から P <= 4*m*(1+gamma)/(1-2*gamma*1e6) + 4*gamma*1e6*P/(1-2*gamma*1e6) を得る。したがって、受理され得る差は4.00001*m未満である。半径8*mの正方形を用い、world bounds の和差を nextafter で外側へ広げる。epsilon 接触と浮動小数点評価誤差の両方に余裕を残す。

一つでもこの determinant 条件を満たさない有効投影があれば、ブロック全体を除外不可とする。既存判定で usable でない三角形は従来も接触を返さない。空のブロックは除外できる。

bounds 内のブロックでは、元の三角形順、二つの除算、height の演算順、法線変換、同じ高さで先の三角形を優先する規則をそのまま使う。近似した接触値や過去の照会結果は返さない。通過できないブロックだけを省くため、失敗出力の保持や数学契約 callback も変えない。

境界 epsilon、負の determinant、非一様 / 負の scale、準備済み容量外の scalar oracle、geometry の直接編集、契約 callback からの入力変更と再入を回帰試験する。Standalone の通常区間で変更ごとに前後を比較し、FPS 計測と診断・画像・姿勢照合の区間を分ける。
