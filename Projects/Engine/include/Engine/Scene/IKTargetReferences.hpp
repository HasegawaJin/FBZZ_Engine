/// @file    IKTargetReferences.hpp
/// @brief   IK の永続ターゲット参照と旧接続の一度だけの移行。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <Engine/Scene/Entity.hpp>

namespace fbzz::scene {

class Scene;
struct IKChain;
struct IKSolverComponent;

/// @brief 旧 GUID・EntityID・一意な名前を一度だけ永続参照へ移行し、現在の Scene で解決する。
/// @pre 全 GO の GUID と親子関係の復元が完了していること。
/// @note 非空 GUID が未解決でも名前へフォールバックしない。名前が複数または未存在なら以後再試行しない。
void InitializeIKTargetReferences(Scene& scene, IKSolverComponent& solver);

/// @brief 正本の GUID だけから実行時 EntityID を更新する。
/// @note 初回のみ旧接続を移行する。名前ラベルを変更せず、欠損 GUID は保持する。
void ResolveIKTargetReferences(Scene& scene, IKSolverComponent& solver);

/// @brief 目標 GO の GUID を設定し、互換 GUID と実行時 EntityID を同期する。
/// @return 対象が存在しなければ false で未変更。INVALID は明示解除として成功する。
/// @note Marker は不要。targetName の既存チェーン識別ラベルを保持する。
[[nodiscard]] bool SetIKTargetReference(Scene& scene, IKChain& chain, EntityID target);

/// @brief Pole GO の GUID を設定し、互換 GUID と実行時 EntityID を同期する。
/// @return 対象が存在しなければ false で未変更。INVALID は明示解除として成功する。
[[nodiscard]] bool SetIKPoleReference(Scene& scene, IKChain& chain, EntityID target);

/// @brief 目標接続を null にし、旧名前・EntityID からの再接続を抑止する。
/// @note SDK のチェーン識別に使う targetName は保持する。
void ClearIKTargetReference(IKChain& chain);

/// @brief Pole 接続を null にし、旧名前・EntityID からの再接続を抑止する。
void ClearIKPoleReference(IKChain& chain);

} /// @note namespace fbzz::scene
