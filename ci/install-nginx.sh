#!/usr/bin/env bash

# Define variables
version=$NGINX_VERSION
nginx_tarball=https://nginx.org/download/nginx-$version.tar.gz

# Exit immediately if a command exits with a non-zero status
set -e

# Make sure the nginx folder exist
mkdir -p "$HOME/nginx"

echo "Installing nginx $version"

curl -Ls $nginx_tarball | tar xz
cd nginx-$version
./configure --prefix="$HOME/nginx" "$@"
make -j$(nproc) && make install

# Clean-up build directory
cd ../
rm -rf nginx-$version
