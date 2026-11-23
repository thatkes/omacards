#!/bin/sh
# Shared parser for the two Local-only blocks in .gitignore.
# local_only_sync_paths <root> prints one normalised path per line, in file
# order. A broken source stops the caller with a named message before any
# copy or lint result is produced.

local_only_sync_paths() {
    local_root=$1
    local_ignore="$local_root/.gitignore"

    if [ ! -r "$local_ignore" ]; then
        printf 'Cannot read local-only source: %s\n' "$local_ignore" >&2
        return 1
    fi

    awk -v root="$local_root" '
        BEGIN {
            n = split("AGENTS.md docs/agents reference", sentinel, " ")
        }
        /^[[:space:]]*#/ {
            if ($0 ~ /Local-only/) {
                headers++
                block_count++
                current = block_count
                seen[current] = 0
            }
            next
        }
        /^[[:space:]]*$/ {
            current = 0
            next
        }
        current == 0 { next }
        {
            line = $0
            sub(/^[[:space:]]+/, "", line)
            sub(/[[:space:]]+$/, "", line)
            sub(/^\/+/, "", line)
            sub(/\/+$/, "", line)
            if (line == "" || line == "build" || line == "pkgbuild/pkg" || \
                line == "pkgbuild/src" || line == "*.pkg.tar.*" || \
                line == ".gitignore") next
            seen[current] = 1
            found[line] = 1
            print line
        }
        END {
            if (headers < 2) {
                printf "Local-only header missing in %s/.gitignore\n", root > "/dev/stderr"
                exit 1
            }
            for (i = 1; i <= block_count; i++) {
                if (!seen[i]) {
                    printf "Local-only block %d is empty in %s/.gitignore\n", i, root > "/dev/stderr"
                    exit 1
                }
            }
            for (i = 1; i <= n; i++) {
                if (!found[sentinel[i]]) {
                    printf "Local-only sentinel missing in %s/.gitignore: %s\n", root, sentinel[i] > "/dev/stderr"
                    exit 1
                }
            }
        }
    ' "$local_ignore"
}
