/// @file    VfxAssetSummary.hpp
/// @brief   `.vfx` を読み取り専用で展開し、層構成と尺を Inspector へ出す。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note `.vfx` の再生面は Prefab 編集モードそのもの (Docs/design/vfx-prefab.md §8.2)。隔離シーンを
///       別に立てると «もう 1 つのエディタ» になり、旧 VFXEditor を捨てた理由を作り直すことになる。
///       Inspector が受け持つのは「開く前に中身の見当を付ける」ところまで。
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

/// `.vfx` 配下の 1 オブジェクト。
struct VfxSummaryEntry {
    std::string name;
    /// 表示用の種別 ("Particle" / "Light" / スクリプト名など)。
    std::string kind;
    std::string materialPath;
    int   depth = 0;

    /// 生存窓を持つ (ParticleEmitter / VFXElement / Trail / Decal)。
    bool  hasWindow = false;
    float start     = 0.0f;
    float end       = 0.0f;
    bool  loop      = false;
    /// 空でなければ時刻ではなく Trigger() で始まる。
    std::string trigger;

    /// simulationMode = Gpu を要求しているか、実際に GPU で回るか。
    bool        gpuRequested = false;
    bool        gpuActive    = false;
    /// 縮退したときに原因となった設定名 ("sortMode" など)。
    std::string gpuFallbackField;
};

/// `.vfx` 1 ファイルの構成。
struct VfxAssetSummary {
    bool        valid = false;
    std::string error;

    bool  hasRoot        = false;
    bool  rootLoop       = false;
    bool  rootPlayOnAwake = true;
    bool  rootAutoDestroy = true;
    float rootSpeed      = 1.0f;
    /// ルートに書かれた尺。0 以下なら配下から自動算出する。
    float authoredDuration = 0.0f;
    /// 実際に鳴る長さ。endless のときは意味を持たない。
    float effectiveDuration = 0.0f;
    /// 配下にループする要素があり、時間では終わらない。
    bool  endless = false;

    std::vector<VfxSummaryEntry> entries;
};

/// パスと最終更新時刻でキャッシュしながら構成を返す。
///
/// @note 失敗はキャッシュしない。Script DLL のホットリロード中は一時的に
///       スクリプト型が引けず展開に失敗しうるので、覚えてしまうと
///       リロードが終わっても «壊れた .vfx» のまま表示が戻らない。
[[nodiscard]] const VfxAssetSummary& GetVfxAssetSummary(const std::string& diskPath);

/// Inspector の `.vfx` 分岐。Prefab 編集モードへの導線と構成一覧を描く。
void DrawVfxAssetInspector(EditorContext& ctx, const std::string& diskPath);

} // namespace fbzz::editor
