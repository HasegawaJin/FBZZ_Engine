// FBZZ Engine
// VFXSequenceExporter.hpp | fbzz::editor
// 決定論スクラブを使った PNG 連番書き出し
// WHY: 代表時刻のスクリーンショットでは「立ち上がりが速すぎる」「消え際が唐突」といった
//      時間方向の破綻を判定できない。全区間を等間隔で書き出せると、外部レビューや
//      AI への提示が 1 枚絵ではなくタイムラインとして行える。
#pragma once

#include <string>

namespace fbzz::editor {

struct EditorContext;
class VFXPreviewController;

// 進行状態は 1 フレームに 1 枚だけ進める。
// WHY: 1 フレームで全カットを回すと、スクラブ結果が RenderTarget へ現れる前に
//      読み戻すことになり、全フレームが 1 コマずれた連番になる。
class VFXSequenceExporter {
public:
    bool running = false;
    int fps = 24;
    int frame = 0;
    int total = 0;
    float duration = 0.0f;
    // scrub 要求からレンダー結果が出るまで 1 フレーム待つためのカウンタ。
    int settleFrames = 0;
    std::string directory;
    std::string message;
    // 連番出力完了後、同じフレーム列をそのままFlipbook Atlasへ結合する。
    bool bakeAtlas = true;
    int atlasColumns = 0;
    bool overwriteAtlas = true;
    std::string atlasFileName = "flipbook_atlas.png";

    [[nodiscard]] bool IsRunning() const { return running; }

    // 1 フレーム分だけ進める。preview から RenderTarget と対象 Entity を借りる。
    void Tick(EditorContext& ctx, VFXPreviewController& preview);
};

} // namespace fbzz::editor
