/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_COMMAND_H__
#define __SPA_COMMAND_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaCommand SpaCommand;

#include <spa/defs.h>

typedef enum {
  SPA_COMMAND_INVALID                 =  0,
  SPA_COMMAND_ACTIVATE,
  SPA_COMMAND_DEACTIVATE,
  SPA_COMMAND_START,
  SPA_COMMAND_STOP,
  SPA_COMMAND_FLUSH,
  SPA_COMMAND_DRAIN,
  SPA_COMMAND_MARKER,
} SpaCommandType;

struct _SpaCommand {
  volatile int   refcount;
  SpaNotify      notify;
  SpaCommandType type;
  uint32_t       port_id;
  void          *data;
  size_t         size;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_COMMAND_H__ */