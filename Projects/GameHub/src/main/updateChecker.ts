/**
 * @file updateChecker.ts
 * @brief GitHub Releases の最新版を問い合わせ、知らせるべき新しい版を決める。
 * @author Hasegawa Jin
 * @date 2026/09/19
 * @see Docs/design/gamehub-update-notice.md
 */

import { net } from 'electron';
import { compareVersions, type UpdateNotice } from '../shared/contracts';

/** hub_config.toml の [update] に残す問い合わせ結果。 */
export interface UpdateState {
  lastCheckedAt: string;
  latestVersion: string;
  latestUrl: string;
  latestNotes: string;
  dismissedVersion: string;
}

export const EMPTY_UPDATE_STATE: UpdateState = {
  lastCheckedAt: '',
  latestVersion: '',
  latestUrl: '',
  latestNotes: '',
  dismissedVersion: '',
};

/**
 * 最新 1 件だけを返す。下書きとプレリリースは含まれない。
 * @see https://docs.github.com/en/rest/releases/releases#get-the-latest-release Get the latest release
 */
const LATEST_RELEASE_API = 'https://api.github.com/repos/HasegawaJin/FBZZ_Engine/releases/latest';
const RELEASE_PAGE_PREFIX = 'https://github.com/HasegawaJin/FBZZ_Engine/releases/';
/**
 * @note 未認証の GitHub API は 1 時間 60 回まで。起動のたびに消費しないよう 1 日 1 回にする。
 * @see https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api Rate limits for the REST API
 */
const CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000;
const REQUEST_TIMEOUT_MS = 8_000;
const NOTES_MAX_LINES = 3;
const NOTES_MAX_CHARS = 280;

/** 前回の問い合わせから 24 時間以内か。 */
export function isFresh(state: UpdateState, nowMs: number): boolean {
  const checkedAt = Date.parse(state.lastCheckedAt);
  return Number.isFinite(checkedAt) && nowMs - checkedAt >= 0 && nowMs - checkedAt < CHECK_INTERVAL_MS;
}

/** このリポジトリのリリースページか。shell.openExternal へ渡す前の検問。 */
export function isReleasePageUrl(url: string): boolean {
  return url.startsWith(RELEASE_PAGE_PREFIX) && !/[\s"'<>]/.test(url);
}

/** リリースノート (Markdown) の冒頭を、見出し記号を外したプレーンテキストにする。 */
export function summarizeNotes(body: string): string {
  const lines = body
    .split(/\r?\n/)
    .map((line) => line.replace(/^#+\s*/, '').replace(/^[-*]\s+/, '・').trim())
    .filter(Boolean)
    .slice(0, NOTES_MAX_LINES)
    .join('\n');
  return lines.length > NOTES_MAX_CHARS ? `${lines.slice(0, NOTES_MAX_CHARS - 1)}…` : lines;
}

/** 保存済みの結果から、知らせるべき新しい版を返す。今の版以下・閉じた版なら null。 */
export function noticeFrom(state: UpdateState, currentVersion: string): UpdateNotice | null {
  if (!state.latestVersion || state.latestVersion === state.dismissedVersion) return null;
  const difference = compareVersions(state.latestVersion, currentVersion);
  if (difference === null || difference <= 0) return null;
  return { currentVersion, latestVersion: state.latestVersion, notes: state.latestNotes };
}

/**
 * 最新のリリースを問い合わせる。
 * @return 失敗 (オフライン・タイムアウト・形式違い) なら null。呼び出し側は確認時刻を更新しない。
 */
export async function fetchLatestRelease(): Promise<Pick<UpdateState, 'latestVersion' | 'latestUrl' | 'latestNotes'> | null> {
  try {
    /** @note net.fetch は OS のプロキシ設定に従う。renderer の CSP を広げずに済むよう main で問い合わせる。 */
    const response = await net.fetch(LATEST_RELEASE_API, {
      headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'FBZZ-GameHub' },
      signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });
    if (!response.ok) {
      console.warn(`新しい版の確認に失敗しました (HTTP ${response.status})。`);
      return null;
    }
    const release = await response.json() as Record<string, unknown>;
    const tag = typeof release.tag_name === 'string' ? release.tag_name : '';
    const url = typeof release.html_url === 'string' ? release.html_url : '';
    const version = tag.replace(/^v/i, '');
    if (compareVersions(version, '0.0.0') === null || !isReleasePageUrl(url)) {
      console.warn('最新リリースの形式を解釈できませんでした。', tag, url);
      return null;
    }
    return {
      latestVersion: version,
      latestUrl: url,
      latestNotes: summarizeNotes(typeof release.body === 'string' ? release.body : ''),
    };
  } catch (error) {
    console.warn('新しい版を確認できませんでした。', error);
    return null;
  }
}
