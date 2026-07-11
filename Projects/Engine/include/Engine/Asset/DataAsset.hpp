// FBZZ Engine
// DataAsset.hpp | fbzz::asset
// 純共有データアセット (Unity の ScriptableObject 相当) の基底とユーザー向けマクロ。
//
// 設計意図 (WHY):
//   調整値を「コンポーネントのインスタンス単位フィールド」で持つと、同種オブジェクトが N 個あると
//   値が N 個に複製され、リバランスが各個編集になる。DataAsset は値を 1 ファイル (.fzdata) に切り出し、
//   複数スクリプトが Asset<T> で同じ実体を共有する。1 か所いじれば参照側すべてに反映される (純共有)。
//
//   実装の肝は「スクリプトと同じリフレクション基盤を流用する」こと。DataAsset は Script と同じ
//   GetTypeName()/Reflect(IReflector&) の仮想インターフェースを持つため、FBZZ_FIELD などの
//   登録マクロ・Inspector の ImGuiReflector・TOML リフレクタをそのまま使い回せる。
//
// 使い方:
//   class EnemyStats : public fbzz::DataAsset {
//       FBZZ_DATA_ASSET(EnemyStats)
//       FBZZ_FIELD_RANGE(float, maxHp, 100.0f, "Max HP", 1.0f, 9999.0f)
//   };
//   FBZZ_REFLECT(EnemyStats)
//
//   // 参照側スクリプト:
//   FBZZ_ASSET(EnemyStats, stats, "Stats")     // Inspector に .fzdata スロットが出る
//   if (stats) health->Init(stats->maxHp);     // 解決は共有キャッシュ経由で自動
#pragma once
#include <Engine/Scene/Script.hpp>            // IReflector / FBZZ_* 登録マクロ / DataAssetRef
#include <Engine/Asset/DataAssetRegistry.hpp> // Asset<T>::Get() の解決先
#include <string>

namespace fbzz {

// 別名: ユーザーは fbzz::DataAsset と書けば済むよう、ここで公開名を与える。
// 実体は fbzz::asset 名前空間。
namespace asset {

class DataAsset {
public:
    virtual ~DataAsset() = default;

    // 型名 (TYPE_NAME)。FBZZ_DATA_ASSET が定義する。.fzdata の "type" キーに保存される。
    virtual const char* GetTypeName() const = 0;

    // フィールドを IReflector へ反映する。FBZZ_REFLECT がフィールド宣言から自動生成する。
    virtual void Reflect(scene::IReflector& r) = 0;
};

// 参照側スクリプトのフィールド型。path で共有実体を解決する型安全ハンドル。
//   - Get()         : 共有実体を T* で返す (未アサイン / 型不一致 / 未存在なら nullptr)。
//   - operator->/*  : if (asset) asset->field; のように Ref<T> と同じ書き味で使える。
template<typename T>
struct Asset {
    scene::DataAssetRef ref;

    // 期待型名を埋めておくことで、Inspector のピッカー絞り込み・ドロップ型チェックが効く。
    Asset() { ref.type = T::TYPE_NAME; }

    [[nodiscard]] T* Get() const
    {
        if (ref.path.empty()) return nullptr;
        DataAsset* a = DataAssetRegistry::Resolve(ref.path);
        // dynamic_cast はスクリプト DLL 内で実体化されるため、DLL 内型同士の RTTI 比較になる。
        return dynamic_cast<T*>(a);
    }

    T* operator->() const { return Get(); }
    T& operator*()  const { return *Get(); }
    explicit operator bool() const { return Get() != nullptr; }
    [[nodiscard]] bool IsAssigned() const { return !ref.path.empty(); }
    void Clear() { ref.path.clear(); }
};

} // namespace asset

// 公開エイリアス: fbzz::DataAsset / fbzz::Asset<T> で使えるようにする。
using asset::DataAsset;
template<typename T> using Asset = asset::Asset<T>;

} // namespace fbzz

// ── ユーザー向けマクロ ───────────────────────────────────────────────────────

// クラス先頭に置く。Reflect() 連鎖の土台・型名・仮想 override を生成する。
// WHY: FBZZ_SCRIPT は Script を一切参照せず、「GetTypeName() const / Reflect(IReflector&) を持つ基底」
//      に対して機能する汎用マクロのため、DataAsset でもそのまま流用できる (DRY)。別名で意図を明示する。
#define FBZZ_DATA_ASSET(T) FBZZ_SCRIPT(T)

// 参照側スクリプトのフィールド宣言。Inspector に .fzdata ドラッグ&ドロップスロットを出す。
// 既定値は不要 (空参照)。Reflect では内包する DataAssetRef を対象にする。
#define FBZZ_ASSET(Type, Name, Display)                                         \
    ::fbzz::asset::Asset<Type> Name{};                                          \
    FBZZ_REFLECT_ENTRY_(Name, r_.Field(FBZZ_DISP_(Display, Name), Name.ref))
