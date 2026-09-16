/**
 * @file Icon.tsx
 * @brief 線幅と形状を統一した軽量 SVG アイコン。
 * @author Hasegawa Jin
 * @date 2026/07/19
 */

export type IconName =
  | 'grid' | 'list' | 'layers' | 'settings' | 'plus' | 'search' | 'folder' | 'play'
  | 'check' | 'cpu' | 'arrow' | 'alert' | 'info' | 'refresh' | 'trash' | 'chevron' | 'close';

const paths: Record<IconName, string[]> = {
  grid: ['M4 4h6v6H4z', 'M14 4h6v6h-6z', 'M4 14h6v6H4z', 'M14 14h6v6h-6z'],
  list: ['M4 6h16', 'M4 12h16', 'M4 18h16'],
  layers: ['m12 3-9 5 9 5 9-5-9-5Z', 'm3 12 9 5 9-5', 'm3 16 9 5 9-5'],
  settings: ['M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7Z', 'M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.12 2.12-.06-.06a1.7 1.7 0 0 0-1.88-.34 1.7 1.7 0 0 0-1.03 1.56V20h-3v-.08a1.7 1.7 0 0 0-1.03-1.56 1.7 1.7 0 0 0-1.88.34l-.06.06-2.12-2.12.06-.06A1.7 1.7 0 0 0 7 14.7 1.7 1.7 0 0 0 5.44 13H5v-3h.44A1.7 1.7 0 0 0 7 8.97 1.7 1.7 0 0 0 6.66 7.1l-.06-.06 2.12-2.12.06.06a1.7 1.7 0 0 0 1.88.34A1.7 1.7 0 0 0 11.7 3.8V3h3v.8a1.7 1.7 0 0 0 1.03 1.56 1.7 1.7 0 0 0 1.88-.34l.06-.06 2.12 2.12-.06.06A1.7 1.7 0 0 0 19.4 9 1.7 1.7 0 0 0 21 10.56V13a1.7 1.7 0 0 0-1.6 2Z'],
  plus: ['M12 5v14', 'M5 12h14'],
  search: ['m21 21-4.35-4.35', 'M19 11a8 8 0 1 1-16 0 8 8 0 0 1 16 0Z'],
  folder: ['M3 6.5A1.5 1.5 0 0 1 4.5 5H9l2 2h8.5A1.5 1.5 0 0 1 21 8.5v9A1.5 1.5 0 0 1 19.5 19h-15A1.5 1.5 0 0 1 3 17.5Z'],
  play: ['m9 7 8 5-8 5Z'],
  check: ['m5 12 4 4L19 6'],
  cpu: ['M8 8h8v8H8z', 'M9 2v3', 'M15 2v3', 'M9 19v3', 'M15 19v3', 'M2 9h3', 'M2 15h3', 'M19 9h3', 'M19 15h3'],
  arrow: ['M5 12h14', 'm14 7 5 5-5 5'],
  alert: ['M12 4 2.7 20h18.6L12 4Z', 'M12 10v4', 'M12 17.2v.1'],
  info: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z', 'M12 11v5', 'M12 7.8v.1'],
  refresh: ['M20 11a8 8 0 1 0-.7 4.5', 'M20 5v6h-6'],
  trash: ['M4 7h16', 'M9 7V4h6v3', 'M6 7l1 13h10l1-13', 'M10 11v5', 'M14 11v5'],
  chevron: ['m9 6 6 6-6 6'],
  close: ['M6 6l12 12', 'M18 6 6 18'],
};

export function Icon({ name, size = 15 }: { name: IconName; size?: number }) {
  return <svg className="ui-icon" width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
    {paths[name].map((definition, index) => <path d={definition} key={`${name}-${index}`} />)}
  </svg>;
}
