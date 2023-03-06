#! /usr/bin/env nix-shell
#! nix-shell -i bash -p bash hyperfine

# shellcheck shell=bash

set -euo pipefail

# NOTE: this has to be normalized for the hash below to match:
readonly SOURCE="ssh://git@github.com/qemu/qemu.git"
readonly REV="2946e1af2704bf6584f57d4e3aec49d1d5f3ecc0"
readonly REF="master"

# readonly SOURCE="ssh://git@github.com/rrbutani/cache-flake-inputs.git"
# readonly REV="fb5cb12ed54cc761ee58047d90f9180b9434e181"
# readonly REF="main"

readonly NIX_STABLE="${1-"/run/current-system/sw/bin/nix"}"
readonly NIX_EXPERIMENTAL="${2-"outputs/out/bin/nix"}"

readonly CACHE_DIR=~/.cache/nix/gitv3/"$(
    "$NIX_STABLE" --experimental-features nix-command hash to-base32 --type sha256 \
        "$(echo -n "${SOURCE}" | sha256sum | cut -d' ' -f1)"
)"

# $1: name, $2: exprArgs
bench() {
    echo "Running: $1 benchmark"

    rm -rf "$CACHE_DIR" # incompat between new nix and old nix

    local exprArgs="$2"
    local cmd=(
        --experimental-features nix-command eval
        --expr "(builtins.fetchGit $exprArgs).outPath"
        --raw --tarball-ttl 0
    )

    echo "  - getting store dir for '$exprArgs'..."
    local store_dir="$("$NIX_STABLE" "${cmd[@]}")"
    echo "  - store dir: ${store_dir}"

    local delete_cmd=(
        "${NIX_STABLE}" --experimental-features nix-command
        store delete "${store_dir}"
            --quiet
    )

    # # Run a delete and then a fetch and see if the cache directory is created:
    # rm -rf "${CACHE_DIR}"
    # "${delete_cmd[@]}" &>/dev/null

    echo "  - checking that cache dir '$CACHE_DIR' is produced..."
    # "${NIX_STABLE}" "${cmd[@]}" &>/dev/null

    if ! [[ -d "${CACHE_DIR}" ]]; then
        echo "ERROR: '$SOURCE' does not seem to use cache dir: ${CACHE_DIR}";
        exit 4
    fi

    echo
    hyperfine \
        --prepare "rm -rf ${CACHE_DIR}; ${delete_cmd[*]}; sync;" \
        -L nix "${NIX_STABLE},${NIX_EXPERIMENTAL}" \
        "{nix} ${cmd[*]@Q}" \
        --runs 2 # --show-output
    echo
    echo "------------------------------------------------------------------------------------------------------------------------------------------"
    echo
}

# bench "normal" "{ url = \"$SOURCE\"; ref = \"$REF\"; rev = \"$REV\"; }"
# bench "shallow" "{ url = \"$SOURCE\"; ref = \"$REF\"; rev = \"$REV\"; shallow = true; }"
# bench "submodules" "{ url = \"$SOURCE\"; ref = \"$REF\"; rev = \"$REV\"; submodules = true; }"
bench "submodules + shallow" "{ url = \"$SOURCE\"; ref = \"$REF\"; rev = \"$REV\"; submodules = true; shallow = true; }"

# TODO: also measure just "reify" times (i.e. cache dir intact, store path deleted)

# TODO: and then measure cache dir intact + 1 commit over times (actually... don't bother with this)
