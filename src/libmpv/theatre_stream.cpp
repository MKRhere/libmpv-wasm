#include "theatre_stream.h"

#include <mpv/stream_cb.h>

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static std::string g_base_url;

struct TheatreStream {
	int id;
	std::string url;
	int64_t pos = 0;
	int64_t size = -1;
};

static int next_stream_id = 1;

static constexpr size_t THEATRE_REGION_SIZE = 2112 + 262144;

uintptr_t theatre_fetch_region_ptr() {
	static uint8_t *region = nullptr;
	if (!region) {
		region = (uint8_t *)malloc(THEATRE_REGION_SIZE);
		if (!region)
			return 0;
		memset(region, 0, THEATRE_REGION_SIZE);
	}
	return (uintptr_t)region;
}

EM_JS(void, theatre_fetch_setup_js, (), {
	if (Module.__theatreFetchReady) return;
	Module.__theatreFetchReady = true;

	const URL_OFFSET = 64;
	const URL_MAX = 2048;
	const DATA_OFFSET = 2112;
	const MAX_READ = 262144;
	const STATUS_WAITING = 1;
	const STATUS_ERROR = 3;
	const STATUS_IDLE = 0;

	Module.__theatreEnsureViews = function () {
		if (!Module.theatreFetchRegionPtr) {
			console.error('[theatre] theatreFetchRegionPtr export missing');
			return false;
		}
		const ptr = Module.theatreFetchRegionPtr();
		if (!ptr) {
			console.error('[theatre] fetch region not allocated');
			return false;
		}
		if (Module.__theatreFetchViews) return true;

		const buf = HEAP8.buffer;
		Module.__theatreFetchViews = {
			control: new Int32Array(buf, ptr, 4),
			posSlot: new Float64Array(buf, ptr + 16, 1),
			sizeSlot: new Float64Array(buf, ptr + 24, 1),
			urlBuf: new Uint8Array(buf, ptr + URL_OFFSET, URL_MAX),
			data: new Uint8Array(buf, ptr + DATA_OFFSET, MAX_READ),
		};
		return true;
	};

	Module.__theatreWriteUrl = function (url) {
		const buf = Module.__theatreFetchViews.urlBuf;
		buf.fill(0);
		for (let i = 0; i < url.length && i < URL_MAX; i++) {
			buf[i] = url.charCodeAt(i) & 0xff;
		}
	};

	Module.__theatreFetchBegin = function () {
		Atomics.store(Module.__theatreFetchViews.control, 0, STATUS_WAITING);
	};

	Module.__theatreFetchWait = function () {
		const control = Module.__theatreFetchViews.control;
		while (Atomics.load(control, 0) === STATUS_WAITING) {
			Atomics.wait(control, 0, STATUS_WAITING, 100);
		}
		return Atomics.load(control, 0) === STATUS_ERROR ? -1 : 0;
	};

	Module.__theatreFetchIdle = function () {
		Atomics.store(Module.__theatreFetchViews.control, 0, STATUS_IDLE);
		Atomics.store(Module.__theatreFetchViews.control, 1, 0);
	};

	Module.__theatreSubmitOp = function (op) {
		if (!Module.__theatreEnsureViews()) return false;
		Module.__theatreFetchBegin();
		Atomics.store(Module.__theatreFetchViews.control, 1, op);
		return true;
	};
});

EM_JS(double, theatre_js_probe_size, (const char *url_ptr), {
	if (!Module.__theatreSubmitOp(1)) return -1;
	Module.__theatreWriteUrl(UTF8ToString(url_ptr));
	if (Module.__theatreFetchWait() < 0) return -1;
	const size = Module.__theatreFetchViews.sizeSlot[0];
	Module.__theatreFetchIdle();
	return size;
});

EM_JS(void, theatre_js_register_stream, (int id, const char *url_ptr, double size), {
	if (!Module.__theatreSubmitOp(2)) return;
	Module.__theatreWriteUrl(UTF8ToString(url_ptr));
	Atomics.store(Module.__theatreFetchViews.control, 2, id);
	Module.__theatreFetchViews.sizeSlot[0] = size;
	Module.__theatreFetchWait();
	Module.__theatreFetchIdle();
});

EM_JS(void, theatre_js_unregister_stream, (int id), {
	if (!Module.__theatreSubmitOp(4)) return;
	Atomics.store(Module.__theatreFetchViews.control, 2, id);
	Module.__theatreFetchWait();
	Module.__theatreFetchIdle();
});

EM_JS(int, theatre_js_read, (int id, char *buf, uint32_t count, double pos), {
	if (!Module.__theatreSubmitOp(3)) return -1;
	Atomics.store(Module.__theatreFetchViews.control, 2, id);
	Atomics.store(Module.__theatreFetchViews.control, 3, count);
	Module.__theatreFetchViews.posSlot[0] = pos;
	if (Module.__theatreFetchWait() < 0) return -1;

	const n = Atomics.load(Module.__theatreFetchViews.control, 2);
	if (n <= 0) {
		Module.__theatreFetchIdle();
		return n;
	}

	HEAPU8.set(Module.__theatreFetchViews.data.subarray(0, n), buf);
	Module.__theatreFetchIdle();
	return n;
});

static std::string theatre_path_to_url(const char *uri) {
	const char *prefix = "theatre://";
	if (strncmp(uri, prefix, strlen(prefix)) != 0)
		return "";

	std::string path = uri + strlen(prefix);
	if (path.empty() || path[0] != '/')
		path = "/" + path;

	if (g_base_url.empty())
		return path;

	std::string base = g_base_url;
	while (!base.empty() && base.back() == '/')
		base.pop_back();

	return base + path;
}

static int theatre_open_ro(void *user_data, char *uri, mpv_stream_cb_info *info) {
	theatre_fetch_setup_js();

	std::string url = theatre_path_to_url(uri);
	if (url.empty()) {
		fprintf(stderr, "theatre_stream: invalid uri %s\n", uri);
		return -1;
	}

	const double size = theatre_js_probe_size(url.c_str());
	const int id = next_stream_id++;

	theatre_js_register_stream(id, url.c_str(), size);

	TheatreStream *stream = new TheatreStream{
		.id = id,
		.url = url,
		.pos = 0,
		.size = size >= 0 ? (int64_t)size : -1,
	};

	info->cookie = stream;
	info->read_fn = [](void *cookie, char *buf, uint64_t nbytes) -> int64_t {
		TheatreStream *s = static_cast<TheatreStream *>(cookie);
		const uint32_t count =
			nbytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(nbytes);
		const int n = theatre_js_read(s->id, buf, count, (double)s->pos);
		if (n > 0)
			s->pos += n;
		return n;
	};
	info->seek_fn = [](void *cookie, int64_t offset) -> int64_t {
		TheatreStream *s = static_cast<TheatreStream *>(cookie);
		if (offset < 0)
			return -1;
		if (s->size >= 0 && offset > s->size)
			return -1;
		s->pos = offset;
		return 0;
	};
	info->size_fn = [](void *cookie) -> int64_t {
		TheatreStream *s = static_cast<TheatreStream *>(cookie);
		return s->size;
	};
	info->close_fn = [](void *cookie) -> void {
		TheatreStream *s = static_cast<TheatreStream *>(cookie);
		theatre_js_unregister_stream(s->id);
		delete s;
	};
	info->cancel_fn = nullptr;

	return 0;
}

void theatre_stream_register(mpv_handle *ctx) {
	theatre_fetch_region_ptr();
	theatre_fetch_setup_js();

	if (mpv_stream_cb_add_ro(ctx, "theatre", nullptr, theatre_open_ro) < 0)
		fprintf(stderr, "theatre_stream: failed to register theatre:// protocol\n");
}

void theatre_stream_set_base_url(const char *url) {
	g_base_url = url ? url : "";
}
