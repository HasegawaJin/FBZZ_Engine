/// @file    ScriptUIProxy.hpp
/// @brief   Script から UI コンポーネントを操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

/// @note UIButton の当たり状態。IsHovered() / IsPressed() の戻り値と 1 対 1 で対応する。
enum class UIButtonPhase {
    Normal,
    Hovered,
    Pressed,
    Disabled,   ///< @note enabled == false または isInteractable == false
};

struct ScriptUIProxy {
    Script* script = nullptr;

    /// @name ボタン入力の取得
    /// @note ポーリング方式: コールバック登録だとスクリプト DLL リロード時に解放済み
    /// @note std::function を指す事故が起きる。UISystem の 1 フレーム限定フラグを読む
    /// @note だけならこの問題が起きない。
    /// @note レンダーパスから呼ばれる UISystem がフレーム N の描画で立てたフラグを、
    /// @note フレーム N+1 の OnUpdate() が読む (1 フレーム遅延)。次の UISystem 呼び出し
    /// @note まで保持されるため、取りこぼしも二重発火も起きない。
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API bool WasClicked() const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasHoverEnter() const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasHoverExit() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsHovered() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsPressed() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsInteractable() const;
    [[nodiscard]] FBZZ_ENGINE_API UIButtonPhase GetButtonPhase() const;

    /// @note GameObject ターゲット版。ボタンが子要素や別 Canvas にある構成 (メニュー画面を
    /// @note 1 つのスクリプトで捌く場合) はこちらを使う。
    [[nodiscard]] FBZZ_ENGINE_API bool WasClicked(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasHoverEnter(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasHoverExit(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsHovered(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsPressed(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsInteractable(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API UIButtonPhase GetButtonPhase(GameObject* go) const;

    FBZZ_ENGINE_API void SetButtonInteractable(bool v) const;
    FBZZ_ENGINE_API void SetButtonInteractable(GameObject* go, bool v) const;
    FBZZ_ENGINE_API void SetImageColor(const math::Vector4& color) const;
    FBZZ_ENGINE_API void SetText(std::string_view text) const;
    /// @note UIText の色。SetImageColor は UIImage しか触らないため、文字だけを
    /// @note 状態に応じて明滅させたい HUD には別の入口が要る。
    FBZZ_ENGINE_API void SetTextColor(const math::Vector4& color) const;
    FBZZ_ENGINE_API void SetCanvasSortOrder(int order) const;

    /// @brief 直近のフレームで UI が解決したマウス位置 (Canvas 空間、原点は左上)。
    /// @note input.GetMousePosition() はビューポートのピクセルで Canvas Scaler /
    /// @note renderMode を通しておらず、要素の矩形と直接比べられるのはこちらだけ。
    /// @note シーンで最初に見つかった Canvas の値。UI が 1 度も回っていなければ (0,0)。
    /// @note ポインター差し替え中も常に OS のマウスを返す (カーソルを動かす側が生の値を要る)。
    [[nodiscard]] FBZZ_ENGINE_API math::Vector2 GetCanvasMousePosition() const;

    /// @note 実ビューポートに対応する Canvas 全体の寸法 (safeArea を引く前)。
    /// @note 未指定時は自分、無ければシーン最初の Canvas。対象・viewport 不在は false、出力未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool TryGetCanvasSize(math::Vector2& outSize, GameObject* canvasObject = nullptr) const;

    /// @name ゲーム内カーソル
    /// @note UI の当たり判定に使う座標と押下状態を、OS のマウスから差し替える。
    /// @note UIButton/UISlider/UIToggle は「Canvas 空間の座標 1 つと押下状態」しか見ないため、
    /// @note 入口を差し替えるだけでマウス/パッド両対応をウィジェット側の変更なしに済ませられる。
    /// @warning 毎フレーム呼ぶこと。1 フレーム途切れると OS のマウスへ戻る。置きっぱなしに
    /// @note できる作りだと、カーソルを持つ側が消えた後 (シーン遷移/Play 停止) に古い座標が
    /// @note 残り、UI が触れない状態の原因が追えなくなる。
    /// @{
    FBZZ_ENGINE_API void SetPointer(const math::Vector2& canvasPosition, bool pressed) const;
    FBZZ_ENGINE_API void ClearPointer() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsPointerOverridden() const;
    /// @}
    /// @note スプライトシートのピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW,texH) で正規化し
    /// @note UIImage の uvMin / uvMax へ書き込む。
    FBZZ_ENGINE_API void SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const;
    /// @note UIImage の絵を差し替える。画像パスのほか、Sprite サブアセット参照
    /// @note (`"Assets/UI/Atlas.png::sprite::<id>"`) をそのまま渡せる。
    /// @note 切り出し矩形・9-slice の余白・タイル寸法は .meta から自動で解決される。
    FBZZ_ENGINE_API void SetImageTexture(std::string_view path) const;
    /// @note 自 GO の UIImage 塗り潰し量 [0,1] を設定する (体力ゲージ等)。
    FBZZ_ENGINE_API void SetImageFillAmount(float amount) const;
    /// @}

    /// @name GameObject ターゲット版
    /// @{
    /// @note 体力バーのように UI が別 GO (HUD キャンバスや子要素) にある場合、
    /// @note スクリプトが保持する GameObject* を直接指定して UI を更新する。
    FBZZ_ENGINE_API void SetImageColor(GameObject* go, const math::Vector4& color) const;
    FBZZ_ENGINE_API void SetImageFillAmount(GameObject* go, float amount) const;
    FBZZ_ENGINE_API void SetImageTexture(GameObject* go, std::string_view path) const;
    FBZZ_ENGINE_API void SetText(GameObject* go, std::string_view text) const;
    FBZZ_ENGINE_API void SetTextColor(GameObject* go, const math::Vector4& color) const;
    FBZZ_ENGINE_API void SetImageEnabled(GameObject* go, bool enabled) const;
    FBZZ_ENGINE_API void SetTextEnabled(GameObject* go, bool enabled) const;

    /// @note 現在値の読み取り。ゲージを「今の値から目標値へ」補間する、点滅の基準色を
    /// @note オーサリング値のまま使う、といった演出は、Script 側に控えを持たずここから読む。
    /// @note 対象コンポーネントが無い場合は 0 / 白 / 空文字列 / false を返す。
    [[nodiscard]] FBZZ_ENGINE_API float         GetImageFillAmount() const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector4 GetImageColor() const;
    [[nodiscard]] FBZZ_ENGINE_API std::string   GetText() const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector4 GetTextColor() const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsImageEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsTextEnabled() const;

    [[nodiscard]] FBZZ_ENGINE_API float         GetImageFillAmount(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector4 GetImageColor(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API std::string   GetText(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector4 GetTextColor(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsImageEnabled(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsTextEnabled(GameObject* go) const;
    /// @}

    /// @name UI マテリアル (UIImage.materialPath)
    /// @{
    /// @note 対象を引数で受ける設計: 要素ごとにスクリプトを付けると HUD 20 要素で 20
    /// @note インスタンスが毎フレーム回るため、1 つの HUD スクリプトが複数要素を指す。
    /// @note .mat は全参照要素が共有するため直接書くと波及する。ここでの上書きはこの
    /// @note UIImage の描画にだけ乗り、シーンにも保存されない。
    /// @note param 名はシェーダーの MaterialConstants の変数名そのまま (綴り違いは無視)。

    /// @name 子を名前で引く
    /// @note 直下の子から名前が一致する 1 つを返す。見つからなければ nullptr。
    /// @note HUD の 1 スクリプトが「行の中の Label/Value/Fill」を指す構成は UI で定型のため、
    /// @note ここに置く (無いと画面ごとに同じ探索が写経される)。写し間違えても nullptr が
    /// @note 返るだけなので、間違いが表に出るのが遅い。
    /// @note 孫は探さない。階層をまたいだ検索は構成変更で黙って壊れるため、1 段ずつ辿らせる。
    [[nodiscard]] FBZZ_ENGINE_API GameObject* Find(GameObject* parent, std::string_view childName) const;

    /// @brief 現在のポインターが入力可能な UI の矩形内にあるかを描画前でも判定する。
    /// @note クリック通知やフォーカスは変えない。ゲーム入力を処理する前に問い合わせる。
    [[nodiscard]] FBZZ_ENGINE_API bool IsPointerOverUI() const;

    /// @name UISlider
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API float GetSliderValue(GameObject* go) const;
    FBZZ_ENGINE_API void SetSliderValue(GameObject* go, float value) const;
    /// @note 直近のフレームで値が動いたか (1 フレーム限定)。
    [[nodiscard]] FBZZ_ENGINE_API bool WasSliderChanged(GameObject* go) const;
    FBZZ_ENGINE_API void SetSliderRange(GameObject* go, float minimum, float maximum) const;
    FBZZ_ENGINE_API void SetSliderInteractable(GameObject* go, bool interactable) const;
    /// @}

    /// @name UIToggle
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API bool IsToggleOn(GameObject* go) const;
    FBZZ_ENGINE_API void SetToggleOn(GameObject* go, bool isOn) const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasToggleChanged(GameObject* go) const;
    FBZZ_ENGINE_API void SetToggleInteractable(GameObject* go, bool interactable) const;
    /// @}

    /// @name UIScrollView
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API math::Vector2 GetScrollPosition(GameObject* go) const;
    FBZZ_ENGINE_API void SetScrollPosition(GameObject* go, const math::Vector2& position) const;
    FBZZ_ENGINE_API void SetScrollContentSize(GameObject* go, const math::Vector2& size) const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasScrollChanged(GameObject* go) const;
    /// @}

    /// @name UIInputField
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API std::string GetFieldText(GameObject* go) const;
    FBZZ_ENGINE_API void SetFieldText(GameObject* go, std::string_view text) const;
    [[nodiscard]] FBZZ_ENGINE_API bool WasFieldChanged(GameObject* go) const;
    /// @note Enter が押された (multiline では立たない)。
    [[nodiscard]] FBZZ_ENGINE_API bool WasSubmitted(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsFieldFocused(GameObject* go) const;
    FBZZ_ENGINE_API void SetFieldFocused(GameObject* go, bool focused) const;
    /// @}

    /// @name UICanvasGroup
    /// @note 画面まるごとのフェードと入力の遮断。要素ごとに色を書いて回らずに済む。
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API float GetGroupAlpha(GameObject* go) const;
    FBZZ_ENGINE_API void SetGroupAlpha(GameObject* go, float alpha) const;
    FBZZ_ENGINE_API void SetGroupInteractable(GameObject* go, bool interactable) const;
    FBZZ_ENGINE_API void SetGroupBlocksRaycasts(GameObject* go, bool blocks) const;
    /// @}

    /// @name フォーカス (パッド操作)
    /// @note UICanvas.navigationEnabled を立てた Canvas でだけ意味を持つ。
    /// @{
    /// @note go のフォーカスを、その go が属する Canvas へ設定する。
    FBZZ_ENGINE_API void SetFocus(GameObject* go) const;
    /// @note シーンで最初に見つかった Canvas のフォーカス。無ければ nullptr。
    [[nodiscard]] FBZZ_ENGINE_API GameObject* GetFocus() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsFocused(GameObject* go) const;
    FBZZ_ENGINE_API void ClearFocus(GameObject* go) const;
    /// @}

    /// @name ドラッグ & ドロップ
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API bool IsDragging(GameObject* go) const;
    /// @note このフレームに離されたか (1 フレーム限定)。
    [[nodiscard]] FBZZ_ENGINE_API bool WasDropped(GameObject* go) const;
    /// @note 落ちた先。受け皿が無ければ nullptr。
    [[nodiscard]] FBZZ_ENGINE_API GameObject* GetDropTarget(GameObject* go) const;
    /// @note 受け皿がこのフレームに受け取ったか (1 フレーム限定)。
    [[nodiscard]] FBZZ_ENGINE_API bool WasReceived(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API GameObject* GetReceivedFrom(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API std::string GetReceivedPayload(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsDropHovered(GameObject* go) const;
    /// @}

    /// @name テキストの 1 文字ずつ表示
    /// @{
    /// @note 描く文字数。-1 で全部。リッチテキストのタグは数に入らない。
    FBZZ_ENGINE_API void SetVisibleCharacters(GameObject* go, int count) const;
    [[nodiscard]] FBZZ_ENGINE_API int  GetVisibleCharacters(GameObject* go) const;
    /// @note この文字列の総文字数 (タグを除く)。送り切ったかの判定に使う。
    [[nodiscard]] FBZZ_ENGINE_API int  GetCharacterCount(GameObject* go) const;
    /// @}

    FBZZ_ENGINE_API void SetMaterial(GameObject* go, std::string_view materialPath) const;
    FBZZ_ENGINE_API void SetMaterialFloat(GameObject* go, std::string_view param, float value) const;
    FBZZ_ENGINE_API void SetMaterialVector2(GameObject* go, std::string_view param, float x, float y) const;
    FBZZ_ENGINE_API void SetMaterialVector4(GameObject* go, std::string_view param,
                            const math::Vector4& value) const;
    FBZZ_ENGINE_API void SetMaterialColor(GameObject* go, std::string_view param,
                          const math::Vector4& color) const;
    /// @note スロット名は "albedo" / "normal" / "tex5" 等 (.mat の [textures] と同じ)。
    FBZZ_ENGINE_API void SetMaterialTexture(GameObject* go, std::string_view slot,
                            std::string_view texturePath) const;
    /// @note 1 つだけ共有 .mat の値へ戻す。
    FBZZ_ENGINE_API void ClearMaterialOverride(GameObject* go, std::string_view param) const;
    /// @note この要素の上書きを全て捨て、共有 .mat そのままの見た目へ戻す。
    FBZZ_ENGINE_API void ClearMaterialOverrides(GameObject* go) const;
    [[nodiscard]] FBZZ_ENGINE_API bool HasMaterialOverride(GameObject* go, std::string_view param) const;
    /// @}
};

} /// @note namespace fbzz::scene
