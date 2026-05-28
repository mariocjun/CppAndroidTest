#!/usr/bin/env bash
# init-template.sh — turn this template into YOUR project: rename the package,
# the app display name, and (optionally) the native library, then validate.
#
# Run ONCE right after "Use this template" / fork, from the repo root:
#
#   bash scripts/init-template.sh com.acme.myapp "My App" [mylib] [--minimal]
#         |                        |              |        |        |
#         |                        |              |        |        +- strip the rooted-device
#         |                        |              |        |           test rig (most forks won't
#         |                        |              |        |           have the author's N975F)
#         |                        |              |        +- new native lib name (default: keep
#         |                        |              |           derived from app name)
#         |                        |              +- app display name (strings.xml app_name + Gradle project)
#         |                        +- new applicationId / namespace / Kotlin package
#         +- (this script)
#
# What it rewrites (the tricky bits the README warns about):
#   - the Kotlin source DIRECTORY is MOVED to match the new package
#   - JNI symbol names in jni.cpp (Java_<pkg>_MainActivity_*) — these MUST stay
#     in lockstep with the Kotlin package or the app crashes at first JNI call
#   - applicationId / namespace (build.gradle.kts), package decl (MainActivity.kt),
#     System.loadLibrary + CMake target + smoke/harness PACKAGE refs
#
# Replacement order matters: the dotted package contains the lib name as a
# substring (com.example.cppandroidtest ⊃ cppandroidtest), so we substitute
# most-specific → least-specific.
#
# After running: commit, push, let CI build (we can't build the APK here to
# verify; the static checks below catch identifier drift, CI catches the rest).
set -euo pipefail

OLD_PKG="com.example.cppandroidtest"
OLD_PKG_US="com_example_cppandroidtest"      # JNI symbol form
OLD_NAME="CppAndroidTest"                     # display name + Gradle project + (some) repo refs
OLD_LIB="cppandroidtest"                      # native lib basename

NEW_PKG="${1:-}"
NEW_NAME="${2:-}"
NEW_LIB="${3:-}"
MINIMAL=0
for a in "$@"; do [ "$a" = "--minimal" ] && MINIMAL=1; done
# If lib not given (or it's the --minimal flag), derive from app name: lowercase, alnum only.
case "${NEW_LIB:-}" in ""|--minimal) NEW_LIB=$(echo "$NEW_NAME" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9'); ;; esac

if [ -z "$NEW_PKG" ] || [ -z "$NEW_NAME" ]; then
    echo "usage: bash scripts/init-template.sh <new.package.id> \"<App Name>\" [libname] [--minimal]"
    exit 2
fi
case "$NEW_PKG" in
    *.*) : ;;  # must be dotted
    *) echo "FAIL: package '$NEW_PKG' must be dotted (e.g. com.acme.myapp)"; exit 1 ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
cd "$REPO"

NEW_PKG_US=$(echo "$NEW_PKG" | tr '.' '_')
OLD_DIR="app/src/main/kotlin/$(echo "$OLD_PKG" | tr '.' '/')"
NEW_DIR="app/src/main/kotlin/$(echo "$NEW_PKG" | tr '.' '/')"

echo "== init-template =="
echo "  package : $OLD_PKG  ->  $NEW_PKG"
echo "  name    : $OLD_NAME  ->  $NEW_NAME"
echo "  lib     : $OLD_LIB  ->  $NEW_LIB"
echo "  minimal : $MINIMAL (strip device-test rig: $([ $MINIMAL = 1 ] && echo yes || echo no))"
echo

# 1. Move the Kotlin source directory to the new package path.
if [ -d "$OLD_DIR" ]; then
    mkdir -p "$(dirname "$NEW_DIR")"
    git mv "$OLD_DIR" "$NEW_DIR" 2>/dev/null || { mkdir -p "$NEW_DIR"; mv "$OLD_DIR"/* "$NEW_DIR"/; rmdir "$OLD_DIR" 2>/dev/null || true; }
    echo "moved Kotlin sources: $OLD_DIR -> $NEW_DIR"
fi

# 2. Text substitution across all source/config (skip .git, build, this script,
#    and the changelog/commit-archive which are historical record).
#    Order: dotted pkg, underscore pkg, display name, then lib (substring-safe).
FILES=$(git ls-files | grep -vE '^(scripts/init-template\.sh|commit-archive\.md|.*\.original\.md)$' || true)
subst() { # old new
    local old=$1 new=$2 f
    for f in $FILES; do
        [ -f "$f" ] || continue
        case "$f" in *.png|*.jpg|*.pcap|*.bin|*.zip|*.apk) continue ;; esac
        if grep -qF "$old" "$f" 2>/dev/null; then
            # portable in-place edit (no sed -i incompat): python rewrite
            REPL_OLD="$old" REPL_NEW="$new" python - "$f" <<'PY'
import os,sys
p=sys.argv[1]; o=os.environ["REPL_OLD"]; n=os.environ["REPL_NEW"]
s=open(p,encoding="utf-8").read()
open(p,"w",encoding="utf-8",newline="\n").write(s.replace(o,n))
PY
        fi
    done
}
subst "$OLD_PKG"    "$NEW_PKG"
subst "$OLD_PKG_US" "$NEW_PKG_US"
subst "$OLD_NAME"   "$NEW_NAME"
subst "$OLD_LIB"    "$NEW_LIB"
echo "rewrote identifiers across $(echo "$FILES" | wc -w) tracked files"

# 3. Optionally strip the rooted-device test rig (most forks have no such device).
if [ "$MINIMAL" = 1 ]; then
    # Plain rm (not git rm): subst already modified some of these, and git rm
    # refuses files with unstaged changes. The user's later `git add -A` stages
    # the deletions.
    rm -f \
        .github/workflows/device-test.yml \
        scripts/device-harness.sh scripts/ui_tap.py scripts/aggregate-runs.py \
        scripts/run-bench.sh scripts/live-dashboard.sh scripts/dashboard.py
    echo "minimal: removed device-test rig + bench driver scripts"
    echo "  (the in-app/ELF profiler stays; only the physical-rooted-device automation is gone)"
fi

# 4. Validate — static, since we can't build the APK locally.
echo
echo "== validation =="
FAIL=0
# 4a. No stale identifiers remain (outside historical files).
for stale in "$OLD_PKG" "$OLD_PKG_US"; do
    HITS=$(git ls-files | grep -vE '^(scripts/init-template\.sh|commit-archive\.md)$' \
            | xargs grep -lF "$stale" 2>/dev/null || true)
    if [ -n "$HITS" ]; then echo "  STALE '$stale' still in:"; echo "$HITS" | sed 's/^/    /'; FAIL=1; fi
done
# 4b. JNI symbols match the new Kotlin package (the lockstep invariant).
if [ -f app/src/main/cpp/jni.cpp ]; then
    JNI_OK=$(grep -c "Java_${NEW_PKG_US}_MainActivity_" app/src/main/cpp/jni.cpp || true)
    if [ "$JNI_OK" -ge 1 ]; then echo "  ok: $JNI_OK JNI symbols carry the new package"; else echo "  FAIL: no JNI symbols with new package"; FAIL=1; fi
fi
# 4c. Kotlin package declaration matches.
if [ -f "$NEW_DIR/MainActivity.kt" ]; then
    grep -q "^package $NEW_PKG\$" "$NEW_DIR/MainActivity.kt" \
        && echo "  ok: MainActivity.kt declares package $NEW_PKG" \
        || { echo "  FAIL: MainActivity.kt package decl mismatch"; FAIL=1; }
fi
# 4d. loadLibrary matches the new lib name.
grep -rq "System.loadLibrary(\"$NEW_LIB\")" "$NEW_DIR" 2>/dev/null \
    && echo "  ok: System.loadLibrary(\"$NEW_LIB\")" || echo "  WARN: check System.loadLibrary name"

echo
if [ "$FAIL" = 0 ]; then
    echo "DONE. Next: review 'git status', then commit + push. CI will build the renamed APK."
    echo "Optional last step: delete this script and tidy README for your project."
else
    echo "VALIDATION FAILED — fix the issues above before committing."
    exit 1
fi
