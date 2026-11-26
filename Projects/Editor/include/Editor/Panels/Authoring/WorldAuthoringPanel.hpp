/// @file    WorldAuthoringPanel.hpp
/// @brief   World cell and PCG editing UI backed exclusively by registered operations.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Editor/Panels/Tools/EditorToolPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/World/WorldPcg.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace fbzz::editor {

class WorldAuthoringPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "World / PCG"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnUpdate(EditorContext& context, float deltaTime) override;
    void DrawSceneDebug(EditorContext& context, const ImVec2& origin, const ImVec2& size) const;

protected:
    const char* GetEditorType() const override { return "World / PCG"; }
    void OnRenderContent(EditorContext& context) override;

private:
    void SyncContext(EditorContext& context);
    void InspectPartition(EditorContext& context);
    void RefreshStreaming(EditorContext& context);
    void ReadPartitionSettings(const OpData& data);
    void DrawPartition(EditorContext& context);
    void DrawPcg(EditorContext& context);
    void DrawPcgProfile(EditorContext& context);
    bool DrawPcgProfileRules(EditorContext& context, scene::WorldPcgProfile& profile);
    bool ReadPcgProfileDraft(const OpData& data);
    void DrawStreaming(EditorContext& context);
    void ClearPreview();
    void RefreshPcgPreview(EditorContext& context, bool automatic);
    void DrawPcgDebugControls(EditorContext& context);
    OpArgs PartitionArgs() const;
    OpArgs PreviewArgs() const;
    bool Run(EditorContext& context, const char* operation, const OpArgs& args, OpData* output = nullptr);
    bool ActionButton(EditorContext& context, const char* label, const char* operation,
                      const OpArgs& args, OpData* output = nullptr);

    std::string m_project; ///< 現在の下書きを保持する対象プロジェクトのルート。
    std::uint64_t m_sceneGeneration = 0; ///< 下書きと診断結果が対応するシーンの世代番号。
    bool m_initialized = false; ///< 現在のプロジェクトとシーンから各入力欄を初期化済みか。
    float m_refreshClock = 0; ///< ワールドの状態を次にポーリングするための経過時間 [s]。
    char m_partition[64]{}; ///< 編集対象の World Partition を識別する入力欄。
    char m_volume[64]{}; ///< 編集対象の PCG Volume を識別する入力欄。
    char m_manifest[512] = "Assets/World/World.worldcells"; ///< World Partition が参照するセル一覧マニフェストの入力パス。
    std::string m_profile; ///< 現在の PCG Volume に割り当てる生成プロファイルのパス。
    std::string m_profileFile; ///< エディターに開いている PCG プロファイルのファイルパス。
    std::string m_profileLoadedPath; ///< 編集中の内容を読み込んだファイル。保存先を変えたときは元ファイルの版と照合しない。
    std::optional<scene::WorldPcgProfile> m_profileDraft; ///< 画面で編集する生成ルールの下書き。保存するまでディスク上の設定とは分けて持つ。
    char m_profileText[65536]{}; ///< PCG プロファイルの編集中のテキスト。
    std::size_t m_selectedRule = 0; ///< 表示中の PCG プロファイルで選択した生成ルールの添字。
    bool m_editProfileText = false; ///< PCG プロファイルテキストの編集欄を開いているか。
    bool m_profileDirty = false; ///< 開いている PCG プロファイルに未保存の変更があるか。
    bool m_volumeDraft = false; ///< PCG Volume の編集値がシーンへ未適用の下書きか。
    std::string m_rockSource; ///< 岩の生成ルールを作る際に使う原本アセットのパス。
    std::string m_pebbleSource; ///< 小石の生成ルールを作る際に使う原本アセットのパス。
    std::string m_profileFileFingerprint; ///< 読み込み・保存時のファイルの識別値。同じ保存先に外部の変更が入ったかを確かめる。
    std::string m_previewFingerprint; ///< 直近のプレビュー結果の識別値。実際の生成が、そのプレビューと同じ条件で行われるかを確かめる。
    std::string m_message; ///< 直近のワールド制作操作の結果を表示するメッセージ。
    std::string m_error; ///< ワールド制作操作で直近に発生した失敗理由。
    std::string m_streamError; ///< ワールドストリーミング状態を取得できなかった理由。
    std::string m_details; ///< 直近の操作について UI に表示する追加情報。
    OpData m_partitionData; ///< 現在の World Partition の照会・設定操作の結果。
    OpData m_previewData; ///< 現在の PCG プレビューで採用・除外した点と診断。
    OpData m_volumeBounds; ///< 現在選択している PCG Volume の検査結果。確定済みの境界を含む。
    std::uint64_t m_previewUndoRevision = 0; ///< プレビュー確認時の履歴の版。Undo や再編集で版が進んだら表示済みの結果を破棄する。
    bool m_livePreview = true; ///< PCG Volume の下書きを変更したときにプレビューを自動更新するか。
    bool m_previewPending = false; ///< PCG プレビューの再計算を次の更新に予約しているか。
    float m_previewDelay = 0.0f; ///< 設定を連続して動かす間の再計算をまとめるための残り待ち時間（秒）。
    std::string m_previewStatus; ///< PCG プレビューの進捗または診断の表示文言。
    bool m_showPcgBounds = true; ///< Scene View に PCG Volume の生成領域を表示するか。
    bool m_showBiomeBounds = false; ///< Scene View にバイオームの範囲を表示するか。
    bool m_showAccepted = true; ///< Scene View に PCG プレビューで採用した点を表示するか。
    bool m_showRejected = false; ///< Scene View に PCG プレビューで除外した点を表示するか。
    std::string m_debugRule; ///< PCG プレビューを表示するルールの絞り込み条件。
    std::string m_debugReason; ///< PCG プレビューに表示する除外理由の絞り込み条件。
    OpData m_streamData; ///< 直近のストリーミング照会結果。常駐セル、読込待ちとソース位置を含む。
    float m_origin[3]{}; ///< World Partition のセルグリッドのワールド原点 [m]。
    float m_cellSize = 256; ///< World Partition のセル一辺の長さ [m]。
    float m_nearDistance = 384; ///< ストリーミングが詳細セルを要求する、ソースからの距離 [m]。
    float m_hlodDistance = 2048; ///< ストリーミングが簡易セルを要求する、ソースからの距離 [m]。
    float m_hysteresis = 64; ///< 境界付近で読込と解放を繰り返さないための距離の余裕 [m]。
    int m_maxConcurrentReads = 4; ///< ストリーミングが同時実行できるセル読み込みの上限。
    int m_maxCommitsPerUpdate = 2; ///< 1 回の更新でシーンへ反映するセル数の上限。ディスクから同時に読み出す件数とは別に設定する。
    bool m_streamInEditor = false; ///< 編集中のシーンでもワールドストリーミングを進めるか。
    float m_boundsMin[3]{}; ///< 編集している PCG Volume のワールド境界ボックスの最小点 [m]。
    float m_boundsMax[3] = {256, 0, 256}; ///< 編集している PCG Volume のワールド境界ボックスの最大点 [m]。
    bool m_overrideBounds = false; ///< PCG Volume の既定の範囲に代わって手入力の境界を使うか。
    bool m_partialUpdate = false; ///< 生成結果を指定の部分範囲だけ更新するか。
    float m_updateBoundsMin[3]{}; ///< 部分更新する範囲のワールド境界ボックスの最小点 [m]。
    float m_updateBoundsMax[3] = {256, 0, 256}; ///< 部分更新する範囲のワールド境界ボックスの最大点 [m]。
    int m_seed = -1; ///< PCG Volume に指定する乱数シード。-1 ならプロファイルの値を使う。
    bool m_protectManual = true; ///< 再生成時に手置きのオブジェクトを保護するか。
    bool m_enabled = true; ///< 現在選んだ PCG Volume を生成対象として有効にするか。
    int m_maxPoints = 256; ///< PCG プレビュー用に取得するサンプル点数の上限。
    bool m_includeScene = false; ///< セルのベイクにシーンオブジェクトを含めるか。
    bool m_includeTerrain = false; ///< セルのベイクに Terrain を含めるか。
    bool m_includeLightProbes = false; ///< セルのベイクにライトプローブを含めるか。
    bool m_omitTerrainCollisionInHlod = false; ///< 簡易表示用 HLOD のベイクから地形の当たり判定を外すか。
    bool m_buildHlod = true; ///< セルのベイク時に遠距離表示用の HLOD を生成するか。
    int m_cell[2]{}; ///< 手動で読み込む等の操作対象にするセルのグリッド座標。
    float m_source[3]{}; ///< ストリーミングソースへ適用する手入力のワールド位置 [m]。
    bool m_overrideSource = false; ///< ストリーミングソースに手入力位置を使うか。
};

} // namespace fbzz::editor
