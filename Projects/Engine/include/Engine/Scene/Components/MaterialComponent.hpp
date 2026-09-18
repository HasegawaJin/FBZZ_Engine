/// @file    MaterialComponent.hpp
/// @brief   GameObject が参照する .mat マテリアルアセットと GPU Material キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/Script.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

/// MaterialSlot — submesh 1 つ分のマテリアル参照・GPU キャッシュ・インスタンス上書き。
/// @note MaterialComponent はこれを継承し「スロット 0」を自分自身として公開することで、
///       単一マテリアル前提の既存コード (mc.materialPath など) をそのまま動かす。
struct MaterialSlot {
    /// 共有 MaterialAsset から解決した GPU 側マテリアル。SyncMaterial が生成・更新する。
    std::unique_ptr<renderer::Material> material;

    asset::AssetHandle<asset::MaterialAsset> materialAsset;

    /// この submesh を描画するか。false のスロットはスキップされる。
    /// @note コンポーネント全体の有効/無効 (MaterialComponent::enabled) とは別軸で、
    ///       モデルの一部だけを出す (VFX の AnimatedMesh、装備の表示切替) 用途に使う。
    bool visible = true;

    /// .mat の assets/ 相対パス。空文字は「マテリアル未割当」。
    /// @note MaterialSlot は参照だけを持ち、シェーダー・パラメータ・テクスチャは共有アセット側へ集約する。
    std::string materialPath;

    /// オブジェクトごとのパラメータ上書き (シェーダー変数名 → float 値配列)。ランタイム専用 (非シリアライズ)。
    /// @note MaterialAsset はパス単位で共有され、そこへ書くと同じ .mat の全インスタンスへ波及する。
    ///       SyncMaterial が共有アセット適用後にここで「この GO 専用」に上書きするため、
    ///       ディゾルブ量や色をインスタンス単位でアニメーションできる。
    std::unordered_map<std::string, std::vector<float>> paramOverrides;
    /// Textureと描画状態も共有.matを変更せず、GameObject単位で上書きする。
    std::unordered_map<std::string, std::string> textureOverrides;
    /// PropertyId のhashから実名を引き、毎フレームの文字列生成と線形検索を避ける。
    /// hash衝突時は呼び出し側が実名を照合して上書きする。
    std::unordered_map<uint64_t, std::string> propertyNameCache;
    /// bitはMaterialInstanceのPropertyKindごとのShader reflection検証済み状態。
    std::unordered_map<uint64_t, uint8_t> propertyValidationCache;
    /// Shader descriptorがhot reloadで差し替わったら検証cacheを破棄する非所有識別子。
    const void* propertyValidationDescriptor = nullptr;

    bool hasBlendModeOverride = false;
    renderer::BlendMode blendModeOverride = renderer::BlendMode::OPAQUE_BLEND;
    bool hasDoubleSidedOverride = false;
    bool doubleSidedOverride = false;
    bool hasRenderQueueOverride = false;
    int32_t renderQueueOverride = renderer::RenderQueue::GEOMETRY;

    MaterialSlot() = default;
    ~MaterialSlot() = default;

    /// @note material は unique_ptr のため既定のコピーが作れない。設定だけを写し、
    ///       ConstantBuffer (所有者を 1 つに保つため) と検証キャッシュ (shader descriptor の
    ///       差し替え検知のため) は複製先で作り直す。
    MaterialSlot(const MaterialSlot& o)
        : material(o.material ? std::make_unique<renderer::Material>(o.material->CloneWithoutGpuResources()) : nullptr)
        , materialAsset(o.materialAsset)
        , visible(o.visible)
        , materialPath(o.materialPath)
        , paramOverrides(o.paramOverrides)
        , textureOverrides(o.textureOverrides)
        , propertyNameCache(o.propertyNameCache)
        , hasBlendModeOverride(o.hasBlendModeOverride)
        , blendModeOverride(o.blendModeOverride)
        , hasDoubleSidedOverride(o.hasDoubleSidedOverride)
        , doubleSidedOverride(o.doubleSidedOverride)
        , hasRenderQueueOverride(o.hasRenderQueueOverride)
        , renderQueueOverride(o.renderQueueOverride)
    {}
    MaterialSlot& operator=(const MaterialSlot& o)
    {
        if (this != &o) {
            material     = o.material ? std::make_unique<renderer::Material>(o.material->CloneWithoutGpuResources()) : nullptr;
            materialAsset = o.materialAsset;
            visible      = o.visible;
            materialPath = o.materialPath;
            paramOverrides = o.paramOverrides;
            textureOverrides = o.textureOverrides;
            propertyNameCache = o.propertyNameCache;
            propertyValidationCache.clear();
            propertyValidationDescriptor = nullptr;
            hasBlendModeOverride = o.hasBlendModeOverride;
            blendModeOverride = o.blendModeOverride;
            hasDoubleSidedOverride = o.hasDoubleSidedOverride;
            doubleSidedOverride = o.doubleSidedOverride;
            hasRenderQueueOverride = o.hasRenderQueueOverride;
            renderQueueOverride = o.renderQueueOverride;
        }
        return *this;
    }
    MaterialSlot(MaterialSlot&&)            = default;
    MaterialSlot& operator=(MaterialSlot&&) = default;

    /// materialPath が設定されていれば AssetManager 経由で共有 MaterialAsset を解決する。
    /// @note BlendMode / RenderQueue は描画キュー振り分け前に必要なため、SyncMaterial より前でも呼べる。
    bool EnsureMaterialAsset()
    {
        if (!materialAsset.IsValid() && !materialPath.empty())
            materialAsset = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
        return materialAsset.IsValid();
    }

    renderer::BlendMode GetBlendMode() const
    {
        if (hasBlendModeOverride) return blendModeOverride;
        const auto* a = asset::AssetManager::Get<asset::MaterialAsset>(materialAsset);
        return a ? a->blendMode : renderer::BlendMode::OPAQUE_BLEND;
    }

    bool IsDoubleSided() const
    {
        if (hasDoubleSidedOverride) return doubleSidedOverride;
        const auto* a = asset::AssetManager::Get<asset::MaterialAsset>(materialAsset);
        return a ? a->doubleSided : false;
    }

    int32_t GetRenderQueue() const
    {
        if (hasRenderQueueOverride) return renderQueueOverride;
        const auto* a = asset::AssetManager::Get<asset::MaterialAsset>(materialAsset);
        return a ? a->renderQueue : renderer::RenderQueue::GEOMETRY;
    }

    const std::string& GetShaderPath() const
    {
        static const std::string empty;
        const auto* a = asset::AssetManager::Get<asset::MaterialAsset>(materialAsset);
        return a ? a->shaderPath : empty;
    }

    asset::MeshType GetMeshType() const
    {
        const auto* a = asset::AssetManager::Get<asset::MaterialAsset>(materialAsset);
        return a ? a->meshType : asset::MeshType::Any;
    }

    /// .mat の割り当てを差し替える。解決済みハンドルと GPU キャッシュを捨てて再解決させる。
    void SetMaterialPath(std::string path)
    {
        if (materialPath == path) return;
        materialPath = std::move(path);
        materialAsset = {};
        material.reset();
        propertyValidationCache.clear();
        propertyValidationDescriptor = nullptr;
    }
};

/// MaterialComponent — GameObject に付く 1 個以上のマテリアルスロット。
/// スロット i は Renderer が描く submesh i に対応する。
/// @note MaterialSlot を継承し「スロット 0 = コンポーネント自身」とすることで、
///       mc.materialPath / mc.GetBlendMode() 等の既存コードが無変更で主スロットを指す。
struct MaterialComponent : MaterialSlot {
    /// コンポーネント全体の有効/無効。false なら全スロットが描画されない。
    bool enabled = true;

    /// submesh 1 以降のスロット。スロット 0 は基底の MaterialSlot 部分が兼ねる。
    /// @note 大多数のオブジェクトは submesh 1 つなので、追加スロットだけを可変長で持ち
    ///       1 マテリアルのケースでヒープ確保が発生しないようにする。
    std::vector<MaterialSlot> extraSlots;

    MaterialComponent() = default;

    [[nodiscard]] size_t SlotCount() const { return 1u + extraSlots.size(); }

    /// 生のスロット参照 (フォールバックなし)。Inspector / シリアライザなど
    /// 「スロットそのもの」を編集したい側が使う。
    [[nodiscard]] MaterialSlot& RawSlotAt(size_t index)
    {
        if (index == 0 || index > extraSlots.size()) return *this;
        return extraSlots[index - 1u];
    }
    [[nodiscard]] const MaterialSlot& RawSlotAt(size_t index) const
    {
        if (index == 0 || index > extraSlots.size()) return *this;
        return extraSlots[index - 1u];
    }

    /// submesh index に対応する「描画に使う」スロットを返す。
    /// 範囲外、または .mat 未割当のスロットは主スロット (0) へフォールバックする。
    /// @note submesh 数とスロット数がずれても画面から消えないための保険 (モデル差し替えで
    ///       submesh が増えた・追加スロットへの割り当て忘れ)。非表示指定 (visible=false) の
    ///       スロットはフォールバックせずそのまま返し、呼び出し側にスキップさせる。
    [[nodiscard]] MaterialSlot& SlotAt(size_t index)
    {
        MaterialSlot& slot = RawSlotAt(index);
        if (!slot.visible || !slot.materialPath.empty()) return slot;
        return *this;
    }
    [[nodiscard]] const MaterialSlot& SlotAt(size_t index) const
    {
        const MaterialSlot& slot = RawSlotAt(index);
        if (!slot.visible || !slot.materialPath.empty()) return slot;
        return *this;
    }

    /// スロット数を count (>=1) に揃える。増やした分は未割当スロットになる。
    void ResizeSlots(size_t count)
    {
        extraSlots.resize(count > 0 ? count - 1u : 0u);
    }

    /// 指定 submesh だけを表示する。index < 0 なら全 submesh を表示。
    /// @note VFX の AnimatedMesh のように「モデルの一部だけを出す」用途を、Renderer 側へ
    ///       submesh 指定を戻さずに実現する。
    void SetOnlyVisibleSlot(int index)
    {
        for (size_t i = 0; i < SlotCount(); ++i)
            RawSlotAt(i).visible = index < 0 || static_cast<size_t>(index) == i;
    }

    const char* GetTypeName() const { return "Material"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("materialPath", materialPath);
        /// @note 追加スロットは可変長配列で IReflector の Field では表現できないため、
        ///       SceneSerializer が "materialSlots" 配列として別途読み書きする。
    }
};

} // namespace fbzz::scene
