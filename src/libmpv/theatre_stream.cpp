#include "theatre_stream.h"

#include <mpv/stream_cb.h>

#include <emscripten.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string g_base_url;

struct TheatreStream {
	int id;
	std::string url;
	int64_t pos = 0;
	int64_t size = -1;
};

static int next_stream_id = 1;

EM_JS(void, theatre_js_init, (), {
	if (!Module.theatreStreams) Module.theatreStreams = new Map();
});

EM_JS(void, theatre_js_register_stream, (int id, const char *url_ptr, double size), {
	Module.theatreStreams.set(id, {
		url: UTF8ToString(url_ptr),
		pos: 0,
		size: size,
	});
});

EM_JS(void, theatre_js_seek, (int id, double pos), {
	const s = Module.theatreStreams.get(id);
	if (s) s.pos = pos;
});

EM_JS(void, theatre_js_unregister_stream, (int id), {
	Module.theatreStreams.delete(id);
});

/** Probe Content-Length via HEAD, then Content-Range via bytes=0-0. */
EM_JS(double, theatre_js_probe_size, (const char *url_ptr), {
	const url = UTF8ToString(url_ptr);

	function sizeFromContentRange(header) {
		if (!header) return -1;
		const slash = header.lastIndexOf('/');
		if (slash < 0) return -1;
		const n = parseInt(header.slice(slash + 1), 10);
		return isNaN(n) ? -1 : n;
	}

	try {
		let xhr = new XMLHttpRequest();
		xhr.open('HEAD', url, false);
		xhr.send();
		if (xhr.status >= 200 && xhr.status < 300) {
			const cl = xhr.getResponseHeader('Content-Length');
			if (cl) return parseInt(cl, 10);
		}
	} catch (e) {}

	try {
		const xhr = new XMLHttpRequest();
		xhr.open('GET', url, false);
		xhr.setRequestHeader('Range', 'bytes=0-0');
		xhr.send();
		if (xhr.status === 206 || xhr.status === 200) {
			const fromRange = sizeFromContentRange(xhr.getResponseHeader('Content-Range'));
			if (fromRange >= 0) return fromRange;
			const cl = xhr.getResponseHeader('Content-Length');
			if (cl) return parseInt(cl, 10);
		}
	} catch (e) {}

	return -1;
});

EM_JS(int64_t, theatre_js_read, (int id, char *buf, uint64_t nbytes), {
	const s = Module.theatreStreams.get(id);
	if (!s) return -1;

	const count = Number(nbytes);
	let end = s.pos + count - 1;
	if (s.size >= 0) end = Math.min(end, s.size - 1);
	if (s.size >= 0 && s.pos >= s.size) return 0;

	const range = s.size >= 0 && end >= s.pos
		? `bytes=${s.pos}-${end}`
		: `bytes=${s.pos}-`;

	try {
		const xhr = new XMLHttpRequest();
		xhr.open('GET', s.url, false);
		xhr.setRequestHeader('Range', range);
		xhr.responseType = 'arraybuffer';
		xhr.send();

		if (xhr.status !== 206 && xhr.status !== 200) return -1;

		const ab = xhr.response;
		if (!ab) return -1;

		const bytes = new Uint8Array(ab);
		const n = Math.min(bytes.length, count);
		if (n <= 0) return 0;

		HEAPU8.set(bytes.subarray(0, n), buf);
		s.pos += n;
		return n;
	} catch (e) {
		return -1;
	}
});

static std::string theatre_path_to_url(const char *uri) {
	// mpv passes e.g. "theatre:///movies/foo.mkv"
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
	theatre_js_init();

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
		return theatre_js_read(s->id, buf, nbytes);
	};
	info->seek_fn = [](void *cookie, int64_t offset) -> int64_t {
		TheatreStream *s = static_cast<TheatreStream *>(cookie);
		if (offset < 0)
			return -1;
		if (s->size >= 0 && offset > s->size)
			return -1;
		s->pos = offset;
		theatre_js_seek(s->id, (double)offset);
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
	theatre_js_init();

	if (mpv_stream_cb_add_ro(ctx, "theatre", nullptr, theatre_open_ro) < 0)
		fprintf(stderr, "theatre_stream: failed to register theatre:// protocol\n");
}

void theatre_stream_set_base_url(const char *url) {
	g_base_url = url ? url : "";
}
