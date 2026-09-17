/// @file    StageProgressState.hpp
/// @brief   ステージ選択が読む、ステージごとの解放状態と自己ベスト。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// @note GameResultState は «直前の 1 周» を運ぶ器でリザルトを出したら役目を終える。
///       寿命が違う «これまで» の記録を同じ器に入れると、リトライで直前の周を
///       消したとき記録まで消える。解放と自己ベストは «周回の結果» なので、
///       config でなく枠と一緒に動く save 側へ書く (Docs/design/game-settings.md)。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptProxy/ScriptSaveProxy.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/UI/StageCatalog.hpp>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace sandbox {

struct StageRecord {
    bool  cleared     = false;
    bool  unlocked    = false;
    int   bestScore   = 0;      ///< 0〜9。ランクはここから引く
    float bestSeconds = 0.0f;
    int   bestChain   = 0;
    /// 最も少なく済ませた被ダメージ。負は «まだ記録が無い»。
    int   leastDamage = -1;
    int scoreVersion = 0;
    int bestTechnique = 0;

    [[nodiscard]] bool HasCurrentScore() const
    { return cleared && scoreVersion == GameResultState::kScoreVersion; }
};

/// ディスクへ落とす形。項目ごとに «ステージ数ぶんの並び» を 1 本持つ。
///
/// @note SaveStore が扱えるのは値と値の配列までで、行を入れ子の表にすると保存できる
///       型から外れる。列で持てば TOML が `cleared = [ true, false, … ]` の並びになり、
///       目で読めるまま往復する。
struct StageProgressSave : fbzz::scene::IScriptSerializable {
    std::vector<bool>  cleared;
    std::vector<bool>  unlocked;
    std::vector<int>   bestScore;
    std::vector<float> bestSeconds;
    std::vector<int>   bestChain;
    std::vector<int>   leastDamage;
    std::vector<int> scoreVersion;
    std::vector<int> bestTechnique;

    void Reflect(fbzz::scene::IReflector& r) override
    {
        r.ListField("cleared",     cleared);
        r.ListField("unlocked",    unlocked);
        r.ListField("bestScore",   bestScore);
        r.ListField("bestSeconds", bestSeconds);
        r.ListField("bestChain",   bestChain);
        r.ListField("leastDamage", leastDamage);
        r.ListField("scoreVersion", scoreVersion);
        r.ListField("bestTechnique", bestTechnique);
    }
};

struct StageProgressState {
    /// ステージ数。増やすときはここと `kStageCount`、シーンの Row の数を合わせる。
    static constexpr int kCount = kStageCount;
    static inline StageRecord stages[kCount] = {};
    static inline int cursor = 0;      ///< 選択画面を出し直したとき、同じ行に戻す

    /// 保存に使う TOML のキー。
    static constexpr const char* kSaveKey = "stages";

    /// 進行データの置き場。設定と同じフォルダに progress.toml を並べる。
    ///
    /// @note エディタと配布ビルドは別 exe なので、相対パスだと «エディタで解放した面が
    ///       製品版では閉じている» になる。設定と同じ per-user フォルダへ寄せる。
    [[nodiscard]] static std::string ResolvePath()
    {
        bool        perUser = true;
        std::string folder  = "GreenWare";
        if (auto* s = GameSettingsComponent::Instance()) {
            perUser = s->perUserConfig;
            folder  = s->configFolder;
        }
        const std::string cfg   = GameSettingsComponent::ResolveConfigPath(perUser, folder);
        const std::size_t slash = cfg.find_last_of('/');
        if (slash == std::string::npos) return "Config/progress.toml";
        return cfg.substr(0, slash + 1) + "progress.toml";
    }

    /// ディスクから読み直し、STAGE 01 を開ける。画面はこちらを呼ぶ。
    ///
    /// @note 読むのは起動して最初に触ったときの 1 回だけ。シーンを移るたびに読み直すと、
    ///       まだ書いていない «今回の記録» がディスクの古い値で上書きされる。
    static void EnsureInit(const fbzz::scene::ScriptSaveProxy& save)
    {
        if (!s_loaded) {
            s_loaded = true;
            Load(save);
        }
        stages[0].unlocked = true;

        /// @note 解放は保存値をそのまま信じず «前をクリアした» と «実体がある» から毎回
        ///       引き直す。Commit は «クリアした瞬間» にしか次の枠を開けないので、後から
        ///       ステージを足すと既存の progress.toml は `unlocked = false` のまま残る。
        for (int i = 0; i + 1 < kCount; ++i)
            if (stages[i].cleared && StageExists(i + 1)) stages[i + 1].unlocked = true;
    }

    /// 保存先を今の設定へ合わせる。既に合っていれば何もしない。
    ///
    /// @note SetPath はテーブルごと差し替えるので、書く直前に毎回呼ぶと同じ枠の他の
    ///       キーが道連れで消える。合わせるのは 1 度だけにする。
    static void Bind(const fbzz::scene::ScriptSaveProxy& save)
    {
        const std::string path = ResolvePath();
        if (save.GetPath() == path) return;
        save.SetPath(path);
        /// @note 初回起動 / 破損なら false。既定値のまま進む
        save.Load();
    }

    static void Load(const fbzz::scene::ScriptSaveProxy& save)
    {
        Bind(save);

        StageProgressSave data;
        if (!save.Read(kSaveKey, data)) return;

        /// @note 保存した後でステージを増減させても壊れないよう、短い方に合わせる。
        for (int i = 0; i < kCount; ++i) {
            StageRecord& r = stages[i];
            if (i < static_cast<int>(data.cleared.size()))     r.cleared     = data.cleared[i];
            if (i < static_cast<int>(data.unlocked.size()))    r.unlocked    = data.unlocked[i];
            if (i < static_cast<int>(data.bestScore.size()))   r.bestScore   = data.bestScore[i];
            if (i < static_cast<int>(data.bestSeconds.size())) r.bestSeconds = data.bestSeconds[i];
            if (i < static_cast<int>(data.bestChain.size()))   r.bestChain   = data.bestChain[i];
            if (i < static_cast<int>(data.leastDamage.size())) r.leastDamage = data.leastDamage[i];
            if (i < static_cast<int>(data.scoreVersion.size())) r.scoreVersion = data.scoreVersion[i];
            if (i < static_cast<int>(data.bestTechnique.size())) r.bestTechnique = data.bestTechnique[i];
        }
    }

    /// 今の内容をディスクへ。書けたら true。
    static bool Save(const fbzz::scene::ScriptSaveProxy& save)
    {
        StageProgressSave data;
        data.cleared.reserve(kCount);
        data.unlocked.reserve(kCount);
        data.bestScore.reserve(kCount);
        data.bestSeconds.reserve(kCount);
        data.bestChain.reserve(kCount);
        data.leastDamage.reserve(kCount);
        data.scoreVersion.reserve(kCount);
        data.bestTechnique.reserve(kCount);
        for (const StageRecord& r : stages) {
            data.cleared.push_back(r.cleared);
            data.unlocked.push_back(r.unlocked);
            data.bestScore.push_back(r.bestScore);
            data.bestSeconds.push_back(r.bestSeconds);
            data.bestChain.push_back(r.bestChain);
            data.leastDamage.push_back(r.leastDamage);
            data.scoreVersion.push_back(r.scoreVersion);
            data.bestTechnique.push_back(r.bestTechnique);
        }

        Bind(save);
        save.Write(kSaveKey, data);
        return save.Save();
    }

    [[nodiscard]] static const char* RankLabel(int score)
    {
        if (score >= 8) return "S";
        if (score >= 6) return "A";
        if (score >= 4) return "B";
        return "C";
    }

    /// リザルトが «勝ちで» 閉じるときに呼ぶ。次のステージを開ける。
    static void Commit(int index)
    {
        if (!GameResultState::victory || index < 0 || index >= kCount
            || index != GameResultState::stageIndex) return;
        StageRecord& r = stages[index];
        if (!r.HasCurrentScore()) {
            r.bestScore = 0;
            r.bestSeconds = 0.0f;
            r.bestChain = 0;
            r.leastDamage = -1;
            r.bestTechnique = 0;
            r.scoreVersion = GameResultState::kScoreVersion;
        }
        r.cleared  = true;
        r.unlocked = true;
        const int score = GameResultState::Score();
        r.bestScore = (std::max)(r.bestScore, score);
        if (r.bestSeconds <= 0.0f || GameResultState::clearSeconds < r.bestSeconds)
            r.bestSeconds = GameResultState::clearSeconds;
        r.bestChain = (std::max)(r.bestChain, GameResultState::bestChain);
        r.bestTechnique = (std::max)(r.bestTechnique, GameResultState::TechniquePoints());
        if (r.leastDamage < 0 || GameResultState::damageTaken < r.leastDamage)
            r.leastDamage = GameResultState::damageTaken;
        /// @note 実体のある枠だけ開ける。解放すると選択画面が «押せる行» として見せ、
        ///       押した先で読み込みに失敗するため。
        if (index + 1 < kCount && StageExists(index + 1)) stages[index + 1].unlocked = true;
    }

    /// 記録を更新してディスクへ落とすところまで。リザルトはこちらを呼ぶ。
    ///
    /// @note 保存を Commit と一体にする。分けると «更新したのに書き忘れた» が
    ///       «次に起動したら解放が消えていた» という形でしか気づけない。
    static bool Commit(int index, const fbzz::scene::ScriptSaveProxy& save)
    {
        Commit(index);
        return Save(save);
    }

private:
    /// ディスクから読んだか。DLL をリロードすると false へ戻るが、
    /// そのときは読み直すだけなので害はない。
    static inline bool s_loaded = false;
};

} // namespace sandbox
