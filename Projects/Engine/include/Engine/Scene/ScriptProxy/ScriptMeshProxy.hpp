/// @file    ScriptMeshProxy.hpp
/// @brief   Script から MeshRenderer / SkinnedMeshRenderer を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// MeshRenderer と SkinnedMeshRenderer のどちらが付いていても同一 API で有効/無効を切り替えられる。
/// 手続きメッシュ (MeshBuilder) を GameObject へ流し込む入口も兼ねる。
#pragma once

#include <Engine/Scene/MeshBuilder.hpp>

#include <span>
#include <string>
#include <string_view>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptMeshProxy {
    Script* script = nullptr;

    /// @brief MeshRenderer または SkinnedMeshRenderer の enabled を切り替える。両方あれば両方を変更する。
    void SetEnabled(bool enabled) const;
    /// @return どちらか一方でも enabled なら true。
    bool IsEnabled() const;

    /// @brief シャドウマップへ影を落とすかを切り替える。両方あれば両方を変更する。
    /// @note 見えているのに影だけ落としたくない演出 (半透明化中のプレイヤー、一時的に
    ///       出す UI メッシュなど) を、描画を止めずに表現できるようにする。
    void SetCastShadows(bool castShadows) const;
    /// @return どちらか一方でも影を落とすなら true。
    bool GetCastShadows() const;

    /// @name MeshRenderer 専用
    /// @note "primitive:cube" / "models/foo.fbx:0" 形式のパスを設定する。実際の引き直しは
    ///       RuntimeMeshSystem が LateUpdate に行うため、同じフレームの描画から効く。
    ///@{
    void        SetMeshPath(std::string_view path) const;
    /// @return MeshRenderer が無ければ空文字列。
    std::string GetMeshPath() const;
    ///@}

    /// @name SkinnedMeshRenderer 専用
    ///@{
    /// @brief modelPath を変更する。変更後は RenderSystem が再ロードする。
    void        SetModelPath(std::string_view path) const;
    /// @return SkinnedMeshRenderer が無ければ空文字列。
    std::string GetModelPath() const;
    ///@}

    /// @name 手続きメッシュ
    /// MeshBuilder で組んだ形を GPU メッシュにして、この GameObject の MeshRenderer へ差す。
    /// MeshRenderer / MaterialComponent は無ければ自動で付く。
    /// @note 値で渡すので «変わったことを伝え忘れる» 事故が起きない。毎フレーム組み直してよい。
    ///@{
    void Apply(const MeshBuilder& builder) const;
    /// @brief 別の GameObject (Spawn した弾やエフェクト) へ流し込む版。
    void Apply(GameObject& target, const MeshBuilder& builder) const;

    /// @brief 積んである頂点への直接アクセス。三角形の繋がりを変えずに頂点だけ動かす演出に使う。
    /// @note 書き換えた後は必ず Touch() を呼ぶこと。手続きメッシュが無ければ空。
    [[nodiscard]] std::span<MeshVertex> Vertices() const;

    /// @brief Vertices() を書き換えたことを伝える。既定はインデックスバッファを上げ直さない。
    void Touch(MeshDirty dirty = MeshDirty::Vertices) const;

    /// @brief 手続きメッシュを外して描画を止める。組んだ内容も破棄する。
    void Clear() const;

    /// @brief 手続きメッシュに当てるマテリアル。空文字列にすると MaterialComponent へ触らなくなる。
    /// @note 既定は Assets/Materials/Fallback/ProceduralMeshFallback.mat (頂点カラー Unlit)。
    void SetProceduralMaterial(std::string_view path) const;
    ///@}
};

} // namespace fbzz::scene
