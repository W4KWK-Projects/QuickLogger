#!/usr/bin/env bash
# Release helper, so a release is two commands that can be run from anywhere
# gh and git work, with nothing to type by hand in between.
#
#   tools/release.sh check [--windows]   everything that should be green
#                                        BEFORE tagging, on the pushed HEAD of
#                                        main: the Build workflow, and (run
#                                        now, by hand) the macOS tests, plus
#                                        Windows' with --windows (for changes
#                                        to how Windows builds)
#   tools/release.sh tag                 tags the version in CMakeLists.txt
#                                        and pushes main and the tag
#                                        together, then waits for the release
#                                        to be published and reports it
#
# `tag` runs `check` first. The release's own macOS job runs the same tests
# as the macOS check, so a tag that passes it doesn't fail there.
set -euo pipefail

repo=W4KWK-Projects/QuickLogger
cd "$(dirname "$0")/.."

version=$(sed -n 's/^project(QuickLogger VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
tag="v$version"

die()
{
    echo "release.sh: $*" >&2
    exit 1
}

# Waits for the newest run of a workflow on a commit to finish, and fails
# unless it succeeded. $1 workflow file, $2 commit, $3 minutes to wait at most.
wait_for()
{
    local workflow=$1 sha=$2 minutes=$3 waited=0 run status conclusion
    while true; do
        run=$(gh run list -R "$repo" --workflow "$workflow" --commit "$sha" -L 1 \
            --json databaseId,status,conclusion --jq '.[0] | "\(.databaseId) \(.status) \(.conclusion)"' 2>/dev/null || true)
        if [ -n "$run" ]; then
            read -r id status conclusion <<<"$run"
            if [ "$status" = "completed" ]; then
                if [ "$conclusion" = "success" ]; then
                    echo "ok: $workflow on ${sha:0:7} (run $id)"
                    return 0
                fi
                die "$workflow on ${sha:0:7} ended '$conclusion': https://github.com/$repo/actions/runs/$id"
            fi
        fi
        if [ "$waited" -ge $((minutes * 60)) ]; then
            die "$workflow on ${sha:0:7} still not done after $minutes minutes"
        fi
        sleep 15
        waited=$((waited + 15))
    done
}

# Starts a workflow_dispatch workflow on main and waits for that run.
dispatch_and_wait()
{
    local workflow=$1 sha=$2 minutes=$3
    gh workflow run "$workflow" -R "$repo" --ref main > /dev/null
    sleep 10
    wait_for "$workflow" "$sha" "$minutes"
}

check()
{
    local windows=${1:-}
    [ "$(git rev-parse --abbrev-ref HEAD)" = main ] || die "not on main"
    [ -z "$(git status --porcelain --untracked-files=no)" ] || die "uncommitted changes"
    git fetch -q origin main
    local sha
    sha=$(git rev-parse HEAD)
    [ "$sha" = "$(git rev-parse origin/main)" ] || die "HEAD isn't what origin/main has: push (or pull) first"
    echo "Checking $tag at ${sha:0:7}"
    wait_for build.yml "$sha" 30
    dispatch_and_wait macos-tests.yml "$sha" 30
    if [ "$windows" = "--windows" ]; then
        dispatch_and_wait windows-binary.yml "$sha" 60
    fi
    echo "All checks passed for $tag."
}

tag_and_publish()
{
    if git ls-remote --exit-code --tags origin "refs/tags/$tag" > /dev/null 2>&1; then
        die "$tag already exists on origin; moving a tag is done by hand"
    fi
    check "${1:-}"
    local sha
    sha=$(git rev-parse HEAD)
    git tag -a "$tag" -m "QuickLogger $version"
    git push --atomic origin main "$tag"
    echo "Pushed $tag. Waiting for the release run..."
    sleep 15
    local run
    run=$(gh run list -R "$repo" --workflow release.yml --commit "$sha" -L 1 --json databaseId --jq '.[0].databaseId')
    [ -n "$run" ] || die "no release run found for ${sha:0:7}"
    local waited=0 state
    while true; do
        state=$(gh run view "$run" -R "$repo" --json jobs \
            --jq '[.jobs[] | select(.name == "publish") | .status + "/" + (.conclusion // "")][0]')
        case "$state" in
            completed/success)
                break
                ;;
            completed/*)
                die "publish ended '$state': https://github.com/$repo/actions/runs/$run"
                ;;
        esac
        # A failed build job skips publish; stop on the first failure.
        if gh run view "$run" -R "$repo" --json jobs --jq '.jobs[] | select(.conclusion == "failure") | .name' | grep -q .; then
            die "a job failed: https://github.com/$repo/actions/runs/$run"
        fi
        [ "$waited" -lt 5400 ] || die "release not published after 90 minutes"
        sleep 30
        waited=$((waited + 30))
    done
    gh release view "$tag" -R "$repo" --json isDraft,assets --jq '"published: draft=\(.isDraft), \(.assets | length) assets"'
    echo "FreeBSD arm64 follows in about 45 minutes. Next: the release notes (gh release edit $tag --notes-file ...)."
}

case "${1:-}" in
    check)
        check "${2:-}"
        ;;
    tag)
        tag_and_publish "${2:-}"
        ;;
    *)
        sed -n '2,17p' "$0"
        exit 2
        ;;
esac
