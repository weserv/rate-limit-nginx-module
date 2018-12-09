# TODO

* The `ngx_http_rate_limit_handler` doesn't work currently (perhaps the upstream finalizes the request (?)).
* The `X-RateLimit-Reset` needs to be an [epoch](https://en.wikipedia.org/wiki/Unix_time) instead than seconds.
  * Should we do the same for the `Retry-After` header?
* Add unit tests / Travis integration.
* Stress testing with [ab](https://httpd.apache.org/docs/2.4/programs/ab.html) and [wrk](https://github.com/wg/wrk).
