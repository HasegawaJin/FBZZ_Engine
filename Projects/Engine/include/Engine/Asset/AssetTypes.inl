/// @file    AssetTypes.inl
/// @brief   Engine が単独所有するアセット型、Importer と再読込方針の共通一覧。
/// @author  Hasegawa Jin
/// @date    2026-10-31
/// @note 複数の登録処理から展開するため include guard を持たない。
FBZZ_ASSET_TYPE(Model, BundledModelImporter, false)
FBZZ_ASSET_TYPE(ModelAsset, ModelAssetImporter, false)
FBZZ_ASSET_TYPE(AnimationClip, AnimationImporter, true)
FBZZ_ASSET_TYPE(Skeleton, SkeletonImporter, false)
FBZZ_ASSET_TYPE(MaterialAsset, MatAssetImporter, true)
FBZZ_ASSET_TYPE(AnimatorControllerAsset, AnimCtrlImporter, true)
FBZZ_ASSET_TYPE(TerrainAsset, TerrainImporter, true)
FBZZ_ASSET_TYPE(TextureAsset, ImageImporter, false)
FBZZ_ASSET_TYPE(IblAsset, IblImporter, false)
FBZZ_ASSET_TYPE(PhysicsMaterialAsset, PhysicsMaterialImporter, true)
FBZZ_ASSET_TYPE(ClothAsset, ClothImporter, true)
FBZZ_ASSET_TYPE(FiberPaintAsset, FiberPaintImporter, true)
FBZZ_ASSET_TYPE(SequenceAsset, SequenceImporter, true)
FBZZ_ASSET_TYPE(fluid::VectorFieldAsset, VectorFieldImporter, true)
FBZZ_ASSET_TYPE(ParticleCurveAsset, ParticleCurveImporter, true)
