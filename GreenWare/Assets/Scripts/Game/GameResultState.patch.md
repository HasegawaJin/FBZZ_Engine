# GameResultState.hpp への追加（差分）

失敗画面がボスの進捗を出すので、Main 側から 2 つだけ渡してもらう必要があります。
それと、クリアタイムの採点基準がボス戦だけの構成に合っていないので引き直します。

```diff
     /// ノーリトライで通したか。リトライしたプレイにはランクを出さない。
     static inline bool noRetry = true;
+
+    /// ボスの残り HP（0〜1）。負けたときに «あと何割だったか» を出すためだけに持つ。
+    /// 勝ったときは 0 になるので、リザルトは victory を見て使い分ける。
+    static inline float bossHpRemain01 = 1.0f;
+    /// 倒れたときのボスのフェーズ（1 or 2）。HP50% でフェーズが変わる（Docs/game-flow.md）。
+    static inline int   bossPhase = 1;
 
     /// 自己ベスト。同じセッションのあいだだけ持つ (保存は GameSettings の担当)。
     static inline float bestSeconds = 0.0f;
```

```diff
-        if (clearSeconds <= 390.0f)      score += 3;   // 6 分 30 秒
-        else if (clearSeconds <= 480.0f) score += 2;   // 8 分
-        else if (clearSeconds <= 600.0f) score += 1;   // 10 分
+        // WHY 基準を短くしたか: Wave 制を廃してボス戦だけの構成になり、1 周が
+        //     7〜9 分から 2〜3 分へ縮んだ。旧基準のままでは全員が 3 点になり、
+        //     タイムが評価軸として働かない。実測が出たら詰め直す。
+        if (clearSeconds <= 180.0f)      score += 3;   // 3 分
+        else if (clearSeconds <= 240.0f) score += 2;   // 4 分
+        else if (clearSeconds <= 300.0f) score += 1;   // 5 分
```

`GameFlowComponent` が敗北を確定させる場所で、次の 2 行を足してください。

```cpp
GameResultState::bossHpRemain01 = boss ? boss->HpRatio() : 0.0f;
GameResultState::bossPhase      = (GameResultState::bossHpRemain01 > 0.5f) ? 1 : 2;
```
