# FolderPilot — Phase 6 Closeout

> **STATUS: Phase 6 (hardening, tests and documentation) — implementation,
> local runtime verification, and formal acceptance COMPLETE; the
> implementation and documentation changes are committed and pushed
> (`origin/main` = `4fd157d` "fix: ensure CI test logs are uploaded",
> following `7f4a9db` "windows cl test").** Hosted CI has run twice
> (2026-11-11): `43052752854` (commit `7f4a9db`) succeeded but uploaded no
> artifact (wrong CTest log path, since fixed), and `43068578754` (commit
> `4fd157d`) succeeded AND created the verified `folderpilot-test-logs`
> artifact; exact hosted test counts remain unverifiable (artifact contents
> require authentication) — see §3–§4. Smart App Control was temporarily
> Off during the authorized local runtime-verification window; its state
> outside that window is not claimed by this record. Record date:
> 2026-11-11.

---

## 1. Phase 6 scope and delivered functionality

Phase 6 scope was "hardening, tests and documentation" (`docs/ARCHITECTURE.md`
§7), with the Phase 5 deferred Unicode CLI item prioritized. Delivered, all
present in the current working tree:

| Area | Change | Evidence |
|---|---|---|
| Unicode CLI (the known Phase 5 deferred-work item) | UTF-16 entry point: `wmain` plus `-municode` (GNU/Win32 only, compile and link); every command-line argument converted with the library's strict UTF-16→UTF-8 converter, failing closed as a usage error on non-representable input; console output code page set to `CP_UTF8` (display-only — pipes, redirection, and test-harness capture are unaffected) | `app/main.cpp:58-77`, `CMakeLists.txt:89-96` |
| Packaging | Install rule copying only the application binary (`install(TARGETS folderpilot RUNTIME DESTINATION bin)`); README documents the static runtime linking and the install command; the static-linking claim was verified against the binary import table (KERNEL32 + api-ms-win-crt-* only) | `CMakeLists.txt:104-109`, `README.md:75-83` |
| CI workflow | `.github/workflows/ci.yml`: Windows runner, MSYS2 UCRT64 (gcc/cmake/ninja), committed presets configure→build→test in Debug and Release; least-privilege `permissions: contents: read`; push/PR triggers scoped to `main`; test-log artifacts upload `if: always()`. It runs only when pushed/triggered to a remote — creating the file triggers nothing. Hosted runs (2026-11-11): `43052752854` — success, no artifact (upload path lacked `Temporary/`, fixed in `4fd157d`); `43068578754` — success, artifact created and verified; see §3 and §4 | `.github/workflows/ci.yml` |
| Tests | New CLI regression cases: Unicode target round-trip and trailing-backslash argument; the controlled A/B rename test reworked to observe the build's actual class-3 behavior and assert report-vs-reality consistency in both documented outcomes instead of assuming error 87; CLI harness argv quoting hardened | `tests/unit/test_cli5.cpp:109-151`, `tests/unit/test_executor.cpp:400-511`, `tests/phase3_fixture.cpp:449-467` |
| Documentation | README: Phase 6 status, exit codes per `app/main.cpp`, Unicode bullet, packaging section, Smart App Control regression-testing guidance, direct-CMake fallback forms with explicit build types, neutral exit-127 note. ARCHITECTURE.md: §1/§4/§6/§7 status refresh, §8 exit codes and Unicode CLI bullet, §11 OS-finding wording ("build-specific observation, not a universal Windows contract"). SAFETY.md: §6/§7 invariant status markers updated | `README.md`, `docs/ARCHITECTURE.md`, `docs/SAFETY.md:57,66` |

No new product features beyond this scope were added; dry-run default,
`--apply`-only movement, no-overwrite, fail-closed behavior, and PathGuard are
unchanged.

## 2. Phase 6.5 audit findings and resolutions

A read-only audit of Phase 6 steps 6.1–6.4 (finding identifiers D1–D4,
recorded in the session log) reviewed the CI workflow, the `wmain`/Unicode
path, the CLI test harness, packaging claims, and documentation consistency.
The resolutions implemented in the working tree:

| Finding area | Resolution | Evidence |
|---|---|---|
| CI workflow hardening | Least-privilege workflow token (`permissions: contents: read`), branch-scoped triggers (`main` only), and test-log artifact upload under `if: always()` so that even failed runs produce evidence | `.github/workflows/ci.yml:19-25,66-74` |
| CLI test-harness argv quoting | Windows argv parsing ends an argument containing n trailing backslashes with only n literal backslashes when the closing quote follows them, so a quoted path ending in `\` lost its closing quote. `run_folderpilot_cli` now doubles trailing backslashes before quoting, with a dedicated CLI regression case asserting the path round-trips | `tests/phase3_fixture.cpp:449-467`, `tests/unit/test_cli5.cpp:109-128` |
| Rename A/B test portability | The class-3 branch no longer hard-codes the build-26300 outcome: it observes the actual behavior and asserts consistency in both documented outcomes (a real anchored move on builds without the regression; fail-closed both-sides-preserved on ERROR_INVALID_PARAMETER 87); any other failure fails the test with its specifics | `tests/unit/test_executor.cpp:417-452` |
| Documentation and claims consistency | Exit codes aligned verbatim to `app/main.cpp`; direct-CMake fallback forms documented with explicit build types; the exit-127 quirk documented neutrally (cause unidentified, not attributed to any security policy without evidence); stale contradictory PLANNED rows removed from ARCHITECTURE.md §1; static-linking and install claims verified against the binary import table and the committed install rule | `README.md:65-83`, `docs/ARCHITECTURE.md:184-193` |

Post-audit runtime discovery (test-side, not an audit finding): the first
Phase 6 runtime-verification window exposed that the two new CLI tests passed
raw forward-slash paths (from the CMake sandbox macro) which
`classify_dos_path` correctly rejects. The authorized narrow correction routes
those arguments through the existing fixture `wide_path()` helper — no
production change, no assertion weakening (`tests/unit/test_cli5.cpp:14-18`).

## 3. Runtime verification — LOCAL results (verified)

Executed 2026-11-10 → 2026-11-11 (local) inside the user-authorized Smart
App Control–off window, using the direct CMake/Ninja/CTest forms (the
documented workaround for the preset-127 quirk, §4). Fixtures are synthetic
and live exclusively under `build/phase3-test-sandbox/` (read-only fixtures)
and `build/phase5-test-sandbox/` (movement fixtures); both were exercised by
the full unfiltered suite.

| Step | Command | Result |
|---|---|---|
| Debug build currency | `ninja -C build/debug` | exit 0 — `no work to do.` |
| Debug suite | `ctest --test-dir build/debug --output-on-failure -V` | **exit 0** — 2.71 s |
| Release build currency | `ninja -C build/release` | exit 0 — `no work to do.` |
| Release suite | `ctest --test-dir build/release --output-on-failure -V` | **exit 0** — 1.69 s |

Both configurations produced the identical doctest summary (doctest prints
only messages and the summary by default; the per-case inventory was confirmed
separately via `--list-test-cases` and is byte-identical between the two
binaries — 144 cases):

```
[doctest] test cases:  144 |  144 passed | 0 failed | 0 skipped
[doctest] assertions: 1762 | 1762 passed | 0 failed |
[doctest] Status: SUCCESS!
```

- **144/144 test cases and 1,762/1,762 assertions passed in BOTH Debug and
  Release; zero failed, zero skipped.**
- Zero compiler diagnostics in the window (no recompilation occurred — both
  `ninja` runs reported `no work to do.`, so the executed binaries are current
  with the working tree; they were built 2026-11-10 21:54 inside the same
  authorized window, warning-clean). Zero warning/error strings in both logs;
  zero fixture-cleanup failures (`FIXTURE CLEANUP FAILURE` count: 0).
- Complete logs retained locally: `build/final-verify-debug.log`,
  `build/final-verify-release.log`. These live under the gitignored `build/`
  tree — they are local evidence, not repository content.

### Coverage (evidence from the actual runs)

| Required area | Test case / evidence |
|---|---|
| Unicode CLI argument handling | `CLI accepts a Unicode target directory argument` (`tests/unit/test_cli5.cpp:130`) — target `ünïcode 文件夹` (space + non-ASCII) round-trips into the dry-run report; passed in both configurations |
| Paths with spaces and non-ASCII | Every CLI case exercises space-containing sandbox paths (the workspace path itself contains spaces); non-ASCII additionally covered at the executor level by the Unicode destination subcase (`文件.txt`, `tests/unit/test_executor.cpp:462-472`) |
| Trailing-backslash CLI regression | `CLI accepts a target argument with a trailing backslash` (`tests/unit/test_cli5.cpp:109`) |
| Portable class-3/class-10 rename A/B | `controlled A/B: Win32 class-3 vs native class-10 anchored rename` (`tests/unit/test_executor.cpp:400`) — ran in both configurations with runtime observation `A win32 class-3 -> 1 error 87` (`test_executor.cpp:431`) exercising the documented fail-closed branch; all B subcases passed: native class-10 success, Unicode destination, existing-destination conflict, 255-unit name, missing-RootDirectory fail-closed (`test_executor.cpp:502-510`) |
| Executor movement, no-overwrite, fail-closed | `CLI --apply moves files and leaves the root clean of sources`; `CLI --apply replans on every invocation: an injected conflict is re-reserved, never overwritten`; `executor reports a conflict when the destination exists and preserves both sides`; `executor rejects a source replaced after planning`; `executor reports a source that vanished after planning`; `executor reports a locked source and continues independent moves`; `executor rejects a category directory replaced after planning`; `batch error isolation: one locked file does not affect later batches`; `root handle moves correctly and the moved-from value is inert`; `CLI dry-run still changes nothing after Phase 5` |
| Phase 1–5 regression | The run was complete and unfiltered (no `-tc` filter, zero skips): scanner, planner, classifier, PathGuard, text, handle, reporter, collision, and native suites are all inside the 144 cases |

### Hosted results — layered evidence (two runs, 2026-11-11)

Retrieved 2026-11-11 via the GitHub Actions API (anonymous, read-only; `gh`
is not installed and nothing was installed). **A workflow/job success alone
does not establish test-case counts** — the layers are stated separately,
per run:

**Run 1 — `43052752854` (commit `7f4a9db`, push): success, NO artifact.**

| Layer | Evidence | Source |
|---|---|---|
| Workflow | Run `43052752854` "FolderPilot CI" — event `push` to `main`, head SHA `7f4a9dbbc1b1c5903bd4a15913bc780462eed899`, attempt 1, created 2026-11-11T15:52:59Z, **conclusion `success`** at 15:54:48Z | Actions API |
| Job | `130634019902` "Windows (UCRT64) - Debug and Release" (`windows-latest` runner, MSYS2 UCRT64) — **success** (15:53:02Z–15:54:47Z) | Actions API |
| Steps | All green: checkout, MSYS2 setup, Configure/Build/**Test** (Debug), Configure/Build/**Test** (Release), Upload test logs, post steps | Actions API |
| CTest | The Test steps run `ctest --preset <debug/release> --output-on-failure`; `ctest` exits non-zero on any failing test, so the succeeding Test steps establish hosted CTest completed **without failing tests** in both configurations — inferred from step outcomes; the ctest output itself was not retrievable | Inference from step results |
| doctest case/assertion counts | **NOT VERIFIED — no claim.** The run's artifact list is empty (HTTP 200, `total_count: 0`): the `folderpilot-test-logs` artifact does not exist, so there is nothing to download and no hosted case/assertion counts can be quoted. Job logs require repository-admin authentication (anonymous attempt: HTTP 403). The "Upload test logs" step's success is not evidence of an upload — `if-no-files-found: warn` permits an empty search to succeed | API + failed retrieval |

**Root cause (established locally after run 1):** CTest writes its log to
`<binaryDir>/Testing/Temporary/LastTest.log` — verified in both local build
trees (1,536 / 1,540 bytes; no `Testing/LastTest.log` exists anywhere) —
while the workflow uploaded `<binaryDir>/Testing/LastTest.log`, a path that
never exists. With `if-no-files-found: warn` the upload step succeeded
while uploading nothing. (The runner's own warning text remained
inaccessible — job logs require repository-admin authentication, HTTP 403 —
so the diagnosis rests on the local path evidence plus the empty artifact
listing.) Commit `4fd157d` ("fix: ensure CI test logs are uploaded")
corrected the workflow: upload paths fixed to `Testing/Temporary/`, a
pre-upload existence-verification step added (fails loudly with per-file
diagnostics), `if-no-files-found: error`, `retention-days: 14` — no
application or test code changed.

**Run 2 (current) — `43068578754` (commit `4fd157d`, push): success, artifact created.**

| Layer | Evidence | Source |
|---|---|---|
| Workflow | Run `43068578754` "FolderPilot CI" — event `push` to `main`, head SHA `4fd157d864b936b4b0e6f2c37716162c9d2a4b5e`, attempt 1, created 2026-11-11T17:28:56Z, **conclusion `success`** at 17:31:07Z | Actions API |
| Job | "Windows (UCRT64) - Debug and Release" (`windows-latest` runner, MSYS2 UCRT64) — **success** (17:29:00Z–17:31:06Z) | Actions API |
| Steps | All green: checkout, MSYS2 setup, Configure/Build/**Test** (Debug), Configure/Build/**Test** (Release), **Verify test logs exist → success**, **Upload test logs → success**, post steps | Actions API |
| CTest | The Test steps succeeded ⇒ `ctest` exited 0 in both configurations (inferred from step outcomes; the ctest output itself was not retrievable) | Inference from step results |
| Artifact | **`folderpilot-test-logs` created** — ID `13876723467`, 1,272 bytes, `expired: false`; artifact metadata `head_sha` matches commit `4fd157d864b936b4b0e6f2c37716162c9d2a4b5e` exactly | Actions API |
| doctest case/assertion counts | **NOT VERIFIED — no claim.** The artifact's *contents* are not anonymously readable (ZIP download: HTTP 401 — authentication required), so the hosted `LastTest.log` files and their doctest summaries could not be inspected in this session | Failed retrieval |

Run 2 changed only the artifact-collection mechanics (commit `4fd157d`
touches `ci.yml`, `README.md`, and two docs files — no application or test
code), so it additionally evidences that the same Debug/Release
configure→build→test pipeline passes with the corrected upload and that the
log-verification guard works on a real runner.

- **Hosted ≠ this machine's OS build (both runs).** The runner is
  `windows-latest`; its exact Windows build is not verified here, and
  nothing in this record claims hosted CI verifies behavior on this
  machine's build 26300. The hosted A/B test's class-3 branch outcome
  (success vs the documented error-87 fail-closed branch) is not observable
  from outside — the test asserts consistency with whichever outcome
  occurred.

## 4. Explicitly not claimed / open items

- **Hosted CI — verified at the workflow/job/CTest-step level plus artifact
  EXISTENCE and metadata; exact hosted test counts are still NOT verifiable.**
  Run `43052752854` (commit `7f4a9db`) succeeded but uploaded no artifact
  (wrong CTest log path — see §3); the corrected workflow in `4fd157d`
  produced run `43068578754`, which succeeded AND created the
  `folderpilot-test-logs` artifact (ID `13876723467`, 1,272 bytes, not
  expired, metadata SHA == commit `4fd157d`). The artifact's *contents*
  require authentication (HTTP 401 anonymously), so no hosted doctest
  case/assertion counts can be quoted — layered evidence in §3. Hosted CI
  does not verify this machine's build-26300 behavior (§3).
- **Smart App Control state is not claimed.** SAC was temporarily Off during
  the authorized verification window (a user action outside this repository).
  Its state after the window is not verified by this record; re-enabling it
  was recommended immediately after testing.
- **Exit code 127 from CMake preset action invocations remains unexplained.**
  Documented neutrally (README "Building"): the cause is unidentified and is
  not attributed to any security policy without evidence; preset usage errors
  and `ctest --preset` listing work on the affected machine; the direct
  `cmake -S . -B … -DCMAKE_BUILD_TYPE=…` / `ninja` / `ctest --test-dir` forms
  are the documented equivalent workaround. The hosted runner's success only
  establishes that the preset commands worked in that environment — the
  local exit-127 issue is not resolved by it.
- **Commit state.** The Phase 6 implementation and documentation changes are
  committed and pushed through `4fd157d` ("fix: ensure CI test logs are
  uploaded" on `origin/main`, live-verified 2026-11-11 via `git ls-remote`;
  parent `7f4a9db`). The hosted-CI artifact-evidence amendment (this update,
  2026-11-11) is uncommitted. Both hosted runs were triggered by authorized
  pushes to `main`; no unauthorized pushes or manual workflow dispatches
  occurred.

## 5. Final Git state (amended 2026-11-11 — hosted-CI artifact evidence)

- Branch `main`, HEAD `4fd157d864b936b4b0e6f2c37716162c9d2a4b5e` ("fix:
  ensure CI test logs are uploaded", parent `7f4a9db` "windows cl test"),
  **pushed**: a live `git ls-remote` on 2026-11-11 confirmed `origin/main`
  == HEAD; the remote default branch is `main`.
- Documentation-trail commit history: `7f4a9db` carried the entire Phase 6 +
  6.5 working set (11 files, +578/−78 versus `d024112`, including the CI
  workflow, the `.gitignore` `.zcode/` entry at line 6, and this closeout
  record); `4fd157d` added the CI artifact fix and README status correction
  (4 files, +136/−62). Both are on `origin/main`.
- **This hosted-CI artifact evidence amendment (2026-11-11) re-opens a small
  uncommitted documentation diff — exactly two tracked files:** `README.md`
  and `docs/PHASE6-CLOSEOUT.md` (+120/−74). The tree was clean beforehand and
  no other tracked file was touched.
- Tracked-diffstat provenance: +253/−52 at closeout → +289/−71 after the
  documentation cleanup → +301/−78 after the ARCHITECTURE.md closure update
  → +104/−57 after the hosted-evidence amendment — **all committed** (the
  first three inside `7f4a9db`, the last inside `4fd157d`); the +120/−74
  above is the only uncommitted remainder.
- Hosted-CI inspection evidence (run/job/artifact metadata, the 403 job-log
  and 401 artifact-zip responses, the new run's step results) was saved
  under the gitignored `build/` tree and is not part of the repository.

## 6. Inconsistencies noted

Status as of the 2026-11-11 documentation follow-ups — the cleanup (editing
`docs/SAFETY.md`, `README.md`, and this record) followed by the ARCHITECTURE.md
closure update (item 3; that update touched only `docs/ARCHITECTURE.md`):

1. **RESOLVED — `docs/SAFETY.md` predated the Phase 5.1 rename resolution.**
   The banner's "KNOWN BLOCKER" paragraph, §4's "blocked on OS build 26300"
   marker, and §9's class-3-only note described movement as failing closed
   on build 26300, and §12 restated the exit codes twice, once with the
   stale "1 usage/unsupported" phrasing. The cleanup rewrites these around
   the implemented native class-10 anchored rename, states that the class-3
   error-87 result is a build-specific observation (not a universal Windows
   guarantee), records that the native path passed the complete Debug and
   Release suites (local runtime verification; hosted CI has not run), and
   leaves a single exit-code list per `app/main.cpp`. Two smaller stale
   statements were reconciled in the same pass: §1's "There is currently no
   code path that mutates user files at all" (contradicted the implemented
   `--apply`) and §11's sandbox list (movement fixtures live under
   `build/phase5-test-sandbox/` alongside the read-only
   `build/phase3-test-sandbox/`).
2. **RESOLVED — `README.md` status figures were stale.** The status block
   now leads with the Phase 6 local verification — Debug and Release each
   144/144 cases, 1,762/1,762 assertions, 0 failed, 0 skipped (hosted CI has
   not run) — and labels the Phase 5 figures (142/142 cases, 1,748/1,748
   assertions) as explicitly historical and superseded.
3. **RESOLVED — `docs/ARCHITECTURE.md` closure update (2026-11-11, formal
   Phase 6 acceptance; documentation-only).** The banner now identifies
   Phase 6 (hardening, tests and documentation) as complete, stating the
   local per-configuration verification figures (144/144 cases,
   1,762/1,762 assertions each, zero failed, zero skipped — Debug and
   Release) and that hosted CI has not run. The §7 phase table now marks
   Phase 6 **COMPLETE** (the §1 planned-work row moved from "Phase 6+" to
   "Phase 7+" for coherence). §6 now lists both verified synthetic sandbox
   directories — `build/phase3-test-sandbox/` (read-only fixtures) and
   `build/phase5-test-sandbox/` (movement fixtures) — matching
   `tests/CMakeLists.txt:40-41` and the fixture implementation
   (`phase3_fixture.hpp:22-23,29`, `phase3_fixture.cpp:158-159`). The
   update touched no other file.
4. **UNRESOLVED — intentionally untouched: ARCHITECTURE.md section numbers
   are not in document order** (§10 precedes §9 precedes §8; §11 closes the
   file). Pre-existing, cosmetic, no functional impact; no cosmetic
   restructuring has been authorized, so it remains as is.

---

**Claim discipline:** this record states only what the cited evidence shows.
The §3 local results are real executions performed in the authorized local
window; the hosted evidence is layered — workflow/job/step success and the
artifact's existence, metadata, and commit binding were verified via the
GitHub API, and no hosted doctest case/assertion counts are claimed anywhere
because the artifact's contents require authentication and were not
inspectable in this session; hosted CI is not treated as verifying this
machine's OS build; security-control state is not inferred. The verification
logs and CI inspection files live under the gitignored `build/` tree and are
not part of the repository.
