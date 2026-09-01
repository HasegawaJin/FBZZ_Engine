# 銃時代の SE を消す (手作業)

企画が Wave 制の銃から双剣へ移ったとき、`SE_WPN_*` を鳴らす場所が
すべて無くなりました。2026-08-30 に `SeLibrary.hpp` からバンク定義と
ヘルパーを畳んだので、**下の 27 本はどこからも参照されていません**。

同名の `.meta` も一緒に消してください (`.meta` だけ残るとインポータが毎回
「元ファイルが無い」と言い続けます)。

場所: `Assets/Sound/SE/Weapon/`

## ビーム (照射) — 6 本
    SE_WPN_Beam_Start_Plus.wav      SE_WPN_Beam_Start_Minus.wav
    SE_WPN_Beam_Loop_Plus.wav       SE_WPN_Beam_Loop_Minus.wav
    SE_WPN_Beam_End_Plus.wav        SE_WPN_Beam_End_Minus.wav

## ペイント (極を塗る) — 10 本
    SE_WPN_Paint_Confirm_Plus_01.wav  … _02 _03 _04
    SE_WPN_Paint_Confirm_Minus_01.wav … _02 _03 _04
    SE_WPN_Paint_Extend_01.wav        SE_WPN_Paint_Extend_02.wav

## タップ (単発) — 4 本
    SE_WPN_Tap_Plus_01.wav   SE_WPN_Tap_Plus_02.wav
    SE_WPN_Tap_Minus_01.wav  SE_WPN_Tap_Minus_02.wav

## バッテリー — 4 本
    SE_WPN_Battery_Empty_R.wav  SE_WPN_Battery_Empty_L.wav
    SE_WPN_Battery_Full_R.wav   SE_WPN_Battery_Full_L.wav

## 旧・中和 / 警告 — 3 本
    SE_WPN_Neutralize_01.wav  SE_WPN_Neutralize_02.wav
    SE_WPN_Warn_Neutral.wav

中和は `SE_BLD_Neutralize_01/02.wav` に置き換わっています
(`se::kNeutralize` の名前はそのままなので、呼び出し側の変更は要りません)。

---

## 消せないもの (参照が生きている)

    Assets/Sound/SE/UI/SE_UI_WaveStart.wav
    Assets/Sound/SE/UI/SE_UI_WaveClear.wav

`WaveDirectorComponent` がまだこの 2 つを鳴らします。あのコンポーネントは
**どのシーンにも置かれていない** ので実際には鳴りませんが、コードが参照して
いる以上バンクを消すとビルドが通りません。

Wave 制を完全に畳むなら、順番はこうです:

1. `Assets/Scripts/Game/WaveDirectorComponent.hpp` を消す
2. `Assets/Scripts/Game/GameFlowComponent.hpp` の `WavesFinished()` を
   `return true;` にする (director が居ない前提そのもの)
3. `SeLibrary.hpp` の `kUiWaveStart` / `kUiWaveClear` を消す
4. 上の wav 2 本を消す

`ScriptList.inl` は ScriptCodeGen が自動更新するので手で触らないこと。
