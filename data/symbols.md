# Symbol fallback test

This file is a quick visual check. With **font fallback** wired up, the
characters below should all render — not as tofu rectangles.

Consolas DOES have these (no fallback needed): → ← ↑ ↓ ° µ ± ÷ × ¼ ½ ¾

Consolas does NOT have these — need Segoe UI Symbol fallback:

★ ✓ ✗ ⌘ ⏎ ☐ ☑ ⚡ ☀ ☁ ⚙ ⚠ ✉ ☎ ⌚ ⌛

CJK — need MS Gothic fallback:

你好世界 (Chinese)
こんにちは (Japanese hiragana)
日本語の漢字 (Japanese kanji)

Emoji (no monochrome system font has these — still expected to tofu):

🦊 🟢 🚧 🎉
