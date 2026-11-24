# FolderPilot — Phase 8 completion and evidence record

## 1. Commit and local verification

> The figures below are the original Phase 8 acceptance evidence. The current
> release commit and verification figures are recorded in §5.

- Feature commit: `b7f4b8a25a524932d2d4081d2eb5107f415812cc`
  (`feat: add recursive organization and retry (Phase 8)`), pushed to
  `origin/main` (`3db3398..b7f4b8a`).
- Working tree at commit time: clean; `git diff --check` reported no
  whitespace errors.
- Clean Debug build (Ninja, MSYS2 UCRT64 GCC 16.1): 0 compiler warnings;
  CTest 1/1 passed; doctest **279/279 test cases, 14,893/14,893 assertions,
  0 failed, 0 skipped**.
- Clean Release build: 0 compiler warnings; CTest 1/1 passed; doctest
  **279/279 test cases, 14,893/14,893 assertions, 0 failed, 0 skipped**.
- Both builds were fresh configure + build in short-path directories
  (`build/fd`, `build/fr`). Two pre-existing Phase 7 tests are sensitive to
  Win32 legacy path length: in a 10-character-longer build directory they
  fail on `CreateDirectoryW`/`fstream` because the sandbox path crosses
  MAX_PATH. This is an environmental limitation of those fixtures, not a
  Phase 8 regression; the committed presets (`build/debug`, `build/release`)
  and the hosted runner use short paths and pass.

## 2. Hosted CI

- Workflow run `45238213410` ("FolderPilot CI", CI #25), head SHA
  `b7f4b8a25a524932d2d4081d2eb5107f415812cc` (exact 40-character match with
  the feature commit).
- Final status: **success**; all Configure/Build/Test steps for Debug and
  Release, the test-log verification and the artifact upload succeeded.
  `head_sha` was verified as the exact 40-character feature commit.
- Artifact: `folderpilot-test-logs`, ID `14851240321`, 2,174 bytes,
  not expired (expires 2026-12-07), API metadata digest
  `sha256:1c1b8a02aaf0a28e514170bf76fe3602106d5af4d7bf84793766659b07fb69ec`.
  The artifact exists and its metadata digest was verified through the
  GitHub API; its bytes could not be downloaded anonymously (HTTP 401), so
  exact hosted doctest counts are not claimed here.

## 3. Reviews and amendments

- Independent code review of the Phase 8 diff: no critical/high findings.
  Medium findings fixed: terminal-entry budget counted traversed directories;
  `StorageRead` mis-mapped to exit 2 on forward apply; over-length/invalid
  relative paths produced a partial plan instead of a whole-plan refusal;
  ack flag accepted on a retry preview; retry full-scan fail-closed behavior
  undocumented.
- Independent security audit: no high/medium findings. Low findings fixed:
  O(N^2) visited set replaced with a hash set and a separate 50,000-directory
  fail-closed cap added; retry-preview lock acquisition documented.
- Root-cause fixes before the commit: a stray `WALKPROBE` debug print that
  broke the build; a dangling `RawDirEntry::name` view used after enumeration
  (category exclusion read freed memory); retry tests holding the journal
  session's mutation lock while reading a retry plan.

## 4. Portable package

> Superseded by §5: the package has been rebuilt from the release-readiness
> commit with new hashes and test figures.

- Package directory: `build/release-package/`
  (`folderpilot.exe`, `README.md`, `LICENSE-NOTICES.txt`, `BUILD-INFO.txt`,
  `SHA256SUMS`, plus the reproducible `prepare-package.ps1` and templates).
- Packaged executable SHA-256:
  `794ab876ef853b14e240c96ebd6c238e151fc2e6c23f3f4c1da63a751bba1b55`.
- `BUILD-INFO.txt` records product 0.1.0, x86_64, Release, MSYS2 UCRT64 GCC,
  C++20, the exact 40-character commit SHA, commit date, clean tree state,
  build UTC and the 279/14,893 test figures.
- Runtime imports: only Windows system DLLs and UCRT API sets; no
  `libstdc++`, `libgcc`, `libwinpthread` imports (statically linked GCC
  runtime).
- Clean-environment smoke on a synthetic fixture with the packaged binary:
  `--version` (0), recursive dry run (no changes), recursive apply (2 moved),
  undo preview (2 candidates), undo apply (2 restored), retry preview
  (`nothing to retry`) — all as expected.

## 5. Release-readiness correction (post-Phase-8; supersedes §§1–4 figures)

- Release commit: `123fdbbcd050b5ef8fbc1d8e166f11917e3920fc`
  (`fix(reporter): correct undo availability and durability disclosures`),
  pushed `296ce21..123fdbb`; worktree clean and `git diff --check` clean before
  the commit.
- Root cause: `lib/folderpilot/reporter.cpp` carried the unconditional
  Phase-5 string "…undo is not available" from commit `113a169`
  (2026-11-07), never updated when Phase 7 added undo. It was stale generic
  text, not a runtime capability limitation: the same executable produced a
  working undo preview.
- Fix: journal-state-conditional undo guidance, plus a `DurabilityCaveat`
  bitmask that distinguishes a structurally unanchored terminal RUN-END (all
  INTENT/RESULT records anchored) from a genuinely unanchored RESULT, a torn
  terminal, and an unterminated coverage-bearing anchor. No ratified
  classification state, parser rule, or disclosure was weakened.
- Test isolation: `FOLDERPILOT_STORAGE_BASE` overrides the storage base for
  tests and smoke checks (production still uses `%LOCALAPPDATA%` when unset);
  the test runner injects a PID-suffixed synthetic base under
  `<build>/storage-test-sandbox`. Full Debug/Release runs created **zero**
  new directories in the user's real `%LOCALAPPDATA%\FolderPilot\journals`.
- Local verification: fresh Debug and Release builds in short-path
  directories; CTest 1/1 each; doctest **285/285 test cases,
  14,918/14,918 assertions, 0 failed, 0 skipped** in both configurations,
  zero compiler warnings.
- Hosted CI: run `45429799769` (CI #27), job "Windows (UCRT64) - Debug and
  Release", `head_sha` exactly `123fdbbcd050b5ef8fbc1d8e166f11917e3920fc`,
  conclusion **success**. Artifact `folderpilot-test-logs`, ID
  `14938707874`, 2,173 bytes, metadata digest
  `sha256:d499e9da40e39b42283500fa33e4a4528040fe4ec26333545b70940bf1cc52f9`;
  downloaded bytes verified to the same hex digest. Both `LastTest.log`
  files show 285/285 cases and 14,918/14,918 assertions.
- Package: rebuilt from that commit. `folderpilot.exe` SHA-256
  `3ac0dccf0950deff0159478fe00c952879e0df4a7ad9e2bc2506d880da4243b5`;
  `BUILD-INFO.txt` records the exact commit, clean tree, the 285/14,918
  figures and CI run `45429799769`; `SHA256SUMS` regenerated.
- Packaged smoke test on a synthetic fixture with injected storage: `--apply`
  exit 0 with no stale undo claim and accurate `--undo` guidance; `--undo`
  preview exit 0 with `newest run: complete`, the terminal-only caveat,
  "all move records are anchored" and one undoable candidate. Real storage
  directory count was 475 before and after.
- Preservation: the 56 synthetic root-key directories created by the earlier
  pre-isolation test runs were deliberately preserved (not deleted or
  modified). An inventory of all 475 directories (names, sizes, mtimes and
  per-file SHA-256) is stored outside the journal tree with aggregate SHA-256
  `3767EEB4D4A6D50596DB511E35F5640D93EBCF0E6700DA91847413CCD4512780`. The
  user's actual test-folder journal/anchor remain byte-identical (journal
  `4BD73601EE1FF17B57B1C202FCFE5E8CF76D8E91944C5EE27F92F51F4E82EF24`,
  anchor `9BFAF66BE78EFD55E1911F40DCC3CE4CD562B368E265E72B1C3F8F9F2061AC2E`).

## 6. Limitations

- The executable is unsigned; Smart App Control may block it where enabled
  (see README).
- There is no project licence file in-tree; `LICENSE-NOTICES.txt` records the
  vendored doctest v2.5.3 MIT licence (test-only, not shipped in the exe)
  and this gap.
- `--retry --apply` re-scans the whole tree recursively; an inaccessible
  unrelated subdirectory aborts the retry (exit 3) by design.
- Undo restores locations only, never file contents.
