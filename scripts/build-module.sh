#!/usr/bin/env bash
# Build Chord Flow module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"
MODULE_ID="chord-flow"

# Prefer local cross toolchain when available.
if [ -z "$CROSS_PREFIX" ] && command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    CROSS_PREFIX="aarch64-linux-gnu-"
fi

# Fall back to Docker only when no explicit/local cross prefix is available.
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "Error: docker not found and no local cross-compiler detected."
        echo "Install docker, or install aarch64-linux-gnu-gcc and retry."
        exit 1
    fi

    MOVE_ANYTHING_SRC_HOST="${MOVE_ANYTHING_SRC:-$REPO_ROOT/../move-anything/src}"
    if [ ! -d "$MOVE_ANYTHING_SRC_HOST/host" ]; then
        echo "Error: host headers not found at: $MOVE_ANYTHING_SRC_HOST/host"
        echo "Set MOVE_ANYTHING_SRC to your move-anything src directory."
        exit 1
    fi

    echo "=== Chord Flow Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -v "$MOVE_ANYTHING_SRC_HOST:/move-anything-src:ro" \
        -e MOVE_ANYTHING_SRC=/move-anything-src \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build-module.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Chord Flow Module ==="
echo "Cross prefix: $CROSS_PREFIX"

MOVE_ANYTHING_SRC="${MOVE_ANYTHING_SRC:-$REPO_ROOT/../move-anything/src}"
if [ ! -d "$MOVE_ANYTHING_SRC/host" ]; then
    echo "Error: host headers not found at: $MOVE_ANYTHING_SRC/host"
    echo "Set MOVE_ANYTHING_SRC to your move-anything src directory."
    exit 1
fi

# Create build directories
mkdir -p build
rm -rf "dist/$MODULE_ID"
mkdir -p "dist/$MODULE_ID/presets"

# Compile and link shared library
echo "Linking dsp.so..."
${CROSS_PREFIX}gcc -g -O3 -fPIC -shared \
    -I src \
    -I src/dsp \
    -I "$MOVE_ANYTHING_SRC" \
    src/dsp/chord_flow_plugin.c \
    -o build/dsp.so \
    -lm

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > "dist/$MODULE_ID/module.json"
cat src/ui.js > "dist/$MODULE_ID/ui.js"
for preset_file in src/presets/*.json; do
    [ -f "$preset_file" ] || continue
    preset_name="$(basename "$preset_file")"
    cat "$preset_file" > "dist/$MODULE_ID/presets/$preset_name"
done
if [ -f "src/help.json" ]; then
    cat src/help.json > "dist/$MODULE_ID/help.json"
fi
cat build/dsp.so > "dist/$MODULE_ID/dsp.so"
chmod +x "dist/$MODULE_ID/dsp.so"

# Create tarball for release
cd dist
tar -czvf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
echo "Tarball: dist/$MODULE_ID-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
