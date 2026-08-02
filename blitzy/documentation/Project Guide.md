# Blitzy Project Guide

**Project:** FFmpeg — IAMF Muxer Heap Buffer Overflow Remediation
**Branch:** `blitzy-eb68ddb3-37ca-4dd3-888b-77145133233d`
**HEAD:** `fcd9b7d31a36aa726e8de4c9783d6acd89f29845` · **Baseline:** `566ad7869ee3c8b6993e1f880e0a50eae18c66ac`
**Change set:** 19 commits · 13 files · +3,530 / −124 lines · all authored as `Blitzy Agent <agent@blitzy.com>`

---

## 1. Executive Summary

### 1.1 Project Overview

This project remediates a HIGH-severity heap buffer overflow in FFmpeg's IAMF (Immersive Audio Model and Formats) muxer, where an attacker-derived layer count indexed a fixed-capacity `recon_gain[6][12]` matrix and wrote out-of-bounds heap bytes directly into the output file — a persisted heap-disclosure primitive. The target consumers are every application, media pipeline and distribution that links `libavformat` with the IAMF muxer enabled, reachable through a routine `-c copy` remux of untrusted input. The technical scope is deliberately narrow: port the IAMF parser's already-correct bounds and consistency invariants onto the writer, add regression coverage, and introduce FFmpeg's first muxer fuzz target — with no public API or ABI change.

### 1.2 Completion Status

```mermaid
%%{init: {'theme':'base','themeVariables':{'pie1':'#5B39F3','pie2':'#FFFFFF','pieStrokeColor':'#B23AF2','pieStrokeWidth':'2px','pieOuterStrokeWidth':'2px','pieTitleTextSize':'16px','pieSectionTextColor':'#B23AF2','pieOpacity':'1'}}}%%
pie showData title Project Completion — 68.0% Complete
    "Completed Work (Dark Blue #5B39F3)" : 170
    "Remaining Work (White #FFFFFF)" : 80
```

| Metric | Value |
|---|---|
| **Total Hours** | **250 h** |
| **Completed Hours (AI 170 + Manual 0)** | **170 h** |
| **Remaining Hours** | **80 h** |
| **Percent Complete** | **68.0 %** |

**Calculation (PA1, AAP-scoped work only):**
`Completion % = Completed Hours ÷ (Completed Hours + Remaining Hours) × 100 = 170 ÷ (170 + 80) × 100 = 170 ÷ 250 × 100 =` **68.0 %**

**AAP requirement classification:** 38 Completed · 1 Partially Completed · 10 Not Started (49 discrete deliverables inventoried).

### 1.3 Key Accomplishments

- [x] **All 7 AAP-designed security fixes delivered and verified in source** — F-1 (layer-count bound), F-1b (recon-gain loop clamp), F-2 (off-by-one predicate), F-3 (assertion → error return), F-4 (channel-count equality, the root enabler), F-5 (aggregate substream cross-check), F-6 (guard reordering)
- [x] **Vulnerability reproduced then proven eliminated under AddressSanitizer** — baseline `566ad786` emits `heap-buffer-overflow … #0 write_parameter_block libavformat/iamf_writer.c:1140:58`, *"located 7 bytes after 136-byte region"*; HEAD returns rc=0 with zero sanitizer output on identical input
- [x] **Full FATE suite 5,470/5,470 PASS, 0 failures** — independently re-run during this assessment on a quiescent tree (baseline was 5,468; the delta is exactly the two new tests)
- [x] **20/20 IAMF acceptance gate PASS** — independently re-run here, including the 7 `fate-mov-mp4-iamf-*` tests that prove the second consumer `libavformat/movenc.c` is protected with **zero edits to it**
- [x] **Byte-identity mechanically proven** — `KEEP_FILES=1` artifacts md5-match the committed references exactly; `fate-iamf-expanded-3_0` = `2fbd17fd4c9544e74be281b770f36cde` @ 28,601 B, bit-for-bit the AAP-stated *pre-fix* value
- [x] **Two new regression tests**, one impossible to pass pre-fix — `fate-iamf-lfe` core-dumped on the baseline because the encoder aborted on a **specification-valid** LFE layout; it now encodes correctly
- [x] **FFmpeg's first muxer fuzz target** — `tools/target_mux_fuzzer.c`, 1,942 lines, structure-aware with layer-chain tables, a mutation harness and a `rejection_reason()` oracle; 2,830,091 executions, 0 crashes, corpus grown 26 → 1,384, zero uncovered functions in `iamf_writer.c`
- [x] **Zero `av_assert` call sites remain** in `iamf_writer.c` (verified `grep -cP 'av_assert[012]\s*\(' = 0`), removing two unconditional input-reachable process aborts — assertions compile out of release builds and are never valid input validation
- [x] **No public API or ABI change** — all 17 declared out-of-scope files verified byte-unchanged, including `libavutil/iamf.h` where `recon_gain[6][12]` is publicly declared; no `doc/APIchanges` entry, no version bump
- [x] **Fail-closed behaviour independently reproduced** — 7-layer element → `Invalid amount of layers 7 … Must be >= 1 and <= 6`, 0-byte output; mask-colliding `FL+FR+FR` → `Unsupported channel layout on stream #0`, 0-byte output; dangling submix → clean error, not SIGABRT
- [x] **Clean build and lint** — CI-identical configure line, zero warnings, clean under `-Wall -Wextra`; `pre-commit` EXIT=0 with all 12 hooks Passed (both re-verified here)
- [x] **Audit trail suitable for maintainer review** — commit bodies name the affected functions, cite all nine CWEs, and reference `iamf_parse.c` 21 times so a reviewer can verify the "ports parser invariants to the writer" claim by direct comparison

### 1.4 Critical Unresolved Issues

| Issue | Impact | Owner | ETA |
|---|---|---|---|
| **Line-level scope exceeds the AAP's binding MINIMAL-change mandate.** `libavformat/iamf_writer.c` grew 1,304 → 2,394 lines (+1,214/−124); diagnostics 27 → 71; comments 35 → 428; ten function definitions added. File-level scope is exact (13/13 in-scope, 17/17 out-of-scope untouched), but the diff is ~10× the designed patch. | FFmpeg maintainers will not accept a ~1,200-line single security patch. Requires a reviewable series split or a formally recorded accept-as-is decision, and materially enlarges the human security review. | Security Engineer + Upstream Maintainer | 14 h |
| **CI build matrix entirely unverified.** Only linux-amd64 static 64-bit was exercised. The AAP (0.4.2, 0.10.3) mandates linux-aarch64 static, linux-amd64 static 32-bit (`--arch=x86_32 -m32`), linux-amd64 shared, plus the win64-gpl full suite under Wine. | Analytically low risk (only int comparisons, one `FFMIN`, `av_log`/`AVERROR`; no assembly, alignment or pointer-width arithmetic) but unmeasured. The shared variant is the only one that would expose an accidental symbol-visibility change. | Build/Release Engineer | 10 h |
| **Human security review and maintainer sign-off not performed.** AAP 0.12.3 requires it before deployment; the diff is far larger than the "unusually straightforward to review" patch the AAP anticipated. | No independent human has read the 44 new rejection paths for false-rejection risk against configurations outside the 8-fixture set. | Security Reviewer | 12 h |
| **No CVE or security advisory exists** for this defect (established in AAP 0.2.2 — no CVE, GHSA or OSV identifier could be located). | Downstream distributions have no discovery or backport channel for a fix to a heap-disclosure primitive. | Security Program Manager | 4 h |
| **Muxer fuzzing is closed structurally but not operationally.** `tools/target_mux_fuzzer.c` exists and ran, but nothing in `.forgejo/workflows` builds or runs it and no seed corpus is committed. | The coverage gap that let this defect class survive will silently reopen. | Fuzzing/Infra Engineer | 8 h |
| **Nothing submitted upstream.** For an FFmpeg security fix, landing upstream is the actual path to production. | Maintainer feedback may require restructuring that invalidates the current 19-commit trail. `CONTRIBUTING.md` routes patches to Forgejo or ffmpeg-devel and states GitHub PRs **"will be ignored"**. | Upstream Liaison | 10 h |

### 1.5 Access Issues

| System / Resource | Type of Access | Issue Description | Resolution Status | Owner |
|---|---|---|---|---|
| Git repository (`blitzy-eb68ddb3-…`) | Read / write / push | None — 19 commits pushed; `HEAD == origin` | ✅ Resolved | — |
| External FATE sample corpus | Filesystem read | Present locally at `/opt/blitzy-tools/fate-suite`; the AAP's "requires network access" limitation no longer applies — all 5 sample-dependent IAMF tests were run | ✅ Resolved | — |
| clang AddressSanitizer + libFuzzer runtimes | Toolchain | Available and exercised (26-scenario sweep, 2.83 M-execution campaign) | ✅ Resolved | — |
| `pre-commit` hook environments | Toolchain | Available fully offline at `/opt/blitzy-tools/precommit-venv`; gate re-verified EXIT=0 | ✅ Resolved | — |
| Package registries / dependency manifests | Registry | Not applicable — the repository contains **zero** manifests (no `package.json`, `requirements.txt`, `pyproject.toml`, `go.mod`, `Cargo.toml`, `pom.xml`, `Gemfile`, `Dockerfile`, `docker-compose.yml`, `*.tf`) | ✅ N/A | — |
| aarch64 and 32-bit cross toolchains | Build environment | Not present in this container — blocks 2 of the 4 mandated CI matrix jobs | ⚠️ Open — environment constraint, not a permission denial | Build/Release Engineer |
| Wine-capable win64 build environment | Build environment | Not present — blocks the win64-gpl full-suite job | ⚠️ Open — environment constraint | Build/Release Engineer |
| `git.ffmpeg.org`, NVD, MITRE, OSS-Fuzz tracker | Network egress | No route from this environment; no CVE could be located and no upstream patch retrieved for comparison | ⚠️ Open — environment constraint | Security Program Manager |
| `code.ffmpeg.org` Forgejo / ffmpeg-devel mailing list | Network + credentials | Unreachable, so no patch series could be submitted | ⚠️ Open — environment constraint | Upstream Liaison |
| Static-analysis tooling (`valgrind`, `cppcheck`, `scan-build`, `clang-tidy`) | Toolchain | Absent, as the AAP already recorded (0.7.4). **No claim is made that any of them was run**; memory-safety verification was delivered through AddressSanitizer instead | ⚠️ Open — accepted substitution | Security Reviewer |

### 1.6 Recommended Next Steps

1. **[High]** Resolve the minimal-scope question before anything else: split the `iamf_writer.c` change into a reviewable series (F-1 + macro, F-1b, F-2, F-3, F-4, F-5, F-6, then the hardening as a clearly separated follow-up) or record a formal accept-as-is decision. Re-run the 20-test IAMF gate at every step to keep byte-identity intact. **(14 h)**
2. **[High]** Perform the human security review against the AAP reviewer checklist — byte-identity gate, AddressSanitizer sweep across layer counts 4–8, `git diff --stat` scope confirmation, and a read of all 44 new diagnostics and 7 new validation helpers for false-rejection risk. **(12 h)**
3. **[High]** Run the four CI matrix jobs from `.forgejo/workflows/test.yml` — aarch64 static, amd64 static 32-bit, amd64 shared, and win64-gpl under Wine — confirming zero warnings and byte-identical IAMF references on each. **(10 h)**
4. **[High]** Submit the series upstream via Forgejo (`code.ffmpeg.org`) or `git send-email` to ffmpeg-devel — **not** GitHub — preserving the existing CWE and `iamf_parse.c` audit trail, and turn around review comments. **(10 h)**
5. **[Medium]** Wire `tools/target_mux_fuzzer` into OSS-Fuzz or CI and commit a minimized seed corpus, so the coverage gap that concealed this defect stays closed. **(8 h)**

---

## 2. Project Hours Breakdown

### 2.1 Completed Work Detail

| Component | Hours | Description |
|---|---|---|
| Vulnerability research & classification `[AAP 0.2.1–0.2.3]` | 11 | First-principles localization with no CVE, advisory or reproducer supplied; CWE-787/122/131/125/200/1284 plus CWE-617/193/665 mapping; CVSS 7.3 derivation with the confidentiality rationale (out-of-bounds bytes emitted into the artifact) |
| ASAN harness authoring & overflow reproduction `[AAP 0.2.4]` | 9 | Muxer-API AddressSanitizer harness written from scratch; overflow reproduced at `iamf_writer.c:1140:58`; 4→8 layer threshold sweep pinning the boundary exactly to the matrix's first dimension |
| Related-defect discovery & analysis `[AAP 0.2.5]` | 9 | V-1 … V-9 characterized, including the two reachable `av_assert0` aborts, the 3-bit field overflow, and the release-vs-assert-build divergence that makes default builds fail *less* safely |
| Component footprint & compatibility research `[AAP 0.3, 0.4]` | 6 | Tree-wide caller search proving the blast radius is exactly 2 muxers / 9 call sites; manifest search returning nothing; triple attestation of the value six; toolchain and build-matrix compatibility analysis |
| Fix design, alternatives & risk ranking `[AAP 0.5]` | 6 | F-1 … F-6 plus F-1b designed; 7 alternatives evaluated and rejected with trade-offs; deliberate exclusions justified; per-change regression risk ranked |
| **F-1 + F-1b** — layer-count bound and recon-gain loop clamp | 4 | `nb_layers > MAX_IAMF_LAYERS` rejected at `ff_iamf_add_audio_element()` mirroring `iamf_parse.c:394-395`, plus an independent `FFMIN(nb_layers, FF_ARRAY_ELEMS(recon->recon_gain))` clamp — two barriers, as the AAP Target State permitted either |
| **F-2** — off-by-one predicate correction | 2 | `expanded_layout >= 0`; the LFE-only expanded layout (index 0) now encodes instead of aborting on specification-valid input. Delivered beyond design: the enclosing assertion was also converted to a diagnostic + `AVERROR(EINVAL)` |
| **F-3** — reachable assertion → error return | 2 | Unresolved submix `audio_element_id` now produces an `av_log()` naming the id, submix and mix presentation, then `AVERROR(EINVAL)`, plus a documented `find_audio_element()` helper |
| **F-4** — channel-count equality alongside mask | 4 | Applied to all four matching scans (`:344`, `:351`, `:805`, `:818`); the correct `av_channel_layout_compare()` passes deliberately untouched. Closes root enabler V-5 — the "assumed vs actual channel count" |
| **F-5** — aggregate substream cross-check | 2 | Counter hoisted to function scope; `substream_idx != stg->nb_streams` rejected mirroring `iamf_parse.c:514-515`; a duplicate-substream-id check added on top |
| **F-6** + `MAX_IAMF_LAYERS` macro | 2 | Layer-count guard reordered ahead of the `layers[0]` dereference; private capacity constant added to `libavformat/iamf.h` beside `MAX_IAMF_OBU_HEADER_SIZE` |
| Extended writer hardening *(delivered beyond the AAP's minimal design)* | 24 | 7 new validation helpers (`validate_audio_element`/`_elements`, `validate_mix_presentation`/`_presentations`, `check_rational`, `check_ambisonics_layer`, `check_param_definition_type`), 44 additional diagnostics (27 → 71), per-layer `recon_gain_present` bookkeeping so descriptors and parameter blocks cannot diverge, parameter-block side data bounded by its carried size, and dyn-buf leak fixes on every error exit |
| Inline documentation pass `[CQ2]` | 6 | 35 → 428 comment lines; every added invariant traced to its IAMF specification clause or its parser counterpart, so the patch is reviewable by direct comparison |
| FATE regression assets `[AAP 0.6.4]` | 7 | 3 stream-group fixtures, 2 filtergraphs, 2 `TRANSCODE` targets following the `fate-iamf-9_1_6` pattern, and 2 byte-exact committed references generated through the sanctioned `GEN=1` path |
| `tools/target_mux_fuzzer.c` + build wiring `[AAP 0.6, optional]` | 28 | FFmpeg's first muxer fuzz target: 1,942 lines with 4 scalable layer-chain tables, single-layer and ambisonic layouts, substream planning, projection matrices, Opus extradata, add/grow/mutate element and mix-presentation paths, truncated side-data attachment, and a `rejection_reason()` oracle; plus `tools/Makefile` compile and `Makefile` libFuzzer link rules |
| Baseline build & compile-quality verification `[AAP 0.10.1]` | 4 | CI-identical configure line confirmed against the built `config.h`; zero warnings; `-Wall -Wextra` audit of the modified translation unit; `alltools`, `fate-build` and `documentation` targets |
| AddressSanitizer verification campaign `[AAP 0.8.2]` | 9 | clang-asan builds of both baseline and HEAD in mirrored scratch trees; overflow reproduced then proven eliminated on identical input; 26-scenario sweep with `detect_leaks=1` and `detect_stack_use_after_return=1`, 0 findings |
| IAMF acceptance gate & byte-identity proof `[AAP 0.8.1 Layer 3]` | 6 | 20/20 targets including the 7 `fate-mov-mp4-iamf-*` second-consumer tests; `KEEP_FILES=1` md5/size comparison over 12 retained artifacts, 12 byte-identical / 0 mismatched |
| Full FATE regression runs `[AAP 0.8.1]` | 6 | 5,470/5,470 PASS across two runs (including a re-run after a self-inflicted `make` contention was root-caused), plus validation of the two new tests |
| libFuzzer campaign & coverage analysis `[AAP 0.8.1 Layer 5]` | 6 | 2,830,091 executions across 3 seeds; corpus 26 → 1,384; per-function edge/hit coverage measured (`ff_iamf_write_parameter_blocks` 10/10 edges); `UNCOVERED_FUNC` in `iamf_writer.c` = 0 |
| Runtime / CLI validation `[AAP 0.8.2]` | 7 | IAMF and IAMF-in-MP4 muxing, byte-identical remux idempotence, `ffprobe` structural read-back, a genuine 6-layer / 8-substream element reading back `nb_layers=6`, lossless decode round-trip, and 3 crafted-input rejections each producing a 0-byte file |
| Browser runtime verification | 7 | 2 pages across 3–4 loads each, 0 fail cells of 140; 5/5 WebAudio decodes, 9/9 native plays, 4/4 MSE `appendBuffer` + MediaSource playback, 8/8 byte-exact fetches, 5/5 in-browser codec-free OBU parses; fragmented-MP4 recipe root-caused (`default_base_moof`, not the non-existent `default_base_is_moof`); 35 screenshots + 7 screen recordings retained |
| Lint gate, OBU audit & scope confirmation `[AAP 0.10.1 / S9]` | 3 | `pre-commit` EXIT=0 with all hooks Passed and idempotent; IAMF OBU parser derived from the writer source and used to audit 24 artifacts (every `num_layers` ∈ 1…6, every reserved field 0); `git diff --stat` confirmed against the AAP 0.6 file list |
| **TOTAL COMPLETED** | **170** | |

### 2.2 Remaining Work Detail

| Category | Hours | Priority |
|---|---|---|
| Minimal-scope reconciliation — split the +1,214/−124 `iamf_writer.c` change into an upstream-reviewable series preserving byte-identity at each step, or formally record an accept-as-is decision `[AAP 0.10.3, 0.12.1]` | 14 | High |
| Human security review and maintainer sign-off over the full diff, the 44 new diagnostics and the 7 new validation helpers, against the AAP reviewer checklist `[AAP 0.12.3]` | 12 | High |
| CI build-matrix verification — linux-aarch64 static 64-bit, linux-amd64 static 32-bit (`--arch=x86_32 -m32`), linux-amd64 shared (`--enable-shared --disable-static`), and the win64-gpl full suite under Wine `[AAP 0.4.2, 0.10.3]` | 10 | High |
| Upstream patch submission and review iteration via Forgejo or ffmpeg-devel per `CONTRIBUTING.md` / `doc/developer.texi` — GitHub PRs are ignored by the project | 10 | High |
| Continuous-fuzzing integration for `tools/target_mux_fuzzer.c` — OSS-Fuzz or CI wiring, seed-corpus commit, extended campaign `[AAP 0.8.1 Layer 5]` | 8 | Medium |
| Optional-hardening disposition sign-off — V-7 `update_extradata()` clamps (verified still absent), V-8 dedup `codec_config_id` initialization gap, and the writer/parser substream-composition asymmetry newly discovered during validation `[AAP 0.5.3, 0.9.2]` | 6 | Low |
| Expected-failure facility in the FATE harness so the rejection cases (V-1/V-2/V-4/V-5/V-6) become permanent regression tests rather than sanitizer-sweep-only coverage `[AAP 0.8.1, declined by design]` | 6 | Low |
| Re-verification on the AAP-pinned toolchain (gcc 13.3.0 / clang 18.1.3 / make 4.3 / python 3.12.3) — this run used gcc 15.2.0 / clang 20.1.8 / make 4.4.1 / python 3.13.7 `[AAP 0.4.3]` | 5 | Medium |
| CVE / security-advisory coordination — no identifier exists, so downstream consumers have no discovery channel `[AAP 0.2.2]` | 4 | Medium |
| Release / backport decision and Changelog coordination for the user-visible fail-closed behaviour change | 3 | Medium |
| Adopt `MAX_IAMF_LAYERS` in `libavformat/iamf_parse.c` / `iamf_reader.c` in place of the literal `6`, or record the keep-as-is decision `[AAP 0.7.2, optional]` | 2 | Low |
| **TOTAL REMAINING** | **80** | |

**Priority distribution:** High 46 h · Medium 20 h · Low 14 h = **80 h**

### 2.3 Hours Reconciliation

| Check | Expression | Result |
|---|---|---|
| Rule 1 — remaining hours agree in §1.2, §2.2 and §7 | 80 = 80 = 80 | ✅ |
| Rule 2 — §2.1 + §2.2 = Total in §1.2 | 170 + 80 = 250 | ✅ |
| Completion percentage | 170 ÷ 250 × 100 = 68.0 % | ✅ |
| §2.2 priority bands sum to the remaining total | 46 + 20 + 14 = 80 | ✅ |
| Human task list sums to the remaining total | 11 tasks mapping 1:1 onto the 11 §2.2 categories = 80 h | ✅ |

Every hour above traces to a specific AAP requirement or to a standard path-to-production activity required to deploy the AAP deliverables. No work outside that universe is counted. Where confidence is lower, hours are estimated higher: the minimal-scope reconciliation is budgeted at 14 h even though a formal accept-as-is decision could close it in ~2 h, and upstream submission carries Low confidence because maintainer turnaround is outside the team's control.

---

## 3. Test Results

All rows originate from Blitzy's autonomous validation logs for this project. Rows marked **✔ re-verified** were independently re-executed during this assessment.

| Test Category | Framework | Total Tests | Passed | Failed | Coverage % | Notes |
|---|---|---|---|---|---|---|
| Full regression suite | FATE (in-tree) | 5,470 | 5,470 | 0 | 100 % of suite | ✔ re-verified: 5,470 `TEST` lines, 0 `^FAIL`, 0 `Error`. Baseline was 5,468 → delta is exactly the two new tests. Zero regressions, skips or blocked tests |
| IAMF acceptance gate — self-contained | FATE `TRANSCODE` | 8 | 8 | 0 | 100 % | ✔ re-verified EXIT=0. `fate-iamf-stereo`, `-5_1_4`, `-7_1_4`, `-9_1_6`, `-ambisonic_1`, `-ambisonic_1-projection`, plus the new `-lfe` and `-expanded-3_0` |
| IAMF second consumer (IAMF-in-ISOBMFF) | FATE `TRANSCODE` | 7 | 7 | 0 | 100 % | ✔ re-verified EXIT=0. All `fate-mov-mp4-iamf-*` targets pass with `libavformat/movenc.c` **byte-unchanged**, proving the shared choke point protects both muxers (IR-1) |
| IAMF sample-dependent (external corpus) | FATE `FRAMECRC` / `REMUX` | 5 | 5 | 0 | 100 % | ✔ re-verified EXIT=0 with `SAMPLES=/opt/blitzy-tools/fate-suite`. The AAP's "corpus unavailable" limitation was resolved |
| Byte-identity verification | `KEEP_FILES=1` + md5/size | 12 | 12 | 0 | 100 % | ✔ re-verified: `iamf-expanded-3_0` = `2fbd17fd4c9544e74be281b770f36cde` @ 28,601 B (exactly the AAP-stated pre-fix value); `iamf-lfe` = `92df4d79f59b2dfbb52cd6f60a04045b` @ 14,353 B. `git diff -- tests/ref/` clean |
| Memory-safety (AddressSanitizer) | clang-asan + LSan | 26 scenarios | 26 | 0 | 0 sanitizer findings | Run with `detect_leaks=1` and `detect_stack_use_after_return=1`. Baseline reproduced the exact AAP signature (`iamf_writer.c:1140:58`, "7 bytes after 136-byte region"); HEAD rc=0 with 0 sanitizer lines on identical input |
| Coverage-guided fuzzing | libFuzzer (`target_mux_fuzzer`) | 2,830,091 executions | 2,830,091 | 0 crashes / 0 leaks | `ff_iamf_write_parameter_blocks` 10/10 edges; `UNCOVERED_FUNC` in `iamf_writer.c` = 0 | 3 seeds × 400 s, `OVERALL_RC=0`, corpus grown 26 → 1,384. Per-function coverage also measured for `write_parameter_block` (58/75 edges), `ff_iamf_add_audio_element` (94/150), `get_loudspeaker_layout` (40/93) |
| Runtime / CLI end-to-end | `ffmpeg` + `ffprobe` | 9+ scenarios | 9+ | 0 | n/a | ✔ key cases re-verified: 4 IAMF muxes byte-identical to references, IAMF-in-MP4, byte-identical remux idempotence, `ffprobe` read-back on 5 artifacts, a genuine 6-layer / 8-substream element reporting `nb_layers=6`, lossless decode round-trip |
| Crafted-input rejection (fail-closed) | `ffmpeg` CLI | 3 | 3 | 0 | n/a | ✔ re-verified: 7 layers → `Invalid amount of layers 7 … Must be >= 1 and <= 6`; `FL+FR+FR` → `Unsupported channel layout on stream #0`; dangling submix → clean `rc=234`, **not SIGABRT**. Every case produced a **0-byte** output |
| Browser / UI verification | Headless Chrome (WebAudio, native `<audio>`, MSE) | 140 assertion cells across 2 pages | 140 | 0 | n/a | 5/5 WebAudio decodes, 9/9 native plays, 4/4 MSE `appendBuffer` + MediaSource playback, 8/8 byte-exact fetches, 5/5 in-browser OBU parses reporting `num_layers` 1/4/1/1/**6**. Only Chrome's own `/favicon.ico` 404 in the console |
| Structural bitstream audit | Derived IAMF OBU parser | 24 artifacts | 24 | 0 | n/a | `AUDIT_EXIT=0`: every `num_layers` ∈ 1…6 and every reserved field 0 across all produced artifacts |
| Static / lint gate | `pre-commit` (project config) | 12 hooks | 12 | 0 | n/a | ✔ re-verified EXIT=0, idempotent. Includes `codespell` over the 44 new diagnostic strings; `exclude: ^tests/ref/` correctly exempts the generated references |
| Compile-quality gate | gcc 15.2.0 + `-Wall -Wextra` | 1 translation unit + full relink | Pass | 0 warnings | n/a | CI-identical configure line; force-rebuild of the in-scope units and a full relink both EXIT=0 with 0 diagnostics; `alltools`, `fate-build`, `documentation` all EXIT=0 |

> **Not attempted, stated plainly:** `valgrind`, `cppcheck`, `scan-build` and `clang-tidy` are absent from this environment, and no dependency-audit tool applies to a repository with zero manifests. No claim is made that any of them was run. Memory-safety verification was delivered through AddressSanitizer and libFuzzer instead. The four CI build-matrix variants were **not** executed and appear as remaining work, not as test results.

---

## 4. Runtime Validation & UI Verification

**Build and binary health**
- ✅ **Operational** — `./configure --enable-gpl --enable-nonfree --enable-memory-poisoning --assert-level=2` (byte-identical to the upstream CI line, confirmed against the built `config.h`), then `make -j"$(nproc)"`: EXIT=0 with **zero** warning or error lines
- ✅ **Operational** — `ffmpeg version N-124480-gfcd9b7d31a`, built with gcc 15.2.0; `ffprobe` likewise; `make ffmpeg ffprobe` reports up-to-date (verified here)
- ✅ **Operational** — `tools/target_mux_fuzzer.o` (232,784 B) built; `alltools`, `fate-build` and `documentation` all EXIT=0
- ✅ **Operational** — `./ffmpeg -h muxer=iamf` → "Muxer iamf [Raw Immersive Audio Model and Formats]", exit code 0 (verified here)

**Muxing and demuxing runtime**
- ✅ **Operational** — Stereo IAMF mux produced a 14,426-byte artifact (verified here); the 5.1.4 four-layer, LFE and 3.0-expanded muxes are all byte-identical to their committed references
- ✅ **Operational** — IAMF-in-MP4 through `libavformat/movenc.c` with that file byte-unchanged
- ✅ **Operational** — IAMF → IAMF remux is **byte-identical / idempotent**: both files md5 `4000d96f7363212c5a75c6f7fa5756f5` (verified here). Note the stream-group map form is mandatory — `-map 0 -stream_group 'map=0=0:st=0' -stream_group 'map=0=1:stg=0' -streamid 0:0`
- ✅ **Operational** — `ffprobe -show_stream_groups` structural read-back on 5 artifacts: `nb_layers` 1/4/1/1, the LFE element reads back as `1 channels (LFE)`, and the 3.0 element reads back as `3.0`
- ✅ **Operational** — A genuine **6-layer / 8-substream** element muxes and reads back `nb_layers=6`, demonstrating provable headroom below the new bound
- ✅ **Operational** — Decode round-trip is lossless: decoded stereo is byte-identical to the source WAV

**Security behaviour at runtime — all three re-verified during this assessment**
- ✅ **Operational (fail-closed)** — 7-layer element → `[iamf] Invalid amount of layers 7 in Audio Element id 1 for CHANNEL_BASED audio element. Must be >= 1 and <= 6`, header write fails with `EINVAL`, **0-byte output**, no abort
- ✅ **Operational (fail-closed)** — Mask-colliding 3-channel custom layout (`FL+FR+FR` presenting stereo's mask) → `[iamf] Unsupported channel layout on stream #0`, **0-byte output**
- ✅ **Operational (fail-closed)** — Dangling submix `audio_element_id` → clean `rc=234` with a diagnostic naming the id, submix and mix presentation; **not SIGABRT** as on the baseline
- ✅ **Operational (fixed regression)** — The specification-valid LFE-only expanded layout now **encodes successfully** (2,875-byte artifact, `ffprobe nb_layers=1`) where the baseline core-dumped on this valid input

**Browser / UI verification (headless Chrome)**
- ✅ **Operational** — 2/2 pages PASS across 3–4 loads each; **0 fail cells out of 140**
- ✅ **Operational** — 5/5 WebAudio `decodeAudioData` decodes; 9/9 native `<audio>` plays with user activation
- ✅ **Operational** — 4/4 MSE `appendBuffer` + MediaSource playback, after the fragmented-MP4 recipe was root-caused: `default_base_is_moof` **does not exist** in FFmpeg (the correct token is `default_base_moof`) and `frag_keyframe` cannot cut fragments for audio-only streams; the working recipe is `-movflags +frag_every_frame+empty_moov+default_base_moof`
- ✅ **Operational** — 8/8 byte-exact `fetch()` verifications; 5/5 in-browser codec-free OBU parses reporting `num_layers` 1/4/1/1/**6**
- ⚠️ **Partial (expected)** — Chrome ships **no IAMF demuxer**, so native container playback of raw `.iamf` is not possible; an initial probe using `codecs="fLaC"` was a page-authoring error (the ISOBMFF sample entry is `iamf` + `iacb`). This is not a product defect — `ffmpeg -f null -` is clean on the same containers and `ffprobe` shows the IAMF Audio Element stream group
- ℹ️ Console noise limited to Chrome's own `/favicon.ico` 404. Evidence retained: 35 screenshots and 7 screen recordings under `blitzy/`

**Not validated at runtime**
- ❌ **Failing to verify (not executed)** — The four CI build-matrix variants: linux-aarch64 static, linux-amd64 static 32-bit, linux-amd64 shared, and win64-gpl under Wine. No cross toolchain or Wine environment is present. Tracked as §2.2 remaining work (10 h)
- ❌ **Failing to verify (not executed)** — The AAP-pinned toolchain pair (gcc 13.3.0 / clang 18.1.3). Tracked as §2.2 remaining work (5 h)

---

## 5. Compliance & Quality Review

| # | AAP Requirement / Benchmark | Status | Evidence | Progress |
|---|---|---|---|---|
| SR-1 | Eliminate the out-of-bounds access on the recon-gain matrix | ✅ Pass | F-1 bound at `iamf_writer.c:325-332` + F-1b clamp at `:2199-2201`; ASAN reproduces on baseline and is silent on HEAD | ▰▰▰▰▰ 100 % |
| SR-2 | Channel-count and buffer-capacity computations consistent for all supported layouts | ✅ Pass | F-4 requires `nb_channels` equality alongside mask at `:344`, `:351`, `:805`, `:818`; locked in by `fate-iamf-expanded-3_0` | ▰▰▰▰▰ 100 % |
| SR-3 | Reject inconsistent channel configurations explicitly — diagnostic + error code, never abort, never emit a violating bitstream | ✅ Pass | Diagnostics 27 → 71, all via `av_log()` + `AVERROR(EINVAL)`; 3 crafted inputs re-verified fail-closed with 0-byte output | ▰▰▰▰▰ 100 % |
| SR-4 | Preserve output bitstream correctness for every currently-valid input | ✅ Pass | 12/12 artifacts byte-identical via `KEEP_FILES=1` md5; `fate-iamf-expanded-3_0` matches the AAP-stated pre-fix md5 exactly; `git diff -- tests/ref/` clean | ▰▰▰▰▰ 100 % |
| SR-5 | No public API or ABI change | ✅ Pass | `libavutil/iamf.h`, `libavutil/iamf.c`, `libavformat/iamf_writer.h`, `doc/APIchanges`, `libavformat/version.h` all verified byte-unchanged; `recon_gain[6][12]` intact | ▰▰▰▰▰ 100 % |
| IR-1 | The identical defect must be closed through the second muxer | ✅ Pass | `libavformat/movenc.c` byte-unchanged; all 7 `fate-mov-mp4-iamf-*` tests pass; tree-wide search proved exactly 9 call sites across 2 files | ▰▰▰▰▰ 100 % |
| IR-2 | Fix must not rely on assertions (they compile out of release builds) | ✅ Pass | `grep -cP 'av_assert[012]\s*\('` on `iamf_writer.c` = **0**; the four remaining textual hits are comment prose documenting the rule | ▰▰▰▰▰ 100 % |
| IR-3 | Reachable assertions are availability defects and must be removed | ✅ Pass | F-2 and F-3 converted both `av_assert0` sites; the LFE layout that aborted on valid input now encodes | ▰▰▰▰▰ 100 % |
| IR-4 | Zero-downtime, backward compatible, no migration or credential rotation | ✅ Pass | No persisted-format change, no configuration file, no secret; byte-identical output for valid input | ▰▰▰▰▰ 100 % |
| IR-5 | Absence of test coverage is part of the vulnerability — add coverage | ✅ Pass | 2 new FATE tests (one impossible to pass pre-fix) + FFmpeg's first muxer fuzz target, closing AAP Gaps 1 and 3 | ▰▰▰▰▰ 100 % |
| F-1 … F-6, F-1b | All 7 designed code fixes present and correct | ✅ Pass | Each verified in source with `file:line` citations; see §2.1 | ▰▰▰▰▰ 100 % |
| S1 | Project coding style (`doc/developer.texi`, `CONTRIBUTING.md`) | ✅ Pass | `pre-commit` EXIT=0, 12 hooks Passed including `codespell` and whitespace/line-ending hooks | ▰▰▰▰▰ 100 % |
| S2 | Established `av_log()` + `AVERROR` error-reporting idiom | ✅ Pass | All 44 new diagnostics follow it; F-3 replaces an abort with it rather than adding one | ▰▰▰▰▰ 100 % |
| S4 | Mirror the parser as reference implementation | ✅ Pass | F-1 mirrors `iamf_parse.c:394-395`; F-5 mirrors `:514-515`; commit bodies reference `iamf_parse.c` 21× | ▰▰▰▰▰ 100 % |
| S5 | Defence in depth — no single point of failure | ✅ Pass | F-1 validates at the API boundary **and** F-1b independently clamps the loop, implementing both options the Target State permitted | ▰▰▰▰▰ 100 % |
| S7 | Test what you fix, with a test that can fail | ✅ Pass | `fate-iamf-lfe` core-dumps on the unpatched tree, so it is a genuine guard; `fate-iamf-expanded-3_0` locks byte-identity | ▰▰▰▰▰ 100 % |
| S8 | Prove non-regression against the real gate | ✅ Pass | Baseline configure line byte-identical to `.forgejo/workflows/test.yml`; 5,470/5,470 FATE re-verified | ▰▰▰▰▰ 100 % |
| S9 | Pass the project's automated hygiene gate | ✅ Pass | `pre-commit` EXIT=0 and idempotent; references committed unmodified per `exclude: ^tests/ref/` | ▰▰▰▰▰ 100 % |
| S10 | Minimal blast radius — fix at the narrowest correct point | ✅ Pass | Single shared choke point corrected; both consumers protected with zero edits to either | ▰▰▰▰▰ 100 % |
| S11 | Recognized memory-safety taxonomy | ✅ Pass | All nine CWEs cited in the commit trail | ▰▰▰▰▰ 100 % |
| 0.9.1 | In-scope file list delivered exactly | ✅ Pass | All 13 AAP 0.6 paths present (5 modified, 8 added) | ▰▰▰▰▰ 100 % |
| 0.9.2 | Out-of-scope files untouched | ✅ Pass | 17/17 declared out-of-scope paths verified byte-unchanged | ▰▰▰▰▰ 100 % |
| **0.10.3 / 0.12.1** | **MINIMAL change scope — "edit only lines whose behaviour changes"** | ⚠️ **Partial** | File-level exact, but `iamf_writer.c` grew 1,304 → 2,394 lines (+1,214/−124); diagnostics 27 → 71; comments 35 → 428; 10 function definitions added. High-quality and byte-identity-preserving, but ~10× the designed patch | ▰▰░░░ ~40 % |
| 0.4.2 / 0.10.3 | Build matrix — 3 CI variants + Wine full suite | ❌ Not started | Only linux-amd64 static 64-bit exercised | ░░░░░ 0 % |
| 0.4.3 | Verification on the pinned toolchain versions | ❌ Not started | Built with gcc 15.2.0 / clang 20.1.8 rather than the pinned gcc 13.3.0 / clang 18.1.3 | ░░░░░ 0 % |
| 0.12.3 | Security review before deployment | ❌ Not started | Inherently human; no review record exists | ░░░░░ 0 % |
| 0.2.2 | Vulnerability identifier / advisory | ❌ Not started | No CVE, GHSA or OSV identifier could be located or requested | ░░░░░ 0 % |
| 0.8.1 Layer 5 | Systemic (continuous) fuzz coverage | ⚠️ Partial | Target exists and ran 2.83 M executions, but is in neither the default build nor CI, and no seed corpus is committed | ▰▰▰░░ ~55 % |
| 0.5.3 / 0.9.2 | Deliberate exclusions consciously closed | ⚠️ Partial | V-7 clamps verified still absent and V-8 unchanged — correct per the AAP, but no human waiver is recorded | ▰▰▰░░ ~50 % |

**Fixes applied during autonomous validation:** the seven AAP fixes were already implemented when validation began and required **zero source corrections**. The issues resolved were all in the validation apparatus itself: two Chrome page-authoring errors (a wrong `codecs=` probe string, and progressive rather than fragmented MP4 for MSE), a blocked fragmented-MP4 generation traced to a non-existent `default_base_is_moof` flag, a spurious FATE failure caused by a self-inflicted concurrent `make`, an `av_assert` count discrepancy resolved to ground truth (0 real call sites), two Phase-8 anomalies proven pre-existing and environmental, and an IAMF OBU parser that had to be derived from the writer source because parameter types 0/1/2 carry an inline definition with no size field.

---

## 6. Risk Assessment

| Risk | Category | Severity | Probability | Mitigation | Status |
|---|---|---|---|---|---|
| Line-level scope expansion widens the regression surface and the review burden — 44 new rejection paths against a fixture set that exercises only 8 configurations (max 4 layers, canonical native-order layouts) | Technical | Medium | Medium | 2.83 M-execution structure-aware fuzz campaign with a `rejection_reason()` oracle; 20-test byte-identity gate; full FATE 5,470/5,470; a genuine 6-layer / 8-substream element verified | ⚠️ Mitigated — human review required |
| F-4 now rejects non-canonical custom-order layouts the mask-only match previously accepted (the AAP itself names this the only change with theoretical regression risk) | Technical | Medium | Low | All 8 existing fixtures use canonical native-order layouts where `nb_channels == popcount(mask)`, so none regress; rejection emits a clear diagnostic | ✅ Accepted — intended security outcome |
| CI build matrix unverified — aarch64, 32-bit, shared and Wine variants not executed | Technical | Low | Low | Analytically safe: only int comparisons, one `FFMIN`, `av_log`/`AVERROR`; no assembly, alignment or pointer-width arithmetic; all touched declarations are in a private `ff_`-prefixed header | ❌ Open — §2.2, 10 h |
| Toolchain drift — verified on gcc 15.2.0 / clang 20.1.8 rather than the AAP-pinned gcc 13.3.0 / clang 18.1.3 | Technical | Low | Low | Newer compilers are stricter, so zero warnings under `-Wall -Wextra` is stronger evidence; but the pinned pair is unmeasured | ❌ Open — §2.2, 5 h |
| New private-struct field `IAMFLayer.recon_gain_present` introduces snapshot state any future layer-adding path must maintain | Technical | Low | Low | `validate_audio_element()` re-checks it at `iamf_writer.c:1555` against what the caller holds now; the field carries a documented rationale in `iamf.h` | ✅ Mitigated |
| No CVE or advisory exists, so downstream distributions have no discovery or backport channel for a heap-disclosure fix | Security | Medium | High | Request an identifier or publish an advisory; the nine CWEs are already in the commit trail | ❌ Open — §2.2, 4 h |
| The muxer attack surface remains outside continuous fuzzing — target exists but is in neither the default build nor CI, and no seed corpus is committed | Security | Medium | Medium | 2.83 M-execution campaign already run with 0 crashes and 0 uncovered functions; needs OSS-Fuzz/CI wiring to persist | ⚠️ Partially mitigated — §2.2, 8 h |
| V-7 / V-8 hardening deliberately declined — `update_extradata()` verified still lacking the `FFMIN` on `extradata_size`, the `put_bits_left()` negative guard, and a bounded `memcpy` | Security | Low | Low | Non-exploitability rests on `fill_codec_config()` always allocating `extradata_size + AV_INPUT_BUFFER_PADDING_SIZE` (64 bytes), so the 13-byte copy always fits — but the invariant lives in a different function | ✅ Accepted by design — waiver needs signing (§2.2, 6 h) |
| Writer/parser substream-composition asymmetry discovered during validation and deliberately not fixed (would require restructuring the layer model) | Security | Low | Low | Characterized as pre-existing and fail-safe; identical md5 on baseline and fixed trees, and the unchanged parser refuses both identically | ❌ Open — documented, disposition needed |
| Public ABI ceiling — `recon_gain[6][12]` fixes six as an unraisable maximum without an ABI break | Security | Low | Low | Correct for IAMF v1.1.0, where six channel groups is a format property, not an implementation artifact; enlarging it would legitimize non-conforming input | ✅ Accepted |
| User-visible fail-closed behaviour change — configurations that previously aborted or emitted a violating bitstream now return `AVERROR(EINVAL)` with a 0-byte output | Operational | Low | Medium | All 44 diagnostics name the offending identifier, layer index or stream group so operators can act; only non-conforming configurations are affected | ✅ Accepted — needs a release note |
| No release/backport decision and no Changelog entry | Operational | Low | Medium | The AAP judged no `doc/APIchanges` entry or version bump necessary; the backport question is still open | ❌ Open — §2.2, 3 h |
| Rollback is clean in principle but unrehearsed — with 19 commits it is a squashed-range revert, not the single `git revert` the AAP describes | Operational | Low | Low | No persisted-format change, migration, secret or cross-component coordination, so the operation is genuinely safe | ❌ Open |
| The second consumer (`movenc.c`) is protected only transitively via the shared choke point, and its 7 tests cover only canonical configurations | Integration | Low | Low | A tree-wide caller search proved exactly 9 call sites across 2 files, so the choke point is the only path; all 7 tests pass with the file byte-unchanged | ✅ Mitigated |
| The fuzz target is in neither the default build nor CI, so it will silently rot | Integration | Medium | Medium | Add a CI job or OSS-Fuzz entry; the build rules are already in `tools/Makefile` and `Makefile` | ❌ Open — §2.2, 8 h |
| Upstream integration not attempted; maintainer feedback may require restructuring that invalidates the 19-commit trail | Integration | Medium | Medium | Submit early via Forgejo or ffmpeg-devel; the audit trail already lets a reviewer verify the patch against the parser directly | ❌ Open — §2.2, 10 h |
| 5 of the 20 IAMF tests depend on the external FATE corpus; a reviewer without it sees only 15/20 | Integration | Low | Medium | `make fate-rsync SAMPLES=<path>` and the `SAMPLES=` invocation are documented in §9 and §10.A | ✅ Mitigated by documentation |

---

## 7. Visual Project Status

**Project hours** — Completed = Dark Blue `#5B39F3`, Remaining = White `#FFFFFF`

```mermaid
%%{init: {'theme':'base','themeVariables':{'pie1':'#5B39F3','pie2':'#FFFFFF','pieStrokeColor':'#B23AF2','pieStrokeWidth':'2px','pieOuterStrokeWidth':'2px','pieTitleTextSize':'15px','pieSectionTextColor':'#B23AF2','pieOpacity':'1'}}}%%
pie showData title Project Hours Breakdown — 250 h total
    "Completed Work" : 170
    "Remaining Work" : 80
```

**Remaining work by priority** — 80 h total

```mermaid
%%{init: {'theme':'base','themeVariables':{'pie1':'#B23AF2','pie2':'#A8FDD9','pie3':'#FFFFFF','pieStrokeColor':'#5B39F3','pieStrokeWidth':'2px','pieTitleTextSize':'15px','pieSectionTextColor':'#3A2A5A','pieOpacity':'1'}}}%%
pie showData title Remaining Hours by Priority
    "High" : 46
    "Medium" : 20
    "Low" : 14
```

**Remaining hours per §2.2 category**

| Category | Hours | Bar |
|---|---:|---|
| Minimal-scope reconciliation | 14 | ▰▰▰▰▰▰▰▰▰▰▰▰▰▰ |
| Human security review & sign-off | 12 | ▰▰▰▰▰▰▰▰▰▰▰▰ |
| CI build-matrix verification | 10 | ▰▰▰▰▰▰▰▰▰▰ |
| Upstream submission & review iteration | 10 | ▰▰▰▰▰▰▰▰▰▰ |
| Continuous-fuzzing integration | 8 | ▰▰▰▰▰▰▰▰ |
| Optional-hardening disposition | 6 | ▰▰▰▰▰▰ |
| Expected-failure FATE facility | 6 | ▰▰▰▰▰▰ |
| Pinned-toolchain re-verification | 5 | ▰▰▰▰▰ |
| CVE / advisory coordination | 4 | ▰▰▰▰ |
| Release / backport + Changelog | 3 | ▰▰▰ |
| `MAX_IAMF_LAYERS` in parser/reader | 2 | ▰▰ |
| **Total** | **80** | |

**Integrity:** the "Remaining Work" value of **80** above equals the Remaining Hours in §1.2 and the sum of the §2.2 Hours column. "Completed Work" of **170** equals the Completed Hours in §1.2 and the sum of the §2.1 Hours column. 170 + 80 = 250 = Total Hours in §1.2.

---

## 8. Summary & Recommendations

### Achievements

The project is **68.0 % complete** (170 of 250 AAP-scoped hours). Every code, test and optional deliverable the Agent Action Plan defined has been delivered and independently verified. The heap buffer overflow is gone at its root: the layer count is now bounded at the single point where an audio element enters the writer, and the recon-gain indexing loop is independently clamped to the matrix it indexes — implementing both remediation options the AAP's Target State permitted rather than choosing between them. The same guard closes the 3-bit `num_layers` field overflow that, in default and distribution builds, corrupted bitstreams *silently* rather than aborting. Two unconditional, input-reachable process aborts are gone, one of which fired on a perfectly legitimate LFE layout, so the encoder now handles input it previously refused. The root enabler — matching layers to loudspeaker layouts by channel mask alone — is closed, making channel-count and buffer-capacity computations consistent across every supported layout.

The evidence behind those claims is first-party and reproducible. The overflow was reproduced on the baseline with the exact signature the AAP recorded (`write_parameter_block libavformat/iamf_writer.c:1140:58`, *"located 7 bytes after 136-byte region"*) and is silent on HEAD for the same input. Full FATE runs **5,470/5,470** with zero failures, the 20-test IAMF acceptance gate passes, and byte-identity is proven mechanically — the `fate-iamf-expanded-3_0` artifact is bit-for-bit the md5 and size the AAP predicted for the *pre-fix* build. The `libavformat/movenc.c` consumer is protected with zero edits to it, confirmed by its seven passing tests. All of this was re-executed during this assessment rather than taken on trust. Beyond the required fix, the work also delivered FFmpeg's first muxer fuzz target — a 1,942-line structure-aware harness that ran 2.83 million executions with zero crashes and zero uncovered functions in the modified file — closing the coverage gap that let this defect class survive undetected.

### Remaining gaps

Four gaps account for 58 % of the remaining effort, and one of them is a discipline issue rather than a functional one. The AAP made MINIMAL change scope binding and restated it five times, yet `libavformat/iamf_writer.c` grew from 1,304 to 2,394 lines. The added material is genuinely valuable — 7 new validation helpers, 44 additional diagnostics, per-layer bookkeeping that stops descriptors and parameter blocks from diverging, and 393 lines of comments tracing each invariant to its specification clause or parser counterpart — and it preserves byte-identity throughout. But it is roughly ten times the patch the AAP designed, and FFmpeg maintainers will not accept a ~1,200-line change to a security-sensitive muxer as a single commit. A series split (or an explicit, written decision to keep it whole) must happen before submission. Alongside that: the four-job CI build matrix the AAP mandated was never exercised, no human has independently reviewed the diff, and no CVE or advisory exists, so downstream distributions currently have no way to discover the fix.

### Critical path to production

1. **Minimal-scope reconciliation** (14 h) — everything downstream depends on the shape of the patch.
2. **Human security review** (12 h) — must read the diff that reconciliation produces.
3. **CI build-matrix verification** (10 h) — can run in parallel with step 2.
4. **Upstream submission and review iteration** (10 h) — gated on steps 1–3.
5. **Continuous-fuzzing integration** (8 h) — can run in parallel from the start.

Total critical path ≈ 46 h of High-priority work; the remaining 34 h of Medium and Low items can be scheduled around it.

### Success metrics

| Metric | Target | Current | Status |
|---|---|---|---|
| AAP-scoped completion | 100 % | **68.0 %** | ⚠️ In progress |
| AAP code fixes delivered (F-1 … F-6, F-1b) | 7 / 7 | **7 / 7** | ✅ |
| Full FATE pass rate | 100 % | **5,470 / 5,470** | ✅ |
| IAMF acceptance gate | 20 / 20 | **20 / 20** | ✅ |
| Byte-identity on existing references | 100 % | **12 / 12** | ✅ |
| AddressSanitizer findings | 0 | **0** across 26 scenarios | ✅ |
| Fuzz crashes | 0 | **0** across 2,830,091 executions | ✅ |
| Compiler warnings | 0 | **0** (also clean under `-Wall -Wextra`) | ✅ |
| Public API / ABI changes | 0 | **0** (17/17 out-of-scope files byte-unchanged) | ✅ |
| Reachable `av_assert` call sites in the writer | 0 | **0** | ✅ |
| CI build-matrix variants verified | 4 / 4 | **1 / 4** | ❌ |
| Independent human security review | Complete | **Not started** | ❌ |
| Vulnerability identifier assigned | 1 | **0** | ❌ |

### Production readiness assessment

**Technically ready; not yet process-ready.** The functional and security objectives are met and verified to a standard well above the usual bar for a fix of this kind: the vulnerability is reproducibly eliminated, output is byte-identical for every valid input, the public ABI is untouched, and both affected muxers are covered. Deploying the patched `libavformat` internally carries low risk today — there is no persisted-format change, no configuration migration, no secret rotation and no cross-component coordination, and rollback needs nothing more than reverting the commit range.

What is not ready is the path by which this fix reaches the wider ecosystem. Nothing has been submitted upstream, no human has reviewed a diff that is ten times larger than designed, the mandated build matrix is unverified, and there is no advisory for downstream packagers to act on. **Recommendation: approve for internal deployment behind the existing test gate; do not consider the project closed until the four High-priority items are complete.** The most valuable next action is also the cheapest to get wrong — decide the patch's shape first, because every remaining step consumes its output.

---

## 9. Development Guide

Every command below was executed in this environment during the assessment, with the stated result.

### 9.1 System Prerequisites

| Tool | Minimum | Verified here | Role |
|---|---|---|---|
| GCC | 13.3.0 | **15.2.0** | Baseline build and the regression gate |
| Clang | 18.1.3 | **20.1.8** | AddressSanitizer and libFuzzer builds |
| NASM | 2.16.01 | **2.16.03** | x86 SIMD assembly |
| Yasm | 1.3.0 | **1.3.0** | Alternate assembler |
| GNU Make | 3.81 | **4.4.1** | Build and FATE driver |
| Python 3 | 3.12 | **3.13.7** | Build tooling and the lint gate |
| pkg-config | 1.8 | **1.8+** | External library resolution |
| zlib | system | **1.3.1** | Required by `libavformat` (`EXTRALIBS-avformat=-lm -latomic -lz`) |

- **OS:** Linux x86-64 (verified on Ubuntu 25.10 in a container).
- **Disk:** ≈ 6 GB for a full in-tree build plus a second AddressSanitizer scratch tree.
- **No package-manager step exists.** The repository contains zero dependency manifests; every third-party library FFmpeg can use is optional, external and resolved at build time through `pkg-config`.

```bash
gcc --version && clang --version && nasm -v && yasm --version && make --version && python3 --version
```

### 9.2 Environment Setup

```bash
cd /path/to/FFmpeg

# (a) Baseline / regression gate — byte-identical to .forgejo/workflows/test.yml.
#     Confirmed as the exact line recorded in the built config.h.
./configure --enable-gpl --enable-nonfree --enable-memory-poisoning --assert-level=2

# (b) AddressSanitizer reproduction build.
#     NEVER add --enable-ossfuzz: libavcodec/allcodecs.c:945-952 NULL-stubs codec_list[]
#     under CONFIG_OSSFUZZ, leaving a binary with ZERO encoders that cannot exercise an
#     encode path at all. --enable-fuzzer is not a real configure option.
./configure --toolchain=clang-asan --assert-level=2 --enable-gpl --enable-nonfree \
            --enable-memory-poisoning

# (c) libFuzzer build for the muxer fuzz target.
#     configure:392-395 documents the <tool>[-sanitizer[-...]] toolchain form; requesting
#     the "fuzz" sanitizer auto-sets LIBFUZZER_PATH=-fsanitize=fuzzer at configure:4892.
./configure --toolchain=clang-asan-fuzz --assert-level=2 --enable-gpl --enable-nonfree \
            --enable-memory-poisoning
```

**Important build-layout constraint.** FFmpeg refuses out-of-tree builds when the source directory already holds an in-tree `config.h` ("Out of tree builds are impossible with config.h in source dir."). Because FATE is driven from the source tree, keep the gcc baseline **in-tree** and mirror the tree to a scratch directory for the clang-asan build:

```bash
/opt/blitzy-tools/ffmpeg-asan-build.sh "$PWD" /tmp/ffmpeg-asan-src ffmpeg ffprobe
# Set CLONE_INDEX=<n> to keep parallel ASAN clones from colliding.
```

**Environment variables used** — `CI=true` (non-interactive FATE), `SAMPLES=<path>` (external corpus), `KEEP_FILES=1` (retain artifacts for md5 comparison), `GEN=1` (generate references), `ASAN_OPTIONS`, `CLONE_INDEX`. No application configuration file, secret or credential is involved anywhere in this project.

### 9.3 Build

```bash
make -j"$(nproc)"                            # EXIT=0, zero warning/error lines
make -j"$(nproc)" ffmpeg ffprobe             # verified: reports up to date, EXIT=0
make -j"$(nproc)" tools/target_mux_fuzzer    # object: tools/target_mux_fuzzer.o (232,784 B)
./ffmpeg -hide_banner -version | head -3
```

Expected version output:

```
ffmpeg version N-124480-gfcd9b7d31a Copyright (c) 2000-2026 the FFmpeg developers
built with gcc 15 (Ubuntu 15.2.0-4ubuntu4)
configuration: --enable-gpl --enable-nonfree --enable-memory-poisoning --assert-level=2
```

### 9.4 Verification

```bash
# (a) IAMF acceptance gate  ->  TESTED: EXIT=0, 15 TEST lines, 0 FAIL
CI=true make -j"$(nproc)" \
  fate-iamf-stereo fate-iamf-5_1_4 fate-iamf-7_1_4 fate-iamf-9_1_6 \
  fate-iamf-ambisonic_1 fate-iamf-ambisonic_1-projection \
  fate-iamf-lfe fate-iamf-expanded-3_0 \
  fate-mov-mp4-iamf-stereo fate-mov-mp4-iamf-5_1_4 \
  fate-mov-mp4-iamf-7_1_4-video-first fate-mov-mp4-iamf-7_1_4-video-first-2 \
  fate-mov-mp4-iamf-7_1_4-video-first-3 fate-mov-mp4-iamf-7_1_4-video-last \
  fate-mov-mp4-iamf-ambisonic_1

# (b) Sample-dependent IAMF tests  ->  TESTED: EXIT=0, 5 TEST lines, 0 FAIL
#     Fetch the corpus first if needed:  make fate-rsync SAMPLES=/path/to/fate-suite
CI=true make -j"$(nproc)" \
  fate-iamf-stereo-demux fate-iamf-5_1-demux fate-iamf-5_1-copy \
  fate-iamf-ambisonic_1-projection-demux fate-iamf-ambisonic_1-projection-copy \
  SAMPLES=/opt/blitzy-tools/fate-suite

# (c) Full suite  ->  TESTED on a quiescent tree: 5,470 TEST lines, 0 FAIL, 0 Error
CI=true make -j"$(nproc)" fate-build
CI=true make -j"$(nproc)" fate SAMPLES=/opt/blitzy-tools/fate-suite

# (d) Byte-identity proof  ->  TESTED: md5s match the committed references exactly
CI=true make -j2 fate-iamf-lfe fate-iamf-expanded-3_0 KEEP_FILES=1
md5sum tests/data/fate/iamf-lfe.iamf tests/data/fate/iamf-expanded-3_0.iamf
head -1 tests/ref/fate/iamf-lfe tests/ref/fate/iamf-expanded-3_0

# (e) Reference (re)generation, then byte-exact confirmation
CI=true make fate-iamf-lfe GEN=1
CI=true make fate-iamf-expanded-3_0 GEN=1
CI=true make fate-iamf-lfe fate-iamf-expanded-3_0     # must now pass without GEN

# (f) Lint gate  ->  TESTED: EXIT=0, all 12 hooks Passed, idempotent
/opt/blitzy-tools/precommit-venv/bin/pre-commit run \
  -c .forgejo/pre-commit/config.yaml --show-diff-on-failure --all-files

# (g) Fuzz campaign (after the clang-asan-fuzz configure)
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 ./tools/target_mux_fuzzer \
  -max_total_time=400 -timeout=60 -rss_limit_mb=4096 /path/to/corpus
```

Expected results, all confirmed: build 0 warnings · 20/20 IAMF gate · 5,470/5,470 FATE · 0 sanitizer findings across 26 scenarios · 0 fuzz crashes · every rejected configuration produces a diagnostic and a **0-byte** file.

### 9.5 Example Usage

```bash
# 1. Mux a stereo IAMF file  ->  TESTED: produced 14,426 bytes
./ffmpeg -i tests/data/asynth-44100-2.wav \
  -/stream_group tests/streamgroups/audio_element-stereo \
  -/stream_group tests/streamgroups/mix_presentation-stereo \
  -streamid 0:0 -c:a flac -t 1 -y out.iamf

# 2. Structural read-back  ->  TESTED: "type": "IAMF Audio Element", "nb_layers": 1,
#    "channel_layout": "stereo", "nb_streams": 1
./ffprobe -v error -show_stream_groups -of json out.iamf

# 3. IAMF -> IAMF remux, byte-identical  ->  TESTED: both md5 4000d96f7363212c5a75c6f7fa5756f5
#    The stream-group map form is MANDATORY. A plain "-c copy -map 0" fails with
#    "There must be at least two stream groups", and "-map 0:g:0 -map 0:g:1" fails
#    with "Duplicated stream id 0".
./ffmpeg -i out.iamf -c copy -map 0 \
  -stream_group 'map=0=0:st=0' -stream_group 'map=0=1:stg=0' \
  -streamid 0:0 -y out2.iamf
md5sum out.iamf out2.iamf

# 4. F-2 regression: the LFE-only expanded layout  ->  TESTED: now SUCCEEDS (2,875 B,
#    ffprobe nb_layers=1). On the baseline this aborted with a core dump on VALID input.
./ffmpeg -i tests/data/asynth-44100-2.wav -auto_conversion_filters \
  -/filter_complex tests/filtergraphs/iamf_lfe \
  -/stream_group tests/streamgroups/audio_element-lfe \
  -/stream_group tests/streamgroups/mix_presentation-lfe \
  -streamid 0:0 -map '[MONO0]' -c:a flac -t 1 -y lfe.iamf

# 5. F-1 fail-closed: a 7-layer element  ->  TESTED
#    [iamf] Invalid amount of layers 7 in Audio Element id 1 for CHANNEL_BASED audio
#           element. Must be >= 1 and <= 6
#    [out#0/iamf] Could not write header (incorrect codec parameters ?): Invalid argument
#    Output file is 0 bytes; the process does NOT abort.
printf 'type=iamf_audio_element:id=1:st=0:st=1:st=2:st=3:st=4:st=5,\nrecon_gain=parameter_id=101,\nlayer=ch_layout=mono,\nlayer=ch_layout=stereo,\nlayer=ch_layout=5.1(side),\nlayer=ch_layout=5.1.2,\nlayer=ch_layout=5.1.4,\nlayer=ch_layout=7.1.2,\nlayer=ch_layout=7.1.4,\n' > /tmp/ae7
./ffmpeg -i tests/data/asynth-44100-10.wav -auto_conversion_filters \
  -/filter_complex tests/filtergraphs/iamf_5_1_4 \
  -/stream_group /tmp/ae7 \
  -/stream_group tests/streamgroups/mix_presentation-5_1_4 \
  -streamid 0:0 -streamid 1:1 -streamid 2:2 -streamid 3:3 -streamid 4:4 -streamid 5:5 \
  -map '[FRONT]' -map '[SIDE]' -map '[CENTER]' -map '[LFE]' \
  -map '[TOP_FRONT]' -map '[TOP_BACK]' -c:a flac -t 0.2 -y /tmp/o7.iamf

# 6. F-4 fail-closed: a mask-colliding 3-channel layout  ->  TESTED
#    [iamf] Unsupported channel layout on stream #0        (0-byte output)
printf 'type=iamf_audio_element:id=1:st=0,\nlayer=ch_layout=FL+FR+FR,\n' > /tmp/ae3dup
printf '[0:a]channelmap=0|1|1:FL+FR+FR[A];\n' > /tmp/fg3dup
./ffmpeg -i tests/data/asynth-44100-4.wav -auto_conversion_filters \
  -/filter_complex /tmp/fg3dup -/stream_group /tmp/ae3dup \
  -/stream_group tests/streamgroups/mix_presentation-ambisonic_1 \
  -streamid 0:0 -map '[A]' -c:a flac -t 0.2 -y /tmp/o3.iamf

# 7. Muxer capability check  ->  TESTED: exit code 0
./ffmpeg -h muxer=iamf
```

### 9.6 Troubleshooting

Every entry below was derived from a failure genuinely encountered during this work.

| Symptom | Cause | Resolution |
|---|---|---|
| `Out of tree builds are impossible with config.h in source dir.` | The gcc baseline lives in-tree because FATE is driven from the source tree | Mirror the tree to a scratch source dir and build clang-asan there: `/opt/blitzy-tools/ffmpeg-asan-build.sh "$PWD" /tmp/ffmpeg-asan-src ffmpeg` |
| An ASAN build reproduces nothing, or `configure` rejects `--enable-fuzzer` | `--enable-fuzzer` is not a real option, and `--enable-ossfuzz` NULL-stubs `codec_list[]` at `libavcodec/allcodecs.c:945-952`, leaving **zero encoders** | Use `--toolchain=clang-asan` for reproduction, or `--toolchain=clang-asan-fuzz` for libFuzzer. Never add `--enable-ossfuzz` |
| FATE reports unrelated failures such as `filter-pixdesc-*` | A concurrent `make` relinked and stripped `ffmpeg` while the suite was running | Run `make fate` on a quiescent tree with no other `make` in flight, then re-run |
| `Error opening file [0:a]channelmap=...` | `-/filter_complex` and `-/stream_group` read from a **file**, not an inline string | Write the graph or stream-group spec to a file and pass its path |
| Only 1 `TEST` line and `EXIT=2` from the sample tests | Wrong target names — there is no `fate-iamf-5_1_4-demux` or `fate-iamf-9_1_6-demux` | Use exactly: `fate-iamf-stereo-demux`, `fate-iamf-5_1-demux`, `fate-iamf-5_1-copy`, `fate-iamf-ambisonic_1-projection-demux`, `fate-iamf-ambisonic_1-projection-copy` |
| `Filter 'channelmap:default' has output N (…) unconnected` | Every filtergraph output needs a matching `-map` and `-streamid` | Add one `-map '[LABEL]'` and one `-streamid n:n` per output |
| `Invalid channel count across substreams in layer N from stream group M` | Substream widths must sum to each layer's channel increment; the new F-5 check also rejects leftover substreams | Re-plan the substream decomposition, or reduce the declared layers |
| `Unable to parse "ch_layout" option value "7.1.6" as channel layout` | Only entries in `ff_iamf_scalable_ch_layouts[10]` and `ff_iamf_expanded_scalable_ch_layouts[13]` are valid | Choose a layout present in `libavformat/iamf.c` |
| `There must be at least two stream groups` on remux | Stream groups are not carried by `-map 0` alone | Use `-map 0 -stream_group 'map=0=0:st=0' -stream_group 'map=0=1:stg=0' -streamid 0:0` |
| `Duplicated stream id 0` on remux | Streams were mapped twice — once directly and once through a stream group | Map streams once; reference them from the stream groups |
| A new FATE test fails with a diff in whitespace only | The pre-commit config's `exclude: ^tests/ref/` means the hooks will not normalize reference files | Commit generated references **exactly as produced**; never reformat them |
| 5 IAMF tests are skipped | The external sample corpus is absent | `make fate-rsync SAMPLES=/path/to/fate-suite`, then pass `SAMPLES=/path/to/fate-suite` |
| MSE `appendBuffer` fails in a browser harness | Progressive MP4 has no `mvex`/`moof`; `default_base_is_moof` **does not exist** in FFmpeg and `frag_keyframe` cannot cut fragments for audio-only streams | Use `-movflags +frag_every_frame+empty_moov+default_base_moof` |

---

## 10. Appendices

### A. Command Reference

| Purpose | Command |
|---|---|
| Baseline configure (CI-identical) | `./configure --enable-gpl --enable-nonfree --enable-memory-poisoning --assert-level=2` |
| ASAN configure | `./configure --toolchain=clang-asan --assert-level=2 --enable-gpl --enable-nonfree --enable-memory-poisoning` |
| libFuzzer configure | `./configure --toolchain=clang-asan-fuzz --assert-level=2 --enable-gpl --enable-nonfree --enable-memory-poisoning` |
| Build everything | `make -j"$(nproc)"` |
| Build the muxer fuzz target | `make -j"$(nproc)" tools/target_mux_fuzzer` |
| IAMF acceptance gate | see §9.4(a) — 15 self-contained + mov-mp4 targets |
| Sample-dependent IAMF tests | see §9.4(b) with `SAMPLES=<corpus>` |
| Fetch the external corpus | `make fate-rsync SAMPLES=/path/to/fate-suite` |
| Full suite | `CI=true make -j"$(nproc)" fate-build && CI=true make -j"$(nproc)" fate SAMPLES=<corpus>` |
| Retain artifacts for md5 comparison | append `KEEP_FILES=1` to any FATE invocation |
| Generate a reference | `CI=true make fate-<name> GEN=1` |
| Lint gate | `pre-commit run -c .forgejo/pre-commit/config.yaml --show-diff-on-failure --all-files` |
| Change set summary | `git diff --stat 566ad7869ee3c8b6993e1f880e0a50eae18c66ac..HEAD` |
| Verify authorship | `git log --author="agent@blitzy.com" 566ad7869ee3c8b6993e1f880e0a50eae18c66ac..HEAD --oneline` |
| Produce a patch series | `git format-patch 566ad7869ee3c8b6993e1f880e0a50eae18c66ac..HEAD` |
| Confirm no reachable assertions | `grep -cP 'av_assert[012]\s*\(' libavformat/iamf_writer.c` → `0` |
| Run the fuzz campaign | `ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 ./tools/target_mux_fuzzer -max_total_time=400 -timeout=60 -rss_limit_mb=4096 <corpus>` |

### B. Port Reference

**No network ports are used.** This project is a C library and command-line muxer fix; `ffmpeg` and `ffprobe` are batch processes with no listening sockets, and no service, daemon or web server is part of the build, the tests or the runtime. The browser verification loaded artifacts from `file://` and from a short-lived local static server used only to serve the retained artifacts to headless Chrome; nothing in the delivered change opens a port.

### C. Key File Locations

| Path | Status | Role |
|---|---|---|
| `libavformat/iamf_writer.c` | **Modified** (+1,214 / −124; 1,304 → 2,394 lines) | All IAMF serialization and validation — contains every fix |
| `libavformat/iamf.h` | **Modified** (+13) | Private `MAX_IAMF_LAYERS 6` at line 36; `IAMFLayer.recon_gain_present` |
| `tools/target_mux_fuzzer.c` | **Added** (1,942 lines / 77,313 B) | FFmpeg's first muxer fuzz target |
| `tools/Makefile` | **Modified** (+3) | `tools/target_mux_fuzzer.o` compile rule |
| `Makefile` | **Modified** (+3) | `tools/target_mux_fuzzer$(EXESUF)` libFuzzer link rule |
| `tests/fate/iamf.mak` | **Modified** (+22) | `fate-iamf-lfe` and `fate-iamf-expanded-3_0` TRANSCODE targets |
| `tests/streamgroups/audio_element-lfe` | **Added** | Single-layer LFE element — positive regression fixture for F-2 |
| `tests/streamgroups/mix_presentation-lfe` | **Added** | Matching single-submix mix presentation |
| `tests/streamgroups/audio_element-3_0` | **Added** | Three-channel expanded layout across two substreams — F-4/F-5 lock-in |
| `tests/filtergraphs/iamf_lfe` | **Added** | `[0:a]channelmap=0:mono[MONO0];` |
| `tests/filtergraphs/iamf_3_0` | **Added** | Stereo + mono decomposition |
| `tests/ref/fate/iamf-lfe` | **Added** (131 lines) | Byte-exact reference, md5 `92df4d79f59b2dfbb52cd6f60a04045b` @ 14,353 B |
| `tests/ref/fate/iamf-expanded-3_0` | **Added** (193 lines) | Byte-exact reference, md5 `2fbd17fd4c9544e74be281b770f36cde` @ 28,601 B |
| `libavformat/iamf_parse.c` | *Reference only, unchanged* | The correct reference implementation whose guards were ported (`:394-395`, `:514-515`) |
| `libavformat/iamf_reader.c` | *Reference only, unchanged* | Hosts the write twin at `:255` |
| `libavformat/movenc.c` | *Reference only, unchanged* | Second consumer at `:7347`, `:7923`, `:7926` — protected transitively |
| `libavutil/iamf.h` | *Reference only, unchanged* | Public `recon_gain[6][12]` at line 166 — deliberately untouched to preserve ABI |
| `blitzy/screenshots/` | Evidence | 35 PNG artifacts from browser verification |
| `blitzy/screen_recordings/` | Evidence | 7 `.webm` recordings of the runtime flows |

### D. Technology Versions

| Component | Version | Notes |
|---|---|---|
| FFmpeg | `N-124480-gfcd9b7d31a` | Built from HEAD `fcd9b7d31a` |
| Baseline commit | `566ad7869ee3c8b6993e1f880e0a50eae18c66ac` | The AAP-declared parent of the upstream fix |
| GCC | 15.2.0 (Ubuntu 15.2.0-4ubuntu4) | Baseline build; AAP pinned 13.3.0 |
| Clang | 20.1.8 | ASAN / libFuzzer; AAP pinned 18.1.3 |
| NASM / Yasm | 2.16.03 / 1.3.0 | Assemblers |
| GNU Make | 4.4.1 | Repository minimum is 3.81 |
| Python 3 | 3.13.7 | Build tooling and the lint gate |
| zlib | 1.3.1 | Required by `libavformat` |
| C standard | C11 minimum, C17 default | Every added construct is valid C99 |
| IAMF specification | AOM IAMF v1.1.0 | Source of the six-channel-group limit |
| Dependency manifests | **none** | No registry, lockfile or version pin exists in the repository |

### E. Environment Variable Reference

| Variable | Example | Purpose |
|---|---|---|
| `CI` | `CI=true` | Non-interactive FATE execution |
| `SAMPLES` | `SAMPLES=/opt/blitzy-tools/fate-suite` | Path to the external FATE sample corpus (needed by 5 of the 20 IAMF tests) |
| `KEEP_FILES` | `KEEP_FILES=1` | Retain `tests/data/` artifacts so md5/size byte-identity can be checked |
| `GEN` | `GEN=1` | Generate a reference file instead of comparing against it |
| `ASAN_OPTIONS` | `abort_on_error=1:detect_leaks=1` | AddressSanitizer / LSan behaviour |
| `CLONE_INDEX` | `CLONE_INDEX=2` | Keeps parallel ASAN scratch clones from colliding |
| `LIBFUZZER_PATH` | auto `-fsanitize=fuzzer` | Set by `configure` when the `fuzz` sanitizer is requested |

**No application configuration file, `.env` file, secret, token, certificate or credential exists or is required anywhere in this project.**

### F. Developer Tools Guide

| Tool | Availability | Use in this project |
|---|---|---|
| GCC + `-Wall -Wextra` | ✅ | Baseline build and the zero-warning gate |
| Clang AddressSanitizer | ✅ | Reproduced the overflow on baseline, proved it eliminated on HEAD; 26-scenario sweep |
| Clang LeakSanitizer | ✅ | Run with `detect_leaks=1` throughout the sweep |
| libFuzzer | ✅ | 2,830,091-execution campaign against `tools/target_mux_fuzzer` |
| `pre-commit` (project config) | ✅ | 12-hook hygiene gate, EXIT=0 and idempotent |
| FATE | ✅ | 5,470-test regression suite plus the 20-test IAMF gate |
| `ffprobe -show_stream_groups` | ✅ | Structural read-back of muxed artifacts |
| Derived IAMF OBU parser | ✅ (authored here) | Audited 24 artifacts: every `num_layers` ∈ 1…6, every reserved field 0 |
| Headless Chrome | ✅ | WebAudio / native `<audio>` / MSE playback verification with screenshots and recordings |
| `valgrind`, `cppcheck`, `scan-build`, `clang-tidy` | ❌ Not installed | **Not used, and no claim is made otherwise.** Memory-safety verification was delivered through AddressSanitizer and libFuzzer |
| `npm audit`, `pip-audit`, SCA scanners | ❌ N/A | No dependency manifest exists to scan |
| aarch64 / 32-bit cross toolchains, Wine | ❌ Not present | Blocks the CI build-matrix verification tracked in §2.2 |

### G. Glossary

| Term | Definition |
|---|---|
| **IAMF** | Immersive Audio Model and Formats — an Alliance for Open Media specification (v1.1.0 here) for scene-based and channel-based immersive audio |
| **Audio Element** | An IAMF descriptor grouping substreams into layers with a declared loudspeaker layout |
| **Layer** | One channel group within a scalable channel layout configuration; the specification permits at most **six**, which is why `recon_gain` has six rows |
| **Substream** | One coded stream inside an audio element — either a single channel or a coupled stereo pair |
| **Recon gain** | Per-layer, per-channel reconstruction-gain values carried in a parameter block; the `uint8_t recon_gain[6][12]` matrix that was overflowed |
| **Mix presentation** | An IAMF descriptor describing how audio elements are mixed for a target playback layout |
| **OBU** | Open Bitstream Unit — the IAMF container's framing unit |
| **FATE** | FFmpeg Automated Testing Environment, the in-tree regression suite |
| **Byte-identity gate** | The acceptance criterion that existing test artifacts must be bit-for-bit unchanged, compared by md5, exact size, extradata CRC, per-frame CRCs and an `ffprobe` structural dump |
| **ASAN / LSan** | AddressSanitizer / LeakSanitizer — Clang runtime memory-error and leak detectors |
| **libFuzzer** | Clang's in-process, coverage-guided fuzzing engine |
| **`av_assert0/1/2`** | FFmpeg assertion macros. `av_assert0` aborts in every build; `av_assert1`/`av_assert2` compile to `((void)0)` below their assert level, which is why neither is valid input validation |
| **AAP** | Agent Action Plan — the binding specification this project was executed against |
| **F-1 … F-6, F-1b** | The AAP's identifiers for the seven designed code fixes |
| **V-1 … V-9** | The AAP's identifiers for the nine defects discovered in the component |
| **CWE-787 / 122 / 131 / 125 / 200 / 1284** | Out-of-bounds write · heap-based buffer overflow · incorrect buffer-size calculation · out-of-bounds read · information exposure · improper validation of a specified quantity in input |
| **CWE-617 / 193 / 665** | Reachable assertion · off-by-one · improper initialization — the secondary defect classes |
