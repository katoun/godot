#!/usr/bin/env bash

set -e

JOBS=28
NICE_LEVEL=5
PLATFORM=linuxbsd
ARCH=x86_64
COMPILE_ARGS=(use_llvm=yes linker=mold)

TARGET="${1:-editor}"

build_godot() {
    local target="$1"
    shift
    
    echo "Building Godot target: $target"
    
    nice -n "$NICE_LEVEL" scons -j"$JOBS" platform="$PLATFORM" target="$target" arch="$ARCH" "${COMPILE_ARGS[@]}" "$@"
}

build_editor() {
    build_godot editor
}

build_templates() {
    build_godot template_debug
    build_godot template_release
}

build_tests() {
    build_godot editor tests=yes
}

case "$TARGET" in
    editor)
        build_editor
        ;;

    templates)
        build_templates
        ;;

    tests)
        build_tests
        ;;

    all)
        build_editor
        build_templates
        ;;

    *)
        echo "Unknown build target: $TARGET"
        echo "Usage: $0 [editor|templates|tests|all]"
        exit 1
        ;;
esac
