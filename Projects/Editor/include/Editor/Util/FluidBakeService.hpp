/// @file    FluidBakeService.hpp
/// @brief   .fluid の焼きとプレビューを受け付けるエディター常駐のジョブ窓口
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 3D の焼きは GPU で毎フレーム Tick が要るため EditorApp が 1 つ持って毎フレーム回す
/// @note パネル所有だと閉じた瞬間に止まる。VolumeFlipbookBaker (160³ の GPU 資源) の所有者もここ 1 つ。
/// @note Enqueue* はすぐ戻り、結果は Find で追う。焼いた出力は Undo で消さない (同名の既存アセットを壊しうるため)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::fluid { struct FluidRecipe; }

namespace fbzz::asset {
struct FluidBakeResult;
struct VolumeFlipbookBakeResult;
struct VolumeFlipbookBakeSettings;
struct VolumePreviewOptions;
struct BakedVolumeFlipbook;
class VolumeFlipbookBaker;
}

namespace fbzz::editor {

struct EditorContext;
enum class FluidEffectTemplate : std::uint8_t;

enum class FluidJobKind : std::uint8_t { Bake, Preview };
enum class FluidJobState : std::uint8_t { Queued, Running, Encoding, Done, Failed, Cancelled };

struct FluidBakeRequest {
    /// @brief 焼く .fluid の実パス。
    std::string fluidPath;
    /// @note Bake の成功には隣の .mat の作成 / 更新も含む。プレビューは FluidPreviewRequest を使う。
    /// @brief 空でなければ、焼き上がったあとその .mat を貼った 1 層の .vfx をこの実パスへ書く。
    std::string vfxPath;
    std::string vfxRootName;
    /// @brief 0 以外なら、この seed で焼く (.fluid は書き換えない — 気に入った seed は呼び手が固定する)。
    std::uint32_t seed = 0;
    /// @brief 仮 Bake は Library に隔離し、解像度とコマ数を落として .mat / .vfx を書かない。
    bool draft = false;
    /// @brief 仮 Bake で未保存の編集内容を使う。通常 Bake では無視する。
    std::shared_ptr<const fluid::FluidRecipe> recipeOverride;
};

struct FluidPreviewRequest {
    std::string fluidPath;
    /// @brief 見たいコマ (0 = 焼き始めの 1 コマ目)。負なら time から求める。
    /// @note 秒で指定するとコマ境界に乗らず «焼いたどのコマでもない絵» になるため、コマ番号を第一級にする。
    int frame = 0;
    /// @brief 焼き始め (warmup 後) からの秒。frame が負のときだけ使い、一番近いコマへ吸着する。
    float time = 0.0f;
    /// @brief 出力 PNG の 1 辺 [px]。コンタクトシートではシート全体の 1 辺。
    int size = 256;
    /// @brief 全コマを焼いたアトラスの並びで 1 枚に並べる (variants > 1 なら seed 違いを並べる)。
    /// @brief 時間方向の当たり外れを 1 往復で見られるので、AI が絵を見て直す輪が短くなる。
    bool contactSheet = false;
    /// @brief 0 以外ならこの seed で解く (.fluid は書き換えない)。
    std::uint32_t seed = 0;
    /// @brief seed を 1 つずつずらした試しを何本並べるか (contactSheet と併用。1 で無効)。
    int variants = 1;
};

/// @brief Enqueue が失敗したときの理由。code は AI Command Bus のエラーコードにそのまま使う
/// @brief (FLUID_NOT_FOUND / FLUID_READ_FAILED / FLUID_BUSY / NO_RENDERER)。
struct FluidJobError {
    std::string code;
    std::string message;
};

struct FluidJobStatus {
    std::uint32_t id = 0;
    FluidJobKind kind = FluidJobKind::Bake;
    FluidJobState state = FluidJobState::Queued;
    /// @brief [0,1]。
    float progress = 0.0f;
    std::string fluidPath;
    /// @brief 仮 Bake の読み戻し用 .fluid。確定版と違い Library 配下にある。
    std::string draftFluidPath;
    bool draft = false;
    /// @brief 工程別の経過秒。3D のシミュレーションと描画は並行するため壁時計上の配分。
    float simulationSeconds = 0.0f;
    float renderSeconds = 0.0f;
    float outputSeconds = 0.0f;
    float elapsedSeconds = 0.0f;
    float slowestSimulationFrameSeconds = 0.0f;
    int slowestSimulationFrame = -1;
    float slowestRenderFrameSeconds = 0.0f;
    int slowestRenderFrame = -1;
    float colorEncodeSeconds = 0.0f;
    float motionEncodeSeconds = 0.0f;
    float sixWayEncodeSeconds = 0.0f;
    /// @brief 見積もれない段階では -1。
    float remainingSeconds = -1.0f;
    /// @brief 0: simulation, 1: render, 2: output。
    int stage = 0;
    std::string message;
    /// @brief 公開したファイルの実パス (テクスチャ・速度場 PNG・材質・VFX など)。
    std::vector<std::string> outputs;
    /// @brief 作った / 更新した .mat と .vfx の実パス (無ければ空)。
    std::string materialPath;
    std::string vfxPath;
    /// @brief Preview の結果 (`<projectRoot>/Library/FluidPreview/` 以下。アセットとしては扱わない)。
    std::string previewPngPath;
    /// @brief 焼いた / 描いた絵の指紋 (BakeFingerprint)。前回と同じなら絵は 1 画素も変わっていない。
    std::string fingerprint;
    /// @brief 実際に解いたソルバー ("cpu" / "gpu")。
    std::string solverUsed;
    /// @brief GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
    std::string fallbackReason;
    /// @brief プレビューで実際に描いたコマ (シートなら先頭のコマ)。
    int previewFrame = 0;
    /// @brief 実際に使った seed。
    std::uint32_t seed = 0;

    [[nodiscard]] bool Finished() const
    {
        return state == FluidJobState::Done || state == FluidJobState::Failed || state == FluidJobState::Cancelled;
    }
};

class FluidBakeService {
public:
    FluidBakeService();
    ~FluidBakeService();
    FluidBakeService(const FluidBakeService&) = delete;
    FluidBakeService& operator=(const FluidBakeService&) = delete;

    /// @brief 受け付けたジョブの id (1 以上)。受け付けられなければ 0 を返し outError に理由を入れる。
    [[nodiscard]] std::uint32_t EnqueueBake(const EditorContext& ctx, const FluidBakeRequest& request,
                                             FluidJobError& outError);
    /// @brief 新規フォルダに素材レシピを作り、順に焼いて複数層の .vfx を書く。directory は Assets 配下の未作成フォルダ。
    [[nodiscard]] std::uint32_t EnqueueEffectTemplate(const EditorContext& ctx, FluidEffectTemplate preset,
        const std::string& directory, FluidJobError& outError);
    [[nodiscard]] std::uint32_t EnqueuePreview(const EditorContext& ctx, const FluidPreviewRequest& request,
                                               FluidJobError& outError);
    /// @brief 走っている / 待っているジョブを止める。見つからないか終わっていれば false。
    bool Cancel(std::uint32_t id);

    /// @brief 終わったジョブも直近 32 件は残す。見つからなければ nullptr。
    [[nodiscard]] const FluidJobStatus* Find(std::uint32_t id) const;
    /// @brief その .fluid を焼いている / 焼く予定のジョブがあるか。
    [[nodiscard]] bool IsBusy(const std::string& fluidPath) const;
    [[nodiscard]] bool IsAnyBusy() const;

    /// @brief EditorApp が毎フレーム、レンダラーのフレーム内で呼ぶ (3D の焼きは GPU を記録する)。
    void Tick(EditorContext& ctx);
    /// @brief 走っているジョブを止めて GPU 資源を返す。
    void Shutdown(EditorContext& ctx);

    /// @name Inspector / Volume Flipbook Baker パネル向け
    /// @{

    /// @brief .fluid を介さない設定で 3D を焼く (パネルの Analytic ソース用)。出力先と名前は settings のまま。
    /// @brief .fluid を焼くときは EnqueueBake を使う (人と AI で同じ道を通す)。
    [[nodiscard]] std::uint32_t EnqueueVolumeBake(const EditorContext& ctx,
                                                  const asset::VolumeFlipbookBakeSettings& settings,
                                                  FluidJobError& outError);
    /// @brief 終わった 2D の焼きの結果。2D の Bake でない・終わっていない・見つからなければ nullptr。
    [[nodiscard]] const asset::FluidBakeResult* FindFlatResult(std::uint32_t id) const;
    /// @brief 終わった 3D の焼きの結果と、焼いた設定。3D の Bake でなければ nullptr。
    [[nodiscard]] const asset::VolumeFlipbookBakeResult* FindVolumeResult(std::uint32_t id) const;
    [[nodiscard]] const asset::VolumeFlipbookBakeSettings* FindVolumeSettings(std::uint32_t id) const;
    /// @brief id の 3D の焼きの Atlas を 1 度だけ引き渡す。より新しい 3D の焼きが始まっていれば false。
    [[nodiscard]] bool TakeBakedVolumeFlipbook(std::uint32_t id, asset::BakedVolumeFlipbook& out);

    /// @brief パネルの 3D プレビューを共有 Baker で描く (フレーム内で呼ぶ)。
    /// @brief ジョブが Baker を使っている間は何もせず false。
    bool RecordVolumePreview(EditorContext& ctx, const asset::VolumeFlipbookBakeSettings& settings, float time,
                             const asset::VolumePreviewOptions& options);
    /// @brief Fluid Editor のライブ 3D。ディスクではなく «今編集しているレシピ» を解く。
    /// @brief recipeRevision が変われば解き直す (FluidDocument::Revision をそのまま渡す)。
    bool RecordVolumePreview(EditorContext& ctx, const asset::VolumeFlipbookBakeSettings& settings, float time,
                             const asset::VolumePreviewOptions& options, const fluid::FluidRecipe* recipe,
                             std::uint64_t recipeRevision);
    /// @brief どのジョブも共有 Baker を使っていないか (パネルがプレビューに使ってよいか)。
    [[nodiscard]] bool IsVolumeBakerFree() const;

    /// @brief 保留中のソルバー切り替えを適用してよいか (再生中はループの折り返しまで false)。
    /// @brief 焼きが共有 Baker を握っている間は無視する。頼む側が居なくなった次の Tick で開き直す。
    void AllowVolumePreviewSwitch(bool allow);
    /// @brief 適用待ちのソルバー切り替えがあるか。
    [[nodiscard]] bool VolumePreviewSwitchPending() const;
    /// @brief 前のソルバーで描いた絵をまだ出しているか。
    [[nodiscard]] bool VolumePreviewStale() const;
    /// @brief プレビューの開き直しで起きたこと (GPU が使えず CPU へ落ちた / 開けなかった)。無ければ空。
    /// @brief 焼きの結果要約はここには出ない (それは Volume Flipbook Baker パネルの担当)。
    [[nodiscard]] const std::string& VolumePreviewNote() const;
    /// @brief 上の報せが «開けなかった» ものか (フォールバックで絵が出ているなら false)。
    [[nodiscard]] bool VolumePreviewNoteIsFailure() const;

    /// @brief 共有 Baker。プレビュー RT・進み具合・PreviewPending の表示に使う (操作は上の窓口から)。
    [[nodiscard]] const asset::VolumeFlipbookBaker& VolumeBaker() const;
    /// @}

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fbzz::editor
