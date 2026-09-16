/// @file    ProceduralMeshComponent.hpp
/// @brief   スクリプトが組んだメッシュを GameObject へ結び付けるランタイム専用コンポーネント
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/MeshBuilder.hpp>

#include <string>

namespace fbzz::scene {

/// MeshBuilder の内容を GPU メッシュにして MeshRenderer へ差す。
///
/// シーンには保存しない (FBZZ_INTERNAL_COMPONENT)。形を決めているのはスクリプトなので、
/// 保存しても Play のたびに上書きされるだけで意味を持たない。Play/Stop で消えるのは
/// SpriteRenderer の runtimeMesh と同じ扱い。
///
/// アップロードは RuntimeMeshSystem が Phase::LateUpdate で行う。ResourceManager を
/// 触れるのがそこだけで、かつ描画より前なので、同じフレームのうちに画面へ出る。
struct ProceduralMeshComponent {
    bool enabled = true;

    /// 形の正本。スクリプトが直接書き換えてよい。
    MeshBuilder builder;

    /// builder のどこが変わったか。アップロード後に None へ戻される。
    MeshDirty dirty = MeshDirty::None;

    /// 差し込むマテリアル。空文字列なら MaterialComponent に触らない
    /// (Inspector で別のマテリアルを付けたい場合はこちらを空にする)。
    std::string materialPath = "Assets/Materials/Fallback/ProceduralMeshFallback.mat";

    /// 最後に MaterialComponent へ書いた値。
    ///
    /// WHY 毎フレーム書き直さないか: 上書きし続けると ScriptMaterialProxy や Inspector で
    ///     指定したマテリアルが毎フレーム剥がれる。差分を持てば «materialPath を変えた
    ///     ときだけ» 反映でき、外から差し替えた側も生き残る。
    std::string appliedMaterialPath;

    /// GPU 側の実体。2 枚交互に使う理由は DoubleBufferedMesh のヘッダーを参照。
    DoubleBufferedMesh runtimeMesh;

    const char* GetTypeName() const { return "Procedural Mesh"; }
};

} // namespace fbzz::scene
