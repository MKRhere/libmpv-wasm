#pragma once

#include <mpv/client.h>

#include <cstdint>

/** Register the `theatre://` read-only stream protocol on an uninitialized
 * mpv handle (call after mpv_create, before mpv_initialize). */
void theatre_stream_register(mpv_handle *ctx);

/** Base URL for media requests, e.g. "http://localhost:3000". Paths from
 * `theatre:///path/to/file.mkv` are appended to this origin. */
void theatre_stream_set_base_url(const char *url);

/** Byte offset into wasm linear memory shared by all pthreads. */
uintptr_t theatre_fetch_region_ptr();
