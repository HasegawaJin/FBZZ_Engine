/// @file SdkNotice.ts
/// @brief 起動に使う SDK と GameHub・選択中 SDK の版を比較する。
/// @author Hasegawa Jin
/// @date 2026-09-25

import { compareVersions } from './contracts';

/// @note プロジェクトの固定先は設定より優先される。未知の ID は旧版と断定しない。
export function outdatedSdkNotice(projectSdkId: string, selectedSdkId: string, hubVersion: string): string | null {
  const sdkId = projectSdkId.trim() || selectedSdkId.trim();
  const valid = (value: string) => /^v?\d+\.\d+\.\d+(?:-[\w.-]+)?$/i.test(value);
  if (!valid(sdkId)) return null;
  const reference = valid(selectedSdkId) && (compareVersions(selectedSdkId, hubVersion) ?? 0) > 0
    ? selectedSdkId : hubVersion;
  if (!valid(reference) || (compareVersions(sdkId, reference) ?? 0) >= 0) return null;
  const source = projectSdkId.trim() ? 'プロジェクトに固定された' : '設定で選択中の';
  return `${source} SDK ${sdkId} は ${reference} より古い版です。新しい版の修正は反映されません。`
    + (projectSdkId.trim() ? ' 設定の SDK を変えても固定先は変わりません。.fbzz_proj の engine.sdk_id を確認してください。' : ' 設定から使用する SDK を確認してください。');
}
