#!/usr/bin/env bash

set -e

JOBS=28
NICE_LEVEL=5
PLATFORM=linuxbsd
ARCH=x86_64

TARGET="${1:-editor}"

build_editor() {
    echo "Building Godot editor..."
    nice -n "$NICE_LEVEL" scons -j"$JOBS" platform="$PLATFORM" target=editor arch="$ARCH"
}

build_templates() {
    echo "Building Godot debug export template..."
    nice -n "$NICE_LEVEL" scons -j"$JOBS" platform="$PLATFORM" target=template_debug arch="$ARCH"

    echo "Building Godot release export template..."
    nice -n "$NICE_LEVEL" scons -j"$JOBS" platform="$PLATFORM" target=template_release arch="$ARCH"
}

case "$TARGET" in
    editor)
        build_editor
        ;;

    templates)
        build_templates
        ;;

    all)
        build_editor
        build_templates
        ;;

    *)
        echo "Unknown build target: $TARGET"
        echo "Usage: $0 [editor|templates|all]"
        exit 1
        ;;
esac