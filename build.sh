#!/bin/bash

# Build script for Factory-HUB
# This script automates the build process

set -e  # Exit on error

echo "🚀 Building Factory-HUB..."

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "📦 Configuring CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release -Wno-dev

# Build
echo "🔨 Building project..."
cmake --build . -j$(nproc)

echo "✅ Build complete!"
echo ""
echo "To run the application:"
echo "  ./bin/Factory-HUB"
echo ""

