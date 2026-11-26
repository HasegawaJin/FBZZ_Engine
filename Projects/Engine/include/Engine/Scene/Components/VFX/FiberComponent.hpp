/// @file    FiberComponent.hpp
/// @brief   静的表面へ毛皮・芝を追加する描画方式と共有マテリアル参照。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Graphics/Effects/Fiber/RenderFiberInput.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Graphics/Renderer/Geometry/FiberMotionHistory.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace fbzz::scene {

using renderer::FiberRenderMode;

/// @note 同じ GameObject の MeshRenderer が根元の表面を持つ。GPU 資源は保存しない。
/// @see Docs/design/fiber-rendering.md
/// @see https://hhoppe.com/fur.pdf
struct FiberComponent {
    bool m_enabled = true; ///< この表面に Fiber の描画を追加する。
    uint64_t m_renderIdentity = 0; ///< レンダラーの履歴と配置キャッシュでこの Fiber 個体を識別する実行時 ID。
    FiberRenderMode m_mode = FiberRenderMode::SHELL; ///< 毛皮・草を Shell、Fin、両者の組み合わせ、または Blade で描く。
    int m_shellCount = 16; ///< 既定は 16 層・距離 LOD 有効。24 層は GBuffer だけで Player 1 体に約 3.5 ms かかった (fiber-rendering.md «負荷と影»)。
    bool m_distanceLod = true; ///< 距離に応じて Fiber の層数を減らし、遠距離で描画を打ち切る。
    int m_minShellCount = 8; ///< 距離 LOD で間引いた Shell の最低層数。
    float m_lodNear = 8.0f; ///< Shell の層数の削減を始める距離（カメラからの m）。
    float m_lodFar = 40.0f; ///< 層数を最小にし、それより遠い Fiber を描画しない距離（カメラからの m）。
    float m_terrainSparseLodStart = 0.0f; ///< 地形 Blade の疎な描画を始める距離 [world m]。0 は従来の距離 LOD。
    math::Vector3 m_terrainGroundColor{0.075f, 0.13f, 0.035f}; ///< 遠景で地形の材質へ混ぜる草の色 (線形 RGB) と強度 [0, 1]。強度 0 は無効。
    float m_terrainGroundCover = 0.0f; ///< 距離により草を間引いた地面へ草の色を混ぜる重み（0〜1）。
    float m_bladeDensity = 400.0f; ///< 葉の候補数の面積密度。チャンク方式ではワールド m²、従来経路ではローカル面積を使う。
    float m_bladeWidth = 0.015f; ///< Blade 1 枚の根元の幅。
    int m_bladeSegments = 6; ///< Blade の高さ分割数。既定 6 は従来の 72 頂点。少ない分割は曲げの輪郭を粗くする。
    int m_shadowBladeSegments = 0; ///< 0 は本描画の分割数を継承。影だけ粗くしても本描画の形状は変えない。
    int m_seed = 1; ///< 同一設定で葉の根元の分布を再現するための乱数シード。
    bool m_chunkedPlacement = false; ///< 新規の静的 Blade 配置。旧シーンは false のまま従来のローカル面積分布を維持する。
    float m_chunkSize = 4.0f; ///< 静的表面の葉の配置とカリングに使うチャンクの辺長（ローカル m）。
    float m_maxSlope = 55.0f; ///< Blade を配置する面の、上向きからの許容最大傾斜（度）。
    int m_maxBlades = 1000000; ///< この Fiber の配置で作成する葉の根元の数の上限。
    std::string m_paintPath; ///< この個体の密度と高さを変える .fiberpaint の参照。空ならペイントなし。
    uint32_t m_placementRootCount = 0; ///< 描画抽出が更新する統計。反射・シリアライズの対象外。
    uint32_t m_placementChunkCount = 0; ///< 前回の描画抽出で生成済みの配置チャンク数。
    bool m_placementFailed = false; ///< 現在の入力で葉の配置キャッシュを作成できなかった。
    int m_terrainPatchCells = 2; ///< 地形 Blade 配置の 1 パッチの辺に含める地形セル数。
    /// @note 地形の層番号。-1 は全面、0 以上はその層の重みで葉を間引く。
    /// @see Docs/design/terrain-layers.md §5 Fiber を層で制御する
    int m_terrainLayer = -1; ///< 葉を置く地形レイヤーの添字。-1 で全面を選ぶ。
    float m_terrainLayerThreshold = 0.25f; ///< 4 隅の層の重みの最大値がこれ未満のセルはパッチから省く [0, 1]。
    int m_flowChannels = -1; ///< 受け取る FlowField のチャンネル (ビットマスク)。-1 は全チャンネル、0 は局所の場を受けない。環境風は常に受ける。
    int m_shadowShellCount = 6; ///< 影だけの層数の上限。影は層の隙間が見えにくく、Shell の負荷の大半を占める (カスケード・光源の面ごとに全層を描く)。
    bool m_castShadows = true; ///< Fiber 自身の投影だけを制御し、元の表面の投影と Fiber の受影は変更しない。
    /// @name 個体ごとの上書き。共有 .mat の値へ掛ける。スクリプトから書き換えてよい
    /// @{
    /// @note 根元色・毛先色の RGB に掛ける (線形)。
    math::Vector3 m_colorTint{ 1.0f, 1.0f, 1.0f }; ///< 材質の根元色と毛先色に掛ける線形 RGB の倍率。
    float m_lengthScale = 1.0f; ///< 共有材質に設定されている毛や葉の長さに掛ける個体別の倍率。
    float m_densityScale = 1.0f; ///< 共有材質の密度に掛ける個体別の倍率。
    float m_clumpingScale = 1.0f; ///< 共有材質の毛束のまとまりに掛ける個体別の倍率。
    std::string m_maskPath; ///< 空なら .mat の fiberMask (tex5)。入れるとこの個体だけ差し替える。
    /// @}
    /// @note 反射・保存の対象外。Play 復元時に古いビューの変形履歴を持ち越さない。
    std::vector<renderer::FiberMotionHistory> m_motionHistories; ///< 時間方向の描画で前フレームと比較するための、ビューごとの変形履歴。
    std::string m_materialPath = "guid:0c678cd29e1841f1b98bd553e2d8c013|Assets/Materials/Fiber/Fur.mat"; ///< Fiber の描画に使う表面材質とは別の専用 .mat 参照。

    const char* GetTypeName() const { return "Fiber"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", m_enabled);
        const char* labels[] = { "Shell", "Fin", "Hybrid", "Blade" };
        int mode = static_cast<int>(m_mode);
        r.Enum("mode", mode, labels);
        m_mode = static_cast<FiberRenderMode>(std::clamp(mode, 0, 3));
        r.IntRange("shellCount", m_shellCount, 1, 64);
        r.Tooltip("Shell の層数。Fin 単独では使用しません。");
        r.Field("distanceLod", m_distanceLod);
        r.Tooltip("lodNear〜lodFar で層数を minShellCount まで減らし、lodFar より遠くは描きません。");
        r.IntRange("minShellCount", m_minShellCount, 1, 64);
        r.FloatRange("lodNear", m_lodNear, 0.0f, 1000.0f);
        r.FloatRange("lodFar", m_lodFar, 0.1f, 2000.0f);
        r.FloatRange("terrainSparseLodStart", m_terrainSparseLodStart, 0.0f, 2000.0f);
        r.Tooltip("地形 Blade の根元を 1/8 にし、高さ分割を 1 にする距離 [m]。0 または lodNear〜lodFar の範囲外なら無効。");
        r.FloatRange("terrainGroundCover", m_terrainGroundCover, 0.0f, 1.0f);
        r.Tooltip("地形 Blade の距離 LOD で、草が減る範囲から地面に草の色を混ぜる強さ。");
        r.Field("terrainGroundColor", m_terrainGroundColor);
        r.Tooltip("遠景の草の地表色 (線形 RGB)。terrainLayer のペイントに従います。");
        r.FloatRange("bladeDensity", m_bladeDensity, 0.001f, 2000.0f);
        // km 規模の地形は疎な候補密度を使い、距離カリング前の根元バッファを抑える。
        r.Tooltip("Blade の本数 / 面積。chunkedPlacement はワールド m²、旧方式はローカル面積です。");
        r.FloatRange("bladeWidth", m_bladeWidth, 0.001f, 0.5f);
        r.IntRange("bladeSegments", m_bladeSegments, 1, 6);
        r.Tooltip("Blade の高さ分割。6 = 従来品質、3 = 半分の頂点、1 = 最小。強い風や接写では曲がり方の差が見えます。");
        r.IntRange("shadowBladeSegments", m_shadowBladeSegments, 0, 6);
        r.Tooltip("影の Blade 分割。0 = 本描画を継承。影だけ軽くする場合は 1〜6 を明示します。");
        r.IntRange("seed", m_seed, 0, 65535);
        r.Field("chunkedPlacement", m_chunkedPlacement);
        r.Tooltip("静的 Mesh の草を内部チャンクへ分割します。草を追加すると自動で有効になります。");
        r.FloatRange("chunkSize", m_chunkSize, 0.025f, 128.0f);
        r.Tooltip("草のチャンクサイズ / ローカル m。元の Mesh は分割しません。");
        r.FloatRange("maxSlope", m_maxSlope, 0.0f, 90.0f);
        r.Tooltip("上向き面からの最大傾斜 / 度。側面・裏面への配置を制限します。");
        r.IntRange("maxBlades", m_maxBlades, 1, 2000000);
        r.FileField("paintPath", m_paintPath, ".fiberpaint");
        r.Tooltip("この個体の密度・高さペイント。空なら全面へ配置します。UV には依存しません。");
        r.IntRange("terrainPatchCells", m_terrainPatchCells, 1, 32);
        r.IntRange("terrainLayer", m_terrainLayer, -1, 254);
        r.Tooltip("Terrain の層番号。-1 = 地形全体。0 以上ならその層の重みで生やします。");
        r.FloatRange("terrainLayerThreshold", m_terrainLayerThreshold, 0.0f, 1.0f);
        r.Tooltip("セルの 4 隅の層の重みの最大値がこの値未満なら、そのセルには生やしません。");
        r.Field("flowChannels", m_flowChannels);
        r.Tooltip("受け取る Flow Field のチャンネル (ビットマスク)。-1 で全チャンネル、0 で局所の場を無視します。環境風は常に受けます。");
        r.FileField("materialPath", m_materialPath, ".mat");
        r.Tooltip("Fiber マテリアル。Mesh Renderer / Skinned Mesh Renderer / Terrain と併用します。");
        r.Field("castShadows", m_castShadows);
        r.Tooltip("毛・草自身が影を落とすかを切り替えます。");
        r.IntRange("shadowShellCount", m_shadowShellCount, 1, 64);
        r.Tooltip("影に描く Shell の層数の上限。影は層の隙間が目立たず、層を減らすと負荷が大きく下がります。");
        r.Field("colorTint", m_colorTint);
        r.Tooltip("この個体だけ根元色・毛先色に掛ける色 (1 で .mat のまま)。");
        r.FloatRange("lengthScale", m_lengthScale, 0.0f, 4.0f);
        r.FloatRange("densityScale", m_densityScale, 0.0f, 4.0f);
        r.FloatRange("clumpingScale", m_clumpingScale, 0.0f, 4.0f);
        r.Tooltip("長さ・密度・毛束を .mat の値に掛けます。密度と毛束は掛けた結果が 1 で頭打ちです。");
        r.FileField("maskPath", m_maskPath, ".png");
        r.Tooltip("この個体だけ fiberMask を差し替えます (R=密度 G=長さ B=毛束 A=毛先色)。空なら .mat の tex5。");
    }
};

}
