#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR="$ROOT_DIR/main"
MARKER="$REPO_DIR/.neupak-generated"

# Older versions of this helper generated an index and package/ subtree. If the
# marker is present, the folder is disposable and can be rebuilt as a flat zip set.
if [[ -e "$REPO_DIR" && -f "$MARKER" ]]; then
    rm -rf "$REPO_DIR"
fi

mkdir -p "$REPO_DIR"
find "$REPO_DIR" -maxdepth 1 -type f -name '*.zip' -delete

while IFS= read -r -d '' package_dir; do
    name=$(basename "$package_dir")
    echo "packaging $name"
    make -C "$package_dir" package
done < <(find "$ROOT_DIR" \
    -mindepth 1 -maxdepth 1 \
    -type d \
    ! -path "$REPO_DIR" \
    -exec test -f '{}/Makefile' ';' \
    -print0 | sort -z)

found=0
while IFS= read -r -d '' zip_path; do
    name=$(basename "$zip_path")
    cp "$zip_path" "$REPO_DIR/$name"
    found=$((found + 1))
    echo "added $name"
done < <(find "$ROOT_DIR" \
    -path "$REPO_DIR" -prune -o \
    -mindepth 2 -maxdepth 3 \
    -path '*/out/*.zip' \
    -type f \
    -print0 | sort -z)

if [[ "$found" -eq 0 ]]; then
    echo "no package zips found under */out/*.zip" >&2
    exit 1
fi

mapfile -d '' repo_zips < <(find "$REPO_DIR" -maxdepth 1 -type f -name '*.zip' -print0 | sort -z)
staging_dir=$(mktemp -d)
trap 'rm -rf "$staging_dir"' EXIT
"$ROOT_DIR/build-local-repo.sh" "$staging_dir" "${repo_zips[@]}"
cp "$staging_dir/index.toml" "$REPO_DIR/index.toml"
cp "$staging_dir/repos.cfg" "$REPO_DIR/repos.cfg"

echo "assembled $found package zip(s) and index.toml in $REPO_DIR"
