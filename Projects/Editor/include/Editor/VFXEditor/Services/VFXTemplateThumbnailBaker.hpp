// FBZZ Engine
// VFXTemplateThumbnailBaker.hpp | fbzz::editor
// Template カタログ用サムネイル PNG の一括生成
// WHY: カタログが出せるのは "Particle x3, Light x1" という内訳文字列だけで、
//      爆発と魔法の区別が名前でしかつかなかった。適用してみるまで中身が判らないので、
//      「とりあえず Replace して見る → Ctrl+Z」を繰り返すことになる。
//      決定論スクラブがあるのだから、代表時刻の 1 枚を焼いて並べれば済む。
// NOTE: 操作用 Preview World を借りて焼くため、実行中はユーザーのプレビューが
//       一時的に Template の絵へ差し替わる。終了時に必ず元の状態へ戻す。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;
class VFXPreviewController;

class VFXTemplateThumbnailBaker {
public:
    bool running = false;
    // 焼き対象の .vfx 実パス。Begin で積む。
    std::vector<std::string> queue;
    std::size_t index = 0;
    int baked = 0;
    int skipped = 0;
    int failed = 0;
    // 既存 PNG を上書きするか。false なら未生成のものだけ焼く。
    bool overwrite = false;
    std::string message;
    // 焼き終わって Preview を元へ戻す必要があるか。View が読んで 1 度だけ処理する。
    // WHY: Preview の差し替え状態は Session (Application 層) が持っているが、
    //      Services から Application を触ると依存が逆流する。フラグだけを渡す。
    bool restoreRequested = false;

    [[nodiscard]] bool IsRunning() const { return running; }

    void Begin(std::vector<std::string> templatePaths, bool overwriteExisting);
    void Cancel();

    // 1 テンプレートを数フレームかけて焼く。scrub 要求から RenderTarget へ結果が
    // 出るまで待つ必要があるため、1 フレームで全件は回せない (Sequence Exporter と同じ理由)。
    void Tick(EditorContext& ctx, VFXPreviewController& preview);

private:
    // 0 = 次の Template を読み込む / 1 = 描画待ち / 2 = 読み戻して書き出す
    int m_stage = 0;
    int m_settleFrames = 0;
};

} // namespace fbzz::editor
