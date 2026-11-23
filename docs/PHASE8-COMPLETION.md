# FolderPilot — Phase 8 completion and evidence record

## 1. Commit and local verification

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

## 5. Limitations

- The executable is unsigned; Smart App Control may block it where enabled
  (see README).
- There is no project licence file in-tree; `LICENSE-NOTICES.txt` records the
  vendored doctest v2.5.3 MIT licence (test-only, not shipped in the exe)
  and this gap.
- `--retry --apply` re-scans the whole tree recursively; an inaccessible
  unrelated subdirectory aborts the retry (exit 3) by design.
- Undo restores locations only, never file contents.
