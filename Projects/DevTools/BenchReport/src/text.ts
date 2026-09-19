// FBZZ Engine
// text.ts | Projects/DevTools/BenchReport
// HTML / SVG / Markdown へ文字列を埋め込むときのエスケープ。

/** HTML と SVG (XML) の本文・属性値へ埋め込む。 */
export function EscapeXml(text: string): string {
    return text
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}

/** Markdown の表のセルへ埋め込む。`|` は列の区切りと解釈されるため逃がす。 */
export function EscapeMarkdownCell(text: string): string {
    return text.replaceAll('\\', '\\\\').replaceAll('|', '\\|').replaceAll('\n', ' ');
}
