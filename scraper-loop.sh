#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="/home/ubuntu/web-scraper"
cd "$REPO_DIR"

while true; do
    echo "=== [$(date -u '+%Y-%m-%dT%H:%M:%SZ')] Starting cycle ==="

    # Pull latest changes
    echo "Pulling latest..."
    git pull --rebase origin main || git pull origin main

    # Compile
    echo "Compiling scraper.cpp..."
    g++ -O2 -o scraper scraper.cpp -lcurl -lzip -lrocksdb
    echo "Compilation succeeded."

    # Run
    echo "Running scraper..."
    ./scraper --resume 100000000 --workers 10
    echo "Scraper finished."

    # Push results
    echo "Pushing changes..."
    git add -A
    git diff --cached --quiet && echo "Nothing to commit." || {
        git commit -m "Auto-update: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        git push origin main
        echo "Pushed successfully."
    }

    echo "=== Cycle complete ==="
done
