#!/bin/bash
# Uninstall old version, rebuild, and install new one
set -e

cd "$(dirname "$0")"

echo "=== Removing old version ==="
sudo dpkg -r predatortune 2>/dev/null || true

echo "=== Cleaning build artifacts ==="
make clean

echo "=== Building new .deb ==="
make deb

echo "=== Installing ==="
sudo dpkg -i predatortune_*_all.deb

echo ""
echo "=== Done! Launch with: predatortune ==="
