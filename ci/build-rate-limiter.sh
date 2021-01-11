#!/usr/bin/env bash

# Define variables
# TODO(kleisauke): Wait for https://github.com/onsigntv/redis-rate-limiter/pull/4
branch=time-improvements
rate_limiter_repo=https://github.com/kleisauke/redis-rate-limiter.git

# Exit immediately if a command exits with a non-zero status
set -e

echo "Building redis-rate-limiter from source"

git clone -b $branch --single-branch $rate_limiter_repo rate-limiter
cd rate-limiter
make -j$(nproc) USE_MONOTONIC_CLOCK=1
