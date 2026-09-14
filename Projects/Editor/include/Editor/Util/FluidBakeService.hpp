/// @file    FluidBakeService.hpp
/// @brief   .fluid の焼きとプレビューを受け付けるエディター常駐のジョブ窓口
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY パネルや Inspector ではなくここが焼くか:
///   3D の焼きは GPU で毎フレーム Tick が要る。パネルが持っていると、閉じた瞬間に止まり、
///   AI から頼んだ焼きはパネルを開くまで進まない。EditorApp が 1 つ持って毎フレーム回す。
///   VolumeFlipbookBaker (160³ の GPU 資源) を 2 つ抱えないよう、所有者もここ 1 つにする。
///
/// Enqueue* はすぐ戻る (AI Command Bus の drain をベイクで止めない)。結果は Find で追う。
/// 焼いた出力は Undo で消さない (同名の既存アセットを壊しうるため)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset {
struct FluidBakeResult;
struct FluidRecipe;
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
    /// 焼く .fluid の実パス。
    std::string fluidPath;
    /// 焼き上がったら .fluid の隣の同名 .mat を作る / 追従させる。
    bool updateMaterial = true;
    /// 空でなければ、焼き上がったあとその .mat を貼った 1 層の .vfx をこの実パスへ書く。
    std::string vfxPath;
    std::string vfxRootName;
    /// 0 以外なら、この seed で焼く (.fluid は書き換えない — 気に入った seed は呼び手が固定する)。
    std::uint32_t seed = 0;
};

struct FluidPreviewRequest {
    std::string fluidPath;
    /// 見たいコマ (0 = 焼き始めの 1 コマ目)。負なら time から求める。
    /// WHY コマ番号を第一級にするか: 秒で指定するとコマ境界に乗らず «焼いたどのコマでもない絵» になる。
    int frame = 0;
    /// 焼き始め (warmup 後) からの秒。frame が負のときだけ使い、一番近いコマへ吸着する。
    float time = 0.0f;
    /// 出力 PNG の 1 辺 [px]。コンタクトシートではシート全体の 1 辺。
    int size = 256;
    /// 全コマを焼いたアトラスの並びで 1 枚に並べる (variants > 1 なら seed 違いを並べる)。
    /// 時間方向の当たり外れを 1 往復で見られるので、AI が絵を見て直す輪が短くなる。
    bool contactSheet = false;
    /// 0 以外ならこの seed で解く (.fluid は書き換えない)。
    std::uint32_t seed = 0;
    /// seed を 1 つずつずらした試しを何本並べるか (contactSheet と併用。1 で無効)。
    int variants = 1;
};

/// Enqueue が失敗したときの理由。code は AI Command Bus のエラーコードにそのまま使う
/// (FLUID_NOT_FOUND / FLUID_READ_FAILED / FLUID_BUSY / NO_RENDERER)。
struct FluidJobError {
    std::string code;
    std::string message;
};

struct FluidJobStatus {
    std::uint32_t id = 0;
    FluidJobKind kind = FluidJobKind::Bake;
    FluidJobState state = FluidJobState::Queued;
    /// [0,1]。
    float progress = 0.0f;
    std::string fluidPath;
    std::string message;
    /// 書いたファイルの実パス (テクスチャ・.vfield など)。
    std::vector<std::string> outputs;
    /// 作った / 更新した .mat と .vfx の実パス (無ければ空)。
    std::string materialPath;
    std::string vfxPath;
    /// Preview の結果 (<projectRoot>/Library/FluidPreview/ 以下。アセットとしては扱わない)。
    std::string previewPngPath;
    /// 焼いた / 描いた絵の指紋 (BakeFingerprint)。前回と同じなら絵は 1 画素も変わっていない。
    std::string fingerprint;
    /// 実際に解いたソルバー ("cpu" / "gpu")。
    std::string solverUsed;
    /// GPU を頼んだのに CPU へ落ちた理由 (落ちていなければ空)。
    std::string fallbackReason;
    /// プレビューで実際に描いたコマ (シートなら先頭のコマ)。
    int previewFrame = 0;
    /// 実際に使った seed。
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

    /// 受け付けたジョブの id (1 以上)。受け付けられなければ 0 を返し outError に理由を入れる。
    [[nodiscard]] std::uint32_t EnqueueBake(const EditorContext& ctx, const FluidBakeRequest& request,
                                             FluidJobError& outError);
    /// 新規フォルダに素材レシピを作り、順に焼いて複数層の .vfx を書く。directory は Assets 配下の未作成フォルダ。
    [[nodiscard]] std::uint32_t EnqueueEffectTemplate(const EditorContext& ctx, FluidEffectTemplate preset,
        const std::string& directory, FluidJobError& outError);
    [[nodiscard]] std::uint32_t EnqueuePreview(const EditorContext& ctx, const FluidPreviewRequest& request,
                                               FluidJobError& outError);
    /// 走っている / 待っているジョブを止める。見つからないか終わっていれば false。
    bool Cancel(std::uint32_t id);

    /// 終わったジョブも直近 32 件は残す。見つからなければ nullptr。
    [[nodiscard]] const FluidJobStatus* Find(std::uint32_t id) const;
    /// その .fluid を焼いている / 焼く予定のジョブがあるか。
    [[nodiscard]] bool IsBusy(const std::string& fluidPath) const;
    [[nodiscard]] bool IsAnyBusy() const;

    /// EditorApp が毎フレーム、レンダラーのフレーム内で呼ぶ (3D の焼きは GPU を記録する)。
    void Tick(EditorContext& ctx);
    /// 走っているジョブを止めて GPU 資源を返す。
    void Shutdown(EditorContext& ctx);

    // ── Inspector / Volume Flipbook Baker パネル向け ──

    /// .fluid を介さない設定で 3D を焼く (パネルの Analytic ソース用)。出力先と名前は settings のまま。
    /// .fluid を焼くときは EnqueueBake を使う (人と AI で同じ道を通す)。
    [[nodiscard]] std::uint32_t EnqueueVolumeBake(const EditorContext& ctx,
                                                  const asset::VolumeFlipbookBakeSettings& settings,
                                                  FluidJobError& outError);
    /// 終わった 2D の焼きの結果。2D の Bake でない・終わっていない・見つからなければ nullptr。
    [[nodiscard]] const asset::FluidBakeResult* FindFlatResult(std::uint32_t id) const;
    /// 終わった 3D の焼きの結果と、焼いた設定。3D の Bake でなければ nullptr。
    [[nodiscard]] const asset::VolumeFlipbookBakeResult* FindVolumeResult(std::uint32_t id) const;
    [[nodiscard]] const asset::VolumeFlipbookBakeSettings* FindVolumeSettings(std::uint32_t id) const;
    /// id の 3D の焼きの Atlas を 1 度だけ引き渡す。より新しい 3D の焼きが始まっていれば false。
    [[nodiscard]] bool TakeBakedVolumeFlipbook(std::uint32_t id, asset::BakedVolumeFlipbook& out);

    /// パネルの 3D プレビューを共有 Baker で描く (フレーム内で呼ぶ)。
    /// ジョブが Baker を使っている間は何もせず false。
    bool RecordVolumePreview(EditorContext& ctx, const asset::VolumeFlipbookBakeSettings& settings, float time,
                             const asset::VolumePreviewOptions& options);
    /// Fluid Editor のライブ 3D。ディスクではなく «今編集しているレシピ» を解く。
    /// recipeRevision が変われば解き直す (FluidDocument::Revision をそのまま渡す)。
    bool RecordVolumePreview(EditorContext& ctx, const asset::VolumeFlipbookBakeSettings& settings, float time,
                             const asset::VolumePreviewOptions& options, const asset::FluidRecipe* recipe,
                             std::uint64_t recipeRevision);
    /// どのジョブも共有 Baker を使っていないか (パネルがプレビューに使ってよいか)。
    [[nodiscard]] bool IsVolumeBakerFree() const;

    /// 保留中のソルバー切り替えを適用してよいか (再生中はループの折り返しまで false)。
    /// 焼きが共有 Baker を握っている間は無視する。頼む側が居なくなった次の Tick で開き直す。
    void AllowVolumePreviewSwitch(bool allow);
    /// 適用待ちのソルバー切り替えがあるか。
    [[nodiscard]] bool VolumePreviewSwitchPending() const;
    /// 前のソルバーで描いた絵をまだ出しているか。
    [[nodiscard]] bool VolumePreviewStale() const;
    /// プレビューの開き直しで起きたこと (GPU が使えず CPU へ落ちた / 開けなかった)。無ければ空。
    /// 焼きの結果要約はここには出ない (それは Volume Flipbook Baker パネルの担当)。
    [[nodiscard]] const std::string& VolumePreviewNote() const;
    /// 上の報せが «開けなかった» ものか (フォールバックで絵が出ているなら false)。
    [[nodiscard]] bool VolumePreviewNoteIsFailure() const;

    /// 共有 Baker。プレビュー RT・進み具合・PreviewPending の表示に使う (操作は上の窓口から)。
    [[nodiscard]] const asset::VolumeFlipbookBaker& VolumeBaker() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fbzz::editor
