/**
 * @file Logo.tsx
 * @brief 六角形と Z を組み合わせた FBZZ のシンボルマーク。
 * @author Hasegawa Jin
 * @date 2026/07/19
 */

export function Logo({ size = 20 }: { size?: number }) {
  return <svg className="logo-mark" width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" role="img" aria-label="FBZZ">
    <path d="M12 2.2 20.5 7v10L12 21.8 3.5 17V7L12 2.2Z" opacity=".55" />
    <path d="M8.6 9.2h6.8l-6.8 5.6h6.8" strokeWidth="1.9" strokeLinecap="round" />
  </svg>;
}
