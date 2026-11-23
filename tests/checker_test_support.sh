#!/bin/sh
# Shared harness for the bin/check-* shell self-tests.
#
# Source from tests/test_check_*.sh after ROOT and CHECKER are set. The
# assertion, case runner and fixture-root helper are shared; each suite keeps
# its own fixture contents in populate_root and its own assertions at the call
# site. No checker behaviour or output text lives here.

expect_result() {
    name=$1
    want_status=$2
    want_text=$3
    forbidden=

    # Three call conventions are in use. Keep all three working so call sites
    # stay unchanged: an already-captured status and output, a fixture root
    # whose checker is run with that root, or forbidden text followed by the
    # checker arguments. A root is path-shaped, so it is still recognised when
    # the fixture has not been created.
    call_shape=checker_args
    if [ "$#" -eq 5 ]; then
        case "$4" in
            ''|*[!0-9]*)
                case "$4" in
                    */*) call_shape=fixture_root ;;
                    *)
                        if [ -d "$4" ]; then
                            call_shape=fixture_root
                        fi
                        ;;
                esac
                ;;
            *) call_shape=captured ;;
        esac
    fi
    case "$call_shape" in
        captured)
            got_status=$4
            output=$5
            ;;
        fixture_root)
            root=$4
            forbidden=${5:-}
            checker=${CHECKER##*/}
            set +e
            output="$("$root/bin/$checker" "$root" 2>&1)"
            got_status=$?
            set -e
            ;;
        checker_args)
            forbidden=$4
            shift 4
            set +e
            output="$("$CHECKER" "$@" 2>&1)"
            got_status=$?
            set -e
            ;;
    esac
    last_output=$output

    if [ "$got_status" -ne "$want_status" ]; then
        printf 'FAIL %s: status %s, wanted %s\n%s\n' \
            "$name" "$got_status" "$want_status" "$output"
        failures=$((failures + 1))
        return
    fi
    if [ -z "$want_text" ]; then
        if [ -n "$output" ]; then
            printf 'FAIL %s: expected no output, got\n%s\n' "$name" "$output"
            failures=$((failures + 1))
            return
        fi
    else
        case "$output" in
            *"$want_text"*) ;;
            *)
                printf 'FAIL %s: output did not contain %s\n%s\n' \
                    "$name" "$want_text" "$output"
                failures=$((failures + 1))
                return
                ;;
        esac
    fi
    if [ -n "$forbidden" ]; then
        case "$output" in
            *"$forbidden"*)
                printf 'FAIL %s: output unexpectedly contained %s\n%s\n' \
                    "$name" "$forbidden" "$output"
                failures=$((failures + 1))
                return
                ;;
        esac
    fi
    printf 'PASS %s\n' "$name"
}

check_case() {
    name=$1
    want_status=$2
    want_text=$3
    root=$4
    checker=${CHECKER##*/}
    set +e
    if [ "${CHECKER_TAKES_ROOT:-0}" -eq 1 ]; then
        output="$("$root/bin/$checker" "$root" 2>&1)"
    else
        output="$("$root/bin/$checker" 2>&1)"
    fi
    got_status=$?
    set -e
    expect_result "$name" "$want_status" "$want_text" "$got_status" "$output"
}

populate_root() {
    :
}

make_root() {
    root=$1
    checker=${CHECKER##*/}
    mkdir -p "$root/bin"
    cp "$CHECKER" "$root/bin/$checker"
    chmod +x "$root/bin/$checker"
    populate_root "$root"
}
