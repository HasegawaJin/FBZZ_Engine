/// @file    WorldPcg.hpp
/// @brief   制作時のバイオーム配置プロファイルとセル分割による決定的な生成。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Instances/InstanceSetAsset.hpp>
#include <Engine/Scene/World/WorldCell.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

struct WorldPcgRule {
    std::string key, name, biome, prototypeRef;
    std::vector<asset::InstanceMeshPart> parts;
    std::vector<float> lodHeights{0.2f, 0.08f, 0.02f, 0.0f};
    /// @note 間隔とノイズの尺度は [ワールド m]、傾斜とヨーは [度]。
    float spacing = 8.0f, density = 1.0f;
    /// @note 強度は一様密度からワールド座標に固定した滑らかな値ノイズによる密度への補間量。
    float noiseScale = 128.0f, clusterStrength = 0.0f;
    float minHeight = -100000.0f, maxHeight = 100000.0f;
    float minSlope = 0.0f, maxSlope = 45.0f;
    float minWaterDistance = 0.0f, maxWaterDistance = 1000000.0f;
    float minScale = 0.8f, maxScale = 1.2f;
    float minYaw = 0.0f, maxYaw = 360.0f;
    bool alignToSurface = false;
    bool includeHlod = true;
    /// @note 埋め込みは面法線の逆方向。保護距離はワールド XZ で測る。
    float embedDepth = 0.0f, protectedDistance = 0.0f;
    /// @note patchGroup と noiseScale を共有するルールはワールド座標に固定した同じ生息地マスクを読む。
    std::string patchGroup;
    float minPatch = 0.0f, maxPatch = 1.0f;
    /// @note 距離は [ワールド XZ m]。0 は間隔、水面フェード、隣接距離の上限をそれぞれ無効にする。
    float minSpacing = 0.0f, waterFade = 0.0f;
    std::string neighborRule;
    float minNeighborDistance = 0.0f, maxNeighborDistance = 0.0f;
    /// @see https://dev.epicgames.com/documentation/unreal-engine/open-world-tools-property-reference-in-unreal-engine Epic Open World Tools: Landscape Layers and Overlap Priority.
    std::string includeMaterial, excludeMaterial, competitionGroup;
    float minMaterialWeight = 0.25f, excludeMaterialWeight = 0.35f;
    float competitionRadius = 0.0f, competitionPriority = 0.0f;
    /// @note フェードは各許容範囲の内側にかかる。高さは [ワールド m]、傾斜と方位角は [度]。
    float heightFade = 0.0f, slopeFade = 0.0f;
    /// @note 下り方向の角度は +Z から +X に向けて増える。半角 180 度で方位フィルターを無効にする。
    float aspectHeading = 0.0f, aspectHalfAngle = 180.0f, aspectMinSlope = 3.0f, aspectFade = 0.0f;
    /// @note ソースタグは生成したルートに対応し、セルの上限を適用した後の配置だけを含む。
    std::string canopySourceGroup, canopyGroup;
    float canopyRadius = 0.0f;
    int minCanopyCount = 0, maxCanopyCount = -1;
    bool scaleCompetitionRadius = false;
    /// @note 符号付き水面差と比高は [ワールド m]。負の水面差は水中を示す。
    bool useWaterLevel = false;
    float minWaterOffset = -1000000.0f, maxWaterOffset = 1000000.0f;
    float minNearRelief = -1000000.0f, maxNearRelief = 1000000.0f;
    float minFarRelief = -1000000.0f, maxFarRelief = 1000000.0f;
    float maxCliffDistance = 0.0f, cliffFarScale = 0.45f;
    float minPathDistance = 0.0f, maxPathDistance = 1000000.0f, pathFade = 0.0f;
    /// @note スケールは lerp(minScale,maxScale,u^scaleBias)。1 より大きいと小石が増え、1 で従来のサンプリングを維持する。
    float scaleBias = 1.0f;
    /// @note スケールと密度に依存せず、embedDepth に決定的な変位 [0,embedJitter] をワールドメートルで加える。
    float embedJitter = 0.0f;
    /// @note 符号付き水位帯 [ワールド m] の内側で密度を徐々に変える。useWaterLevel が必要。
    float waterLevelFade = 0.0f;
    /// @note +Y を面法線、+Z を下り方向に揃え、シードで決まるばらつき角 [度] を適用する。平地では制作時のヨーを保つ。
    bool alignDownslope = false;
    float downslopeJitter = 0.0f;

    bool UsesPathBand() const { return minPathDistance > 0.0f || maxPathDistance < 1000000.0f || pathFade > 0.0f; }
    bool UsesNearRelief() const { return minNearRelief > -1000000.0f || maxNearRelief < 1000000.0f; }
    bool UsesFarRelief() const { return minFarRelief > -1000000.0f || maxFarRelief < 1000000.0f; }
};

struct WorldPcgProfile {
    std::string key = "WorldPcg", name;
    WorldCellGrid grid;
    /// @note 埋め込み後のルート位置がワールド XZ の半開区間内にあるかで所属を決める。Y は使わない。
    WorldCellBounds bounds{{0,0,0},{256,0,256}};
    uint64_t seed = 1;
    uint32_t maxInstances = 100000;
    uint64_t maxCandidates = 1000000;
    std::vector<WorldPcgRule> rules;
    /// @note 除外ボックスは境界を含むワールド XYZ の閉区間。
    std::vector<WorldCellBounds> exclusions;
    /// @note grid.origin を基準とする各 128 m の XZ セルで、全ルールを合わせた上限。0 で無効。
    uint32_t maxPlacementsPerCell = 0;
    std::string pathMaterial;
};

struct WorldPcgMaterialWeight {
    std::string_view material;
    float weight = 0.0f;
};

struct WorldPcgSurfaceSample {
    math::Vector3 position{}, normal{0,1,0};
    float waterDistance = 1000000.0f, biomeDensity = 1.0f;
    std::string biome;
    bool water = false, excluded = false;
    /// @note 重みのキーはプロファイル参照。次のサンプラー呼び出しまで読み取れる必要がある。
    std::span<const WorldPcgMaterialWeight> materialWeights;
    /// @note NaN は利用できる生息地データがないことを示す。
    float waterOffset = std::numeric_limits<float>::quiet_NaN();
    float nearRelief = std::numeric_limits<float>::quiet_NaN();
    float farRelief = std::numeric_limits<float>::quiet_NaN();
    float cliffDistance = std::numeric_limits<float>::quiet_NaN();
    float pathDistance = std::numeric_limits<float>::quiet_NaN();
    bool onPath = false;
};

/// @note 呼び出しスレッドで同期実行する。地面に穴があるか取得できない場合は false。
/// @note ワーカーには固定済みの不変サンプルを参照するコールバックを渡す。実行中の Scene は参照しない。
using WorldPcgSampler = std::function<bool(float worldX, float worldZ, WorldPcgSurfaceSample& out)>;
/// @note 地形、材質、マスクの基本フィルター通過後に、必要なら生息地フィールドを埋める。
using WorldPcgHabitatSampler = std::function<void(const WorldPcgRule& rule, WorldPcgSurfaceSample& sample)>;

struct WorldPcgCellGroup {
    WorldCellId cell;
    std::string ruleKey, prototypeRef;
    bool includeHlod = true;
    std::vector<asset::InstanceMeshPart> parts;
    std::vector<float> lodHeights;
    /// @note 位置は WorldCellOrigin(profile.grid,cell) を基準としたローカル値。ID は正の 31 ビットキー。
    std::vector<renderer::InstancePlacement> placements;
    /// @note 完全な候補キーでハッシュ衝突を区別し、制作時の上書きを識別する。
    std::vector<uint64_t> candidateKeys;
};

struct WorldPcgStats {
    uint64_t candidates = 0, accepted = 0, noSurface = 0, biomeRejected = 0;
    uint64_t densityRejected = 0, heightRejected = 0, slopeRejected = 0;
    uint64_t waterRejected = 0, exclusionRejected = 0, protectedRejected = 0;
    uint64_t cellBudgetRejected = 0;
    uint64_t patchRejected = 0, neighborRejected = 0, spacingRejected = 0;
    uint64_t materialRejected = 0, competitionRejected = 0;
    uint64_t aspectRejected = 0;
    uint64_t canopyRejected = 0;
    uint64_t waterLevelRejected = 0, reliefRejected = 0, cliffRejected = 0, pathRejected = 0;
};

struct WorldPcgResult {
    std::vector<WorldPcgCellGroup> groups;
    WorldPcgStats stats;
    std::string fingerprint;
};

inline constexpr std::size_t MAX_WORLD_PCG_DEBUG_SAMPLES = 1024;

struct WorldPcgRejectedSample {
    math::Vector3 position;
    std::string ruleKey;
    /// @note プレビューの棄却件数と共有する固定の理由キー。
    std::string_view reason;
};

struct WorldPcgDiagnostics {
    std::size_t maxSamples = 0;
    std::vector<WorldPcgRejectedSample> rejected;
};

/// @brief 層状の巨岩、周囲の岩屑、岸辺の砂利、水没石、崖下のがれ場のルールを構築する。
/// @note 入力はメートル尺度の形状と接地位置のピボットを持つ岩や小石のプロトタイプ。アセット I/O や Scene の変更は行わない。
/// @note 既定の範囲は 256 m。生成前にプレビューで確認してプロトタイプの倍率と水位帯を調整する。
[[nodiscard]] FBZZ_ENGINE_API WorldPcgProfile CreateRockNaturePcgProfile(std::string_view rockPrototype, std::string_view pebblePrototype);

[[nodiscard]] FBZZ_ENGINE_API bool ValidateWorldPcgProfile(const WorldPcgProfile& profile, std::string& diagnostic);
/// @return 候補やインスタンスの上限超過、ID 衝突を含め、失敗時は out を変更しない。
/// @note ワールド空間に固定したジッター格子により、範囲やセル寸法を変えても既存候補の位置は変わらない。
/// @note 任意の配置上限はフィルター後の候補を決定的に選び、ストリーミングセルは変えない。
/// @note 結果は生成した個体だけを所有し、保護済み配置と手動配置は読み取り専用入力とする。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-generation-modes-in-unreal-engine Epic PCG partitioned generation concepts.
[[nodiscard]] FBZZ_ENGINE_API bool GenerateWorldPcg(const WorldPcgProfile& profile, const WorldPcgSampler& sample,
    std::span<const math::Vector3> protectedPositions, WorldPcgResult& out, std::string& diagnostic,
    const WorldPcgHabitatSampler& sampleHabitat = {});
/// @note 棄却した地表地点を件数制限付きで決定的に抜き出す。地面のない地点は件数だけを記録する。
/// @return 失敗時は out と diagnostics を変更しない。採取サンプルは配置や指紋に影響しない。
[[nodiscard]] FBZZ_ENGINE_API bool GenerateWorldPcg(const WorldPcgProfile& profile, const WorldPcgSampler& sample,
    std::span<const math::Vector3> protectedPositions, WorldPcgResult& out, std::string& diagnostic,
    const WorldPcgHabitatSampler& sampleHabitat, WorldPcgDiagnostics* diagnostics);
/// @return 検証済みの UTF-8 TOML。失敗時は outText を変更しない。ファイル I/O は行わない。
[[nodiscard]] FBZZ_ENGINE_API bool SerializeWorldPcgProfile(const WorldPcgProfile& profile, std::string& outText, std::string& diagnostic);
/// @return 不正なテキストや未知の版では out を変更しない。ファイル I/O は行わない。
[[nodiscard]] FBZZ_ENGINE_API bool DeserializeWorldPcgProfile(std::string_view text, WorldPcgProfile& out, std::string& diagnostic);
/// @note ネイティブ UTF-8 TOML の .pcg プロファイルはルールと表示定義を保持する。生成した配置は保存しない。
/// @return 無効、未知の版、読み取り不能な入力では out を変更しない。
[[nodiscard]] FBZZ_ENGINE_API bool LoadWorldPcgProfile(std::string_view path, WorldPcgProfile& out, std::string& diagnostic);
/// @return 検証後にアトミックに書き込む。検証に失敗したら既存ファイルを置き換えない。
[[nodiscard]] FBZZ_ENGINE_API bool SaveWorldPcgProfile(std::string_view path, const WorldPcgProfile& profile, std::string& diagnostic);

}
