#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [[ "$#" -eq 0 ]]; then
    echo "usage: $0 PACKAGE..." >&2
    exit 2
fi

declare -A states=()
resolved=()

resolve_package() {
    local name=$1
    local manifest section depends_value compact dependency
    local -a dependencies=()

    if [[ ! "$name" =~ ^[A-Za-z0-9._+-]+$ ]]; then
        echo "invalid package name: $name" >&2
        return 1
    fi

    case "${states[$name]:-0}" in
        1)
            echo "package dependency cycle includes: $name" >&2
            return 1
            ;;
        2)
            return 0
            ;;
    esac

    manifest="$ROOT_DIR/$name/package/manifest.toml"
    if [[ ! -f "$manifest" ]]; then
        echo "package manifest not found: $manifest" >&2
        return 1
    fi

    section=$(sed -n 's/^\[\([^]]*\)\]$/\1/p' "$manifest" | head -n 1)
    if [[ "$section" != "$name" ]]; then
        echo "package manifest section mismatch for $name: ${section:-missing}" >&2
        return 1
    fi

    depends_value=$(sed -n \
        's/^depends[[:space:]]*=[[:space:]]*\[\(.*\)\][[:space:]]*$/\1/p' \
        "$manifest" | head -n 1)
    if ! grep -q '^depends[[:space:]]*=' "$manifest"; then
        echo "package manifest has no depends array: $manifest" >&2
        return 1
    fi

    compact=$(printf '%s' "$depends_value" | tr -d '[:space:]')
    if [[ -n "$compact" ]]; then
        if [[ ! "$compact" =~ ^\"[A-Za-z0-9._+-]+\"(,\"[A-Za-z0-9._+-]+\")*$ ]]; then
            echo "unsupported depends array in $manifest" >&2
            return 1
        fi
        compact=${compact//\"/}
        IFS=',' read -r -a dependencies <<< "$compact"
    fi

    states[$name]=1
    for dependency in "${dependencies[@]}"; do
        resolve_package "$dependency"
    done
    states[$name]=2
    resolved+=("$name")
}

for package in "$@"; do
    resolve_package "$package"
done

printf '%s\n' "${resolved[*]}"
