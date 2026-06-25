// FBZZ Engine
// TrailComponent.hpp | fbzz::scene
// 移動体の軌跡をリボン状メッシュとして描画するための制御点・外観パラメータ
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

// TrailPoint — トレイルを構成する制御点 1 個分のワールド座標と生成時刻。
// WHY: 点列だけを保存し、リボン幅・色・UV は描画時に再計算することでパラメータ変更を即時反映する。
struct TrailPoint {
    math::Vector3 position = math::Vector3::ZERO;
    float         timestamp = 0.0f;
};

// TrailAlignment — リボン断面をどの基準方向に向けるかを表す。
// CameraFacing は剣閃など常に見やすいエフェクト、WorldUp はタイヤ跡など地面基準の帯に使う。
enum class TrailAlignment : uint8_t {
    CameraFacing = 0,
    WorldUp      = 1,
};

// TrailUVMode — U 座標をトレイル全体へ正規化するか、ワールド長でタイルするかを選ぶ。
// Stretch は剣閃の一枚絵、Tile は長い軌跡へ繰り返し模様を流す用途に使う。
enum class TrailUVMode : uint8_t {
    Stretch = 0,
    Tile    = 1,
};

// TrailWidthEasing — 古い点から新しい点へ幅を補間するときの曲線。
// WHY: 線形だけでは先端だけ鋭く細る軌跡や、根元を長く太く残す演出を作りにくい。
enum class TrailWidthEasing : uint8_t {
    Linear    = 0,
    EaseIn    = 1,
    EaseOut   = 2,
    EaseInOut = 3,
};

// TrailComponent — GameObject に追従するトレイルの設定とランタイム状態。
// WHAT: System がリングバッファへ制御点を追加し、毎フレーム GPU 頂点バッファへリボンを展開する。
struct TrailComponent {
    bool enabled = true;

    float duration       = 1.0f;
    int   maxPoints      = 64;
    float sampleInterval = 1.0f / 30.0f;
    float minVertexDist  = 0.02f;

    float widthStart = 0.20f;
    float widthEnd   = 0.02f;
    TrailWidthEasing widthEasing = TrailWidthEasing::Linear;
    math::Vector4 colorStart = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 colorEnd   = { 1.0f, 1.0f, 1.0f, 0.0f };

    TrailAlignment alignment = TrailAlignment::CameraFacing;
    int smoothSubdivisions = 0;
    // attachBone / attachOffset — SkinnedMeshRenderer のボーン GameObject にサンプル位置を追従させる。
    // WHY: 武器の先端や手首など、GameObject 原点以外から Trail を発生させたいケースを Component 単体で扱う。
    std::string attachBone;
    math::Vector3 attachOffset = math::Vector3::ZERO;
    // clearOnDisable — enabled=false 時に点列を即破棄するか、duration による自然消滅を待つか。
    // WHY: ScriptTrailProxy::SetEnabled(false, false) で「記録だけ止めてフェードアウト」を選べるようにする。
    bool clearOnDisable = true;

    // .mat アセットへの参照。albedo テクスチャを .mat から解決する。
    // WHY: テクスチャを .mat に集約することで複数 Trail 間での共有と Editor ピッカーによるアセット管理を可能にする。
    //      空文字のとき texturePath へフォールバックするため既存シーンデータは無変更で動く。
    std::string materialPath;
    // texturePath — deprecated。materialPath が空のときのフォールバック。
    std::string texturePath;
    TrailUVMode uvMode = TrailUVMode::Stretch;
    float uvScrollSpeed = 0.0f;
    float uvTiling      = 1.0f;

    // 固定サイズリングバッファ。maxPoints 変更時は TrailRenderSystem が再初期化する。
    // WHY: std::deque ではなく連続メモリにすることで、毎フレームの走査と頂点展開のキャッシュ効率を保つ。
    std::vector<TrailPoint> pointBuffer;
    int   ringHead       = 0;
    int   ringTail       = 0;
    int   ringCount      = 0;
    float lastSampleTime = -1.0f;

    // GPU リソースは保存対象ではない。Component に持たせることで GameObject 単位の最大頂点数変更に追従する。
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    renderer::ResourceHandle<renderer::ConstantBufferTag> trailCB;
    std::string loadedTexturePath;
    std::string loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。
    int allocatedMaxPoints = 0;
    int allocatedSmoothSubdivisions = 0;

    const char* GetTypeName() const { return "Trail"; }

    // Reflect — Inspector / Serializer から編集・保存する公開パラメータ。
    // WHY: enum は IReflector が直接扱わないため int に変換し、無効値は CameraFacing / WorldUp にクランプする。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("duration", duration);
        r.Field("maxPoints", maxPoints);
        r.Field("sampleInterval", sampleInterval);
        r.Field("minVertexDist", minVertexDist);
        r.Field("widthStart", widthStart);
        r.Field("widthEnd", widthEnd);
        int widthEasingValue = static_cast<int>(widthEasing);
        r.Field("widthEasing", widthEasingValue);
        widthEasingValue = widthEasingValue < 0 ? 0 : (widthEasingValue > 3 ? 3 : widthEasingValue);
        widthEasing = static_cast<TrailWidthEasing>(widthEasingValue);
        r.Field("colorStart", colorStart);
        r.Field("colorEnd", colorEnd);

        int alignmentValue = static_cast<int>(alignment);
        r.Field("alignment", alignmentValue);
        alignmentValue = alignmentValue < 0 ? 0 : (alignmentValue > 1 ? 1 : alignmentValue);
        alignment = static_cast<TrailAlignment>(alignmentValue);

        r.Field("smoothSubdivisions", smoothSubdivisions);
        r.Field("attachBone", attachBone);
        r.Field("attachOffset", attachOffset);
        r.Field("clearOnDisable", clearOnDisable);
        r.Field("materialPath", materialPath);
        r.Field("texturePath", texturePath);
        int uvModeValue = static_cast<int>(uvMode);
        r.Field("uvMode", uvModeValue);
        uvModeValue = uvModeValue < 0 ? 0 : (uvModeValue > 1 ? 1 : uvModeValue);
        uvMode = static_cast<TrailUVMode>(uvModeValue);
        r.Field("uvScrollSpeed", uvScrollSpeed);
        r.Field("uvTiling", uvTiling);
    }
};

} // namespace fbzz::scene
