#!/usr/bin/env bash
# One-command build wrapper for the mind_one 6.12 kernel (MT6789/MT8781V-CA) — main track.
# Wraps the exact steps documented in README.md — does not replace them.
#
# Usage:
#   TOOLCHAIN=/opt/toolchains/llvm-19.1.4-aarch64 ./build.sh [OUT_DIR]
#
# Requires: LLVM/clang 19.1.4 for aarch64 (see README.md — the toolchain version is
# not cosmetic, it changes module CRCs / module_layout that out-of-tree modules
# in mindone/modules must match) and pahole 1.31 for BTF. Host: Linux aarch64 or x86_64 with
# a cross-capable clang/lld.
set -euo pipefail
cd "$(dirname "$0")"

TOOLCHAIN="${TOOLCHAIN:-/opt/toolchains/llvm-19.1.4-aarch64}"
OUT="${1:-$(pwd)/../k612-out}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

if [ ! -x "$TOOLCHAIN/bin/clang" ]; then
    echo "!! no clang at $TOOLCHAIN/bin/clang — set TOOLCHAIN=/path/to/llvm-19.1.4-aarch64" >&2
    exit 1
fi

export PATH="$TOOLCHAIN/bin:$PATH"
export ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu-

# Pin what the kernel would otherwise take from whoever is building it.
#
# Without these three the image embeds the builder's account and machine name and
# the moment of the build:
#     Linux version 6.12.92-4k+ (someone@their-laptop) ... #1 SMP <build time>
# That makes two builds of the same commit differ for no useful reason, and it
# publishes a name and a hostname inside a binary that gets flashed onto other
# people's phones. Both are avoidable.
#
# The timestamp comes from the commit being built, so it is a property of the
# source rather than of when you happened to run make. Override SOURCE_DATE_EPOCH
# if you are building something that is not a git checkout.
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-infinix}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-infinix}"
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH="$(git log -1 --format=%ct 2>/dev/null || echo 0)"
fi
export SOURCE_DATE_EPOCH
export KBUILD_BUILD_TIMESTAMP="${KBUILD_BUILD_TIMESTAMP:-$(date -u -d "@$SOURCE_DATE_EPOCH" 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH" 2>/dev/null || date -u)}"

mkdir -p "$OUT"

echo "== toolchain: $("$TOOLCHAIN/bin/clang" --version | head -1)"
echo "== source:    $(pwd) @ $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "== stamps:    $KBUILD_BUILD_USER@$KBUILD_BUILD_HOST, $KBUILD_BUILD_TIMESTAMP"
echo "== output:    $OUT"

make O="$OUT" mindone_defconfig
make O="$OUT" -j"$JOBS" Image modules dtbs

REL=$(cat "$OUT/include/config/kernel.release" 2>/dev/null || echo unknown)
ML=$(grep -m1 -P '\tmodule_layout\t' "$OUT/Module.symvers" 2>/dev/null | cut -f1 || echo unknown)
echo "== done =="
echo "kernel.release  $REL   (expected: 6.12.92-4k+)"
echo "module_layout   $ML"
echo "Image           $OUT/arch/arm64/boot/Image"
echo "DTB             $OUT/arch/arm64/boot/dts/mediatek/X6837.dtb"
echo "Module.symvers  $OUT/Module.symvers   (ABI reference for building the modules against this kernel)"
