# OpenTune — Product Requirements Document

**Status:** Draft v1 · **Owner:** Janu Chaudhary · **Date:** 2026-09-04
**Companion documents:** `specs.md` (engineering requirements) · `constitution.md` (principles) · `tasks.md` (execution)

This document answers *why* and *for whom*. `specs.md` answers *what, precisely*.
When they disagree, this document is wrong about engineering and `specs.md` is wrong
about product — fix whichever is out of its lane.

---

## 1. Summary

OpenTune is a real-time vocal pitch correction engine, free to use and built to be
built upon, delivered first as a desktop tool and then as an Android app for
short-form video creators.

Professional pitch correction costs hundreds of dollars and requires a DAW. The free
mobile alternatives reach tens of millions of people but ship a decade-old quality
level and hide every meaningful control behind presets. Nobody is serving the
creator who records a vocal on their phone, wants it to sound like a record, and
doesn't know what a "key" is.

The engine is the product. Apps are how people reach it.

## 2. The problem

### 2.1 For creators

A large and growing population records vocals on phones for Reels, Shorts, and
TikTok. Their options today:

- **Upload raw.** Most do. The platform's own voice effects are toys.
- **Voloco** (50M+ downloads, 4.8★). Real-time, works, but: correction strength is a
  preset not a dial, formants are not preserved (the "chipmunk" artifact), it is
  tuned for rap hard-tune rather than singing, and the free tier lets you edit for
  twenty minutes and then paywalls the export.
- **Smule / StarMaker.** Autotune exists but is invisible and non-adjustable, and
  the output lives inside their walled garden.
- **BandLab / GarageBand.** Full DAWs. Correction is one feature among fifty; the
  creator has to learn the DAW to reach it.
- **Antares Auto-Tune for iOS.** The real engine — as an AUv3 plugin, meaning it only
  runs inside another app. No Android version exists.

Every one of these either sounds cheap, hides the controls, or requires a
producer's vocabulary to operate.

### 2.2 For developers

There is no free, permissively-usable, high-quality pitch correction engine.
Anyone building a karaoke app, a game, a voice tool, or a plugin either licenses
proprietary code, uses a GPL library that infects their product, or writes their
own — and pitch correction is hard enough that "write your own" usually means
"ship something that sounds bad."

### 2.3 Why now

- Auto-Tune's foundational patents have expired. The field is buildable.
- Permissive, high-quality DSP building blocks now exist (Signalsmith Stretch, MIT;
  WORLD vocoder, BSD; miniaudio, public domain).
- Short-form video made "everyone records vocals" true for the first time.
- The state of the art moved to formant-aware, resynthesis-based correction;
  mobile competitors did not follow.

## 3. Users

### Primary — the short-form creator
Records vocals on a phone, several times a week, for social video. Cannot read
music. Does not know what key they sing in. Will not read a manual or watch a
tutorial. Judges the result by whether it sounds like the songs they listen to.

**Needs:** press record, sound good, export, post. Zero configuration required.
**Fails if:** anything requires musical vocabulary, or the free path dead-ends.

### Secondary — the developer
Building something that needs pitch correction. Wants a library, not an app.

**Needs:** clean C++ interfaces, a licence they can ship under, a build that works
in five minutes, documentation that explains the parameters.
**Fails if:** the licence is unclear or the engine drags platform code with it.

### Tertiary — the learner
Wants to understand how pitch correction actually works, by reading working code.

**Needs:** readable source, comments that explain the *why*, tests that show the
expected behaviour.
**Fails if:** the code is clever rather than clear.


## Market decision — Hindi first (2026-09-04)

The owner set Hindi/Indian film music as the primary market. This is a product decision with
engine consequences, recorded here because the spec records *what* and this records *why*.

**Why it is a real wedge, not a localisation.** The competitive audit at the top of this
document found the category leader is skewed toward rap and hard-tune, and weak for actual
singing. It is also built entirely around Western scales. An Indian creator singing a film-song
cover gets their notes snapped to a chromatic or major-scale grid that has nothing to do with
the raga the song is in. That is not a rough edge; it is the tool being wrong about the music.
No incumbent serves it, and it is the one audience where "better Voloco" is not the pitch.

**What it changes in the engine** (detail in `specs.md` §5 and FR5/FR6/FR16):
- Raga note-sets, with ascending and descending sets that differ. Not a scale table.
- Tonic identification rather than key detection — Sa moves with the singer.
- *Meend* and ornaments must survive correction. `retuneMs` becomes the central control.

**What it deliberately does not change.** Bollywood is harmonium-led and effectively 12-TET, so
the equal-tempered maths already built stays valid, and classical 22-shruti just intonation is
out of scope. Chasing microtonality would serve trained classical performers — a different,
smaller audience — at the cost of the creator this product is for.

**Consequence for the plan:** musical intelligence moved from Stage 5 to Stage 2.5, ahead of
real time and the strength dial. Until it lands, every Hindi demo is musically wrong.

## 4. Jobs to be done

| When… | I want to… | So that… |
|---|---|---|
| I've recorded a take and one note is flat | fix it without re-recording | I can post today |
| I'm singing into headphones | hear myself corrected as I sing | I sing better and commit to the take |
| I want the T-Pain sound | turn correction all the way up | it sounds intentional, not accidental |
| I want it to sound natural | turn correction down | nobody can tell it was tuned |
| I don't know my key | have the app figure it out | I don't have to learn music theory |
| I'm building an app | drop in a pitch correction engine | I don't spend six months on DSP |
| I'm learning DSP | read how it works | I understand it, not just use it |

## 5. Product principles

1. **The engine is the product.** Everything else is a way to reach it. Effort goes
   to the engine until it is proven.
2. **One dial.** Transparent to hard-tune is one continuous control, not two modes.
   Presets are shortcuts to dial positions, never the only way in.
3. **Zero-config first.** It must produce a good result with nothing set. Every
   control is an improvement on a good default, never a prerequisite.
4. **No paywall ambush.** If a free path exists, it goes all the way to export.
5. **Measured, not asserted.** "Sounds good" is a hypothesis; a number confirms it.
6. **Built to be built on.** Interfaces are stable, documented, and swappable.

## 6. Scope

### v1 — the engine, proven
- Real-time pitch correction engine, ≤ 20 ms round-trip, mono, 48 kHz
- Continuous strength control from transparent to hard-tune
- Formant preservation
- Retune speed and humanize controls
- Chromatic, then manual key, then automatic key detection
- CLI tool (WAV in → WAV out)
- Minimal Linux desktop app: record, monitor live, adjust, export WAV
- Minimal Android app with the same loop

### v2 — the creator app (not before v1 is proven)
- Reverb, EQ, compression with sensible defaults
- Vertical video capture with corrected audio
- Direct export at Reels/Shorts/TikTok specs
- Presets that map to dial positions
- Harmony generation

### Explicitly out, for now
- iOS (no macOS hardware available)
- VST3/AU plugin (needs a plugin framework; JUCE is licence-blocked)
- Beat library (licensing and hosting; a business problem)
- Vocal isolation, noise reduction, de-essing (adjacent products)
- Offline lookahead mode (higher quality, but real-time is the differentiator)

Full engineering detail, interfaces, and acceptance thresholds: `specs.md` §6–§9.

## 7. Success metrics

### Engine (gates v1 — from `specs.md` §9)
- Detection within ±5 cents on synthetic tones; ±15 cents on real vocals
- Correction lands within ±10 cents of target
- ≤ 20 ms round-trip latency; zero dropouts over 10 minutes
- Passes a blind listening checklist on five voices

### Product (measured after v1 ships)
| Metric | Target | Why it matters |
|---|---|---|
| Time from install to first exported corrected vocal | < 2 minutes, zero settings touched | Proves zero-config |
| Blind preference vs. Voloco on the same take | ≥ 50% prefer OpenTune | Proves the quality claim |
| Builds from clean clone, all platforms | < 5 minutes | Proves developer usability |
| External contributor merges an engine improvement | ≥ 1 within 6 months of public release | Proves "built to be built on" |

## 8. Roadmap

| Milestone | Delivers | Proves |
|---|---|---|
| M0 | CLI, naive engine, audible | The pipeline exists |
| M1 | Accurate detection (YIN) | We can find the pitch |
| M2 | Quality correction (Signalsmith), formants | We can fix it without ruining it |
| M3 | Real-time desktop monitor | It works live |
| M4 | The full dial | Transparent ↔ hard-tune in one engine |
| M5 | Key selection and auto-detect | Zero-config is real |
| M6 | Desktop app | A human can use it |
| M7 | Android app | The target audience can use it |
| v2 | Effects, video, harmony | A complete creator product |

Detailed tasks and done-criteria: `tasks.md`.

## 9. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Engine quality never reaches "competitive" | Medium | Fatal | Prove it first (M0–M2) before any app work; blind A/B against Voloco at M2 |
| Real-time latency unachievable on Android | Medium | High | Desktop first; measure per-device at M7; AAudio path; document minimum device |
| Solo developer new to DSP burns out | Medium | Fatal | Every stage ends in something audible; small tasks; TDD catches silent regressions |
| Signalsmith formant handling insufficient | Medium | Medium | WORLD vocoder (BSD) is the pre-identified fallback; decision point at M2 |
| "OpenTune" conflicts with Antares' "Auto-Tune" mark | Low–Medium | Medium | Trademark search before any store listing; name is cheap to change before launch |
| Licence chosen too late, dependencies constrain it | Low | High | Permissive-only dependency rule from day one keeps every option open |
| Patent claims in pitch correction | Low | High | Core patents expired; WORLD authors state no patents; avoid Antares-specific techniques |

## 10. Open product questions

| # | Question | Decide by |
|---|---|---|
| P1 | Licence: permissive (adoption), copyleft (stays free), or proprietary | First public release |
| P2 | Name and trademark clearance | Before any store submission |
| P3 | Business model, if any — free forever, donations, paid v2 app on a free engine | Before v2 planning |
| P4 | Distribution: GitHub only, F-Droid, Play Store | M7 |
| P5 | Community: accept contributors before or after v1 | M3, when there is something to contribute to |

Engineering open questions (Signalsmith vs WORLD, Android audio API, GUI toolkit,
commit signing) are tracked in `specs.md` §12 and resolved in `docs/decisions/`.

## 11. Competitive reference

| Product | Platform | Model | Strength | Gap we exploit |
|---|---|---|---|---|
| Voloco | iOS/Android/desktop | Freemium | Real-time, 50M users, auto key | Preset-only control, no formants, rap-tuned, export paywall |
| Antares Auto-Tune Pro X | Desktop DAW plugin | ~$400 | The reference for quality | Price; DAW required; no Android |
| Waves Tune Real-Time | Desktop DAW plugin | Paid | Best live hard-tune latency | Same |
| Melodyne 5 | Desktop | Paid | Most transparent, note-level editing | Not real-time; price |
| BandLab | Web/mobile DAW | Free | Free, has note-level Retune | It's a DAW; correction is buried |
| Smule / StarMaker | Mobile social | Freemium | Frictionless | Invisible controls; walled garden |
| Antares Auto-Tune (iOS AUv3) | iOS only | Paid | Real engine | Plugin-only; no Android |

Sources: product pages and reviews as of 2026-09-03 — see the research summary in the
project history, and `specs.md` §11 for verified library licences.
