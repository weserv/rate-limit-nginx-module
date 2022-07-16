#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

# Define variables
branch=master
rate_limiter_repo=https://github.com/onsigntv/redis-rate-limiter.git

echo "Building redis-rate-limiter from source"

git clone -b $branch --single-branch $rate_limiter_repo rate-limiter
cd rate-limiter
make -j$(nproc) USE_MONOTONIC_CLOCK=1
