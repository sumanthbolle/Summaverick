# New look and guided setup: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development.

**Goal:** The whole app in a black and white Apple-classic look with the owner's logo, followed by a guided first-time setup.

**Design reference:** `~/Downloads/summaverick-shared/setup-screens-mockup.html` (approved by the owner, 2026-11-09). Voice: `docs/superpowers/2026-11-09-voice-guide.md`.

## Global Constraints

- **Owner's words:** "make sure the white mode applies at all the screens. considering elderly people they might want brighter screens."
- **Modes:** Appearance setting with System (default), White, Dark. Every screen, sheet, alert tint, widget and Live Activity is correct in both. White is truly bright: white backgrounds, near-black text.
- **Elderly readers, white mode:** body and secondary text at least 7:1 contrast on its background (secondary `#515154` or darker on white), all text uses Dynamic Type styles with no fixed sizes that block scaling, tap targets at least 44 pt, no meaning carried by colour alone.
- **Fonts:** Apple system fonts only (SF, and the system's Indic fallbacks). No serif.
- **Logo:** the owner's original artwork, `~/Downloads/summaverick_group.png`, cropped without the wordmark. Never redrawn, recoloured or animated beyond a fade. On a white screen it sits in a black rounded tile, so the artwork stays unchanged.
- **Colour:** black, greys and white. Red only for recording and real warnings. Medicine and health emphasis stays clearly visible in both modes. No blue accent.
- **Motion:** only in setup and light moments, never while notes are being written. Reduce Motion gives still versions.
- **No behaviour changes** outside appearance in Task L1. Notes, medicine rules and the pipeline stay as they are.
- **Mac battery:** no AI model runs on the Mac. Unit tests, smoke `api`, builds and simulator screenshots are fine.
- **Every string:** en and te, in the voice guide's voice; `check_strings` prints `STRINGS OK`.

---

### Task L1: The look across the whole app

- `Theme`: one place for colours and styles, adapting to White and Dark, with the 7:1 rule pinned in a swiftc check that computes the contrast of each text and background pair in both modes.
- Appearance setting in Settings (System, White, Dark), applied at the app root, stored in `AppSettings`.
- Replace the blue gradient, the arc mark and the accent colour everywhere: app icon from the owner's artwork, `Brand`, tab bar, buttons as Apple-style pills, cards, chips, chat bubbles, widgets and the Live Activity.
- Proof: a simulator screenshot of every main screen in both modes, saved to `~/Downloads/summaverick-shared/look-check/`.

### Task L2: Guided setup

Screens as in the mockup: welcome, name and language, where notes are written, download, permissions, tour on the real screens, try it.
- Shown once for a new install. An existing install with notes or consent already given never sees it and never loses data. Settings offers "Show me around again" for the tour.
- Name saves to the profile. Language: dropdown of English, हिन्दी, తెలుగు, தமிழ், മലയാളം. Only English and Telugu can be chosen now; the rest show "Coming soon", because their accuracy and medicine checks are not done.
- Where notes are written: this phone is chosen. "Your own account" shows "Coming soon" and cannot be picked, since connectors are not built.
- Download uses the existing `ModelManager`; setup carries on during it. Permissions use the existing requests, and the consent line sets `consentAcknowledged` only when the person agrees.
- Tour: four skippable tips on the real tabs. Try it: opens the recorder; "I'll try it later" leaves it.
- Pure rules with swiftc checks: when setup shows, which step comes next, which languages can be chosen.
- Proof: simulator screenshots of each step in both modes, in the same folder.
