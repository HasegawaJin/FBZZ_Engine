/// @file    RenderPipelineAsset.hpp
/// @brief   Volume の対象外である描画設定を共有・永続化する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Data/DataAsset.hpp>
#include <Graphics/Renderer/View/RenderSettings.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

struct RenderPipelineAssetCodec;

/// @note Volume の見た目、環境の所有権、プレイヤー設定、エディター診断はこのアセットの管理外。
/// @see https://docs.unity3d.com/6000.1/Documentation/Manual/srp-setting-render-pipeline-asset.html Unity のレンダーパイプライン設定アセット。
class RenderPipelineAsset final : public DataAsset {
public:
    static constexpr const char* TYPE_NAME = "RenderPipelineAsset"; ///< 保存文書と型レジストリーで描画パイプライン設定を識別する名前。
    static constexpr int SCHEMA_VERSION = 1; ///< 保存した描画パイプライン設定のフィールド構成の版。
    const char* GetTypeName() const override { return TYPE_NAME; }
    FBZZ_ENGINE_API void Reflect(scene::IReflector& reflector) override;

    /// @return 永続化する値が不正な場合はアセットを変更しない。
    [[nodiscard]] FBZZ_ENGINE_API bool Capture(const renderer::RenderSettings& source);
    /// @note アセットが所有する項目だけを置き換え、Volume・環境・実行時設定・診断設定は保持する。
    FBZZ_ENGINE_API void ApplyTo(renderer::RenderSettings& target) const;
    [[nodiscard]] const renderer::RenderSettings& Settings() const { return m_settings; }

private:
    friend struct RenderPipelineAssetCodec;
    renderer::RenderSettings m_settings; ///< このアセットが供給する描画パイプラインの制作設定。
    std::string m_preservedToml; ///< スキーマ 1 の未知の項目を編集と保存で保持し、公開 API には TOML の型を出さない。
    std::vector<size_t> m_preservedPassIndices; ///< 構造の編集では元項目の識別を保つ。新規項目を番兵値で示し、名前を変えても別パスの項目は継承しない。
};

/// @pre プロジェクト相対参照や GUID 参照を解決する場合は AssetManager を初期化しておく。
/// @return true なら有効なアセットを選択する。false なら割り当てが空の場合も含め inlineFallback をそのままコピーする。
[[nodiscard]] FBZZ_ENGINE_API bool ResolveRenderPipelineSettings(const renderer::RenderSettings& inlineFallback,
    std::string_view assetReference, renderer::RenderSettings& out);

/// @note 既存ファイルは上書きしない。初回の原子的書き込み前に検証し、失敗時は呼び出し側の割り当てを変更しない。
/// @return 成功後、呼び出し側が既存のアセットブラウザー更新を通じて結果の .meta と GUID を登録する。
[[nodiscard]] FBZZ_ENGINE_API bool CreateRenderPipelineAsset(std::string_view path, const renderer::RenderSettings& source);

} // namespace fbzz::asset
