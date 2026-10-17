# http.c

Enough HTTP/1.1 for a local JSON service, and no more.
- **Server side:** reads one request (Content-Length or chunked, 64 KB of
  headers, 64 MB of body) and answers `Expect: 100-continue`. Responses
  are JSON with `Connection: close`.
- **Client side:** one call to `host:port` with a timeout. The engine's
  `serve --detach` uses it to wait for `/health`, `query` to reach a
  running server, and the learner to ask the engine.
