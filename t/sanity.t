#!/usr/bin/env perl

use Test::Nginx::Socket;

plan tests => repeat_each() * (blocks() * 10 - 2);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};

       # a pool with at most 1024 connections
       keepalive 1024;
    }

    error_log logs/error.log debug;
};

no_long_string();
#no_diff();

run_tests();

__DATA__

=== TEST 1: headers
--- http_config eval: $::HttpConfig
--- config
    location /quota {
        rate_limit $remote_addr requests=700 period=3m burst=699;
        rate_limit_quantity 0;
        rate_limit_pass redis;
        rate_limit_headers on;

        error_page 404 =200 @quota;
    }

    location @quota {
        default_type application/json;
        return 200 '{"X-RateLimit-Limit":$sent_http_x_ratelimit_limit, "X-RateLimit-Remaining":$sent_http_x_ratelimit_remaining, "X-RateLimit-Reset":$sent_http_x_ratelimit_reset}';
    }
--- request
    GET /quota
--- response_headers
X-RateLimit-Limit: 700
X-RateLimit-Remaining: 700
X-RateLimit-Reset: 0
!Retry-After
--- response_body: {"X-RateLimit-Limit":700, "X-RateLimit-Remaining":700, "X-RateLimit-Reset":0}

=== TEST 2: too many requests
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        rate_limit $remote_addr requests=4 period=5s burst=3;
        rate_limit_prefix a;
        rate_limit_pass redis;
        rate_limit_headers on;

        error_page 404 =200 @hit;
    }

    location @hit {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request eval
["GET /hit", "GET /hit", "GET /hit", "GET /hit", "GET /hit"]
--- response_headers eval
["X-RateLimit-Remaining: 3", "X-RateLimit-Remaining: 2", "X-RateLimit-Remaining: 1", "X-RateLimit-Remaining: 0", "Retry-After: 1"]
--- response_body_like eval
["200 OK", "200 OK", "200 OK", "200 OK", "429 Too Many Requests"]
--- error_code eval
[200, 200, 200, 200, 429]

=== TEST 3: configurable quantity
--- http_config eval: $::HttpConfig
--- config
    location /hit {
        rate_limit $remote_addr requests=4 period=5s burst=3;
        rate_limit_prefix b;
        rate_limit_quantity 5;
        rate_limit_pass redis;
        rate_limit_headers on;

        error_page 404 =200 @hit;
    }

    location @hit {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /hit
--- response_headers
X-RateLimit-Limit: 4
X-RateLimit-Remaining: 4
X-RateLimit-Reset: 0
!Retry-After
--- response_body_like: 429 Too Many Requests
--- error_code: 429
--- error_log: rate limit exceeded for key "b_127.0.0.1"
