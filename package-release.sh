#!/usr/bin/env bash

set -e
shopt -s extglob

if [ -z "$1" ] || [ -z "$2" ]; then
  echo "Usage: $0 version destdir [--no-package] [--dev-build]"
  exit 1
fi

DXVK_VERSION="$1"
DXVK_SRC_DIR="$(dirname "$(readlink -f "$0")")"
DXVK_BUILD_DIR="$(realpath "$2")/dxvk-$DXVK_VERSION"
DXVK_ARCHIVE_PATH="$(realpath "$2")/dxvk-$DXVK_VERSION.tar.gz"

if [ -e "$DXVK_BUILD_DIR" ]; then
  echo "Build directory $DXVK_BUILD_DIR already exists"
  exit 1
fi

shift 2

opt_nopackage=0
opt_devbuild=0
opt_buildid=false

crossfile="build-win"

while [ $# -gt 0 ]; do
  case "$1" in
  "--no-package")
    opt_nopackage=1
    ;;
  "--dev-build")
    opt_nopackage=1
    opt_devbuild=1
    ;;
  "--build-id")
    opt_buildid=true
    ;;
  *)
    echo "Unrecognized option: $1" >&2
    exit 1
  esac
  shift
done

function build_arch {
  export WINEARCH="win$1"
  export WINEPREFIX="$DXVK_BUILD_DIR/wine.$1"

  cd "$DXVK_SRC_DIR"

  opt_strip=
  if [ $opt_devbuild -eq 0 ]; then
    opt_strip=--strip
  fi

  meson setup --cross-file "$DXVK_SRC_DIR/$crossfile$1.txt" \
        --buildtype "release"                               \
        --prefix "$DXVK_BUILD_DIR"                          \
        $opt_strip                                          \
        --bindir "x$1"                                      \
        --libdir "x$1"                                      \
        -Dbuild_id=$opt_buildid                             \
        "$DXVK_BUILD_DIR/build.$1"

  cd "$DXVK_BUILD_DIR/build.$1"
  ninja install

  if [ $opt_devbuild -eq 0 ]; then
    # Ensure DLLs are present before packaging
    if ls "$DXVK_BUILD_DIR/x$1/"*.dll 1> /dev/null 2>&1; then
      echo "DLLs found in x$1 directory."
    else
      echo "Warning: No DLLs found in x$1 directory!"
    fi

    # Remove unnecessary .a files
    rm -f "$DXVK_BUILD_DIR/x$1/"*.!(dll)
    rm -rf "$DXVK_BUILD_DIR/build.$1"
  fi
}

function package {
  cd "$(dirname "$DXVK_BUILD_DIR")"

  # Ensure DLLs exist before packaging
  if find "$DXVK_BUILD_DIR" -name "*.dll" | grep -q .; then
    echo "Packaging DXVK build..."
    tar -czf "$DXVK_ARCHIVE_PATH" "$(basename "$DXVK_BUILD_DIR")"
    echo "Package created at: $DXVK_ARCHIVE_PATH"
  else
    echo "Error: No DLL files found! Packaging aborted."
    exit 1
  fi

  # Clean up after packaging
  rm -rf "$DXVK_BUILD_DIR"
}

build_arch 64
build_arch 32

if [ $opt_nopackage -eq 0 ]; then
  package
fi
