# Summaverick Classic Studio Design

**Date:** 2026-09-16

## Goal

Reposition summaverick.com as a broad, luxury-grade software product engineering company website for startup product leaders and enterprise technology leaders. Preserve the complete legacy learning library while making the company home feel authored, calm, technically credible, and visually distinctive.

## Decisions

- Lead with product strategy, design, software engineering, and intelligent systems. ServiceNow is a specialist practice, not the primary category.
- Use a Classic Studio visual language: white, pearl gray, silver, graphite, and a restrained blue reserved for primary actions and active states.
- Use the **Uncontained Sum** as the working logo: a sculpted summation symbol with one detached element. Its brand line is: `Σ + the element that refuses containment`.
- Use Sumanth Bolle’s approved partial-reveal portrait as the homepage hero artwork. The portrait is an editorial founder presence, not a replacement for the logo.
- Keep company proof factual. The home may show real Summaverick products, architecture, and publishing; it must not invent clients, business outcomes, testimonials, logos, or metrics.
- Preserve the source content unchanged: 52 posts and 57 interviews, their original identifiers, metadata, body HTML, and legacy-compatible slugs.

## Brand System

### Logo

Create a vector `Uncontained Sum` mark from the approved concept. It must work in one color at 16px, in graphite on light surfaces, and in white on graphite surfaces. The decorative metallic rendering belongs only to the hero presentation; it is not a separate logo asset.

The wordmark uses the existing Geist variable font at a refined medium weight. The mark and wordmark are separate elements so compact navigation can use the mark alone.

### Tokens

Use these values as the homepage-specific visual direction while keeping shared interface colors accessible:

| Role | Value | Use |
| --- | --- | --- |
| Studio white | `#FFFFFF` | primary ground |
| Pearl | `#F5F5F7` | hero, recessed sections |
| Silver | `#D2D2D7` | rules, object depth |
| Graphite | `#1D1D1F` | primary type, dark specialist section |
| Quiet gray | `#6E6E73` | secondary type |
| Action blue | `#0071E3` | primary action, active state only |

Typography remains Geist Variable. Display text uses tight tracking and strong size contrast; prose stays below a 68ch measure. Avoid all-caps decorative eyebrows, persistent gradients, generic pill kits, and excessive rounded-card grids.

## Homepage Information Architecture

1. **Fixed navigation** — Uncontained Sum mark + wordmark; Expertise, Work, Company, Insights; “Start a project” action. A compact accessible mobile menu exposes the same destinations.
2. **Hero** — “We turn product ambition into working software.” The approved founder portrait is partially hidden behind a pearl-white reveal plane. The left half remains quiet for copy, two actions, and immediate comprehension.
3. **Product-engineering statement** — A concise bridge for both audiences: founders get momentum; enterprises get rigor; both get an accountable team.
4. **Operating practice** — A non-card, three-step vertical story: Shape, Build, Evolve. Each step names one outcome and avoids process theatre.
5. **Real systems** — Two or more selected, fact-based Summaverick product entries. Initial candidates are the evidence-gated research agent and ServiceNow product systems. The presentation links to working routes or transparent technical material.
6. **Specialist depth** — A graphite section framing AI systems and enterprise platforms/ServiceNow as deep capabilities inside the broad practice.
7. **Knowledge library** — A bridge to Learn and Interviews, explicitly retaining all 52 technical essays and 57 interview answers. This remains a content invitation, not a fabricated authority claim.
8. **Project enquiry** — “Have a product worth building well?” The main action scrolls to or opens the existing contact interaction. Copy promises a useful next conversation, never an automated sales sequence.
9. **Footer** — Product routes, expertise links, company links, and real external profiles/contact details already supported by the site.

## Motion and Interaction

Motion earns its place by revealing a change:

- On first load, the portrait and reveal plane settle into place while headline and action appear in a single short sequence.
- The Uncontained Sum may receive a one-time material highlight or detached-element settle in the hero only.
- Work entries can respond to pointer/focus with a restrained surface shift; no perpetual floating, scroll-jacking, or every-section fade-up choreography.
- `prefers-reduced-motion: reduce` disables transform/clip animations and renders every final state immediately.
- Navigation, route links, content, and contact must work before JavaScript runs.

## Content and Migration Integrity

`scripts/data/posts.json` and `scripts/data/interviews.json` are committed migration snapshots. The seed script maps them to `content` rows and R2 bodies. The redesign must not change their body HTML, titles, categories, dates, IDs, or slugs.

The Learn, Interview, and Article route structure remains:

- `/learn`
- `/interviews`
- `/article/:slug`

Their UI may receive the shared Classic Studio chrome, readable typography, filters, loading states, and responsive layout improvements. The content API and Worker route contracts remain unchanged.

## Implementation Boundaries

### Modify

- `public/index.html` — replace current company-home markup with the approved story and semantic image/navigation structure.
- `public/assets/css/tokens.css` — align shared tokens where needed without reducing contrast on existing tools.
- `public/assets/css/site.css` — add Classic Studio layout, logo, portrait, responsive, focus, and reduced-motion styling. Remove only home-specific obsolete animation rules that conflict with the new design.
- `public/assets/js/boot.js` and/or a small home-only module — implement progressive hero motion without making core page function dependent on JavaScript.
- `public/assets/img/` — add versioned portrait and SVG logo assets; leave prior files untouched unless a reference is intentionally replaced.
- `public/learn.html`, `public/interviews.html`, and their shared styles/scripts only where needed to make library chrome coherent while retaining content behavior.
- Tests and verification scripts to cover the asset references and content invariants introduced by this work.

### Do not modify

- Worker API route contracts, D1 schema, R2 key layout, session/auth logic, quiz grading, research safeguards, or migration semantics.
- Legacy post/interview source data, except to re-copy it verbatim from `sumanthbolle/sumanthbolle.github.io` if an integrity check proves the committed snapshot diverged.

## Failure Handling and Accessibility

- The portrait uses meaningful alternative text and preserves the headline’s white-space layout if the image cannot load.
- API-backed library views retain their existing loading/empty/error states and never erase content already rendered.
- Images receive explicit dimensions or an aspect-ratio container to prevent layout shifts.
- All actions are keyboard reachable with a visible focus treatment; the mobile menu exposes correct `aria-expanded` and focus behavior.
- Text/background combinations meet WCAG AA contrast; the blue action color is never the only indicator of meaning.

## Acceptance Criteria

1. The homepage clearly presents a broad product-engineering company before mentioning ServiceNow.
2. The Uncontained Sum mark appears as a crisp SVG in navigation and is usable at favicon scale.
3. The approved partial-reveal founder portrait appears in the hero with correct responsive cropping and no layout shift.
4. The page works and reads coherently with JavaScript disabled and with reduced motion enabled.
5. No fictional customer proof or unsupported metric is introduced.
6. Learn and Interviews remain reachable and preserve all 52/57 items, body content, metadata, and existing URLs.
7. `pnpm typecheck`, `pnpm test`, and project verification pass; new automated checks cover source-content counts and new critical asset references.
8. Desktop and mobile manual visual checks confirm unclipped portrait, readable navigation, tappable actions, and no horizontal overflow.

## Out of Scope

- New CMS/editorial workflow, new backend routes, authentication changes, data migration changes, Cloudflare deployment configuration, or fabrication of a portfolio/case-study database.
- A complete rebrand of every tool interface in this pass; the focus is the company home plus the library surfaces required for visual continuity.
