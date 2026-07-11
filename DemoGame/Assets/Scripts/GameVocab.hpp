// DemoGame
// GameVocab.hpp | sandbox
// ゲーム共通語彙 (アニメ状態名 / パラメータ名 / タグ)。
//
// 設計意図 (WHY):
//   魔法文字列がスクリプトに散在するとタイプミスが黙って「常に false」になる。
//   ここに 1 箇所集約することで、状態追加は 1 箇所修正で済み、誤字は constexpr で
//   コンパイルエラー化する。補完も効く。
//
// 現在のキャラクター (MyPlayer) は 4 クリップ構成:
//   Idle / Walk / Run  … Locomotion ブレンドツリー (Speed 駆動)
//   Idle_Combat        … 戦闘構え (右クリック保持で遷移)
// 攻撃・被弾・死亡などのモーションは未実装。追加時はこの語彙から拡張する。
#pragma once

namespace sandbox {

namespace Tags {
    inline constexpr const char* Player = "Player";
}

namespace AnimState {
    // Idle/Walk/Run を Speed でブレンドする既定ステート
    inline constexpr const char* Locomotion  = "Locomotion";
    // 戦闘構え。構え中はカメラ前方を向いてストレイフ移動する
    inline constexpr const char* IdleCombat  = "Idle_Combat";
}

namespace AnimParam {
    // 水平移動速度 (m/s)。Locomotion ブレンドツリーの入力
    inline constexpr const char* Speed  = "Speed";
    // 戦闘構えフラグ。true で Idle_Combat へ遷移
    inline constexpr const char* Combat = "Combat";
}

} // namespace sandbox
