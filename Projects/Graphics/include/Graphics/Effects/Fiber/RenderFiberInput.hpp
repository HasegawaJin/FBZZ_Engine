/// @file    RenderFiberInput.hpp
/// @brief   繊維表面と解決済み材質・接触・流れの描画入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <Graphics/Effects/Fiber/FiberMaterialSettings.hpp>
#include <Graphics/Renderer/Geometry/FiberMotionHistory.hpp>
#include <memory>
#include <Graphics/Renderer/Shaders/ShaderCapabilities.hpp>
#include <Graphics/Renderer/Shaders/ShaderDescriptor.hpp>
#include <vector>
namespace fbzz::renderer {
class PreparedSkinningTicket;
enum class FiberRenderMode { SHELL, FIN, HYBRID, BLADE };
/// @brief 繊維の b5。風・時刻・層数・LOD と局所 FlowField の範囲。
/// @note LAYOUT: Assets/Shaders/Fiber/FiberCommon.hlsli の FiberFrameConstants と一致させる。
/// @note 風はワールド [m/s]、時刻 [s]。lod.x は Blade の提出率、lod.y は根元ディザの残存率 (1 で全部残す)。
/// @note lod.z は Blade の高さ分割数 [1,6]。0 は直接描画する既存呼出元との互換値で 6 として読む。
/// @note lod.w が 1 のときは 1 インスタンスに 1 枚のリボンを割り当て、頂点 ID は高さ段ごとの左右を指す。
struct FiberFrameCB {
    math::Vector3 m_wind; ///< ファイバーの風変形に使うワールド速度 [m/s]。
    float m_time = 0.0f; ///< 風変形と接触を評価する描画時刻 [s]。
    float m_shellCount = 1.0f; ///< この描画で重ねるシェル数。
    float m_hybrid = 0.0f; ///< シェルとフィンを併用する場合は 1。
    float m_turbulence = 0.0f; ///< 風の乱れによる曲げの強さ。
    float m_pulseFrequency = 0.0f; ///< 風の脈動の周波数。
    math::Vector4 m_lod{1,1,0,0}; ///< Blade の提出率、根元ディザの残存率、高さ分割数、インスタンスごとにリボンを割り当てるフラグ。
    /// @note 今フレームは [0, count)、前フレームは [previousFirst, +previousCount)。
    uint32_t m_flowCount = 0; ///< FlowField バッファ先頭から読む当該フレームの件数。
    uint32_t m_flowPreviousFirst = 0; ///< FlowField バッファ内の前フレーム分の開始添字。
    uint32_t m_flowPreviousCount = 0; ///< m_flowPreviousFirst から読む前フレーム分の件数。
    uint32_t m_flowChannels = 0; ///< 変形に取り込む FlowField チャネルのビット集合。
    /// @note X is the minimum dry root height in world metres, Y enables the terrain-only clip.
    math::Vector4 m_terrainWaterClip{}; ///< x は水面より上に残す根元のワールド Y 下限 [m]、y は地形限定クリップの有効フラグ。
};

/// @brief 繊維の b10。描画範囲に届く接触 32 件 × (現在, 前フレーム)。
/// @note LAYOUT: FiberCommon.hlsli の FiberContactConstants と一致させる。radius (centers.w) が 0 の枠は無効。
struct FiberContactCB {
    std::array<math::Vector4,64> m_centers{}; ///< xyz は接触形状のワールド中心 [m]、w は半径 [m]。半径 0 は未使用枠。
    std::array<math::Vector4,64> m_times{}; ///< 接触の開始時刻 [s]、強度、回復時間 [s]、形状の識別値。
    /// @note times.w: 0 = 球、1 = カプセル、2 = 箱。extents.xyz はカプセルのワールド半軸または箱の半寸法 [m]。
    std::array<math::Vector4,64> m_extents{}; ///< カプセルのワールド半軸または箱の半寸法 [m]。
    /// @note 箱の local → world 単位 Quaternion。球とカプセルでは読まない。
    std::array<math::Vector4,64> m_rotations{}; ///< 箱のローカル座標からワールド座標への単位クォータニオン。
    /// @note シェーダーが走査する枠数 [0,32]。今フレームは発生済みで回復しきっていない最後の枠 + 1。
    uint32_t m_count = 0; ///< 当該フレームで走査する接触枠数。最後の有効枠の次まで読む。
    /// @note 前フレームは評価時刻が履歴ごとに違うため時刻で絞らず、半径を持つ最後の枠 + 1。
    uint32_t m_previousCount = 0; ///< 前フレームで半径を持つ最後の接触枠の次までの件数。
    /// @note 現在・前フレームで32件の描画予算から省いた履歴数。GPUでは予約領域。
    uint32_t m_padding[2]{}; ///< 描画予算から省いた現在・前フレームの接触履歴数。GPU 側は予約領域として扱う。
};

/// @brief シェーダーが走査する接触の枠数を詰め直す。
/// @param[in] time 今フレームの評価時刻 [s]。FiberFrameCB::m_time と同じ値を渡す。
/// @note 今フレームは «発生済み (age >= 0) かつ回復しきっていない (age < recovery)» 最後の枠 + 1。途中の無効枠はシェーダー側で飛ばす。
/// @note 前フレームは速度履歴ごとに評価時刻が 1〜2 フレーム前へずれるため、時刻では絞らず半径を持つ最後の枠 + 1。
inline void UpdateFiberContactCounts(FiberContactCB& data, float time)
{
    data.m_count = 0;
    data.m_previousCount = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        const float age = time - data.m_times[i].x;
        const float recovery = data.m_times[i].z < 0.05f ? 0.05f : data.m_times[i].z;
        if (data.m_centers[i].w > 0.0f && age >= 0.0f && age < recovery) {
            data.m_count = i + 1;
        }
        if (data.m_centers[i + 32].w > 0.0f) {
            data.m_previousCount = i + 1;
        }
    }
}


inline constexpr float TERRAIN_SPARSE_BLADE_DENSITY_SCALE = 0.125f;

struct FiberDrawSettings {
    FiberRenderMode m_mode = FiberRenderMode::SHELL; ///< シェル、フィン、併用、草ブレードの描き方を選ぶ。
    int m_shellCount = 16; ///< 近景で重ねるシェル数。
    int m_minShellCount = 8; ///< 距離 LOD でシェルを減らす際の下限。
    int m_shadowShellCount = 6; ///< 影を描くときに重ねるシェル数。
    int m_bladeSegments = 6; ///< 草ブレードの高さ方向の分割数。
    int m_shadowBladeSegments = 0; ///< 影用の高さ分割数。0 は色描画の設定を使う。
    bool m_distanceLod = true; ///< 距離でファイバーの詳細度を変えるかどうか。
    float m_lodNear = 8; ///< ファイバーの距離 LOD を変え始める位置 [m]。
    float m_lodFar = 40; ///< ファイバーの距離 LOD が遠景側へ達する位置 [m]。
    float m_terrainSparseLodStart = 0.0f; ///< 地形の草を間引き始めるカメラからの距離 [m]。
};
struct FiberResolvedGeometry {
    ResourceHandle<BufferTag> m_vertices; ///< この形状を描く GPU 頂点バッファへの非所有ハンドル。
    ResourceHandle<BufferTag> m_indices; ///< この形状を描く GPU インデックスバッファへの非所有ハンドル。
    uint32_t m_indexCount = 0; ///< m_indices から読むインデックス数。
    ResourceHandle<StructuredBufferTag> m_blades; ///< 草ブレードの根元と形状を読む構造化バッファへの非所有ハンドル。
    uint32_t m_bladeCount = 0; ///< m_blades 内の草ブレード描画候補数。
};
/// @note A project family keeps the engine vertex, wind, coverage and 128-byte prefix in every pass.
struct FiberAppearance {
    ShaderVertexContract geometry = ShaderVertexContract::UNKNOWN; ///< 一連の描画シェーダーが共有する頂点・変形の契約。
    /// @note Color/GBuffer/Velocity/Shadow/Selection, each static then skinned; Blade has static entries only.
    std::array<ResourceHandle<ShaderTag>, 10> shaders{}; ///< 色、GBuffer、速度、影、選択の順で、各パスの静的・スキンド変種を並べた配列。
    /// @note Optional Shell companion StemFin family; Hybrid must have all ten compatible entries.
    std::array<ResourceHandle<ShaderTag>, 10> finShaders{}; ///< シェルに組み合わせるフィン変種。併用時は対応する全パスが必要。
    bool supportsHybrid = false; ///< シェルとフィンを同じ外観として描けるかどうか。
    std::vector<uint8_t> params; ///< 材質定数バッファに転送するバイト列。先頭 128 バイトは共通レイアウト。
    ResourceHandle<ConstantBufferTag> materialCB; ///< 各描画パスで共有する材質定数バッファへの非所有ハンドル。
};

[[nodiscard]] constexpr bool MatchesFiberAppearance(ShaderVertexContract geometry, FiberRenderMode mode, bool hasFinFamily = false)
{
    return (geometry == ShaderVertexContract::FIBER_SHELL
            && (mode == FiberRenderMode::SHELL || (mode == FiberRenderMode::HYBRID && hasFinFamily)))
        || (geometry == ShaderVertexContract::FIBER_FIN && mode == FiberRenderMode::FIN)
        || (geometry == ShaderVertexContract::FIBER_BLADE && mode == FiberRenderMode::BLADE);
}


/// @note These offsets form the geometry/deformation ABI. Project fields must follow the first 128 bytes.
[[nodiscard]] inline bool HasFiberMaterialPrefix(const ShaderDescriptor& desc)
{
    if (desc.cbufferSize < sizeof(FiberMaterialSettings)) {
        return false;
    }
    constexpr const char* names[]{"rootColor", "tipColor", "fiberLength", "fiberDensity", "fiberThickness",
        "fiberTaper", "fiberFrequency", "windResponse", "maxBend", "gravityBend", "roughness",
        "specularStrength", "transmission", "rootOcclusion", "planarShading", "worldMapping",
        "finSilhouetteWidth", "specularShift", "secondarySpecular", "colorVariation", "clumping", "clumpTwist"};
    constexpr uint32_t offsets[]{0, 16, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108};
    for (size_t index = 0; index < std::size(names); ++index) {
        const auto* field = desc.FindVar(names[index]);
        if (!field || field->offset != offsets[index] || field->varType != ShaderVarType::Float
            || field->elements || field->rows != 1 || field->columns != (index < 2 ? 4 : 1)) {
            return false;
        }
    }
    for (const auto& texture : desc.textures) {
        if (texture.slot == 5) {
            return texture.constantOffset == 112;
        }
    }
    return false;
}

/// @note One material buffer is shared across a family; every pass and companion must agree on all reflected fields and texture indices.
[[nodiscard]] inline bool HaveMatchingFiberMaterialLayouts(const ShaderDescriptor& first, const ShaderDescriptor& second)
{
    if (first.cbufferSize != second.cbufferSize || first.textureMaskOffset != second.textureMaskOffset
        || first.vars.size() != second.vars.size() || first.textures.size() != second.textures.size()) {
        return false;
    }
    for (const auto& field : first.vars) {
        const auto* other = second.FindVar(field.name);
        if (!other || field.offset != other->offset || field.size != other->size
            || field.rows != other->rows || field.columns != other->columns
            || field.varClass != other->varClass || field.varType != other->varType
            || field.elements != other->elements || field.arrayStride != other->arrayStride
            || field.matrixStride != other->matrixStride || field.rowMajor != other->rowMajor) {
            return false;
        }
    }
    for (const auto& texture : first.textures) {
        bool matched = false;
        for (const auto& other : second.textures) {
            if (texture.name == other.name && texture.slot == other.slot
                && texture.constantOffset == other.constantOffset) { matched = true; break; }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

struct RenderFiberInput {
    uint64_t identity = 0; ///< 当該ファイバー表面の過去姿勢を照合する識別子。
    uint32_t layer = 0; ///< ビューのカリングマスクに照合するレイヤー番号。
    bool selected = false; ///< 選択マスクを描く対象にするかどうか。
    bool skinned = false; ///< ボーン姿勢に従って頂点を変形するかどうか。
    bool cast = true; ///< 光源視点の影パスで描くかどうか。
    bool validPreviousSkin = true; ///< 速度を求めるための前フレームのボーン姿勢が有効かどうか。
    bool hasBounds = false; ///< カリングに使える境界球を持つかどうか。
    float boundsRadius = 0; ///< 描画形状を覆うワールド境界球の半径 [m]。
    math::Vector3 boundsCenter{}; ///< 描画形状を覆うワールド境界球の中心 [m]。
    math::Vector3 lodCenter{}; ///< LOD 判定で視点からの距離を測るワールド空間の基準点。
    PerObjectCB object{}; ///< 形状のワールド変換と描画フラグを渡す定数。
    FiberDrawSettings settings; ///< 形状の生成方式、シェル数、LOD の解決済み設定。
    FiberMaterialSettings material; ///< エンジンのファイバー材質の共通定数。
    std::shared_ptr<const FiberAppearance> appearance; ///< 当該表面で共有する不変のプロジェクト外観。未指定なら既定材質を使う。
    FiberFrameCB frame; ///< この表面に適用する風、時刻、LOD、流れの定数。
    FiberContactCB contacts; ///< 当該フレームと前フレームの局所接触変形の入力。
    ResourceHandle<BufferTag> vertices; ///< 元の表面頂点を読む非所有ハンドル。
    ResourceHandle<BufferTag> indices; ///< 元の表面インデックスを読む非所有ハンドル。
    ResourceHandle<BufferTag> skinnedVertices; ///< スキニングに使う頂点ストリームの非所有ハンドル。
    std::shared_ptr<PreparedSkinningTicket> preparedSkinning; ///< 公開済みの変形入力と GPU 出力の寿命を保つ共有チケット。
    uint32_t preparedSkinningWrite = UINT32_MAX; ///< チケット内の変形出力の添字。UINT32_MAX は未割り当て。
    uint32_t indexCount = 0; ///< indices から読むインデックス数。
    ResourceHandle<ConstantBufferTag> skin; ///< 現在のボーン行列を読む定数バッファ。
    ResourceHandle<ConstantBufferTag> previousSkin; ///< 前フレームのボーン行列を読む定数バッファ。
    ResourceHandle<StructuredBufferTag> flows; ///< 現在・前フレームの FlowField 行を読む構造化バッファ。
    ResourceHandle<TextureTag> velocityField; ///< 局所速度場を読み取るテクスチャの非所有ハンドル。
    FiberResolvedGeometry fins; ///< 今回の表面用に解決したフィン形状。
    FiberResolvedGeometry blades; ///< 今回の表面用に解決した草ブレード形状。
};
}
