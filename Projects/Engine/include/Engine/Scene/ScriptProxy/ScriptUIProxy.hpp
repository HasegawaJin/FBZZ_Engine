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

/// UIButton の当たり状態。IsHovered() / IsPressed() の戻り値と 1 対 1 で対応する。
enum class UIButtonPhase {
    Normal,
    Hovered,
    Pressed,
    Disabled,   ///< enabled == false または isInteractable == false
};

struct ScriptUIProxy {
    Script* script = nullptr;

    /// @name ボタン入力の取得
    /// @note ポーリング方式: コールバック登録だとスクリプト DLL リロード時に解放済み
    ///       std::function を指す事故が起きる。UISystem の 1 フレーム限定フラグを読む
    ///       だけならこの問題が起きない。
    /// @note レンダーパスから呼ばれる UISystem がフレーム N の描画で立てたフラグを、
    ///       フレーム N+1 の OnUpdate() が読む (1 フレーム遅延)。次の UISystem 呼び出し
    ///       まで保持されるため、取りこぼしも二重発火も起きない。
    /// @{
    [[nodiscard]] bool WasClicked() const;
    [[nodiscard]] bool WasHoverEnter() const;
    [[nodiscard]] bool WasHoverExit() const;
    [[nodiscard]] bool IsHovered() const;
    [[nodiscard]] bool IsPressed() const;
    [[nodiscard]] bool IsInteractable() const;
    [[nodiscard]] UIButtonPhase GetButtonPhase() const;

    /// GameObject ターゲット版。ボタンが子要素や別 Canvas にある構成 (メニュー画面を
    /// 1 つのスクリプトで捌く場合) はこちらを使う。
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
    /// UIText の色。SetImageColor は UIImage しか触らないため、文字だけを
    /// 状態に応じて明滅させたい HUD には別の入口が要る。
    void SetTextColor(const math::Vector4& color) const;
    void SetCanvasSortOrder(int order) const;

    /// @brief 直近のフレームで UI が解決したマウス位置 (Canvas 空間、原点は左上)。
    ///        input.GetMousePosition() はビューポートのピクセルで Canvas Scaler /
    ///        renderMode を通しておらず、要素の矩形と直接比べられるのはこちらだけ。
    /// @note シーンで最初に見つかった Canvas の値。UI が 1 度も回っていなければ (0,0)。
    ///       ポインター差し替え中も常に OS のマウスを返す (カーソルを動かす側が生の値を要る)。
    [[nodiscard]] math::Vector2 GetCanvasMousePosition() const;

    /// 実ビューポートに対応する Canvas 全体の寸法 (safeArea を引く前)。
    /// 未指定時は自分、無ければシーン最初の Canvas。対象・viewport 不在は false、出力未変更。
    [[nodiscard]] bool TryGetCanvasSize(math::Vector2& outSize, GameObject* canvasObject = nullptr) const;

    /// @name ゲーム内カーソル
    /// UI の当たり判定に使う座標と押下状態を、OS のマウスから差し替える。
    /// @note UIButton/UISlider/UIToggle は「Canvas 空間の座標 1 つと押下状態」しか見ないため、
    ///       入口を差し替えるだけでマウス/パッド両対応をウィジェット側の変更なしに済ませられる。
    /// @warning 毎フレーム呼ぶこと。1 フレーム途切れると OS のマウスへ戻る。置きっぱなしに
    ///          できる作りだと、カーソルを持つ側が消えた後 (シーン遷移/Play 停止) に古い座標が
    ///          残り、UI が触れない状態の原因が追えなくなる。
    ///@{
    void SetPointer(const math::Vector2& canvasPosition, bool pressed) const;
    void ClearPointer() const;
    [[nodiscard]] bool IsPointerOverridden() const;
    ///@}
    /// スプライトシートのピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW,texH) で正規化し
    /// UIImage の uvMin / uvMax へ書き込む。
    void SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const;
    /// UIImage の絵を差し替える。画像パスのほか、Sprite サブアセット参照
    /// (`"Assets/UI/Atlas.png::sprite::<id>"`) をそのまま渡せる。
    /// 切り出し矩形・9-slice の余白・タイル寸法は .meta から自動で解決される。
    void SetImageTexture(std::string_view path) const;
    /// 自 GO の UIImage 塗り潰し量 [0,1] を設定する (体力ゲージ等)。
    void SetImageFillAmount(float amount) const;
    /// @}

    /// @name GameObject ターゲット版
    /// @{
    /// 体力バーのように UI が別 GO (HUD キャンバスや子要素) にある場合、
    /// スクリプトが保持する GameObject* を直接指定して UI を更新する。
    void SetImageColor(GameObject* go, const math::Vector4& color) const;
    void SetImageFillAmount(GameObject* go, float amount) const;
    void SetImageTexture(GameObject* go, std::string_view path) const;
    void SetText(GameObject* go, std::string_view text) const;
    void SetTextColor(GameObject* go, const math::Vector4& color) const;
    void SetImageEnabled(GameObject* go, bool enabled) const;
    void SetTextEnabled(GameObject* go, bool enabled) const;

    /// 現在値の読み取り。ゲージを「今の値から目標値へ」補間する、点滅の基準色を
    /// オーサリング値のまま使う、といった演出は、Script 側に控えを持たずここから読む。
    /// 対象コンポーネントが無い場合は 0 / 白 / 空文字列 / false を返す。
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
    /// @}

    /// @name UI マテリアル (UIImage.materialPath)
    /// @{
    /// @note 対象を引数で受ける設計: 要素ごとにスクリプトを付けると HUD 20 要素で 20
    ///       インスタンスが毎フレーム回るため、1 つの HUD スクリプトが複数要素を指す。
    /// @note .mat は全参照要素が共有するため直接書くと波及する。ここでの上書きはこの
    ///       UIImage の描画にだけ乗り、シーンにも保存されない。
    /// @note param 名はシェーダーの MaterialConstants の変数名そのまま (綴り違いは無視)。

    /// @name 子を名前で引く
    /// 直下の子から名前が一致する 1 つを返す。見つからなければ nullptr。
    /// @note HUD の 1 スクリプトが「行の中の Label/Value/Fill」を指す構成は UI で定型のため、
    ///       ここに置く (無いと画面ごとに同じ探索が写経される)。写し間違えても nullptr が
    ///       返るだけなので、間違いが表に出るのが遅い。
    /// @note 孫は探さない。階層をまたいだ検索は構成変更で黙って壊れるため、1 段ずつ辿らせる。
    [[nodiscard]] GameObject* Find(GameObject* parent, std::string_view childName) const;

    /// @brief ポインターがこのフレームに UI へ吸われたか。これが無いと、メニューの上での
    ///        クリックがそのままゲーム側 (射撃・カメラ操作) にも届く。撃つ前にこれを見て降りる。
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
    /// @}
};

} // namespace fbzz::scene
