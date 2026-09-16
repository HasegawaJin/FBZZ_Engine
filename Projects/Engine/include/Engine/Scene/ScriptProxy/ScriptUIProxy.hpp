/// @file    ScriptUIProxy.hpp
/// @brief   Script から UI コンポーネントを操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

// UIButton の当たり状態。IsHovered() / IsPressed() の戻り値と 1 対 1 で対応する。
enum class UIButtonPhase {
    Normal,
    Hovered,
    Pressed,
    Disabled,   // enabled == false または isInteractable == false
};

struct ScriptUIProxy {
    Script* script = nullptr;

    // ── ボタン入力の取得 ──────────────────────────────────────────────────────
    // WHY ポーリングにするか: コールバック登録にすると、スクリプト DLL がリロードされた
    //     瞬間に UIButton 側へ登録済みの std::function が解放済みコードを指す。
    //     UISystem が立てる 1 フレーム限定フラグを読むだけなら寿命問題が起きない。
    //
    // WHY 1 フレーム遅れるか: UISystem はレンダーパスから呼ばれるため、フレーム N の描画で
    //     立ったフラグをフレーム N+1 の OnUpdate() が読む。フラグは次の UISystem 呼び出しまで
    //     保持されるので、取りこぼしも二重発火も起きない。
    [[nodiscard]] bool WasClicked() const;
    [[nodiscard]] bool WasHoverEnter() const;
    [[nodiscard]] bool WasHoverExit() const;
    [[nodiscard]] bool IsHovered() const;
    [[nodiscard]] bool IsPressed() const;
    [[nodiscard]] bool IsInteractable() const;
    [[nodiscard]] UIButtonPhase GetButtonPhase() const;

    // GameObject ターゲット版。ボタンが子要素や別 Canvas にある構成 (メニュー画面を
    // 1 つのスクリプトで捌く場合) はこちらを使う。
    [[nodiscard]] bool WasClicked(GameObject* go) const;
    [[nodiscard]] bool WasHoverEnter(GameObject* go) const;
    [[nodiscard]] bool WasHoverExit(GameObject* go) const;
    [[nodiscard]] bool IsHovered(GameObject* go) const;
    [[nodiscard]] bool IsPressed(GameObject* go) const;
    [[nodiscard]] bool IsInteractable(GameObject* go) const;
    [[nodiscard]] UIButtonPhase GetButtonPhase(GameObject* go) const;

    void SetButtonInteractable(bool v) const;
    void SetButtonInteractable(GameObject* go, bool v) const;
    void SetImageColor(const math::Vector4& color) const;
    void SetText(std::string_view text) const;
    // UIText の色。SetImageColor は UIImage しか触らないため、文字だけを
    // 状態に応じて明滅させたい HUD には別の入口が要る。
    void SetTextColor(const math::Vector4& color) const;
    void SetCanvasSortOrder(int order) const;

    /// 直近のフレームで UI が解決したマウス位置 (Canvas 空間、原点は左上)。
    ///
    /// WHY input.GetMousePosition() で足りないか: あちらはビューポート内のピクセルで、
    ///     Canvas Scaler や renderMode を通していない。要素の矩形と直接比べられるのは
    ///     こちらだけ。スライダーのドラッグのように「掴んだ位置」を要素の座標系で
    ///     測る操作で使う。
    /// NOTE: シーンで最初に見つかった Canvas の値。UI が 1 度も回っていなければ (0,0)。
    ///       ポインターを差し替えていても、ここは常に OS のマウスを返す
    ///       (カーソルを動かす側が生の値を要るため)。
    [[nodiscard]] math::Vector2 GetCanvasMousePosition() const;

    /// 実ビューポートに対応する Canvas 全体の寸法 (safeArea を引く前)。
    /// 未指定時は自分、無ければシーン最初の Canvas。対象・viewport 不在は false、出力未変更。
    [[nodiscard]] bool TryGetCanvasSize(math::Vector2& outSize, GameObject* canvasObject = nullptr) const;

    /// @name ゲーム内カーソル
    /// UI の当たり判定に使う座標と押下状態を、OS のマウスから差し替える。
    ///
    /// WHY これだけで足りるか: UIButton / UISlider / UIToggle の判定は
    ///     「Canvas 空間の座標 1 つと押下状態」しか見ていない。入口を差し替えれば、
    ///     マウスとパッドの両対応をウィジェット側に 1 行も書かずに済む。
    /// NOTE: **毎フレーム呼ぶこと。** 1 フレーム途切れると OS のマウスへ戻る。
    ///       置きっぱなしにできる作りだと、カーソルを持つ側が消えた後 (シーン遷移や
    ///       Play 停止) に古い座標が残り、UI が触れない状態の原因が追えなくなる。
    ///@{
    void SetPointer(const math::Vector2& canvasPosition, bool pressed) const;
    void ClearPointer() const;
    [[nodiscard]] bool IsPointerOverridden() const;
    ///@}
    // スプライトシートのピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW,texH) で正規化し
    // UIImage の uvMin / uvMax へ書き込む。
    void SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const;
    /// UIImage の絵を差し替える。画像パスのほか、Sprite サブアセット参照
    /// ("Assets/UI/Atlas.png::sprite::<id>") をそのまま渡せる。
    /// 切り出し矩形・9-slice の余白・タイル寸法は .meta から自動で解決される。
    void SetImageTexture(std::string_view path) const;
    // 自 GO の UIImage 塗り潰し量 [0,1] を設定する (体力ゲージ等)。
    void SetImageFillAmount(float amount) const;

    // ── GameObject ターゲット版 ───────────────────────────────────────────────
    // 体力バーのように UI が別 GO (HUD キャンバスや子要素) にある場合、
    // スクリプトが保持する GameObject* を直接指定して UI を更新する。
    void SetImageColor(GameObject* go, const math::Vector4& color) const;
    void SetImageFillAmount(GameObject* go, float amount) const;
    void SetImageTexture(GameObject* go, std::string_view path) const;
    void SetText(GameObject* go, std::string_view text) const;
    void SetTextColor(GameObject* go, const math::Vector4& color) const;
    void SetImageEnabled(GameObject* go, bool enabled) const;
    void SetTextEnabled(GameObject* go, bool enabled) const;

    // 現在値の読み取り。ゲージを「今の値から目標値へ」補間する、点滅の基準色を
    // オーサリング値のまま使う、といった演出は、Script 側に控えを持たずここから読む。
    // 対象コンポーネントが無い場合は 0 / 白 / 空文字列 / false を返す。
    [[nodiscard]] float         GetImageFillAmount() const;
    [[nodiscard]] math::Vector4 GetImageColor() const;
    [[nodiscard]] std::string   GetText() const;
    [[nodiscard]] math::Vector4 GetTextColor() const;
    [[nodiscard]] bool          IsImageEnabled() const;
    [[nodiscard]] bool          IsTextEnabled() const;

    [[nodiscard]] float         GetImageFillAmount(GameObject* go) const;
    [[nodiscard]] math::Vector4 GetImageColor(GameObject* go) const;
    [[nodiscard]] std::string   GetText(GameObject* go) const;
    [[nodiscard]] math::Vector4 GetTextColor(GameObject* go) const;
    [[nodiscard]] bool          IsImageEnabled(GameObject* go) const;
    [[nodiscard]] bool          IsTextEnabled(GameObject* go) const;

    // ── UI マテリアル (UIImage.materialPath) ────────────────────────────────
    //
    // WHY 「自分」版を持たないか:
    //   UI 要素 1 つ 1 つにスクリプトを付ける構成にすると、HUD が 20 要素あれば
    //   20 個の Script インスタンスが毎フレーム回る。実際の使い方は
    //   「1 つの HUD スクリプトが複数の要素を指して値を流す」なので、
    //   対象を引数で受ける形だけを出す。
    //
    // WHY 共有 .mat ではなく要素ごとの上書きになるか:
    //   .mat は参照する全要素が共有する実体で、そこへ書くと 1 本のゲージを
    //   動かしたつもりが同じ .mat の全ゲージへ波及する。ここで設定した値は
    //   その UIImage の描画にだけ乗り、シーンにも保存されない。
    //
    // param 名はシェーダーの MaterialConstants に宣言した変数名そのまま。
    // 綴りが違っても失敗しない (存在しない変数は無視される) ので、
    // 効かないときはまずシェーダーの変数名と突き合わせること。
    /// @name 子を名前で引く
    /// 直下の子から名前が一致する 1 つを返す。見つからなければ nullptr。
    ///
    /// WHY プロキシに置くか: HUD の 1 スクリプトが「行の中の Label / Value / Fill」を
    ///     指す構成は UI では定型で、これが無いと画面ごとに同じ探索が写経される。
    ///     写し間違えても nullptr が返るだけなので、間違いが表に出るのが遅い。
    /// NOTE: 孫は探さない。階層をまたいだ検索は構成の変更で黙って壊れるため、
    ///       1 段ずつ辿らせる。
    [[nodiscard]] GameObject* Find(GameObject* parent, std::string_view childName) const;

    /// ポインターがこのフレームに UI へ吸われたか。
    ///
    /// WHY 要るか: これが無いと、メニューの上でクリックした入力がそのまま
    ///     ゲーム側 (射撃・カメラ操作) にも届く。撃つ前にこれを見て降りる。
    [[nodiscard]] bool IsPointerOverUI() const;

    /// @name UISlider
    ///@{
    [[nodiscard]] float GetSliderValue(GameObject* go) const;
    void SetSliderValue(GameObject* go, float value) const;
    /// 直近のフレームで値が動いたか (1 フレーム限定)。
    [[nodiscard]] bool WasSliderChanged(GameObject* go) const;
    void SetSliderRange(GameObject* go, float minimum, float maximum) const;
    void SetSliderInteractable(GameObject* go, bool interactable) const;
    ///@}

    /// @name UIToggle
    ///@{
    [[nodiscard]] bool IsToggleOn(GameObject* go) const;
    void SetToggleOn(GameObject* go, bool isOn) const;
    [[nodiscard]] bool WasToggleChanged(GameObject* go) const;
    void SetToggleInteractable(GameObject* go, bool interactable) const;
    ///@}

    /// @name UIScrollView
    ///@{
    [[nodiscard]] math::Vector2 GetScrollPosition(GameObject* go) const;
    void SetScrollPosition(GameObject* go, const math::Vector2& position) const;
    void SetScrollContentSize(GameObject* go, const math::Vector2& size) const;
    [[nodiscard]] bool WasScrollChanged(GameObject* go) const;
    ///@}

    /// @name UIInputField
    ///@{
    [[nodiscard]] std::string GetFieldText(GameObject* go) const;
    void SetFieldText(GameObject* go, std::string_view text) const;
    [[nodiscard]] bool WasFieldChanged(GameObject* go) const;
    /// Enter が押された (multiline では立たない)。
    [[nodiscard]] bool WasSubmitted(GameObject* go) const;
    [[nodiscard]] bool IsFieldFocused(GameObject* go) const;
    void SetFieldFocused(GameObject* go, bool focused) const;
    ///@}

    /// @name UICanvasGroup
    /// 画面まるごとのフェードと入力の遮断。要素ごとに色を書いて回らずに済む。
    ///@{
    [[nodiscard]] float GetGroupAlpha(GameObject* go) const;
    void SetGroupAlpha(GameObject* go, float alpha) const;
    void SetGroupInteractable(GameObject* go, bool interactable) const;
    void SetGroupBlocksRaycasts(GameObject* go, bool blocks) const;
    ///@}

    /// @name フォーカス (パッド操作)
    /// UICanvas.navigationEnabled を立てた Canvas でだけ意味を持つ。
    ///@{
    /// go のフォーカスを、その go が属する Canvas へ設定する。
    void SetFocus(GameObject* go) const;
    /// シーンで最初に見つかった Canvas のフォーカス。無ければ nullptr。
    [[nodiscard]] GameObject* GetFocus() const;
    [[nodiscard]] bool IsFocused(GameObject* go) const;
    void ClearFocus(GameObject* go) const;
    ///@}

    /// @name ドラッグ & ドロップ
    ///@{
    [[nodiscard]] bool IsDragging(GameObject* go) const;
    /// このフレームに離されたか (1 フレーム限定)。
    [[nodiscard]] bool WasDropped(GameObject* go) const;
    /// 落ちた先。受け皿が無ければ nullptr。
    [[nodiscard]] GameObject* GetDropTarget(GameObject* go) const;
    /// 受け皿がこのフレームに受け取ったか (1 フレーム限定)。
    [[nodiscard]] bool WasReceived(GameObject* go) const;
    [[nodiscard]] GameObject* GetReceivedFrom(GameObject* go) const;
    [[nodiscard]] std::string GetReceivedPayload(GameObject* go) const;
    [[nodiscard]] bool IsDropHovered(GameObject* go) const;
    ///@}

    /// @name テキストの 1 文字ずつ表示
    ///@{
    /// 描く文字数。-1 で全部。リッチテキストのタグは数に入らない。
    void SetVisibleCharacters(GameObject* go, int count) const;
    [[nodiscard]] int  GetVisibleCharacters(GameObject* go) const;
    /// この文字列の総文字数 (タグを除く)。送り切ったかの判定に使う。
    [[nodiscard]] int  GetCharacterCount(GameObject* go) const;
    ///@}

    void SetMaterial(GameObject* go, std::string_view materialPath) const;
    void SetMaterialFloat(GameObject* go, std::string_view param, float value) const;
    void SetMaterialVector2(GameObject* go, std::string_view param, float x, float y) const;
    void SetMaterialVector4(GameObject* go, std::string_view param,
                            const math::Vector4& value) const;
    void SetMaterialColor(GameObject* go, std::string_view param,
                          const math::Vector4& color) const;
    /// スロット名は "albedo" / "normal" / "tex5" 等 (.mat の [textures] と同じ)。
    void SetMaterialTexture(GameObject* go, std::string_view slot,
                            std::string_view texturePath) const;
    /// 1 つだけ共有 .mat の値へ戻す。
    void ClearMaterialOverride(GameObject* go, std::string_view param) const;
    /// この要素の上書きを全て捨て、共有 .mat そのままの見た目へ戻す。
    void ClearMaterialOverrides(GameObject* go) const;
    [[nodiscard]] bool HasMaterialOverride(GameObject* go, std::string_view param) const;
};

} // namespace fbzz::scene
