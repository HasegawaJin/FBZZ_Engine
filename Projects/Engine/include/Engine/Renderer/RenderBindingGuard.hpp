/// @file    RenderBindingGuard.hpp
/// @brief   パス境界でレンダーターゲット束縛を無効化し、暗黙の依存を炙り出す診断スイッチ。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note 各パス Execute 直前に 1x1 ダミー RT を束縛する。自分で SetRenderTarget を呼ばない
///       パスは «前のパスの束縛» へ描くため 1 ピクセルへ潰れて消え、それが束縛漏れの目印になる。
///       RenderGraph は依存で並べ替えるため束縛を前パス任せにすると構成変更で壊れる順序依存を
///       能動的に炙り出す。既定は無効 (束縛漏れが残る間は絵が出ない) で、追う日に環境変数で立てる。
#pragma once

namespace fbzz::renderer::bindingguard {

/// この構成に診断コードが入っているか (Debug / Development のみ)。
/// @note GPU 検証と同じ定義を使うのは、どちらも «追う日にだけ立てる GPU 診断» で構成を
///       分ける理由が無いため。増やすと «どちらを付けた構成か» が分からなくなる。
inline constexpr bool kBuiltIn =
#if defined(FBZZ_GPU_VALIDATION)
    true;
#else
    false;
#endif

/// 実際に毒を撒くか。環境変数 `FBZZ_RENDER_BINDING_GUARD` (0 / 1) で切り替える。
/// 既定は「立てない」。Debug でも明示的に 1 を渡したときだけ動く。
[[nodiscard]] bool IsEnabled();

} // namespace fbzz::renderer::bindingguard
