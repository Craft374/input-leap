#!/bin/bash

set -euo pipefail
cd "$(dirname "$0")"

pause_on_exit() {
    status=$?
    trap - EXIT
    echo
    if [ "$status" -eq 0 ]; then
        echo "Build completed: $PWD/build/bundle/InputLeafPlus.app"
    else
        echo "Build failed. See the error above."
    fi
    if [ -t 0 ]; then
        read -r -p "Press Enter to close..." _
    fi
    exit "$status"
}
trap pause_on_exit EXIT

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode Command Line Tools are required. Run: xcode-select --install"
    exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required: https://brew.sh"
    exit 1
fi

required_packages=(cmake ninja qt openssl@3)
missing_packages=()
for package in "${required_packages[@]}"; do
    if ! brew list --versions "$package" >/dev/null 2>&1; then
        missing_packages+=("$package")
    fi
done
if [ "${#missing_packages[@]}" -gt 0 ]; then
    brew install "${missing_packages[@]}"
fi

qt_root="$(brew --prefix qt)"
openssl_root="$(brew --prefix openssl@3)"
export PATH="$qt_root/bin:$PATH"
export CMAKE_PREFIX_PATH="$qt_root:$openssl_root${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export OPENSSL_ROOT_DIR="$openssl_root"
export B_BUILD_TYPE=Release
export B_OSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion | awk -F. '{print $1 "." $2}')"
export B_CMAKE_FLAGS="${B_CMAKE_FLAGS:-} -DCMAKE_OSX_ARCHITECTURES=arm64"

./clean_build.sh
