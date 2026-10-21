# Game asynchronous logging

Game and the headless runtime tests use one owned spdlog worker shared by the `HEngine` and `Python` loggers. Console/file writes run on that worker; message formatting and enqueueing still run on the caller. Editor retains its synchronous logger because its custom UI sink accesses editor-owned state.

The queue holds at most 8192 records. Saturation uses explicit blocking backpressure, never overwrite/discard. This bounds queued record count, not arbitrary message byte size; it also means full queues can still stall a caller. This is not a guarantee against every frame-time spike.

Each logger has a separate `dist_sink_mt`, sharing console/file sinks but retaining its own Agent diagnostic sink. `AddLogSink` registers runtime sinks through these synchronized routes, so it never mutates a sink vector being traversed by the worker; engine/client event sources are not duplicated or mixed. One worker preserves enqueue order across both loggers. Sink errors go directly to `Log::WriteFallbackError` on stderr without recursive queueing.

`LogSession` lives outside the `main` try/catch in Launch and TestMain. It runs after application/script cleanup and after exception diagnostics. `ShutdownLogSession` enqueues flushes, then destroys/joins the owned pool: FIFO terminate follows the flush records, so returning from drain proves they completed. Remaining static-destruction diagnostics use synchronous loggers. Callers must stop producers before this boundary. Forced process termination cannot guarantee queued logs are retained; fatal catch paths retain the independent synchronous stderr fallback.

`hectl test AsyncLogging` uses a gated sink, condition-variable/future handshakes, bounded waits, and an RAII release/join guard. It checks off-thread dispatch while a sink is held, the two-record test capacity, overflow backpressure, cross-logger order with no dropped records, and flush/drain completion. No GPU is required.

The performance investigation measured synchronous state logging at about 3–4 ms, maximum 8.814 ms in the instrumented run `out/agent/aster-benchmark/20260922-014215`. That run did not reproduce the earlier 500 ms spike and does not establish logging as its cause. The retained timing samples must be compared with a subsequent run before claiming an asynchronous logging performance result.
