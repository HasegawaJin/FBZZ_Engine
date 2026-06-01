// FBZZ Engine
// WaterAssetSerializer.hpp | fbzz::scene
// WaterComponent の描画パラメータを .fbzzwater として保存・復元する
//
// WHY: Scene / Prefab にすべての水面パラメータを直接埋め込むとシーンファイルが肥大化し、
//      同じプリセット（Ocean / Lake / River 等）を複数シーンで再利用できない。
//      Terrain と同様に外部アセット化して参照保存することでこれを解消する。
//      シーン側には位置・サイズ情報（extentX/Z, resolutionX/Z）のみ残す。
#pragma once

#include <string>

namespace fbzz::scene {

struct WaterComponent;

// WaterAssetSerializer — WaterComponent の描画プリセットを .fbzzwater に分離保存する。
// WHAT: 色・Fresnel・法線マップ・波・泡・コースティクス等を TOML 形式で外部ファイルに書き出す。
//       読み込み後は meshDirty / foamDirty / texDirty を立てて GPU リソースの再構築をトリガーする。
class WaterAssetSerializer {
public:
    // Save — component の現在のパラメータを path に書き出す。失敗時は false を返す。
    static bool Save(const WaterComponent& component, const std::string& path);

    // Load — path から component にパラメータを読み込む。
    //        読み込み後は dirty フラグを立てる。失敗時は false を返す。
    static bool Load(const std::string& path, WaterComponent& component);
};

} // namespace fbzz::scene
