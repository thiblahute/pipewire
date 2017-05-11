/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <sys/mman.h>

#include <SDL2/SDL.h>

#include <spa/include/spa/type-map.h>
#include <spa/include/spa/format-utils.h>
#include <spa/include/spa/video/format-utils.h>
#include <spa/include/spa/format-builder.h>
#include <spa/include/spa/props.h>

#include <pinos/client/pinos.h>
#include <pinos/client/sig.h>
#include <spa/lib/debug.h>

typedef struct {
  uint32_t format;
  uint32_t props;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatVideo format_video;
  SpaTypeVideoFormat video_format;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
}

#define WIDTH   640
#define HEIGHT  480
#define BPP    3

typedef struct {
  Type type;

  const char *path;

  SDL_Renderer *renderer;
  SDL_Window *window;
  SDL_Texture *texture;

  bool running;
  PinosLoop *loop;
  SpaSource *timer;

  PinosContext *context;
  PinosListener on_state_changed;

  PinosStream *stream;
  PinosListener on_stream_state_changed;
  PinosListener on_stream_format_changed;
  PinosListener on_stream_new_buffer;

  SpaVideoInfoRaw format;
  int32_t stride;

  uint8_t params_buffer[1024];
  int counter;
} Data;

static void
handle_events (Data *data)
{
  SDL_Event event;
  while (SDL_PollEvent (&event)) {
    switch (event.type) {
      case SDL_QUIT:
        exit (0);
        break;
    }
  }
}

static void
on_stream_new_buffer (PinosListener *listener,
                      PinosStream   *stream,
                      uint32_t       id)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_stream_new_buffer);
  SpaBuffer *buf;
  uint8_t *map;
  void *sdata, *ddata;
  int sstride, dstride, ostride;
  int i;
  uint8_t *src, *dst;

  buf = pinos_stream_peek_buffer (data->stream, id);

  if (buf->datas[0].type == data->type.data.MemFd) {
    map = mmap (NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ,
                    MAP_PRIVATE, buf->datas[0].fd, 0);
    sdata = SPA_MEMBER (map, buf->datas[0].mapoffset, uint8_t);
  }
  else if (buf->datas[0].type == data->type.data.MemPtr) {
    map = NULL;
    sdata = buf->datas[0].data;
  } else
    return;

  if (SDL_LockTexture (data->texture, NULL, &ddata, &dstride) < 0) {
    fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
    return;
  }
  sstride = buf->datas[0].chunk->stride;
  ostride = SPA_MIN (sstride, dstride);

  src = sdata;
  dst = ddata;
  for (i = 0; i < data->format.size.height; i++) {
    memcpy (dst, src, ostride);
    src += sstride;
    dst += dstride;
  }
  SDL_UnlockTexture(data->texture);

  SDL_RenderClear (data->renderer);
  SDL_RenderCopy (data->renderer, data->texture, NULL, NULL);
  SDL_RenderPresent (data->renderer);

  if (map)
    munmap (map, buf->datas[0].maxsize);

  pinos_stream_recycle_buffer (data->stream, id);

  handle_events (data);
}

static void
on_stream_state_changed (PinosListener  *listener,
                         PinosStream    *stream)
{
  printf ("stream state: \"%s\"\n", pinos_stream_state_as_string (stream->state));
}

static struct {
  Uint32 format;
  uint32_t id;
} video_formats[] = {
  { SDL_PIXELFORMAT_UNKNOWN,      offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_INDEX1LSB,    offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_UNKNOWN,      offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_INDEX1LSB,    offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_INDEX1MSB,    offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_INDEX4LSB,    offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_INDEX4MSB,    offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_INDEX8,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGB332,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGB444,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGB555,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_BGR555,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_ARGB4444,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGBA4444,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_ABGR4444,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_BGRA4444,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_ARGB1555,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGBA5551,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_ABGR1555,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_BGRA5551,     offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGB565,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_BGR565,       offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGB24,        offsetof (SpaTypeVideoFormat, RGB), },
  { SDL_PIXELFORMAT_RGB888,       offsetof (SpaTypeVideoFormat, RGB), },
  { SDL_PIXELFORMAT_RGBX8888,     offsetof (SpaTypeVideoFormat, RGBx), },
  { SDL_PIXELFORMAT_BGR24,        offsetof (SpaTypeVideoFormat, BGR), },
  { SDL_PIXELFORMAT_BGR888,       offsetof (SpaTypeVideoFormat, BGR), },
  { SDL_PIXELFORMAT_BGRX8888,     offsetof (SpaTypeVideoFormat, BGRx), },
  { SDL_PIXELFORMAT_ARGB2101010,  offsetof (SpaTypeVideoFormat, UNKNOWN), },
  { SDL_PIXELFORMAT_RGBA8888,     offsetof (SpaTypeVideoFormat, RGBA), },
  { SDL_PIXELFORMAT_ARGB8888,     offsetof (SpaTypeVideoFormat, ARGB), },
  { SDL_PIXELFORMAT_BGRA8888,     offsetof (SpaTypeVideoFormat, BGRA), },
  { SDL_PIXELFORMAT_ABGR8888,     offsetof (SpaTypeVideoFormat, ABGR), },
  { SDL_PIXELFORMAT_YV12,         offsetof (SpaTypeVideoFormat, YV12), },
  { SDL_PIXELFORMAT_IYUV,         offsetof (SpaTypeVideoFormat, I420), },
  { SDL_PIXELFORMAT_YUY2,         offsetof (SpaTypeVideoFormat, YUY2), },
  { SDL_PIXELFORMAT_UYVY,         offsetof (SpaTypeVideoFormat, UYVY), },
  { SDL_PIXELFORMAT_YVYU,         offsetof (SpaTypeVideoFormat, YVYU), },
  { SDL_PIXELFORMAT_NV12,         offsetof (SpaTypeVideoFormat, NV12), },
  { SDL_PIXELFORMAT_NV21,         offsetof (SpaTypeVideoFormat, NV21), }
};

static uint32_t
sdl_format_to_id (Data *data, Uint32 format)
{
  int i;

  for (i = 0; i < SPA_N_ELEMENTS (video_formats); i++) {
    if (video_formats[i].format == format)
      return *SPA_MEMBER (&data->type.video_format, video_formats[i].id, uint32_t);
  }
  return data->type.video_format.UNKNOWN;
}

static Uint32
id_to_sdl_format (Data *data, uint32_t id)
{
  int i;

  for (i = 0; i < SPA_N_ELEMENTS (video_formats); i++) {
    if (*SPA_MEMBER (&data->type.video_format, video_formats[i].id, uint32_t) == id)
      return video_formats[i].format;
  }
  return SDL_PIXELFORMAT_UNKNOWN;
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)

static void
on_stream_format_changed (PinosListener  *listener,
                          PinosStream    *stream,
                          SpaFormat      *format)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_stream_format_changed);
  PinosContext *ctx = stream->context;
  SpaPODBuilder b = { NULL };
  SpaPODFrame f[2];
  SpaAllocParam *params[2];

  if (format) {
    Uint32 sdl_format;

    spa_debug_format (format, data->context->type.map);

    spa_format_video_raw_parse (format, &data->format, &data->type.format_video);

    sdl_format = id_to_sdl_format (data, data->format.format);
    if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
      pinos_stream_finish_format (stream, SPA_RESULT_ERROR, NULL, 0);
      return;
    }

    data->texture = SDL_CreateTexture (data->renderer,
                                       sdl_format,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       data->format.size.width,
                                       data->format.size.height);
    data->stride = data->format.size.width * 4;

    spa_pod_builder_init (&b, data->params_buffer, sizeof (data->params_buffer));
    spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_buffers.Buffers,
        PROP      (&f[1], ctx->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT,
                                                               data->stride * data->format.size.height),
        PROP      (&f[1], ctx->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, data->stride),
        PROP_U_MM (&f[1], ctx->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, 32, 2, 32),
        PROP      (&f[1], ctx->type.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
    params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_meta_enable.MetaEnable,
      PROP      (&f[1], ctx->type.alloc_param_meta_enable.type, SPA_POD_TYPE_ID, ctx->type.meta.Header),
      PROP      (&f[1], ctx->type.alloc_param_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    pinos_stream_finish_format (stream, SPA_RESULT_OK, params, 2);
  }
  else {
    pinos_stream_finish_format (stream, SPA_RESULT_OK, NULL, 0);
  }
}
static void
on_state_changed (PinosListener  *listener,
                  PinosContext   *context)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_state_changed);

  switch (context->state) {
    case PINOS_CONTEXT_STATE_ERROR:
      printf ("context error: %s\n", context->error);
      data->running = false;
      break;

    case PINOS_CONTEXT_STATE_CONNECTED:
    {
      SpaFormat *formats[1];
      uint8_t buffer[1024];
      SpaPODBuilder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
      SpaPODFrame f[2];
      SDL_RendererInfo info;
      int i, c;

      printf ("context state: \"%s\"\n", pinos_context_state_as_string (context->state));

      data->stream = pinos_stream_new (context, "video-play", NULL);

      SDL_GetRendererInfo(data->renderer, &info);

      spa_pod_builder_push_format (&b, &f[0], data->type.format,
         data->type.media_type.video, data->type.media_subtype.raw);

      spa_pod_builder_push_prop (&b, &f[1], data->type.format_video.format,
                                            SPA_POD_PROP_FLAG_UNSET |
                                            SPA_POD_PROP_RANGE_ENUM);
      for (i = 0, c = 0; i < info.num_texture_formats; i++) {
        uint32_t id = sdl_format_to_id (data, info.texture_formats[i]);
        if (id == 0)
          continue;
        if (c++ == 0)
          spa_pod_builder_id (&b, id);
        spa_pod_builder_id (&b, id);
      }
      for (i = 0; i < SPA_N_ELEMENTS (video_formats); i++) {
        uint32_t id = *SPA_MEMBER (&data->type.video_format, video_formats[i].id, uint32_t);
        if (id != data->type.video_format.UNKNOWN)
          spa_pod_builder_id (&b, id);
      }
      spa_pod_builder_pop (&b, &f[1]);
      spa_pod_builder_add (&b,
         PROP_U_MM (&f[1], data->type.format_video.size,
                           SPA_POD_TYPE_RECTANGLE, WIDTH, HEIGHT,
                                                   1, 1,
                                                   info.max_texture_width, info.max_texture_height),
         PROP_U_MM (&f[1], data->type.format_video.framerate,
                           SPA_POD_TYPE_FRACTION,  25, 1,
                                                   0, 1,
                                                   30, 1),
         0);

      spa_pod_builder_pop (&b, &f[0]);
      formats[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

      printf ("supported formats:\n");
      spa_debug_format (formats[0], data->context->type.map);

      pinos_signal_add (&data->stream->state_changed,
                        &data->on_stream_state_changed,
                        on_stream_state_changed);
      pinos_signal_add (&data->stream->format_changed,
                        &data->on_stream_format_changed,
                        on_stream_format_changed);
      pinos_signal_add (&data->stream->new_buffer,
                        &data->on_stream_new_buffer,
                        on_stream_new_buffer);

      pinos_stream_connect (data->stream,
                            PINOS_DIRECTION_INPUT,
                            PINOS_STREAM_MODE_BUFFER,
                            data->path,
                            PINOS_STREAM_FLAG_AUTOCONNECT,
                            1,
                            formats);
      break;
    }
    default:
      printf ("context state: \"%s\"\n", pinos_context_state_as_string (context->state));
      break;
  }
}

int
main (int argc, char *argv[])
{
  Data data = { 0, };

  pinos_init (&argc, &argv);

  data.loop = pinos_loop_new ();
  data.running = true;
  data.context = pinos_context_new (data.loop, "video-play", NULL);
  data.path = argc > 1 ? argv[1] : NULL;

  init_type (&data.type, data.context->type.map);

  if (SDL_Init (SDL_INIT_VIDEO) < 0) {
    printf ("can't initialize SDL: %s\n", SDL_GetError ());
    return -1;
  }

  if (SDL_CreateWindowAndRenderer (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
    printf ("can't create window: %s\n", SDL_GetError ());
    return -1;
  }

  pinos_signal_add (&data.context->state_changed,
                    &data.on_state_changed,
                    on_state_changed);

  pinos_context_connect (data.context, PINOS_CONTEXT_FLAG_NO_REGISTRY);

  pinos_loop_enter (data.loop);
  while (data.running) {
    pinos_loop_iterate (data.loop, -1);
  }
  pinos_loop_leave (data.loop);

  pinos_context_destroy (data.context);
  pinos_loop_destroy (data.loop);

  return 0;
}
