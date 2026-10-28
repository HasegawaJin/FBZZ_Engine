/// @file    SettingsValidationPurpose.cs
/// @brief   編集途中の設定、実行前の要件、保存復元の検証目的を分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
namespace Fbzz.ManagedHost;

/// @note Bridge flags 0/1 は既存の実行前検証/復元との互換を維持する。
public enum SettingsValidationPurpose : uint
{
    RUNTIME = 0,
    SNAPSHOT = 1,
    AUTHORING = 2
}
