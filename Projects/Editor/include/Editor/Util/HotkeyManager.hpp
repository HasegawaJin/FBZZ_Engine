/// @file    HotkeyManager.hpp
/// @brief   エディター全体のキーショートカットの単一の登録先。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note 各パネルが IsKeyPressed を直接叩くと再割り当ても F1 一覧生成もできない。
///       ホットキーは必ずここへ登録し、一覧は登録内容から生成する (手書きの二重管理をしない)。
#pragma once
#include <Editor/Util/HotkeyScope.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

/// HotkeyScope の定義は Editor/Util/HotkeyScope.hpp にある。
/// @note パネル (IPanel) や EditorContext もこの enum を名乗るため、登録簿ごと include させないよう分離した。

/// 一覧のグループ見出し。表示の並び順にもなる。
enum class HotkeyCategory {
    File,
    Edit,
    Selection,
    Viewport,
    Gizmo,
    Play,
    Panels,
    /// Operator の "Tools" / "Render" カテゴリ (Build Settings、デバッグ表示など)。
    /// @note 分類が無いと HotkeyCategoryFromOperator が Edit へ倒し、F1 一覧で Build Settings が Edit 節に紛れる。
    Tools,
};

struct Hotkey {
    std::string    name;

    /// 対応する Operator の id ("scene.new")。Operator 経由で登録されたものだけが持つ。
    /// @note リバインドの保存鍵はこの id を使う。表示名 (name) を鍵にすると、ラベル変更だけで
    ///       保存済みのリバインドが一致しなくなり黙って既定へ戻る。
    std::string    operatorId;

    int            imguiKey = 0;
    bool           ctrl     = false;
    bool           shift    = false;
    bool           alt      = false;
    HotkeyScope    scope    = HotkeyScope::Global;
    HotkeyCategory category = HotkeyCategory::Edit;

    std::function<void()> callback;

    /// 追加の有効条件 (選択がある / Play 中でない 等)。null なら常に有効。
    /// @note コールバック内で早期 return すると一覧では「効くはず」に見えて実際は無反応になる。
    ///       判定を外に出すことで UI 側の淡色表示にも使える。
    std::function<bool()> enabled;

    /// キー 1 つで表現できない操作 (マウスドラッグ・数字キー列など) の説明専用エントリ。
    /// 入力処理では一切参照せず、一覧にだけ出る。
    /// @note 別表に切り出すと結局そこが手書きの二重管理になるため、同じ器に入れて一覧を 1 本にする。
    bool        infoOnly = false;
    std::string infoBinding;   ///< infoOnly のときに表示する文字列 ("RMB drag" 等)
};

class HotkeyManager {
public:
    void Register(Hotkey hotkey);

    /// 説明専用エントリを追加する (入力処理の対象外)。
    void RegisterInfo(std::string name,
                      std::string binding,
                      HotkeyCategory category,
                      HotkeyScope scope = HotkeyScope::Global);

    /// 現在アクティブな scope を判定する述語。EditorApp が EditorContext を見て答える。
    /// 未設定のときは Global だけが有効。
    void SetScopeResolver(std::function<bool(HotkeyScope)> resolver)
    {
        m_scopeResolver = std::move(resolver);
    }

    /// このフレームだけ、その Operator に割り当てられた全体のホットキーを発火させない。ProcessInput のたびに消える。
    /// @note ドキュメント編集パネル (Fluid Editor 等) にフォーカスがある間の Ctrl+S で、全体の scene.save も
    ///       一緒に走ってシーンまで保存されるのを防ぐ。
    /// @note 呼ぶのは押された瞬間ではなく「フォーカスを持っている間毎フレーム」。ProcessInput は
    ///       EditorApp::BeginFrame でパネル描画より先に走るため、次フレームの ProcessInput に向けて先出しする。
    void SuppressOperatorThisFrame(std::string_view operatorId);

    void ProcessInput();  ///< 毎フレーム EditorApp から呼ぶ
    void Clear();

    const std::vector<Hotkey>& GetHotkeys() const { return m_hotkeys; }

    /// operator id に割り当てられているホットキーを返す (無ければ nullptr)。
    /// @note メニュー右側のショートカット表示をここから引く。固定文字列にすると、リバインド後も
    ///       メニュー表示だけ古いキーのまま嘘になる。
    [[nodiscard]] const Hotkey* FindByOperator(std::string_view operatorId) const;

    /// 既存ホットキーのキーバインドだけを変更する (コールバックは保持)。一致するものが無い、または
    /// 説明専用エントリの場合は何もしない。
    /// @note key は operatorId か表示名 (name) のどちらでもよい。保存鍵を表示名から operatorId へ
    ///       移行する途中で、旧設定ファイル (表示名で保存済み) も読めるようにしている。
    void Rebind(const std::string& key, int imguiKey, bool ctrl, bool shift, bool alt);

    /// 現在このホットキーが発火しうるか (scope と enabled の両方を満たすか)。
    /// 一覧の淡色表示に使う。
    [[nodiscard]] bool IsCurrentlyActive(const Hotkey& hk) const;

    /// 同じ scope で同じキー組み合わせに割り当てられた別のホットキーがあれば、その名前を返す。
    /// @note リバインド時にこれを確認しないと、既存の割り当てを黙って潰し「効かなくなった」と後で気づく。
    [[nodiscard]] std::string FindConflict(const std::string& name,
                                           int imguiKey, bool ctrl, bool shift, bool alt) const;

    /// 表示用に "Ctrl+Shift+K" 形式へ整形する (説明専用エントリは infoBinding をそのまま返す)。
    [[nodiscard]] static std::string FormatBinding(const Hotkey& hk);
    [[nodiscard]] static const char* CategoryLabel(HotkeyCategory category);

private:
    [[nodiscard]] bool ScopeActive(HotkeyScope scope) const;

    std::vector<Hotkey>              m_hotkeys;
    std::function<bool(HotkeyScope)> m_scopeResolver;

    /// 前回の ProcessInput 以降に申告された operator id (重複は積まない)。
    std::vector<std::string>         m_suppressedOperators;
};

} // namespace fbzz::editor
