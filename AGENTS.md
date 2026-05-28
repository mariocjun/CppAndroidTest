# AGENTS.md

Entry point for AI coding agents (Cursor, Copilot, Codex, Claude Code, etc.).
This is a **template**: a forkable C++/NDK Android base + on-device profiler.

**Read [`CLAUDE.md`](CLAUDE.md) first — it is the full project map.** This file
is the 60-second orientation; CLAUDE.md is the detail.

## The 7 things to know before you touch anything

1. **App shape:** Kotlin `MainActivity` → JNI bridge (`app/src/main/cpp/jni.cpp`)
   → C++ profiler in `libcppandroidtest.so`. No NativeActivity.
2. **Never rename the package by hand.** JNI symbols
   (`Java_<pkg>_MainActivity_*`), the Kotlin dir path, and `applicationId` are
   coupled. Use `scripts/init-template.sh`.
3. **New SIMD must be HWCAP-gated** (`bench::has_*()` from `getauxval`), or it
   SIGILLs on kernels that disable the feature for userspace. See `bench/hwcaps`.
4. **Per-file `-march`** for ISA-extension TUs (dot_int8/i8mm/sve2) — keep it
   off the project-wide flags so the autovectorizer can't leak those
   instructions into other code.
5. **Add a benchmark** via the registry pattern only — see
   `.claude/skills/add-benchmark/SKILL.md`. Dispatch is automatic; don't edit
   `bench_main.cpp`/`jni.cpp`.
6. **Perf claims need ≥5 runs** (median + CV); single runs are DVFS/scheduler
   noise. `scripts/device-harness.sh full-stats`.
7. **Windows/MSYS:** `MSYS_NO_PATHCONV=1` for adb paths, `=0` for `python.exe`.

## Commands

```bash
./gradlew assembleDebug                                   # build APK
bash scripts/build-bench.sh                               # build profiler ELF
cmake -S tests -B tests/build -G Ninja && ctest --test-dir tests/build   # unit tests
git tag vX.Y.Z && git push origin vX.Y.Z                  # release (CI builds + publishes)
```

## Invokable skills (`.claude/skills/`)

- `add-benchmark` — scaffold + wire a new benchmark following the registry/
  HWCAP/`-march` conventions.
- `device-bench` — run the profiler on a connected (rooted) device via the
  harness, with median/CV aggregation.

## Don't

- Don't reintroduce `-u ANativeActivity_onCreate` (removed with NativeActivity).
- Don't hard-code `-std=c++23` (Clang 17 in the NDK wants `c++2b`; let
  `CMAKE_CXX_STANDARD` decide).
- Don't enable a self-hosted runner trigger on `pull_request` (public repo).
