// FBZZ Engine
// Blackboard.hpp | fbzz::ai
// エージェントが「知っていること」を型付きで保持する共有データ領域
//
// WHY 必要か:
//   BT のノード同士は直接値を渡し合わない (木の形を変えると配線が壊れるため)。
//   代わりに Blackboard を介して間接的にやり取りする。
//   知覚システムが書き、条件ノードが読み、アクションノードが使う、という
//   一方向の流れになるので、木の構造とデータの流れを独立に変更できる。
#pragma once
#include <Engine/AI/BehaviorTreeTypes.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::ai {

// 1 キーの定義。アセット側が持ち、ランタイムはこの配列の添字をキーとして使う。
struct BlackboardDef {
    std::string    name;
    BlackboardType type = BlackboardType::Bool;

    // 既定値。type に応じて 1 つだけ意味を持つ。
    // WHY variant を使わないか: TOML へ素直に書き出せる形にしたい。
    //     VFXGraphNode と同じ「平置き」方式で揃える。
    bool          defaultBool    = false;
    int           defaultInt     = 0;
    float         defaultFloat   = 0.0f;
    math::Vector3 defaultVector3 = math::Vector3::ZERO;
    std::string   defaultString;

    // PerceptionSystem / BehaviorTreeSystem が自動で書き込む予約キーか。
    // エディタは名前変更と削除を禁止する。
    bool reserved = false;
};

// 予約キーの固定添字。
//
// WHY 固定添字にするか:
//   PerceptionSystem はこれらを毎 tick 書く。名前検索を挟むとエージェント数 ×
//   キー数の文字列比較が毎フレーム走る。CompileBehaviorTree() が
//   EnsureReservedBlackboardKeys() で先頭へ強制的に並べるので、
//   添字は常にこの値になることが保証される。
namespace bb {
inline constexpr BlackboardKey Self              = 0;
inline constexpr BlackboardKey TargetEntity      = 1;
inline constexpr BlackboardKey TargetPosition    = 2;
inline constexpr BlackboardKey LastKnownPosition = 3;
inline constexpr BlackboardKey HasTarget         = 4;
inline constexpr BlackboardKey HomePosition      = 5;
inline constexpr BlackboardKey Health01          = 6;
inline constexpr BlackboardKey Awareness         = 7;
inline constexpr BlackboardKey NoisePosition     = 8;
inline constexpr std::uint16_t ReservedCount     = 9;
} // namespace bb

// 予約キーの定義列を返す (name / type / reserved が埋まったもの)。
[[nodiscard]] const std::vector<BlackboardDef>& ReservedBlackboardDefs();

class Blackboard {
public:
    // 定義列に合わせて領域を作り直し、全キーを既定値で初期化する。
    void Reset(const std::vector<BlackboardDef>& defs);

    // ── 書き込み ────────────────────────────────────────────────────────────
    // 型が一致しない場合は false を返し、値を書き換えない。
    // WHY assert しないか: 型不一致はアセットとスクリプトが別々に編集された結果の
    //     オーサリングミスであって、プログラムのバグではない。
    //     Docs/conventions/error_handling.md の「回復可能エラーは bool」に従う。
    bool SetBool   (BlackboardKey key, bool value);
    bool SetInt    (BlackboardKey key, int value);
    bool SetFloat  (BlackboardKey key, float value);
    bool SetVector3(BlackboardKey key, const math::Vector3& value);
    bool SetEntity (BlackboardKey key, scene::EntityID value);
    bool SetString (BlackboardKey key, std::string_view value);

    // ── 読み出し ────────────────────────────────────────────────────────────
    // 型不一致 / 範囲外なら false を返し、out を書き換えない。
    [[nodiscard]] bool GetBool   (BlackboardKey key, bool& out) const;
    [[nodiscard]] bool GetInt    (BlackboardKey key, int& out) const;
    [[nodiscard]] bool GetFloat  (BlackboardKey key, float& out) const;
    [[nodiscard]] bool GetVector3(BlackboardKey key, math::Vector3& out) const;
    [[nodiscard]] bool GetEntity (BlackboardKey key, scene::EntityID& out) const;
    [[nodiscard]] bool GetString (BlackboardKey key, std::string& out) const;

    // ── 書き込み履歴 ────────────────────────────────────────────────────────
    // 最後に書かれた tick 番号。一度も書かれていなければ 0。
    //
    // WHY 持つか: 「最後にプレイヤーを見てから 5 秒経ったら警戒を解く」という
    //     記憶の減衰は AI の頻出要件。各ノードが自前タイマーを持つと同じコードが
    //     散乱するので、Blackboard 側で書き込み時刻を持つ。
    [[nodiscard]] std::uint32_t GetLastWriteTick(BlackboardKey key) const;
    // 最後に書かれた時刻 [s]。一度も書かれていなければ 0。
    [[nodiscard]] float         GetLastWriteTime(BlackboardKey key) const;
    [[nodiscard]] bool          IsSet(BlackboardKey key) const;

    // 現在の tick 番号と経過時刻を設定する。書き込み履歴の基準になる。
    // WHY 時刻も持つか: 「5 秒以内に更新された値か」という条件は AI の頻出要件だが、
    //     tickRate が可変なので tick 数から秒を復元できない。System が実時刻を渡す。
    void SetTick(std::uint32_t tick) { m_tick = tick; }
    void SetTime(float seconds) { m_time = seconds; }
    [[nodiscard]] std::uint32_t GetTick() const { return m_tick; }
    [[nodiscard]] float         GetTime() const { return m_time; }

    [[nodiscard]] std::size_t Size() const { return m_entries.size(); }
    [[nodiscard]] bool IsValidKey(BlackboardKey key) const { return key < m_entries.size(); }
    [[nodiscard]] BlackboardType TypeOf(BlackboardKey key) const;

private:
    struct Entry {
        BlackboardType type = BlackboardType::Bool;
        bool           written = false;
        std::uint32_t  lastWriteTick = 0;
        float          lastWriteTime = 0.0f;

        bool            b = false;
        int             i = 0;
        float           f = 0.0f;
        math::Vector3   v = math::Vector3::ZERO;
        scene::EntityID e = scene::EntityID::INVALID;
        std::string     s;
    };

    // 型が一致すれば書き込み記録を更新して Entry を返す。不一致なら nullptr。
    Entry* Acquire(BlackboardKey key, BlackboardType expected);
    [[nodiscard]] const Entry* Peek(BlackboardKey key, BlackboardType expected) const;

    std::vector<Entry> m_entries;
    std::uint32_t      m_tick = 0;
    float              m_time = 0.0f;
};

} // namespace fbzz::ai
