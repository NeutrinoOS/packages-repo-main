#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <package-root>" >&2
    exit 2
fi

package_root=$1
main_binary="$package_root/binary/java"
library_root="$package_root/library/openjdk-8"
readelf_tool=${READELF:-x86_64-elf-readelf}

for required in "$main_binary" \
                "$library_root/libjvm.so" \
                "$library_root/libverify.so" \
                "$library_root/libjava.so"; do
    if [ ! -f "$required" ]; then
        echo "ELF audit: missing required runtime object: $required" >&2
        exit 1
    fi
done

audit_tmp=$(mktemp -d)
trap 'rm -rf "$audit_tmp"' EXIT HUP INT TERM
audit_failed=0

defined_symbols() {
    "$readelf_tool" --dyn-syms --wide "$1" | awk '
        $7 != "UND" &&
        ($5 == "GLOBAL" || $5 == "WEAK" || $5 == "UNIQUE" ||
         $5 == "GNU_UNIQUE") && $6 != "HIDDEN" {
            name = $8
            sub(/@.*/, "", name)
            if (name != "") print name
        }'
}

required_symbols() {
    "$readelf_tool" --dyn-syms --wide "$1" | awk '
        $7 == "UND" && $5 != "WEAK" {
            name = $8
            sub(/@.*/, "", name)
            if (name != "") print name
        }'
}

append_definitions() {
    defined_symbols "$1" >> "$2"
    sort -u -o "$2" "$2"
}

check_symbols() {
    object=$1
    providers=$2
    label=${object#"$package_root/"}
    required_symbols "$object" | sort -u > "$audit_tmp/required"
    comm -23 "$audit_tmp/required" "$providers" > "$audit_tmp/missing"
    if [ -s "$audit_tmp/missing" ]; then
        echo "ELF audit: unresolved strong imports in $label:" >&2
        sed 's/^/  /' "$audit_tmp/missing" >&2
        audit_failed=1
        touch "$audit_tmp/failure"
    fi
}

check_relocations() {
    object=$1
    label=${object#"$package_root/"}
    "$readelf_tool" --relocs --wide "$object" | awk '
        /R_X86_64_/ &&
        $3 != "R_X86_64_64" &&
        $3 != "R_X86_64_GLOB_DAT" &&
        $3 != "R_X86_64_JUMP_SLOT" &&
        $3 != "R_X86_64_RELATIVE" { print $3 }' | sort -u \
        > "$audit_tmp/relocations"
    if [ -s "$audit_tmp/relocations" ]; then
        echo "ELF audit: loader-incompatible relocations in $label:" >&2
        sed 's/^/  /' "$audit_tmp/relocations" >&2
        audit_failed=1
        touch "$audit_tmp/failure"
    fi
}

needed_libraries() {
    "$readelf_tool" --dynamic --wide "$1" | awk '
        /\(NEEDED\)/ {
            name = $5
            gsub(/^\[/, "", name)
            gsub(/\]$/, "", name)
            print name
        }'
}

check_dependencies() {
    object=$1
    providers=$2
    label=${object#"$package_root/"}
    needed_libraries "$object" > "$audit_tmp/needed"
    while IFS= read -r needed; do
        [ -n "$needed" ] || continue
        dependency="$package_root/library/$needed"
        if [ ! -f "$dependency" ]; then
            echo "ELF audit: $label needs missing $needed" >&2
            audit_failed=1
            touch "$audit_tmp/failure"
            continue
        fi
        append_definitions "$dependency" "$providers"
    done < "$audit_tmp/needed"
}

check_soname() {
    object=$1
    label=${object#"$package_root/"}
    expected="openjdk-8/${object##*/}"
    actual=$("$readelf_tool" --dynamic --wide "$object" | awk '
        /\(SONAME\)/ {
            name = $5
            gsub(/^\[/, "", name)
            gsub(/\]$/, "", name)
            print name
            exit
        }')
    if [ "$actual" != "$expected" ]; then
        echo "ELF audit: $label has SONAME '$actual', expected '$expected'" >&2
        audit_failed=1
        touch "$audit_tmp/failure"
    fi
}

if [ -n "$(needed_libraries "$main_binary")" ]; then
    echo "ELF audit: the java launcher must remain a self-contained static PIE" >&2
    audit_failed=1
    touch "$audit_tmp/failure"
fi

required_symbols "$main_binary" | sort -u > "$audit_tmp/main-imports"
if [ -s "$audit_tmp/main-imports" ]; then
    echo "ELF audit: unresolved imports in the java launcher:" >&2
    sed 's/^/  /' "$audit_tmp/main-imports" >&2
    audit_failed=1
    touch "$audit_tmp/failure"
fi
check_relocations "$main_binary"

defined_symbols "$main_binary" | sort -u > "$audit_tmp/core-providers"
for core_name in libjvm.so libverify.so libjava.so; do
    core_object="$library_root/$core_name"
    check_dependencies "$core_object" "$audit_tmp/core-providers"
    check_symbols "$core_object" "$audit_tmp/core-providers"
    check_relocations "$core_object"
    check_soname "$core_object"
    append_definitions "$core_object" "$audit_tmp/core-providers"
done

find "$library_root" -type f -name '*.so' | sort | while IFS= read -r object; do
    case "$object" in
        "$library_root/libjvm.so"|"$library_root/libverify.so"|"$library_root/libjava.so")
            continue
            ;;
    esac
    cp "$audit_tmp/core-providers" "$audit_tmp/object-providers"
    check_dependencies "$object" "$audit_tmp/object-providers"
    check_symbols "$object" "$audit_tmp/object-providers"
    check_relocations "$object"
    check_soname "$object"
    append_definitions "$object" "$audit_tmp/object-providers"
done

# The loop above runs in a subshell on POSIX shells, so use marker files for
# failures that must propagate to the parent process.
if find "$audit_tmp" -name failure -print -quit | grep -q .; then
    audit_failed=1
fi

if [ "$audit_failed" -ne 0 ]; then
    exit 1
fi

echo "ELF audit: all runtime dependencies, strong imports, and relocations are supported"
