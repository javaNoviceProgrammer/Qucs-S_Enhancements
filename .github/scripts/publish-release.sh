#!/usr/bin/env bash
# Publishes every file in DIR as the GitHub release of TAG, for the publish job
# of .github/workflows/release.yml. Runs as well from a shell against a test
# repository: set the environment below and pass the folder.
#
#   GH_TOKEN, GITHUB_REPOSITORY, GITHUB_SHA   as in Actions
#   TAG    the release's tag: a version tag, or "continuous"
#   KIND   "tag" for a version tag, anything else for the rolling pre-release
#
# A release is never deleted. On 10 November 2026 the old way, deleting the
# release and creating it again with --target, lost v27.0.3: the delete went
# through and the create was refused with HTTP 403. GitHub asks for a
# "workflows" permission, which an Actions token cannot have, to create a
# release whose target commit has other workflow files than the default
# branch (cli/cli#9514). So:
#   * a version tag must exist already, and its release is created with
#     --verify-tag and no --target: the tag pins the commit;
#   * an existing release keeps its page and has its files swapped in place,
#     the new ones uploaded under a temporary name before any old one goes;
#   * the rolling "continuous" tag is moved to the commit before its release
#     is touched, so a refused move leaves the release as it was.
# Releases here carry no text, the rolling one included: an empty body shows
# the tag's message, so the body is "&nbsp;". Only the newest version tag
# takes Latest.
set -euo pipefail

DIR=${1:?usage: publish-release.sh <folder of files>}
REPO=${GITHUB_REPOSITORY:?}
SHA=${GITHUB_SHA:?}
TAG=${TAG:?}
NOTES='&nbsp;'
PREFIX='incoming.'   # temporary name of a file while it is being uploaded

shopt -s nullglob
FILES=("$DIR"/*)
[ ${#FILES[@]} -gt 0 ] || { echo "nothing to publish in $DIR" >&2; exit 1; }
NAMES=()
for f in "${FILES[@]}"; do NAMES+=("$(basename "$f")"); done

api() { gh api -H 'X-GitHub-Api-Version: 2022-11-28' "$@"; }
tag_exists() { api "repos/$REPO/git/ref/tags/$1" --silent 2>/dev/null; }

if [ "${KIND:-}" = tag ]; then
  TITLE="Qucs-S enhanced ${TAG}"
  STATE=(--prerelease=false)
  tag_exists "$TAG" || { echo "tag $TAG is not on $REPO" >&2; exit 1; }
  # Latest goes to the highest version tag only, so that building an older
  # tag again leaves it where it is.
  NEWEST=$(api "repos/$REPO/git/matching-refs/tags/v" --paginate --jq '.[].ref' \
             | sed -n 's#^refs/tags/\(v[0-9].*\)#\1#p' | sort -V | tail -n 1)
  if [ "$NEWEST" = "$TAG" ]; then LATEST=true; else LATEST=false; fi
  echo "$TAG; newest version tag $NEWEST; Latest: $LATEST"
else
  TITLE="Continuous build (${SHA::7})"
  STATE=(--prerelease)
  LATEST=false
  # Move the tag first: if GitHub refuses, nothing has changed yet.
  if tag_exists "$TAG"; then
    api -X PATCH "repos/$REPO/git/refs/tags/$TAG" -f sha="$SHA" -F force=true --silent
  else
    api -X POST "repos/$REPO/git/refs" -f ref="refs/tags/$TAG" -f sha="$SHA" --silent
  fi
  echo "tag $TAG now at $SHA"
fi

RELEASE=$(api "repos/$REPO/releases/tags/$TAG" 2>/dev/null || true)
if [ -z "$RELEASE" ] || ! jq -e .id >/dev/null 2>&1 <<<"$RELEASE"; then
  echo "creating the release $TAG"
  gh release create "$TAG" "${FILES[@]}" --repo "$REPO" --verify-tag \
    --title "$TITLE" --notes "$NOTES" "${STATE[@]}" --latest="$LATEST"
  echo "Published $TAG with:"; printf '  %s\n' "${NAMES[@]}"
  exit 0
fi

ID=$(jq -r .id <<<"$RELEASE")
echo "replacing the files of the existing release $TAG (id $ID) in place"

asset_ids() {   # prints "id<TAB>name" for every file of the release
  api "repos/$REPO/releases/$ID/assets" --paginate --jq '.[] | "\(.id)\t\(.name)"'
}
drop_incoming() {   # removes temporary files a failed run left behind
  asset_ids | while IFS=$'\t' read -r aid name; do
    if [[ $name == "$PREFIX"* ]]; then
      echo "  removing leftover $name"; api -X DELETE "repos/$REPO/releases/assets/$aid" --silent
    fi
  done
}

drop_incoming
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
for f in "${FILES[@]}"; do
  ln "$f" "$STAGE/$PREFIX$(basename "$f")" 2>/dev/null || cp "$f" "$STAGE/$PREFIX$(basename "$f")"
done

# 1. Upload everything under the temporary names, next to the old files.
if ! gh release upload "$TAG" "$STAGE"/* --repo "$REPO"; then
  echo "upload failed; the release keeps its old files" >&2
  drop_incoming
  exit 1
fi

# 2. Put each new file in place of the old one of the same name.
ASSETS=$(asset_ids)
asset_id() { awk -F'\t' -v n="$1" '$2 == n { print $1; exit }' <<<"$ASSETS"; }
built() { local n; for n in "${NAMES[@]}"; do [ "$n" = "$1" ] && return 0; done; return 1; }
for name in "${NAMES[@]}"; do
  [ -n "$(asset_id "$PREFIX$name")" ] || { echo "uploaded $PREFIX$name is missing" >&2; exit 1; }
done
for name in "${NAMES[@]}"; do
  old=$(asset_id "$name")
  [ -z "$old" ] || api -X DELETE "repos/$REPO/releases/assets/$old" --silent
  api -X PATCH "repos/$REPO/releases/assets/$(asset_id "$PREFIX$name")" -f name="$name" --silent
  echo "  $name"
done

# 3. Remove the files this build no longer makes.
while IFS=$'\t' read -r aid name; do
  [[ $name == "$PREFIX"* ]] || built "$name" && continue
  echo "  removing $name, no longer built"
  api -X DELETE "repos/$REPO/releases/assets/$aid" --silent
done <<<"$ASSETS"

# 4. Title, text and state. Latest is only ever claimed, never given up here.
EDIT=(--title "$TITLE" --notes "$NOTES" "${STATE[@]}")
if [ "$LATEST" = true ]; then EDIT+=(--latest); fi
gh release edit "$TAG" --repo "$REPO" "${EDIT[@]}" >/dev/null
echo "Published $TAG with:"; printf '  %s\n' "${NAMES[@]}"
