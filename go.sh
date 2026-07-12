#!/usr/bin/env bash

DEBUG=0

do_help()
{
    echo "Usage: go.sh [--debug] <command>"
    echo ""
    echo "Commands:"
    echo "  build   Compile apexswitcher"
    echo "  prep    Install build dependencies via apt"
    echo "  help    Show this help text"
    echo ""
    echo "Options:"
    echo "  --debug   Build with debug symbols and no optimisation (-g -O0)"
}

do_prep()
{
    sudo apt install libx11-dev libxft-dev libxinerama-dev build-essential libfreetype-dev
}

do_build()
{
    if [ "$DEBUG" -eq 1 ] ; then
        BUILD_FLAGS="-g -O0"
    else
        BUILD_FLAGS="-O2"
    fi

    gcc $BUILD_FLAGS -o apexswitcher apexswitcher.c -I/usr/include/freetype2 -I/usr/include/libpng16 -lX11 -lXft -lXinerama
}

# Parse arguments.
CMD=""
for arg in "$@" ; do
    case "$arg" in
        --debug) DEBUG=1 ;;
        build)   CMD="build" ;;
        prep)    CMD="prep" ;;
        help)    CMD="help" ;;
        *)
            echo "Unknown argument: $arg"
            echo "Run 'go.sh help' for usage."
            exit 1
        ;;
    esac
done

if [ -z "$CMD" ] ; then
    do_help
    exit 0
fi

case "$CMD" in
    build) do_build ;;
    prep)  do_prep  ;;
    help)  do_help  ;;
esac
