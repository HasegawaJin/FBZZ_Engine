// FBZZ Engine
// HotkeyManager.hpp | fbzz::editor
// エディター全体のキーショートカットの単一の登録先
//
// WHY: 以前はここに登録されていたのが 9 個だけで、実際に一番使う操作
//      (W/E/R のギズモ切替・F フォーカス・Delete・Ctrl+D 等) は各パネルの中で
//      IsKeyPressed を直接叩いていた。その結果
//        - HotkeyEditorPanel から再割り当てできるのが 9 個だけ
//        - F1 のショートカット一覧が手書きの固定文字列になり、実装と二重管理
//      という状態になり、実際に一覧の側が実装から取り残されていた。
//
//      「キーを足したら一覧にも書き足す」という運用は必ず破れるので、
//      登録先を 1 箇所にして一覧を登録内容から生成する。
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

// ホットキーが効く文脈。ビットマスクなので複数指定できる
// (例: Delete は Scene View と Hierarchy の両方で効く)。
//
// WHY: 同じキーがパネルごとに違う意味を持つ以上、「どこにフォーカスがあるか」を
//      条件に含めないと 1 箇所へ集約できない。Scene View の W (ギズモ) と
//      カメラ移動の W が衝突していたのも、この概念が無かったため。
enum class HotkeyScope : std::uint32_t {
    None          = 0,
    Global        = 1u << 0,   // フォーカス位置によらず有効
    SceneViewport = 1u << 1,
    Hierarchy     = 1u << 2,
    AssetBrowser  = 1u << 3,
};

constexpr HotkeyScope operator|(HotkeyScope a, HotkeyScope b)
{
    return static_cast<HotkeyScope>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool HasScope(HotkeyScope set, HotkeyScope test)
{
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(test)) != 0;
}

// 一覧のグループ見出し。表示の並び順にもなる。
enum class HotkeyCategory {
    File,
    Edit,
    Selection,
    Viewport,
    Gizmo,
    Play,
    Panels,
    // Operator の "Tools" / "Render" カテゴリ (Build Settings、デバッグ表示など)。
    // WHY 足したか: 対応する分類が無いと HotkeyCategoryFromOperator が Edit へ
    //     倒すため、F1 の一覧で Build Settings が Edit 節に並ぶ。
    Tools,
};

struct Hotkey {
    std::string    name;

    // 対応する Operator の id ("scene.new")。Operator 経由で登録されたものだけが持つ。
    // WHY: リバインドの保存鍵をこちらへ移すため。従来は表示名 (name) を鍵にしていたので、
    //      ラベルを "Open Scene" から "Open Scene..." のように変えるだけで、
    //      保存済みのリバインドが誰にも一致しなくなり黙って既定へ戻っていた。
    //      id は表示名と独立して安定する。
    std::string    operatorId;

    int            imguiKey = 0;
    bool           ctrl     = false;
    bool           shift    = false;
    bool           alt      = false;
    HotkeyScope    scope    = HotkeyScope::Global;
    HotkeyCategory category = HotkeyCategory::Edit;

    std::function<void()> callback;

    // 追加の有効条件 (選択がある / Play 中でない 等)。null なら常に有効。
    // WHY: 条件をコールバックの中で早期 return すると、一覧では「効くはず」に見えるのに
    //      押しても何も起きない。判定を外へ出しておけば、UI 側で淡色表示にもできる。
    std::function<bool()> enabled;

    // キー 1 つで表現できない操作 (マウスドラッグ・数字キー列など) の説明専用エントリ。
    // 入力処理では一切参照せず、一覧にだけ出る。
    // WHY: これらも「操作の一覧」としてはユーザーが知りたい情報なのに、
    //      ホットキーとして登録できないという理由だけで別表に切り出すと、
    //      結局そこがまた手書きの二重管理になる。同じ器に入れて一覧を 1 本にする。
    bool        infoOnly = false;
    std::string infoBinding;   // infoOnly のときに表示する文字列 ("RMB drag" 等)
};

class HotkeyManager {
public:
    void Register(Hotkey hotkey);

    // 説明専用エントリを追加する (入力処理の対象外)。
    void RegisterInfo(std::string name,
                      std::string binding,
                      HotkeyCategory category,
                      HotkeyScope scope = HotkeyScope::Global);

    // 現在アクティブな scope を判定する述語。EditorApp が EditorContext を見て答える。
    // 未設定のときは Global だけが有効。
    void SetScopeResolver(std::function<bool(HotkeyScope)> resolver)
    {
        m_scopeResolver = std::move(resolver);
    }

    void ProcessInput();  // 毎フレーム EditorApp から呼ぶ
    void Clear();

    const std::vector<Hotkey>& GetHotkeys() const { return m_hotkeys; }

    // operator id に割り当てられているホットキーを返す (無ければ nullptr)。
    // WHY: メニュー右側のショートカット表示を実際の割り当てから引くため。
    //      以前は "Ctrl+S" のような固定文字列だったので、リバインドすると
    //      メニューの表示だけが古いキーのまま嘘になっていた。
    [[nodiscard]] const Hotkey* FindByOperator(std::string_view operatorId) const;

    // 既存ホットキーのキーバインドだけを変更する (コールバックは保持)。
    // key には operatorId か表示名 (name) のどちらを渡してもよい。
    // WHY: 保存済み設定の鍵を表示名から operatorId へ移行する途中で、
    //      古い設定ファイル (表示名で保存されている) も読めるようにしておく。
    // 一致するものが無い、または説明専用エントリの場合は何もしない。
    void Rebind(const std::string& key, int imguiKey, bool ctrl, bool shift, bool alt);

    // 現在このホットキーが発火しうるか (scope と enabled の両方を満たすか)。
    // 一覧の淡色表示に使う。
    [[nodiscard]] bool IsCurrentlyActive(const Hotkey& hk) const;

    // 同じ scope で同じキー組み合わせに割り当てられた別のホットキーがあれば、その名前を返す。
    // WHY: リバインドで既存の割り当てを黙って潰すと、後から「効かなくなった」と気づく。
    [[nodiscard]] std::string FindConflict(const std::string& name,
                                           int imguiKey, bool ctrl, bool shift, bool alt) const;

    // 表示用に "Ctrl+Shift+K" 形式へ整形する (説明専用エントリは infoBinding をそのまま返す)。
    [[nodiscard]] static std::string FormatBinding(const Hotkey& hk);
    [[nodiscard]] static const char* CategoryLabel(HotkeyCategory category);

private:
    [[nodiscard]] bool ScopeActive(HotkeyScope scope) const;

    std::vector<Hotkey>              m_hotkeys;
    std::function<bool(HotkeyScope)> m_scopeResolver;
};

} // namespace fbzz::editor
