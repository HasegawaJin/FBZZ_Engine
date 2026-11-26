/// @file    WorldPcgAuthoringComponent.hpp
/// @brief   保存対象の PCG 領域と、再生成の検証に使う生成物の所有関係。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct WorldHlodSourceComponent {
    bool includeHlod = true; ///< このオブジェクトをセルの遠景 HLOD のベイク対象に含める。
    bool farOnlyMesh = false; ///< このメッシュを遠景用の代替形状にし、近景ロードでは非表示にする。
    bool omitTerrainCollisionInHlod = false; ///< 遠景 HLOD の地形データから当たり判定を省く。
    const char* GetTypeName() const { return "WorldHlodSource"; }
    void Reflect(IReflector& r)
    {
        r.Field("includeHlod", includeHlod);
        r.Field("farOnlyMesh", farOnlyMesh);
        r.Tooltip("Use a standalone MeshRenderer as an authored cell HLOD proxy. The near payload hides this mesh.");
        r.Field("omitTerrainCollisionInHlod", omitTerrainCollisionInHlod);
    }
};

struct WorldBiomeVolumeComponent {
    bool enabled = true; ///< このバイオーム領域を環境物の手続き生成の選択対象にする。
    std::string biome = "forest"; ///< 生成プロファイルのルールと照合するバイオーム名。
    math::Vector3 boundsMin{}; ///< バイオームの適用範囲の最小ワールド座標。
    math::Vector3 boundsMax{256,100000,256}; ///< バイオームの適用範囲の最大ワールド座標。
    float density = 1.0f; ///< このバイオーム領域内で生成密度に掛ける倍率。
    int priority = 0; ///< 複数のバイオームが重なる箇所で採用する領域を決める優先度。
    const char* GetTypeName() const { return "WorldBiomeVolume"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled); r.Field("biome", biome);
        r.Field("boundsMin", boundsMin); r.Field("boundsMax", boundsMax);
        r.Field("density", density); r.Field("priority", priority);
    }
};

struct WorldPcgVolumeComponent {
    bool enabled = true; ///< この領域の設定を手続き生成処理に使用する。
    std::string profilePath; ///< 生成ルールや対象領域の既定値を定義する生成プロファイルへの参照。
    bool overrideBounds = false; ///< 生成プロファイルの境界の代わりにこのコンポーネントの設定値を使う。
    math::Vector3 boundsMin{}; ///< 生成範囲に含むワールド XZ の最小境界。Y は判定に使わない。
    math::Vector3 boundsMax{256,0,256}; ///< 生成範囲に含まないワールド XZ の最大境界。Y は判定に使わない。
    bool overrideSeed = false; ///< 生成プロファイルの乱数シードを seed で上書きする。
    int seed = 1; ///< 領域を再生成する際の決定論的な乱数シードの上書き値。
    bool protectManual = true; ///< 明示的に手動編集として保護した生成物を再生成で上書きしない。
    const char* GetTypeName() const { return "WorldPcgVolume"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("profilePath", profilePath);
        r.Field("overrideBounds", overrideBounds);
        r.Field("boundsMin", boundsMin); r.Field("boundsMax", boundsMax);
        r.Field("overrideSeed", overrideSeed); r.Field("seed", seed);
        r.Field("protectManual", protectManual);
    }
};

struct WorldPcgGeneratedComponent {
    std::string volumeId; ///< このオブジェクトを生成した手続き生成領域の識別子。
    std::string ruleKey; ///< このオブジェクトの生成時に適用したプロファイルのルール名。
    std::string signature; ///< 生成結果の照合に使う入力の署名。
    int cellX = 0; ///< このオブジェクトの生成先セルの X 添字。
    int cellZ = 0; ///< このオブジェクトの生成先セルの Z 添字。
    bool manualOverride = false; ///< 保護された生成物は、所有者を再生成する前に明示的に切り離す。
    bool includeHlod = true; ///< 生成したオブジェクトをセルの遠景 HLOD に含める。
    const char* GetTypeName() const { return "WorldPcgGenerated"; }
    void Reflect(IReflector& r)
    {
        r.Field("volumeId", volumeId); r.Field("ruleKey", ruleKey);
        r.Field("cellX", cellX); r.Field("cellZ", cellZ);
        r.Field("signature", signature); r.Field("manualOverride", manualOverride);
        r.Field("includeHlod", includeHlod);
    }
};

}
