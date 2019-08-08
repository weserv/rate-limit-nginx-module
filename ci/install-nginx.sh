#!/usr/bin/env bash

# Define default arguments
use_cache=true
configure_args=()

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-cache) use_cache=false ;;
        --) shift; configure_args+=("$@"); break ;;
        *) configure_args+=("$1") ;;
    esac
    shift
done

# Define variables
version=${NGINX_VERSION}
nginx_tarball=https://nginx.org/download/nginx-${version}.tar.gz

# Exit immediately if a command exits with a non-zero status
set -e

# Do we already have the correct nginx built?
if [[ -d "$HOME/nginx/sbin" ]]; then
    installed_version=$($HOME/nginx/sbin/nginx -v 2>&1 | awk -F/ '{print $2}')
    echo "Need nginx $version"
    echo "Found nginx $installed_version"

    if [[ "$installed_version" == "$version" ]] && [[ "$use_cache" = true ]]; then
        echo "Using cached nginx directory"
        exit 0
    fi
fi

# Make sure the nginx folder exist
if [[ ! -d "$HOME/nginx" ]]; then
    mkdir "$HOME/nginx"
fi

echo "Installing nginx $version"

curl -L ${nginx_tarball} | tar xz
cd nginx-${version}
./configure --prefix="$HOME/nginx" "${configure_args[@]}"
make -j${JOBS} && make install

# Clean-up build directory
cd ../
rm -rf nginx-${version}
