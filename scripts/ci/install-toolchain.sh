#!/usr/bin/env bash
# CI toolchain for the hosted Ubuntu runner: LLVM at the given major version, Conan, and the
# PipeWire headers. The PipeWire daemon is installed only with --with-pipewire-session: the gate
# runs the no-daemon tests, which must find no PipeWire session.
# Usage: sudo bash scripts/ci/install-toolchain.sh 21 [--with-pipewire-session]
set -euo pipefail
ver=${1:?usage: install-toolchain.sh <llvm-major> [--with-pipewire-session]}
session=${2:-}
apt-get update
apt-get install -y cmake ninja-build cppcheck python3-pip wget ca-certificates libpipewire-0.3-dev
if [[ $session == --with-pipewire-session ]]; then
    apt-get install -y pipewire wireplumber pipewire-bin dbus-x11
fi
# apt.llvm.org provides clang-21, clang-tidy-21 and clang-format-21 on this runner.
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
/tmp/llvm.sh "$ver"
apt-get install -y "clang-tidy-$ver" "clang-format-$ver"
pip install --break-system-packages "conan>=2,<3"
# pip installs into ~/.local/bin on the runner; the next steps must see it.
echo "$HOME/.local/bin" >>"${GITHUB_PATH:-/dev/null}"
pipewire --version
