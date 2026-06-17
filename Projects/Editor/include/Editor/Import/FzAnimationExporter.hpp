// FBZZ Engine
// FzAnimationExporter.hpp | fbzz::editor
// aiAnimation を .anim バイナリに書き出す
#pragma once
#include <string>

struct aiAnimation;

namespace fbzz::editor {

class FzAnimationExporter {
public:
    // aiAnimation を .anim バイナリとして outputPath に書き出す。
    // @ret 成功なら true
    // unitScale: FBX 単位 → メートル換算係数 (Skeleton と同じ値を渡す)
    // WHY: Skeleton の bindTranslation は unitScale 済み。アニメーションの
    //      ポジションキーも同スケールにしないと再生時にボーンが飛ぶ。
    static bool Export(const aiAnimation* anim,
                       const std::string& outputPath,
                       float unitScale = 1.0f);
};

} // namespace fbzz::editor
