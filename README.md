# rate-limit-nginx-module

[![CI status](https://github.com/weserv/rate-limit-nginx-module/workflows/CI/badge.svg)](https://github.com/weserv/rate-limit-nginx-module/actions)

A Redis backed rate limit module for Nginx web servers.

This implementation is based on the following [Redis module](https://redis.io/topics/modules-intro):

* [redis-rate-limiter](https://github.com/onsigntv/redis-rate-limiter)

Which offers a straightforward implementation of the fairly sophisticated [generic cell rate algorithm](https://en.wikipedia.org/wiki/Generic_cell_rate_algorithm), in 130 lines of C, without external dependencies.
 
*This module is not distributed with the Nginx source.*

## Status

This module is production ready.

## Synopsis

```nginx
upstream redis {
   server 127.0.0.1:6379;

   # Or: server unix:/var/run/redis/redis.sock;

   # a pool with at most 1024 connections
   keepalive 1024;
}

geo $limit {
    default 1;
    10.0.0.0/8 0;
    192.168.0.0/24 0;
}

map $limit $limit_key {
    0 "";
    1 $remote_addr;
}

rate_limit_status 429;

location = /limit {
    rate_limit $limit_key requests=15 period=1m burst=20;
    rate_limit_pass redis;
}

location = /limit_b {
    rate_limit $limit_key requests=20 period=1m burst=25;
    rate_limit_prefix b;
    rate_limit_pass redis;
}

location = /quota {
    rate_limit $limit_key requests=15 period=1m burst=20;
    rate_limit_quantity 0;
    rate_limit_pass redis;
    rate_limit_headers on;
}
```

## Installation

*Note: You will need to install the Redis module first, see the install instructions [here](https://github.com/onsigntv/redis-rate-limiter#install).*

You can install this module manually by recompiling the standard Nginx core as follows:

1. Grab the nginx source code from [nginx.org](http://nginx.org) (this module is tested on version 1.19.6).
2. Clone this repository into a newly created directory (for e.g. `./rate-limit-nginx-module`)
3. Build the nginx source with this module:
```bash
wget https://nginx.org/download/nginx-1.19.6.tar.gz
tar -xzvf nginx-1.19.6.tar.gz
cd nginx-1.19.6/

git clone https://github.com/weserv/rate-limit-nginx-module rate-limit-nginx-module

# Here we assume you would install you nginx under /opt/nginx/.
./configure --prefix=/opt/nginx \
            --add-module=rate-limit-nginx-module/

make -j$(nproc)
make install
```

## Test suite

The following dependencies are required to run the test suite:

* Nginx version >= 1.9.11

* Perl modules:
    * [Test::Nginx](https://metacpan.org/pod/Test::Nginx::Socket)

* Nginx modules:
	* ngx_http_rate_limit_module (i.e., this module)

* Redis modules:
    * [redis-rate-limiter](https://github.com/onsigntv/redis-rate-limiter)

* Applications:
	* redis: listening on the default port, 6379.

To run the whole test suite in the default testing mode:
```bash
cd /path/to/rate-limit-nginx-module
export PATH=/path/to/your/nginx/sbin:$PATH
prove -I/path/to/test-nginx/lib -r t
```

To run specific test files:
```bash
cd /path/to/rate-limit-nginx-module
export PATH=/path/to/your/nginx/sbin:$PATH
prove -I/path/to/test-nginx/lib t/sanity.t
```

To run a specific test block in a particular test file, add the line 
`--- ONLY` to the test block you want to run, and then use the `prove` 
utility to run that `.t` file.
