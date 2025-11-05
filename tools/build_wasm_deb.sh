#!/bin/bash

# Change to project root
cd "$(dirname "$0")/.." || exit

# Support ReleaseMini, ReleaseAll, ReleaseExporter, all, clean, or individual clean options
BUILD_TYPE=${1:-ReleaseMini}

# Validate build type
if [[ "$BUILD_TYPE" != "ReleaseMini" && "$BUILD_TYPE" != "ReleaseAll" && "$BUILD_TYPE" != "ReleaseExporter" && "$BUILD_TYPE" != "all" && "$BUILD_TYPE" != "clean" && "$BUILD_TYPE" != "clean-mini" && "$BUILD_TYPE" != "clean-all" && "$BUILD_TYPE" != "clean-exporter" ]]; then
    echo "Error: Only ReleaseMini, ReleaseAll, ReleaseExporter, all, clean, clean-mini, clean-all, or clean-exporter are supported"
    echo "Usage: $0 [ReleaseMini|ReleaseAll|ReleaseExporter|all|clean|clean-mini|clean-all|clean-exporter]"
    exit 1
fi

# Handle clean options
if [[ "$BUILD_TYPE" == "clean" ]]; then
    echo "Cleaning all build caches..."
    rm -rf build_wasm_mini build_wasm_all build_wasm_exporter
    echo "All build caches cleaned!"
    exit 0
elif [[ "$BUILD_TYPE" == "clean-mini" ]]; then
    echo "Cleaning mini build cache..."
    rm -rf build_wasm_mini
    echo "Mini build cache cleaned!"
    exit 0
elif [[ "$BUILD_TYPE" == "clean-all" ]]; then
    echo "Cleaning all-importers build cache..."
    rm -rf build_wasm_all
    echo "All-importers build cache cleaned!"
    exit 0
elif [[ "$BUILD_TYPE" == "clean-exporter" ]]; then
    echo "Cleaning exporter build cache..."
    rm -rf build_wasm_exporter
    echo "Exporter build cache cleaned!"
    exit 0
fi

echo "Building AssimpJS for $BUILD_TYPE..."

# Source Emscripten environment
source emsdk/emsdk_env.sh

# Get number of processors for parallel builds
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

if [[ "$BUILD_TYPE" == "all" ]]; then
    echo "Building all AssimpJS variants..."
    
    # Build mini version
    echo "Building mini-importers version..."
    emcmake cmake -B build_wasm_mini -G "Unix Makefiles" -DEMSCRIPTEN=1 -DCMAKE_BUILD_TYPE=ReleaseMini . || exit 1
    emmake make -C build_wasm_mini -j"$NPROC" AssimpJS || exit 1

    # Build all-importers version
    echo "Building all-importers version..."
    emcmake cmake -B build_wasm_all -G "Unix Makefiles" -DEMSCRIPTEN=1 -DCMAKE_BUILD_TYPE=ReleaseAll . || exit 1
    emmake make -C build_wasm_all -j"$NPROC" AssimpJS || exit 1

    # Build exporter version
    echo "Building exporter version..."
    emcmake cmake -B build_wasm_exporter -G "Unix Makefiles" -DEMSCRIPTEN=1 -DCMAKE_BUILD_TYPE=ReleaseExporter . || exit 1
    emmake make -C build_wasm_exporter -j"$NPROC" AssimpJS || exit 1

    echo "Running tests..."
    npm run test || exit 1

    # Copy all artifacts using pattern matching
    echo "Creating distribution..."
    mkdir -p dist docs/dist
    cp build_wasm_mini/ReleaseMini/assimpjs-mini.* dist/ 2>/dev/null || true
    cp build_wasm_all/ReleaseAll/assimpjs-all.* dist/ 2>/dev/null || true
    cp build_wasm_exporter/ReleaseExporter/assimpjs-exporter.* dist/ 2>/dev/null || true
else
    # Map build type to build directory
    case "$BUILD_TYPE" in
        "ReleaseMini")
            BUILD_DIR="build_wasm_mini"
            ;;
        "ReleaseAll")
            BUILD_DIR="build_wasm_all"
            ;;
        "ReleaseExporter")
            BUILD_DIR="build_wasm_exporter"
            ;;
    esac
    
    echo "Building single AssimpJS target ($BUILD_TYPE)..."
    emcmake cmake -B "$BUILD_DIR" -G "Unix Makefiles" -DEMSCRIPTEN=1 -DCMAKE_BUILD_TYPE="$BUILD_TYPE" . || exit 1
    emmake make -C "$BUILD_DIR" -j"$NPROC" AssimpJS || exit 1

    echo "Running tests..."
    npm run test || exit 1

    # Copy artifacts using pattern matching
    echo "Creating distribution..."
    mkdir -p dist docs/dist
    cp "$BUILD_DIR"/"$BUILD_TYPE"/assimpjs*.* dist/ 2>/dev/null || true
fi

# Copy to docs distribution and license files
cp dist/assimpjs*.* docs/dist/ 2>/dev/null || true
cp assimp/LICENSE dist/license.assimp.txt 2>/dev/null || true
cp LICENSE.md dist/license.assimpjs.txt 2>/dev/null || true
cp dist/license*.txt docs/dist/ 2>/dev/null || true

echo "Build completed!"

# Print size summary
if ls dist/assimpjs*.* >/dev/null 2>&1; then
    echo ""
    echo "Build Size Summary:"
    ls -lh dist/assimpjs*.* | awk '{printf "  %-30s %s\n", $9, $5}'
fi
