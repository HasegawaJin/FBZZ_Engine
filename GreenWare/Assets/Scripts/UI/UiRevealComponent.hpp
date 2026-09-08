/// @file    UiRevealComponent.hpp
/// @brief   画面に入ったとき、名前で指した UI 要素を段差付きで «置いていく»
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// 使い方:
///   Targets に要素の名前を «出したい順» に並べる。各要素は From (px) だけ
///   ずれた位置・α 0 から始まり、Stagger 秒ずつ遅れて Duration 秒で本来の
///   位置と色へ収まる。Children を付けると、指した要素の直下の子も一緒に動く
///   (行 = Bar + Label のような組を 1 名で指せる)。
///
/// WHY 画面ごとのスクリプトに書かないか:
///   題字・罫線・バージョン表記・法則の一文は、どの画面でも «置いてあるだけ»
///   で誰も触らない。その出現を各画面の Component に書くと、同じ 30 行が
///   4 つの画面に写され、間隔を直すたびに 4 か所を開く。«誰も触らない要素の
///   出現» だけをここへ切り出し、触る要素 (メニュー行・タブ) は各画面が持つ。
///
/// WHY 終わったら書くのをやめるか:
///   出現が済んだあとも毎フレーム色を書き続けると、あとから別のスクリプトが
///   同じ要素の色を変えたときに、どちらが勝つかがスクリプトの並び順で決まる。
///   置き切ったら手を離す ─ その後は元の色のまま、他が触れる。
///
/// WHY 触ってはいけない要素があるか (使う側の約束):
///   毎フレーム色を書く要素 (メニューの Label、タブの文字) をここに入れると、
///   出現の最中に 2 つのスクリプトが交互に色を書いて、ちらつく。そういう要素は
///   持ち主のスクリプトが自分の出現を持つ (TitleMenuComponent の Intro など)。
#pragma once

#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class UiRevealComponent : public Script {
    FBZZ_SCRIPT(UiRevealComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_LIST_FIELD(std::string, targets, "対象")
    FBZZ_TOOLTIP("出したい順に要素の名前を並べる。見つからない名前は飛ばす")
    FBZZ_FIELD(bool, includeChildren, true, "Children")
    FBZZ_TOOLTIP("指した要素の直下の子も一緒に動かす (Bar + Label の行など)")

    FBZZ_GROUP("動き")
    FBZZ_FIELD(float, fromX, -28.0f, "From X")
    FBZZ_TOOLTIP("出る前の位置のずれ [px]。負で左から滑り込む")
    FBZZ_FIELD(float, fromY, 0.0f, "From Y")
    FBZZ_FIELD_RANGE(float, delay, 0.12f, "遅延", 0.0f, 3.0f)
    FBZZ_TOOLTIP("画面に入ってから 1 つ目が動き始めるまでの秒数")
    FBZZ_FIELD_RANGE(float, stagger, 0.06f, "のけぞり", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 つごとにずらす秒数。0 で全部が同時に出る")
    FBZZ_FIELD_RANGE(float, duration, 0.42f, "継続時間", 0.05f, 3.0f)
    FBZZ_TOOLTIP("1 つが本来の位置へ収まるまでの秒数")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugSlots, 0, "スロット")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "経過")

    void OnStart() override;
    void OnUpdate() override;

    /// 最初からやり直す (画面の中で «別のページへ切り替えた» ときに使える)。
    void Replay();
    /// 置き切ったか。入力を受け始めるかの判断に使う。
    [[nodiscard]] bool Done() const { return m_done; }

private:
    void Capture(GameObject* go, int group);
    void Write(float elapsed);

    struct Entry {
        uimotion::Slot slot;
        int            group = 0;   ///< 段差の番号。子は親と同じ番号を持つ
    };
    std::vector<Entry> m_entries;
    float m_elapsed = 0.0f;
    bool  m_done    = false;
};

FBZZ_REFLECT(UiRevealComponent)

inline void UiRevealComponent::Capture(GameObject* go, int group)
{
    if (!go) return;
    Entry e;
    e.slot.go     = go;
    e.slot.origin = go->transform.position;
    e.slot.image  = go->GetComponent<UIImage>() != nullptr;
    e.slot.text   = go->GetComponent<UIText>() != nullptr;
    // 色は «置いてある» ものが正本。読めない要素 (子を束ねるだけの空の GameObject)
    // は白のまま、位置だけ動かす。
    if (e.slot.image)     e.slot.color = ui.GetImageColor(go);
    else if (e.slot.text) e.slot.color = ui.GetTextColor(go);
    e.group = group;
    m_entries.push_back(e);
}

inline void UiRevealComponent::OnStart()
{
    m_entries.clear();
    int group = 0;
    for (const std::string& name : targets) {
        if (name.empty()) continue;
        GameObject* go = scene.Find(name);
        if (!go) {
            debug.LogWarning("UiRevealComponent: '" + name + "' が見つかりません");
            continue;
        }
        Capture(go, group);
        if (includeChildren)
            for (int i = 0, n = go->GetChildCount(); i < n; ++i) Capture(go->GetChild(i), group);
        ++group;
    }
    debugSlots = static_cast<int>(m_entries.size());
    Replay();
}

inline void UiRevealComponent::Replay()
{
    m_elapsed = 0.0f;
    m_done    = m_entries.empty();
    // 1 フレーム目から «出ていない» 状態で描く。OnUpdate を待つと、シーンに
    // 置いた位置で 1 フレームだけ見えてから引っ込む。
    Write(0.0f);
}

inline void UiRevealComponent::Write(float elapsed)
{
    const Vector3 from = { fromX, fromY, 0.0f };
    for (Entry& e : m_entries) {
        const float t = uimotion::Stagger(elapsed - delay, e.group, stagger, duration);
        // α は位置より少し先に立ち上げる。位置と同時だと «薄いまま動いている» 時間が
        // 長く、出現が眠く見える。
        const Vector4 color = uimotion::Place(e.slot, t, from, uimotion::OutQuint(t * 1.25f));
        if (e.slot.image)     ui.SetImageColor(e.slot.go, color);
        else if (e.slot.text) ui.SetTextColor(e.slot.go, color);
    }
}

inline void UiRevealComponent::OnUpdate()
{
    if (m_done) return;
    // 実時間。UI 画面はスローもヒットストップも掛からない (UiMotion.hpp の約束)。
    m_elapsed += (std::max)(time.UnscaledDeltaTime(), 0.0f);
    debugElapsed = m_elapsed;

    const int   last  = m_entries.empty() ? 0 : m_entries.back().group;
    const float total = delay + stagger * static_cast<float>(last) + duration;
    Write(m_elapsed);
    if (m_elapsed >= total) {
        // 置き切ったら正確に 1 で書いて手を離す (WHY はファイル頭)。
        Write(total + 1.0f);
        m_done = true;
    }
}

} // namespace sandbox
