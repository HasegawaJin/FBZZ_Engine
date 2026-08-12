# Sprite AssetBrowser / Script SerializeField / Material API

更新日: 2026-07-31  
対象: `Projects/Engine/` / `Projects/Editor/` / `Projects/Tests/`

FBZZ Script の宣言だけを正本として、Inspector、Scene / Prefab 保存、Undo、
Hot Reload、AI schema が同じフィールド定義へ追従する Unity 相当の authoring 体験を提供する。
Material 操作は共有 `.mat` と GameObject 単位の runtime override を分離し、
スクリプト DLL 境界へ Engine 実装型を公開しない。

## 実装チェックリスト

### Sprite AssetBrowser

- [x] 元Textureをatlas全体の見た目で表示する ✅
- [x] Sprite Textureに展開トグルを表示する ✅
- [x] 展開時にSprite数分の仮想サブアセットを元素材の直後へ表示する ✅
- [x] 各SpriteRectをUVで切り抜いたIconを表示する ✅
- [x] `texture::sprite::stable-id` 形式でD&D可能な参照を提供する ✅
- [x] Sprite名変更後もstable IDと旧名aliasで参照を維持する ✅
- [x] 選択中は素材を塗り潰さず、細いaccentと軽いhoverだけを表示する ✅
- [x] Sprite参照と`.meta`の回帰テストを追加する ✅

### SerializeField schema

- [x] 永続キーと Inspector 表示名を分離する ✅
- [x] 旧フィールド名を読む `FBZZ_FIELD_MIGRATED` を提供する ✅
- [x] 旧 Script 型名を読む `FBZZ_SCRIPT_FORMERLY_NAMED` を提供する ✅
- [x] 未知・欠落 Script の保存済みデータを維持する ✅
- [x] Hot Reload 前後でScene snapshot経由のフィールド値を維持する ✅
- [x] Undo snapshotとAI schemaも同じ永続キー・旧名を使用する ✅

### 型付き Asset 参照とコンテナ

- [x] `MaterialRef` ✅
- [x] `TextureRef` ✅
- [x] `SpriteRef` ✅
- [x] `AudioClipRef` ✅
- [x] `AnimationClipRef` ✅
- [x] `SceneRef` ✅
- [x] `ShaderRef` ✅
- [x] GUID ベース保存と移動後のパス解決 ✅
- [x] Inspector の型フィルター付き D&D / Picker ✅
- [x] `std::vector<T>` の保存、復元、Inspector編集 ✅
- [x] `std::vector<EntityRef>` の保存、復元、Inspector編集 ✅
- [x] 固定長 `std::array<T, N>` の保存、復元、Inspector編集 ✅
- [x] ネストした serializable struct ✅
- [x] Enum flags ✅
- [x] ポリモーフィックデータの `SerializeReference` 相当 ✅
- [x] 未登録のポリモーフィック型の保存データを維持する ✅

### Inspector 属性

- [x] `Range` ✅
- [x] `Min` ✅
- [x] `Step` ✅
- [x] `Tooltip` ✅
- [x] `Group / Header` ✅
- [x] `Space` ✅
- [x] `Multiline / TextArea` ✅
- [x] `Color` ✅
- [x] `Angle` ✅
- [x] `LayerMask` ✅
- [x] `Tag` ✅
- [x] `ShowIf / EnableIf` ✅
- [x] `ReadOnly` ✅
- [x] `HideInInspector` ✅
- [x] `FileExtension` ✅
- [x] `AssetType` ✅
- [x] Reorderable List ✅

### Lifecycle

- [x] `Reset` ✅
- [x] `OnValidate` ✅
- [x] `OnBeforeSerialize` ✅
- [x] `OnAfterDeserialize` ✅
- [x] Script追加時に `Reset` / `OnValidate` を通知する ✅
- [x] Inspector変更とUndo / Redoで `OnValidate` を通知する ✅
- [x] Scene読込とHot Reloadでdeserialize lifecycleを通知する ✅

### Material API

- [x] `MaterialComponent*` / `GameObject*` を Script API から除去する ✅
- [x] opaqueな `MaterialInstance` を提供する ✅
- [x] shared material割当とinstance overrideを分離する ✅
- [x] material slotをAPIで指定し、現行single-material rendererではslot 0だけを受理する ✅
- [x] `MaterialPropertyId` とComponent cacheで文字列・reflection検索をキャッシュする ✅
- [x] Float / Int / Vector3 / Vector4 / Color / Texture のSet / Get ✅
- [x] 個別・全overrideの解除 ✅
- [x] Shader reflectionによる型・存在検証と失敗ログ ✅
- [x] Blend mode / Double sided / Render queueのinstance override ✅
- [x] `EntityRef` で子・別GameObjectを対象にできる ✅

### 品質確認

- [x] Sprite stable ID / 旧名 / `.meta` round-tripテストを追加する ✅
- [x] Scene保存キー / 欠落Script維持テストを追加する ✅
- [x] Script旧名alias / Asset GUID保存テストを追加する ✅
- [x] Material property ID / shared-instance分離テストを追加する ✅
- [ ] Visual Studio 2022からEditor全体とテストをビルド・実行する

## 使用例

```cpp
class PlayerComponent final : public fbzz::scene::Script {
public:
    FBZZ_SCRIPT(PlayerComponent)
    FBZZ_SCRIPT_FORMERLY_NAMED("LegacyPlayerComponent")

    FBZZ_FIELD_MIGRATED(float, moveSpeed, 5.0f, "Move Speed", "speed")
    FBZZ_FIELD_RANGE(float, stamina, 100.0f, "Stamina", 0.0f, 100.0f)
    FBZZ_FIELD_TAG(targetTag, "Enemy", "Target Tag")
    FBZZ_ASSET_FIELD(fbzz::scene::MaterialRef, hitMaterial, "Hit Material")
    FBZZ_LIST_FIELD(fbzz::math::Vector3, patrolPoints, "Patrol Points")

    void OnValidate() override
    {
        moveSpeed = (std::max)(0.0f, moveSpeed);
    }
};

FBZZ_REFLECT(PlayerComponent)
```

```cpp
constexpr fbzz::scene::MaterialPropertyId kHitColor("hitColor");

void PlayerComponent::ApplyHitFlash()
{
    const auto instance = material.Instance();
    instance.SetColor(kHitColor, { 1.0f, 0.2f, 0.1f, 1.0f });
}

void PlayerComponent::AssignSharedMaterial()
{
    material.SetSharedMaterial(hitMaterial);
}
```

ネスト型は `IScriptSerializable` を継承して `FBZZ_SERIALIZABLE` / `FBZZ_REFLECT` を使う。
`FBZZ_SERIALIZE_REFERENCE` から選択する派生型は `FBZZ_REGISTER_SERIALIZABLE` で登録する。

## 設計原則

1. 保存キーはC++メンバー名を既定とし、表示名変更で保存データを壊さない。
2. renameは旧キーaliasで明示的に移行し、保存時は新キーだけを書く。
3. Asset参照はGUIDを正本とし、表示とruntime load時だけパスへ解決する。
4. Script APIはPOD値、`EntityRef`、typed AssetRef、opaque handleだけを公開する。
5. runtimeのMaterial変更はGameObject単位のoverrideとし、共有Asset割当は明示APIに限定する。
6. Inspector専用metadataをSceneデータへ混ぜない。
7. Reflect宣言をInspector、Serializer、Undo、AI schemaの唯一の正本にする。
