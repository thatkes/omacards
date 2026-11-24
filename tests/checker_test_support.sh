#!/bin/sh
# Shared harness for the bin/check-* shell self-tests.
#
# Source from tests/test_check_*.sh after ROOT and CHECKER are set. The
# assertion, case runner and fixture-root helper are shared; each suite keeps
# its own fixtures and cases. No checker behaviour or output text lives here.

pass() {
    printf 'PASS %s\n' "$1"
}

fail() {
    printf 'FAIL %s: %s\n' "$1" "$2"
    failures=$((failures + 1))
}

expect_result() {
    name=$1
    want_status=$2
    want_text=$3
    forbidden=

    # Three call conventions are in use. Keep all three working so call sites
    # stay unchanged: an already-captured status and output (optionally followed
    # by forbidden text), a fixture root whose checker is run with that root, or
    # forbidden text followed by the checker arguments. A root is path-shaped,
    # so it is still recognised when the fixture has not been created.
    call_shape=checker_args
    if [ "$#" -eq 5 ] || [ "$#" -eq 6 ]; then
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
            forbidden=${6:-}
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
    pass "$name"
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

# Runs CHECKER against a synthetic log path, as the check-test-summary suite
# does; the checker's own CLI stays a fixture concern.
check_log_case() {
    clc_name=$1
    clc_want_status=$2
    clc_want_text=$3
    clc_log=$4
    set +e
    clc_output=$("$CHECKER" "$clc_log" 2>&1)
    clc_status=$?
    set -e
    expect_result "$clc_name" "$clc_want_status" "$clc_want_text" "$clc_status" "$clc_output"
}

# Runs an arbitrary command, for checkers whose CLI is not "$CHECKER [root]".
# The command and its arguments are the remaining case parameters.
expect_command_result() {
    ecr_name=$1
    ecr_want_status=$2
    ecr_want_text=$3
    ecr_forbidden=$4
    shift 4
    set +e
    ecr_output=$("$@" 2>&1)
    ecr_status=$?
    set -e
    last_output=$ecr_output
    expect_result "$ecr_name" "$ecr_want_status" "$ecr_want_text" \
        "$ecr_status" "$ecr_output" "$ecr_forbidden"
}

assert_status() {
    as_name=$1
    as_want=$2
    as_got=$3
    as_detail=${4:-}
    if [ "$as_got" -eq "$as_want" ]; then
        pass "$as_name"
    else
        fail "$as_name" "status $as_got, wanted $as_want: $as_detail"
    fi
}

assert_has() {
    ah_name=$1
    ah_text=$2
    ah_output=$3
    case "$ah_output" in
        *"$ah_text"*) pass "$ah_name" ;;
        *) fail "$ah_name" "output did not contain '$ah_text': $ah_output" ;;
    esac
}

assert_lacks() {
    al_name=$1
    al_text=$2
    al_output=$3
    case "$al_output" in
        *"$al_text"*) fail "$al_name" "output unexpectedly contained '$al_text': $al_output" ;;
        *) pass "$al_name" ;;
    esac
}

assert_file() {
    af_name=$1
    af_path=$2
    if [ -f "$af_path" ]; then
        pass "$af_name"
    else
        fail "$af_name" "missing file: $af_path"
    fi
}

assert_absent_file() {
    aaf_name=$1
    aaf_path=$2
    if [ -e "$aaf_path" ]; then
        fail "$aaf_name" "file should not exist: $aaf_path"
    else
        pass "$aaf_name"
    fi
}

assert_equal() {
    ae_name=$1
    ae_got=$2
    ae_want=$3
    if [ "$ae_got" = "$ae_want" ]; then
        pass "$ae_name"
    else
        fail "$ae_name" "got '$ae_got', wanted '$ae_want'"
    fi
}

assert_line_count() {
    alc_name=$1
    alc_want=$2
    alc_output=$3
    if [ -z "$alc_output" ]; then
        alc_count=0
    else
        alc_count=$(printf '%s\n' "$alc_output" | wc -l | tr -d '[:space:]')
    fi
    assert_equal "$alc_name" "$alc_count" "$alc_want"
}

assert_file_has_text() {
    afh_name=$1
    afh_file=$2
    afh_text=$3
    if grep -qF -- "$afh_text" "$afh_file"; then
        pass "$afh_name"
    else
        fail "$afh_name" "output did not contain $afh_text: $(cat "$afh_file")"
    fi
}

assert_file_lacks_text() {
    afl_name=$1
    afl_file=$2
    afl_text=$3
    if grep -qF -- "$afl_text" "$afl_file"; then
        fail "$afl_name" "output unexpectedly contained $afl_text: $(cat "$afl_file")"
    else
        pass "$afl_name"
    fi
}

count_matches() {
    cm_count=$(grep -cF -- "$2" "$1" || true)
    printf '%s' "$cm_count"
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
