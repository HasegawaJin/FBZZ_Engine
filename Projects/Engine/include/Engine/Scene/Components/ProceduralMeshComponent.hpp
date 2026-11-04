/// @file    ProceduralMeshComponent.hpp
/// @brief   スクリプトが組んだメッシュを GameObject へ結び付けるランタイム専用コンポーネント
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Scene/Components/TransientGpuState.hpp>

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/MeshBuilder.hpp>

#include <string>

namespace fbzz::scene {

/// @note シーンへ保存しない個体専用メッシュ。コピー先は RuntimeMeshSystem が LateUpdate で全体を再作成する。
struct ProceduralMeshComponentGpuState {
    MeshDirty dirty = MeshDirty::None;
    DoubleBufferedMesh runtimeMesh;
    /// @note builder の値は残り、GPU だけが空になるので、次の更新で全体を再転送する。
    void OnResetGpuRuntime() { dirty = MeshDirty::All; }
};

struct ProceduralMeshComponent : TransientGpuState<ProceduralMeshComponentGpuState> {
    bool enabled = true;

    /// @note 形の正本。スクリプトが直接書き換えてよい。
    MeshBuilder builder;

    /// @note 差し込むマテリアル。空文字列なら MaterialComponent に触らない
    /// @note (Inspector で別のマテリアルを付けたい場合はこちらを空にする)。
    std::string materialPath = "Assets/Materials/Fallback/ProceduralMeshFallback.mat";

    /// @note 最後に MaterialComponent へ書いた値。
    /// @note 毎フレーム書き直すと ScriptMaterialProxy や Inspector が指定したマテリアルが
    /// @note 毎フレーム剥がれる。差分を持ち «materialPath を変えたときだけ» 反映する。
    std::string appliedMaterialPath;

    const char* GetTypeName() const { return "Procedural Mesh"; }
};

} /// @note namespace fbzz::scene
