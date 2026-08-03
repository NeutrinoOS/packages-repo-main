#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -lt 2 ]]; then
    echo "usage: $0 OUTPUT_DIR PACKAGE.zip..." >&2
    exit 2
fi

output_dir=$(realpath -m "$1")
shift

if [[ "$output_dir" == "/" ]]; then
    echo "refusing to assemble a repository at /" >&2
    exit 2
fi

mkdir -p "$output_dir"
find "$output_dir" -maxdepth 1 -type f \( -name '*.zip' -o -name 'index.toml' -o -name 'repos.cfg' \) -delete

for archive in "$@"; do
    if [[ ! -f "$archive" ]]; then
        echo "missing package archive: $archive" >&2
        exit 1
    fi
    cp "$archive" "$output_dir/$(basename "$archive")"
done

index_tmp="$output_dir/index.toml.tmp"
: > "$index_tmp"

while IFS= read -r -d '' archive; do
    manifest=$(unzip -p "$archive" manifest.toml)
    section=$(printf '%s\n' "$manifest" | sed -n 's/^\[\([^]]*\)\]$/\1/p' | head -n 1)
    if [[ -z "$section" ]]; then
        echo "package has no manifest section: $archive" >&2
        exit 1
    fi

    filename=$(basename "$archive")
    size=$(stat -c '%s' "$archive")
    sha256=$(sha256sum "$archive" | awk '{print $1}')
    {
        printf '[%s]\n' "$section"
        printf 'package = "%s"\n' "$filename"
        printf 'sha256 = "%s"\n' "$sha256"
        printf 'size = %s\n' "$size"
        printf '%s\n' "$manifest" | sed -n \
            -e '/^version[[:space:]]*=/p' \
            -e '/^strategy[[:space:]]*=/p' \
            -e '/^description[[:space:]]*=/p' \
            -e '/^depends[[:space:]]*=/p'
        printf '\n'
    } >> "$index_tmp"
done < <(find "$output_dir" -maxdepth 1 -type f -name '*.zip' -print0 | sort -z)

mv "$index_tmp" "$output_dir/index.toml"
printf '%s\n' '[live]' 'indexurl = "file:///packages/index.toml"' > "$output_dir/repos.cfg"

echo "assembled local repository in $output_dir"
