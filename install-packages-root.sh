#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -lt 2 ]]; then
    echo "usage: $0 ROOT_DIR PACKAGE.zip..." >&2
    exit 2
fi

root_dir=$(realpath -m "$1")
shift

if [[ "$root_dir" == "/" ]]; then
    echo "refusing to preinstall packages over /" >&2
    exit 2
fi

mkdir -p "$root_dir/config/neupak"
install_db="$root_dir/config/neupak/install.db"
files_db="$root_dir/config/neupak/files.db"
: > "$install_db"
: > "$files_db"

declare -A owners=()

toml_string() {
    local key=$1
    sed -n "s/^${key}[[:space:]]*=[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -n 1
}

for archive in "$@"; do
    if [[ ! -f "$archive" ]]; then
        echo "missing package archive: $archive" >&2
        exit 1
    fi

    manifest=$(unzip -p "$archive" manifest.toml)
    name=$(printf '%s\n' "$manifest" | sed -n 's/^\[\([^]]*\)\]$/\1/p' | head -n 1)
    version=$(printf '%s\n' "$manifest" | toml_string version)
    strategy=$(printf '%s\n' "$manifest" | toml_string strategy)
    if [[ -z "$name" || -z "$version" || -z "$strategy" ]]; then
        echo "invalid manifest in $archive" >&2
        exit 1
    fi
    if [[ ! "$name" =~ ^[A-Za-z0-9._+-]+$ ||
          ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][A-Za-z0-9._-]+)?$ ]]; then
        echo "invalid package identity in $archive" >&2
        exit 1
    fi
    if [[ "$strategy" != "over_root" ]]; then
        echo "cannot preinstall $name with unsupported strategy: $strategy" >&2
        exit 1
    fi

    mapfile -t entries < <(unzip -Z1 "$archive")
    for entry in "${entries[@]}"; do
        if [[ "$entry" == /* || "$entry" == *\\* || "$entry" == *\"* ||
              "$entry" == ".." || "$entry" == ../* || "$entry" == */../* || "$entry" == */.. ]]; then
            echo "unsafe archive path in $archive: $entry" >&2
            exit 1
        fi
        if [[ "$entry" == "manifest.toml" || "$entry" == */ ]]; then
            continue
        fi
        logical_path="/$entry"
        if [[ -n "${owners[$logical_path]:-}" ]]; then
            echo "package file conflict: $logical_path belongs to ${owners[$logical_path]} and $name" >&2
            exit 1
        fi
        owners[$logical_path]=$name
    done

    unzip -q -o "$archive" -d "$root_dir"
    rm -f "$root_dir/manifest.toml"

    {
        printf '[%s]\n' "$name"
        printf 'version = "%s"\n' "$version"
        printf 'strategy = "%s"\n\n' "$strategy"
    } >> "$install_db"

    {
        printf '[%s]\n' "$name"
        printf 'files = [\n'
        for entry in "${entries[@]}"; do
            if [[ "$entry" != "manifest.toml" && "$entry" != */ ]]; then
                printf '    "/%s",\n' "$entry"
            fi
        done
        printf ']\n\n'
    } >> "$files_db"
done

echo "preinstalled $# package(s) into $root_dir"
