#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_events.h>

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <emscripten/proxying.h>

#include <filesystem>
#include <iostream>
#include <string>

#include <AL/al.h>
#include <AL/alc.h>

#include "theatre_stream.h"

using namespace emscripten;
using namespace std;

static Uint32 wakeup_on_mpv_render_update, wakeup_on_mpv_events;
int width = 1920;
int height = 1080;
int viewport_width = 0;
int viewport_height = 0;
int64_t video_width = 1920;
int64_t video_height = 1080;
SDL_Window *window;
mpv_handle *mpv;
mpv_render_context *mpv_gl;
pthread_t main_thread;
pthread_t side_thread;
em_proxying_queue* main_queue = em_proxying_queue_create();

void main_loop();
void create_mpv_map_obj(mpv_node_list *map);
int get_shader_count();
void get_tracks();
void get_chapters();
static void *get_proc_address_mpv(void *fn_ctx, const char *name);
static void on_mpv_events(void *ctx);
static void on_mpv_render_update(void *ctx);
intptr_t get_main_thread();
void die(const char *msg);
void quit();

void func() {}

void* loop(void* args) {
    emscripten_set_main_loop(func, 0, 1);
    return NULL;
}

int main(int argc, char const *argv[]) {
    main_thread = pthread_self();
    pthread_create(&side_thread, NULL, loop, NULL);

    mpv = mpv_create();
    if (!mpv) die("context init failed");

    mpv_set_property_string(mpv, "vo", "libmpv");
    theatre_stream_register(mpv);

    if (mpv_initialize(mpv) < 0)
        die("mpv init failed");

    // stream_cb sources are real streams — enable read-ahead cache so mpv
    // exposes paused-for-cache / demuxer-cache-state for buffering detection.
    mpv_set_property_string(mpv, "cache", "yes");
    mpv_set_property_string(mpv, "demuxer-max-bytes", "150MiB");
    mpv_set_property_string(mpv, "keepaspect", "yes");

    mpv_request_log_messages(mpv, "v");

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        die("SDL init failed");
    
    emscripten_get_screen_size(&width, &height);
    window = SDL_CreateWindow("mpv Media Player", width, height, SDL_WINDOW_OPENGL);

    if (!window) die("failed to create SDL window");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_GLContext glcontext = SDL_GL_CreateContext(window);
    if (!glcontext) die("failed to create SDL GL context");

    mpv_opengl_init_params init_params = { get_proc_address_mpv };
    int advanced_control = 1;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {(mpv_render_param_type) 0}
    };

    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        die("failed to initialize mpv GL context");

    wakeup_on_mpv_render_update = SDL_RegisterEvents(1);
    wakeup_on_mpv_events = SDL_RegisterEvents(1);
    if (wakeup_on_mpv_render_update == (Uint32) - 1 || wakeup_on_mpv_events == (Uint32) - 1)
        die("could not register events");

    mpv_set_wakeup_callback(mpv, on_mpv_events, NULL);
    mpv_render_context_set_update_callback(mpv_gl, on_mpv_render_update, NULL);

    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "vid", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "aid", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "sid", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "metadata/by-key/title", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 0, "playlist-current-pos", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "dheight", MPV_FORMAT_INT64);

    emscripten_set_main_loop(main_loop, 0, 1);

    return 0;
}

void main_loop() {
    SDL_Event event;
    if (SDL_WaitEvent(&event) != 1)
        die("event loop error");
    int redraw = 0;
    switch (event.type) {
        case SDL_EVENT_QUIT:
            quit();
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            redraw = 1;
            break;
        default:
            if (event.type == wakeup_on_mpv_render_update) {
                uint64_t flags = mpv_render_context_update(mpv_gl);
                if (flags & MPV_RENDER_UPDATE_FRAME)
                    redraw = 1;
            }
            if (event.type == wakeup_on_mpv_events) {
                while (1) {
                    mpv_event *mp_event = mpv_wait_event(mpv, 0);
                    if (mp_event->event_id == MPV_EVENT_NONE)
                        break;
                    switch (mp_event->event_id) {
                        case MPV_EVENT_IDLE:
                            EM_ASM({
                                postMessage(JSON.stringify({ type: 'idle', shaderCount: $0 }));
                            }, get_shader_count());
                            break;
                        case MPV_EVENT_LOG_MESSAGE: {
                            mpv_event_log_message *msg = (mpv_event_log_message*)mp_event->data;
                            EM_ASM({
                                console.warn('[mpv]', UTF8ToString($0), UTF8ToString($1));
                            }, msg->prefix, msg->text);
                            break;
                        }
                        case MPV_EVENT_FILE_LOADED:
                            get_tracks();
                            get_chapters();
                            break;
                        case MPV_EVENT_START_FILE:
                            EM_ASM(postMessage(JSON.stringify({ type: 'file-start' })););
                            break;
                        case MPV_EVENT_END_FILE: {
                            mpv_event_end_file *ev = (mpv_event_end_file *)mp_event->data;
                            EM_ASM({
                                postMessage(JSON.stringify({
                                    type: 'file-end',
                                    reason: $0,
                                    error: $1,
                                }));
                            }, (int)ev->reason, ev->error);
                            break;
                        }
                        case MPV_EVENT_GET_PROPERTY_REPLY:
                        case MPV_EVENT_PROPERTY_CHANGE: {
                            mpv_event_property *evt = (mpv_event_property*)mp_event->data;
                            
                            switch (evt->format) {
                                case MPV_FORMAT_NONE:
                                    EM_ASM({
                                        postMessage(JSON.stringify({
                                            type: 'property-change',
                                            name: UTF8ToString($0),
                                            value: 0
                                        }));
                                    }, evt->name);
                                    break;
                                case MPV_FORMAT_STRING: {
                                    const char **data = (const char **)evt->data;
                                    EM_ASM({
                                        postMessage(JSON.stringify({
                                            type: 'property-change',
                                            name: UTF8ToString($0),
                                            value: UTF8ToString($1)
                                        }));
                                    }, evt->name, *data);
                                    break;
                                }
                                case MPV_FORMAT_FLAG: {
                                    int *data = (int *)evt->data;
                                    EM_ASM({
                                        postMessage(JSON.stringify({
                                            type: 'property-change',
                                            name: UTF8ToString($0),
                                            value: $1
                                        }));
                                    }, evt->name, *data);
                                    break;
                                }
                                case MPV_FORMAT_DOUBLE: {
                                    double *data = (double *)evt->data;
                                    EM_ASM({
                                        postMessage(JSON.stringify({
                                            type: 'property-change',
                                            name: UTF8ToString($0),
                                            value: $1
                                        }));
                                    }, evt->name, *data);
                                    break;
                                }
                                case MPV_FORMAT_INT64: {
                                    int64_t *data = (int64_t *)evt->data;
                                    EM_ASM({
                                        postMessage(JSON.stringify({
                                            type: 'property-change',
                                            name: UTF8ToString($0),
                                            value: $1.toString()
                                        }));
                                    }, evt->name, *data);
                                    break;
                                }
                                case MPV_FORMAT_NODE: {
                                    mpv_node *data = (mpv_node *)evt->data;
                                    mpv_node_list *list;
                                    mpv_node_list *map;
    
                                    if (strcmp(evt->name, "track-list") != 0 && strcmp(evt->name, "chapter-list") != 0)
                                        break;

                                    list = (mpv_node_list *)data->u.list;
                                    EM_ASM(arr = [];);
                                    
                                    for (int i = 0; i < list->num; i++) {
                                        map = (mpv_node_list *)list->values[i].u.list;
                                        create_mpv_map_obj(map);
                                        EM_ASM(arr.push(obj););
                                    }
                                    if (strcmp(evt->name, "track-list") == 0) {
                                        EM_ASM({
                                            postMessage(JSON.stringify({
                                                type: 'track-list',
                                                tracks: arr
                                            }));
                                        });
                                    }
                                    if (strcmp(evt->name, "chapter-list") == 0) {
                                        EM_ASM({
                                            postMessage(JSON.stringify({
                                                type: 'chapter-list',
                                                chapters: arr
                                            }));
                                        });
                                    }
                                    break;
                                }
                                default:
                                    printf("property-change: { name: %s, format: %d }\n", evt->name, evt->format);
                            }
                            break;
                        }
                        default:
                            break;
                        //     printf("event: %s\n", mpv_event_name(mp_event->event_id));
                    }
                }
            }
    }
    if (redraw) {
        int draw_w = 0;
        int draw_h = 0;
        SDL_GetWindowSize(window, &draw_w, &draw_h);
        if (draw_w <= 0 || draw_h <= 0) {
            draw_w = width;
            draw_h = height;
        }
        mpv_opengl_fbo fbo = { 0, draw_w, draw_h };
        int flip_y = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {(mpv_render_param_type) 0}
        };
        mpv_render_context_render(mpv_gl, params);
        SDL_GL_SwapWindow(window);
    }
}

typedef struct {
    string path;
    string options;
} load_file_args_t;

void load_file_proxy(void* args) {
    load_file_args_t* load_file_args = (load_file_args_t*)args;

    filesystem::path path = load_file_args->path;
    string root_name = *next(path.begin());
    string root_path = "/" + root_name;
    
    if (!filesystem::is_directory(root_path)) {
        backend_t backend = wasmfs_create_externalfs_backend(root_name.c_str());
        int err = wasmfs_create_directory(root_path.c_str(), 0777, backend);
        if (err) {
            fprintf(stderr, "Couldn't mount directory at %s\n", root_path.c_str());
            return;
        }
    }
    
    // printf("loading %s with options %s\n", path.c_str(), load_file_args->options.c_str());
    
    if (!filesystem::exists(path)) {
        fprintf(stderr, "file does not exist\n");
        return;
    }

    if (load_file_args->options.empty()) {
        const char *cmd[] = {"loadfile", path.c_str(), "replace", NULL};
        mpv_command_async(mpv, 0, cmd);
    } else {
        const char *cmd[] = {"loadfile", path.c_str(), "replace", load_file_args->options.c_str(), NULL};
        mpv_command_async(mpv, 0, cmd);
    }
    free(args);
}

void load_file(string path, string options) {
    load_file_args_t* args_ptr = (load_file_args_t*)malloc(sizeof(load_file_args_t));
    args_ptr->path = path;
    args_ptr->options = options;
    emscripten_proxy_async(main_queue, side_thread, load_file_proxy, args_ptr);
}

typedef struct {
    string path;
    string options;
} load_stream_args_t;

void load_stream_proxy(void* args) {
    load_stream_args_t* load_args = (load_stream_args_t*)args;

    string rel = load_args->path;
    if (rel.empty() || rel[0] != '/')
        rel = "/" + rel;

    string uri = string("theatre://") + rel;
    if (load_args->options.empty()) {
        const char *cmd[] = {"loadfile", uri.c_str(), "replace", NULL};
        mpv_command_async(mpv, 0, cmd);
    } else {
        const char *cmd[] = {"loadfile", uri.c_str(), "replace", load_args->options.c_str(), NULL};
        mpv_command_async(mpv, 0, cmd);
    }
    free(args);
}

void load_stream(string path, string options) {
    load_stream_args_t* args_ptr = (load_stream_args_t*)malloc(sizeof(load_stream_args_t));
    args_ptr->path = path;
    args_ptr->options = options;
    emscripten_proxy_async(main_queue, side_thread, load_stream_proxy, args_ptr);
}

void set_stream_base_url(string base_url) {
    theatre_stream_set_base_url(base_url.c_str());
}

void load_files(vector<string> paths) {
    // printf("loading %lu paths\n", paths.size());

    for (auto path : paths) {
        if (!filesystem::exists(path))
            fprintf(stderr, "%s does not exist\n", path.c_str());

        const char * cmd[] = {"loadfile", path.c_str(), "append-play", NULL};
        mpv_command_async(mpv, 0, cmd);
    }
}

void toggle_play() {
    const char * cmd[] = {"cycle", "pause", NULL};
    mpv_command_async(mpv, 0, cmd);
}

void set_paused(bool paused) {
    int flag = paused ? 1 : 0;
    mpv_set_property_async(mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void stop() {
    const char * cmd[] = {"stop", NULL};
    mpv_command_async(mpv, 0, cmd);
}

void set_playback_time_pos(double time) {
    mpv_set_property_async(mpv, 0, "playback-time", MPV_FORMAT_DOUBLE, &time);
}

void set_ao_volume(double volume) {
    mpv_set_property_async(mpv, 0, "ao-volume", MPV_FORMAT_DOUBLE, &volume);
}

void get_tracks() {
    mpv_get_property_async(mpv, 0, "track-list", MPV_FORMAT_NODE);
}

void get_metadata() {
    mpv_get_property_async(mpv, 0, "metadata", MPV_FORMAT_NODE);
}

void get_chapters() {
    mpv_get_property_async(mpv, 0, "chapter-list", MPV_FORMAT_NODE);
}

void set_video_track(int64_t idx) {
    mpv_set_property_async(mpv, 0, "vid", MPV_FORMAT_INT64, &idx);
}

void set_audio_track(int64_t idx) {
    mpv_set_property_async(mpv, 0, "aid", MPV_FORMAT_INT64, &idx);
}

void set_subtitle_track(int64_t idx) {
    mpv_set_property_async(mpv, 0, "sid", MPV_FORMAT_INT64, &idx);
}

void set_chapter(int64_t idx) {
    mpv_set_property_async(mpv, 0, "chapter", MPV_FORMAT_INT64, &idx);
}

void skip_forward() {
    const char * cmd[] = {"seek", "10", NULL};
    mpv_command_async(mpv, 0, cmd);
}

void skip_backward() {
    const char * cmd[] = {"seek", "-10", NULL};
    mpv_command_async(mpv, 0, cmd);
}

void add_shaders() {
    const char *shader_list = "/shaders/Anime4K_Clamp_Highlights.glsl:/shaders/Anime4K_Restore_CNN_VL.glsl:/shaders/Anime4K_Upscale_CNN_x2_VL.glsl:/shaders/Anime4K_AutoDownscalePre_x2.glsl:/shaders/Anime4K_AutoDownscalePre_x4.glsl:/shaders/Anime4K_Upscale_CNN_x2_M.glsl";
    const char * cmd[] = {"change-list", "glsl-shaders", "set", shader_list, NULL};
    mpv_command_async(mpv, 0, cmd);
}

void clear_shaders() {
    const char * cmd[] = {"change-list", "glsl-shaders", "clr", "", NULL};
    mpv_command_async(mpv, 0, cmd);
}

int get_shader_count() {
    auto dirIter = std::filesystem::directory_iterator("/shaders");

    int fileCount = std::count_if(
        begin(dirIter),
        end(dirIter),
        [](auto& entry) { return entry.is_regular_file(); }
    );

    return fileCount - 1;
}

intptr_t get_main_thread() {
    return (intptr_t)main_thread;
}

static void *get_proc_address_mpv(void *fn_ctx, const char *name) {
    return (void *)SDL_GL_GetProcAddress(name);
}

static void on_mpv_events(void *ctx) {
    SDL_Event event = {.type = wakeup_on_mpv_events};
    SDL_PushEvent(&event);
}

static void on_mpv_render_update(void *ctx) {
    SDL_Event event = {.type = wakeup_on_mpv_render_update};
    SDL_PushEvent(&event);
}

void quit() {
    mpv_render_context_free(mpv_gl);
    mpv_destroy(mpv);

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    SDL_RenderPresent(renderer);
    SDL_Quit();

    emscripten_cancel_main_loop();

    printf("properly terminated\n");
}

void set_viewport_size(int w, int h) {
    viewport_width = w > 0 ? w : 0;
    viewport_height = h > 0 ? h : 0;
}

void match_window_screen_size() {
    if (viewport_width > 0 && viewport_height > 0) {
        width = viewport_width;
        height = viewport_height;
    } else if (viewport_width > 0) {
        width = viewport_width;
        height = width;
        if (video_width > 0 && video_height > 0) {
            const double aspect_ratio = (double)video_height / video_width;
            height = (int)(aspect_ratio * width);
        }
    } else {
        emscripten_get_screen_size(&width, &height);
    }

    SDL_SetWindowSize(window, width, height);
}

void create_mpv_map_obj(mpv_node_list *map) {
    mpv_node node;
    char* key;
    int is_video = 0;
    int is_first = 0;
    int w = 16;
    int h = 9;
    EM_ASM(obj = {};);
    for (int i = 0; i < map->num; i++) {
        key = map->keys[i];
        node = map->values[i];
        if (strcmp(key, "id") == 0 && node.u.int64 == 1) 
            is_first = 1;
        if (strcmp(key, "type") == 0 && node.format == MPV_FORMAT_STRING && strcmp(node.u.string, "video") == 0) 
            is_video = 1;
        if (strcmp(key, "demux-w") == 0) 
            w = node.u.int64;
        if (strcmp(key, "demux-h") == 0) 
            h = node.u.int64;
        switch (node.format) {
            case MPV_FORMAT_INT64:
                EM_ASM({
                    obj[UTF8ToString($0)] = $1.toString();
                }, key, node.u.int64);
                break;
            case MPV_FORMAT_STRING:
                EM_ASM({
                    obj[UTF8ToString($0)] = UTF8ToString($1);
                }, key, node.u.string);
                break;
            case MPV_FORMAT_FLAG:
                EM_ASM({
                    obj[UTF8ToString($0)] = $1;
                }, key, node.u.flag);
                break;
            case MPV_FORMAT_DOUBLE:
                EM_ASM({
                    obj[UTF8ToString($0)] = $1;
                }, key, node.u.double_);
                break;
            default:
                printf("%s, format: %d\n", key, node.format);
        }
    }

    if (is_video && is_first) {
        video_width = w;
        video_height = h;
    }
}

void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

EMSCRIPTEN_BINDINGS(libmpv) {
    register_vector<string>("StringVector");

    emscripten::function("loadFile", &load_file);
    emscripten::function("loadStream", &load_stream);
    emscripten::function("setStreamBaseUrl", &set_stream_base_url);
    emscripten::function("loadFiles", &load_files);
    emscripten::function("togglePlay", &toggle_play);
    emscripten::function("setPaused", &set_paused);
    emscripten::function("stop", &stop);
    emscripten::function("setPlaybackTime", &set_playback_time_pos);
    emscripten::function("setVolume", &set_ao_volume);
    emscripten::function("getTracks", &get_tracks);
    emscripten::function("getChapters", &get_chapters);
    emscripten::function("setVideoTrack", &set_video_track);
    emscripten::function("setAudioTrack", &set_audio_track);
    emscripten::function("setSubtitleTrack", &set_subtitle_track);
    emscripten::function("setChapter", &set_chapter);
    emscripten::function("skipForward", &skip_forward);
    emscripten::function("skipBackward", &skip_backward);
    emscripten::function("getMpvThread", &get_main_thread);
    emscripten::function("addShaders", &add_shaders);
    emscripten::function("clearShaders", &clear_shaders);
    emscripten::function("getShaderCount", &get_shader_count);
    emscripten::function("setViewportSize", &set_viewport_size);
    emscripten::function("matchWindowScreenSize", &match_window_screen_size);
    emscripten::function("theatreFetchRegionPtr", &theatre_fetch_region_ptr);
}