// FBZZ Engine
// VFXGraphInspector.hpp | fbzz::editor
// 選択ノード / リンク / グループ枠と公開パラメーターの編集 View
// WHY: Inspector はグラフを最も細かく書き換える面で、編集箇所は 60 以上ある。
//      ここを Panel から分けておくことで、「編集は Inspector、実行反映は Session が判定」
//      という一方向の流れが読み取れるようになる。
#pragma once

#include <Engine/Asset/TextureAnalysis.hpp>
#include <string>

namespace fbzz::scene { struct ParticleEmitter; }

namespace fbzz::editor {

struct EditorContext;
class VFXEditorSession;
class VFXGraphCanvas;
class VFXPreviewView;

class VFXGraphInspector {
public:
    // NOTE: canvas / previewView への参照は、Inspector から「ノード削除」「参照画像の設定」を
    //       操作するためのもの。View 同士の直接参照はこの 2 つに限定する。
    VFXGraphInspector(VFXEditorSession& session, VFXGraphCanvas& canvas, VFXPreviewView& previewView)
        : m_session(session), m_canvas(canvas), m_previewView(previewView) {}

    void Draw(EditorContext& ctx);
    // 公開パラメーターの定義と、実プロパティへの結線。
    void DrawParameters();

private:
    // 割り当て済みテクスチャの解析結果と、そこから決まる推奨設定を折りたたみで出す。
    // 推奨は 1 クリックで適用できる。
    // WHY: blendMode / alphaSource / spriteColumns は素材の中身で正解が変わるが、
    //      これまで中身を知る手段が目視しか無く、アルファが機能していない素材や
    //      事前乗算素材を取り違えたまま気付けなかった。AI が使うのと同じ解析を
    //      同じ画面へ出すことで、人も同じ根拠で判断できるようにする。
    // NOTE: 解析対象は .mat の albedo であって Emitter ではない。ParticleEmitter は
    //       テクスチャを直接持たないため、対象パスは呼び出し側 (DrawMaterialAnalysis) が
    //       解決して渡す。以前はここで particle.materialPath を読み直しており、
    //       .mat が入っていれば必ず早期 return する = 解析が一切出ない状態だった。
    // @return 設定を適用して .vfx が変わったか
    bool DrawTextureAnalysis(scene::ParticleEmitter& particle,
                             const std::string& texturePath);

    // .mat が割り当てられているときの解析。描画に使われるのは .mat 側の
    // テクスチャとブレンド設定なので、Texture フィールドを見ても実物と一致しない。
    // WHY: materialPath を設定した Emitter では blendMode が実行時に .mat から
    //      上書きされる。それを知らずに Emitter 側の blendMode を触ると、
    //      変えても絵が変わらないまま原因を探すことになる。最初に警告として出す。
    bool DrawMaterialAnalysis(scene::ParticleEmitter& particle);

    // 解析は数十 ms かかるためフレームごとには回さない。対象パスが変わったときだけ更新する。
    std::string m_analyzedTexturePath;
    asset::TextureAnalysis m_textureAnalysis;
    std::string m_analyzedMaterialPath;
    asset::MaterialAnalysis m_materialAnalysis;

    VFXEditorSession& m_session;
    VFXGraphCanvas& m_canvas;
    VFXPreviewView& m_previewView;
};

} // namespace fbzz::editor
