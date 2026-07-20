// FBZZ GameHub
// Logo.tsx | renderer
// FBZZのZ形状と立方体を組み合わせたコードネイティブロゴ

export function Logo({ compact = false }: { compact?: boolean }) {
  return <div className={`logo-symbol ${compact ? 'compact' : ''}`} aria-label="FBZZ">
    <svg viewBox="0 0 48 48" role="img">
      <defs><linearGradient id="fbzz-logo" x1="8" y1="5" x2="40" y2="43" gradientUnits="userSpaceOnUse"><stop stopColor="#c4b5fd"/><stop offset=".5" stopColor="#8b5cf6"/><stop offset="1" stopColor="#5b21b6"/></linearGradient></defs>
      <path d="M24 3 42 13.5v21L24 45 6 34.5v-21L24 3Z" fill="url(#fbzz-logo)"/>
      <path d="m15 16 9-5 9 5-9 5-9-5Zm0 16 9 5 9-5M24 21v16" fill="none" stroke="rgba(255,255,255,.38)" strokeWidth="1.2"/>
      <path d="M15.5 24h17l-11 9h11" fill="none" stroke="white" strokeWidth="3.2" strokeLinecap="round" strokeLinejoin="round"/>
    </svg>
  </div>;
}
