#!/usr/bin/env bash

# Define variables
# TODO(kleisauke): Wait for https://github.com/onsigntv/redis-rate-limiter/pull/4
branch=time-improvements
rate_limiter_repo=https://github.com/kleisauke/redis-rate-limiter.git

# Exit immediately if a command exits with a non-zero status
set -e

echo "Installing redis-rate-limiter from source"

git clone -b ${branch} --single-branch ${rate_limiter_repo} "$HOME/rate-limiter"
cd "$HOME/rate-limiter"
make -j${JOBS}

sudo cp ratelimit.so /etc/redis
