/// @file    Blackboard.hpp
/// @brief   エージェントが「知っていること」を型付きで保持する共有データ領域。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note BT のノード同士は直接値を渡し合わない (木の形を変えると配線が壊れる)。Blackboard を介した間接的なやり取りにすることで、木の構造とデータの流れを独立に変更できる。
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

/// @brief 1 キーの定義。アセット側が持ち、ランタイムはこの配列の添字をキーとして使う。
struct BlackboardDef {
    std::string    name;
    BlackboardType type = BlackboardType::Bool;

    /// @note 既定値。type に応じて 1 つだけ意味を持つ。TOML へ素直に書き出すため variant は使わず、BTNodeDef と同じ平置き方式で揃える。
    bool          defaultBool    = false;
    int           defaultInt     = 0;
    float         defaultFloat   = 0.0f;
    math::Vector3 defaultVector3 = math::Vector3::ZERO;
    std::string   defaultString;

    bool reserved = false;   ///< PerceptionSystem / BehaviorTreeSystem が自動で書き込む予約キーか。エディタは名前変更と削除を禁止する。
};

/// @brief 予約キーの固定添字。
/// @note PerceptionSystem がこれらを毎 tick 書くため、名前検索を挟むとエージェント数×キー数の文字列比較が毎フレーム走る。CompileBehaviorTree() が EnsureReservedBlackboardKeys() で先頭へ強制的に並べ、添字を固定する。
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

/// @brief 予約キーの定義列を返す (name / type / reserved が埋まったもの)。
[[nodiscard]] const std::vector<BlackboardDef>& ReservedBlackboardDefs();

class Blackboard {
public:
    /// @brief 定義列に合わせて領域を作り直し、全キーを既定値で初期化する。
    void Reset(const std::vector<BlackboardDef>& defs);

    /// @name 書き込み
    /// @note 型が一致しない場合は false を返し値を書き換えない。型不一致はアセットとスクリプトが別々に編集された結果のオーサリングミスでありプログラムのバグではないため assert しない (`Docs/conventions/error_handling.md`)。
    /// @{
    bool SetBool   (BlackboardKey key, bool value);
    bool SetInt    (BlackboardKey key, int value);
    bool SetFloat  (BlackboardKey key, float value);
    bool SetVector3(BlackboardKey key, const math::Vector3& value);
    bool SetEntity (BlackboardKey key, scene::EntityID value);
    bool SetString (BlackboardKey key, std::string_view value);
    /// @}

    /// @name 読み出し
    /// @note 型不一致 / 範囲外なら false を返し、out を書き換えない。
    /// @{
    [[nodiscard]] bool GetBool   (BlackboardKey key, bool& out) const;
    [[nodiscard]] bool GetInt    (BlackboardKey key, int& out) const;
    [[nodiscard]] bool GetFloat  (BlackboardKey key, float& out) const;
    [[nodiscard]] bool GetVector3(BlackboardKey key, math::Vector3& out) const;
    [[nodiscard]] bool GetEntity (BlackboardKey key, scene::EntityID& out) const;
    [[nodiscard]] bool GetString (BlackboardKey key, std::string& out) const;
    /// @}

    /// @name 書き込み履歴
    /// @{
    /// @brief 最後に書かれた tick 番号。一度も書かれていなければ 0。
    /// @note 「最後にプレイヤーを見てから 5 秒経ったら警戒を解く」記憶の減衰は AI の頻出要件。各ノードが自前タイマーを持つ重複を避け Blackboard 側で書き込み時刻を持つ。
    [[nodiscard]] std::uint32_t GetLastWriteTick(BlackboardKey key) const;
    /// @brief 最後に書かれた時刻 [s]。一度も書かれていなければ 0。
    [[nodiscard]] float         GetLastWriteTime(BlackboardKey key) const;
    [[nodiscard]] bool          IsSet(BlackboardKey key) const;
    /// @}

    /// @brief 現在の tick 番号と経過時刻を設定する。書き込み履歴の基準になる。
    /// @note 「5 秒以内に更新された値か」は tickRate が可変で tick 数から秒を復元できないため、時刻も System から渡す。
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

    /// @brief 型が一致すれば書き込み記録を更新して Entry を返す。
    /// @return 不一致なら nullptr。
    Entry* Acquire(BlackboardKey key, BlackboardType expected);
    [[nodiscard]] const Entry* Peek(BlackboardKey key, BlackboardType expected) const;

    std::vector<Entry> m_entries;
    std::uint32_t      m_tick = 0;
    float              m_time = 0.0f;
};

} // namespace fbzz::ai
