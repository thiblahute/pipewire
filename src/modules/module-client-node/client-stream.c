/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <spa/node/node.h>
#include <spa/buffer/alloc.h>
#include <spa/pod/parser.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#include "pipewire/core.h"
#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/control.h"
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"
#include "client-node.h"
#include "client-stream.h"

/** \cond */

struct node {
	struct spa_node node;

	struct impl *impl;

	struct spa_log *log;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	uint32_t seq;
};

struct impl {
	struct pw_client_stream this;

	struct pw_core *core;

	struct node node;

	struct spa_hook node_listener;
	struct spa_hook client_node_listener;
	struct spa_hook resource_listener;

	enum spa_direction direction;

	struct spa_node *cnode;
	struct spa_node *adapter;
	struct spa_node *adapter_mix;

	bool use_converter;

	struct pw_client_node *client_node;
	struct pw_port *client_port;
	struct pw_port_mix client_port_mix;

	struct spa_io_buffers *io;
	struct spa_io_range range;

	struct spa_buffer **buffers;
	uint32_t n_buffers;
	struct pw_memblock *mem;

	struct {
		struct spa_graph_link link;
	} rt;
};

/** \endcond */

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	return 0;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	return 0;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct node *this;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if ((res = spa_node_send_command(impl->adapter, command)) < 0)
		return res;

	if (impl->adapter != impl->cnode) {
		if ((res = spa_node_send_command(impl->cnode, command)) < 0)
			return res;
	}

	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	this->callbacks = callbacks;
	this->callbacks_data = data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	struct node *this;
	struct impl *impl;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (impl->adapter) {
		spa_node_get_n_ports(impl->adapter,
				n_input_ports,
				max_input_ports,
				n_output_ports,
				max_output_ports);

		if (impl->direction == SPA_DIRECTION_OUTPUT) {
			if (n_input_ports)
				*n_input_ports = 0;
			if (max_input_ports)
				*max_input_ports = 0;
		} else {
			if (n_output_ports)
				*n_output_ports = 0;
			if (max_output_ports)
				*max_output_ports = 0;
		}
	} else {
		uint32_t n_inputs = 0, n_outputs = 0;

		if (impl->direction == SPA_DIRECTION_OUTPUT)
			n_outputs++;
		else
			n_inputs++;

		if (n_input_ports)
			*n_input_ports = n_inputs;
		if (max_input_ports)
			*max_input_ports = n_inputs;
		if (n_output_ports)
			*n_output_ports = n_outputs;
		if (max_output_ports)
			*max_output_ports = n_outputs;
	}
	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->impl->adapter) {
		return spa_node_get_port_ids(this->impl->adapter,
				input_ids,
				n_input_ids,
				output_ids,
				n_output_ids);
	}
	else {
		if (input_ids && n_input_ids > 0)
			input_ids[0] = 0;
		if (output_ids && n_output_ids > 0)
			output_ids[0] = 0;
	}

	return 0;
}

static int
impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct node *this;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_add_port(impl->adapter_mix, direction, port_id)) < 0)
		return res;

	if ((res = spa_node_port_set_io(impl->adapter_mix,
					direction, port_id,
					SPA_IO_Range,
					&impl->range,
					sizeof(&impl->range))) < 0)
			return res;

	return res;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct node *this;
	struct impl *impl;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != this->impl->direction)
		return -EINVAL;

	return spa_node_remove_port(impl->adapter_mix, direction, port_id);
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id,
			const struct spa_port_info **info)
{
	struct node *this;
	struct impl *impl;

	if (node == NULL || info == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	return spa_node_port_get_info(impl->adapter, direction, port_id, info);
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct node *this;
	struct impl *impl;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	return spa_node_port_enum_params(impl->adapter, direction, port_id, id,
			index, filter, result, builder);
}

static int debug_params(struct impl *impl, struct spa_node *node,
                enum spa_direction direction, uint32_t port_id, uint32_t id, struct spa_pod *filter)
{
	struct node *this = &impl->node;
        struct spa_pod_builder b = { 0 };
        uint8_t buffer[4096];
        uint32_t state;
        struct spa_pod *param;
        int res;

        spa_log_error(this->log, "params %s:", spa_debug_type_find_name(spa_type_param, id));

        state = 0;
        while (true) {
                spa_pod_builder_init(&b, buffer, sizeof(buffer));
                res = spa_node_port_enum_params(node,
                                       direction, port_id,
                                       id, &state,
                                       NULL, &param, &b);
                if (res <= 0) {
			if (res < 0)
				spa_log_error(this->log, "  error: %s", spa_strerror(res));
                        break;
		}
                spa_debug_pod(2, NULL, param);
        }

        spa_log_error(this->log, "failed filter:");
        if (filter)
                spa_debug_pod(2, NULL, filter);

        return 0;
}


static int negotiate_format(struct impl *impl)
{
	struct node *this = &impl->node;
	uint32_t state;
	struct spa_pod *format;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	int res;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_log_debug(this->log, "%p: negiotiate", impl);

	state = 0;
	if ((res = spa_node_port_enum_params(impl->adapter_mix,
				SPA_DIRECTION_REVERSE(impl->direction), 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) <= 0) {
		debug_params(impl, impl->adapter_mix, SPA_DIRECTION_REVERSE(impl->direction), 0,
				SPA_PARAM_EnumFormat, NULL);
		return -ENOTSUP;
	}

	state = 0;
	if ((res = spa_node_port_enum_params(impl->cnode,
				       impl->direction, 0,
				       SPA_PARAM_EnumFormat, &state,
				       format, &format, &b)) <= 0) {
		debug_params(impl, impl->cnode, impl->direction, 0,
				SPA_PARAM_EnumFormat, format);
		return -ENOTSUP;
	}

	spa_pod_fixate(format);
	spa_debug_format(0, NULL, format);

	if ((res = spa_node_port_set_param(impl->adapter_mix,
				   SPA_DIRECTION_REVERSE(impl->direction), 0,
				   SPA_PARAM_Format, 0,
				   format)) < 0)
			return res;

	if ((res = spa_node_port_set_param(impl->cnode,
					   impl->direction, 0,
					   SPA_PARAM_Format, 0,
					   format)) < 0)
			return res;

	return res;
}

static int negotiate_buffers(struct impl *impl)
{
	struct node *this = &impl->node;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t state;
	struct spa_pod *param = NULL;
	int res, i;
	bool in_alloc, out_alloc;
	int32_t size, buffers, blocks, align, flags;
	uint32_t *aligns, data_size;
	struct spa_data *datas;
	const struct spa_port_info *in_info, *out_info;
        struct spa_buffer_alloc_info info = { 0, };
        void *skel;

	spa_log_debug(this->log, "%p: %d", impl, impl->n_buffers);

	if (impl->n_buffers > 0)
		return 0;

	state = 0;
	if ((res = spa_node_port_enum_params(impl->adapter_mix,
			       SPA_DIRECTION_REVERSE(impl->direction), 0,
			       SPA_PARAM_Buffers, &state,
			       param, &param, &b)) <= 0) {
		debug_params(impl, impl->adapter_mix, SPA_DIRECTION_REVERSE(impl->direction), 0,
				SPA_PARAM_Buffers, param);
		return -ENOTSUP;
	}
	if (res == 0)
		param = NULL;

	state = 0;
	if ((res = spa_node_port_enum_params(impl->cnode,
			       impl->direction, 0,
			       SPA_PARAM_Buffers, &state,
			       param, &param, &b)) < 0) {
		debug_params(impl, impl->cnode, impl->direction, 0,
				SPA_PARAM_Buffers, param);
		return res;
	}

	spa_pod_fixate(param);

	if ((res = spa_node_port_get_info(impl->cnode,
					impl->direction, 0,
					&out_info)) < 0)
		return res;
	if ((res = spa_node_port_get_info(impl->adapter_mix,
					SPA_DIRECTION_REVERSE(impl->direction), 0,
					&in_info)) < 0)
		return res;

	in_alloc = SPA_FLAG_CHECK(in_info->flags, SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS);
	out_alloc = SPA_FLAG_CHECK(out_info->flags, SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS);

	flags = 0;
	if (out_alloc || in_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		if (out_alloc)
			in_alloc = false;
	}

	if (spa_pod_object_parse(param,
			":", SPA_PARAM_BUFFERS_buffers, "i", &buffers,
			":", SPA_PARAM_BUFFERS_blocks, "i", &blocks,
			":", SPA_PARAM_BUFFERS_size, "i", &size,
			":", SPA_PARAM_BUFFERS_align, "i", &align,
			NULL) < 0)
		return -EINVAL;

	spa_log_debug(this->log, "%p: buffers %d, blocks %d, size %d, align %d",
			impl, buffers, blocks, size, align);

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	spa_buffer_alloc_fill_info(&info, 0, NULL, blocks, datas, aligns);
	info.skel_size = SPA_ROUND_UP_N(info.skel_size, 16);

	if (impl->buffers)
		free(impl->buffers);
	impl->buffers = calloc(buffers, sizeof(struct spa_buffer *) + info.skel_size);
	if (impl->buffers == NULL)
		return -ENOMEM;

        skel = SPA_MEMBER(impl->buffers, sizeof(struct spa_buffer *) * buffers, void);
	data_size = info.meta_size + info.chunk_size + info.data_size;

	if (impl->mem) {
		pw_memblock_free(impl->mem);
		impl->mem = NULL;
	}

	if ((res = pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
				     PW_MEMBLOCK_FLAG_MAP_READWRITE |
				     PW_MEMBLOCK_FLAG_SEAL, buffers * data_size,
				     &impl->mem)) < 0)
		return res;

	impl->n_buffers = buffers;

        spa_buffer_alloc_layout_array(&info, impl->n_buffers, impl->buffers,
			skel, impl->mem->ptr);

	if (in_alloc) {
		if ((res = spa_node_port_alloc_buffers(impl->adapter_mix,
			       SPA_DIRECTION_REVERSE(impl->direction), 0,
			       NULL, 0,
			       impl->buffers, &impl->n_buffers)) < 0)
			return res;
	}
	else {
		if ((res = spa_node_port_use_buffers(impl->adapter_mix,
			       SPA_DIRECTION_REVERSE(impl->direction), 0,
			       impl->buffers, impl->n_buffers)) < 0)
			return res;
	}
	if (out_alloc) {
		if ((res = spa_node_port_alloc_buffers(impl->client_port->mix,
			       impl->direction, 0,
			       NULL, 0,
			       impl->buffers, &impl->n_buffers)) < 0)
			return res;
	}
	else {
		if ((res = spa_node_port_use_buffers(impl->client_port->mix,
			       impl->direction, 0,
			       impl->buffers, impl->n_buffers)) < 0)
			return res;
	}

	return 0;
}

static void try_link_controls(struct impl *impl, struct pw_port *port, struct pw_port *target)
{
	struct pw_control *cin, *cout;
	int res;

	pw_log_debug("module %p: trying controls", impl);
	spa_list_for_each(cout, &port->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &target->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_link(cout, cin)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
		}
	}
	spa_list_for_each(cin, &port->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &target->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_link(cout, cin)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
		}
	}
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct node *this;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_port_set_param(impl->adapter_mix, direction, port_id, id,
			flags, param)) < 0)
		return res;

	if (id == SPA_PARAM_Format && impl->use_converter) {
		if (param == NULL) {
			if ((res = spa_node_port_set_param(impl->adapter_mix,
				SPA_DIRECTION_REVERSE(direction), 0, id,
					0, NULL)) < 0)
				return res;
			impl->n_buffers = 0;
		}
		else {
			if (port_id == 0)
				res = negotiate_format(impl);
		}
	}
	return res;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct node *this;
	struct impl *impl;
	int res = 0;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	spa_log_debug(this->log, "set io %d %d %d", id, direction, impl->direction);
	if (direction != impl->direction)
		return -EINVAL;

	if (impl->use_converter)
		res = spa_node_port_set_io(impl->adapter_mix, direction, port_id, id, data, size);

	if (id == SPA_IO_Buffers && size >= sizeof(struct spa_io_buffers)) {
		impl->io = data;
	}

	return res;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct node *this;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_port_use_buffers(impl->adapter_mix,
					direction, port_id, buffers, n_buffers)) < 0)
		return res;


	spa_log_debug(this->log, "%p: %d %d", impl, n_buffers, port_id);

	if (n_buffers > 0 && impl->use_converter) {
		if (port_id == 0)
			res = negotiate_buffers(impl);
	}
	return res;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct node *this;
	struct impl *impl;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	return spa_node_port_alloc_buffers(impl->adapter_mix, direction, port_id,
			params, n_params, buffers, n_buffers);
}

static int
impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct node *this;
	struct impl *impl;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	return spa_node_port_reuse_buffer(impl->adapter, port_id, buffer_id);
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id, const struct spa_command *command)
{
	struct node *this;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_port_send_command(impl->adapter, direction, port_id, command)) < 0)
		return res;

	return res;
}

static int impl_node_process(struct spa_node *node)
{
	struct node *this = SPA_CONTAINER_OF(node, struct node, node);
	struct impl *impl = this->impl;
	struct pw_driver_quantum *q = impl->this.node->driver_node->rt.quantum;
	int status, trigger;

	impl->range.min_size = impl->range.max_size = q->size * sizeof(float);

	spa_log_trace(this->log, "%p: process %d", this, impl->range.max_size);

	if (impl->use_converter) {
		status = spa_node_process(impl->adapter);
	}
	else {
		struct spa_io_buffers tmp;

		spa_log_trace(this->log, "%p: process %d/%d %d/%d", this,
				impl->io->status, impl->io->buffer_id,
				impl->client_port_mix.io->status,
				impl->client_port_mix.io->buffer_id);

		tmp = *impl->io;
		*impl->io = *impl->client_port_mix.io;
		*impl->client_port_mix.io = tmp;

		status = impl->client_port_mix.io->status | impl->io->status;
	}
	spa_log_trace(this->log, "%p: process %d", this, status);

	if (impl->direction == SPA_DIRECTION_OUTPUT) {
		if (!(status & SPA_STATUS_HAVE_BUFFER))
			spa_log_warn(this->log, "%p: process underrun", this);
		trigger = status & SPA_STATUS_NEED_BUFFER;
	}
	else
		trigger = status & SPA_STATUS_HAVE_BUFFER;

	if (trigger && !impl->this.node->driver)
		spa_graph_run(impl->client_node->node->rt.root.graph);

	return status;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process,
};

static int
node_init(struct node *this,
	  struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	uint32_t i;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}
	this->node = impl_node;

	this->seq = 1;

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static void client_node_initialized(void *data)
{
	struct impl *impl = data;
        uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports, state;
	uint32_t media_type, media_subtype;
	struct spa_pod *format;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
	int res;
	const struct pw_properties *props;
	const char *str, *dir, *type;
	char media_class[64];
	bool exclusive;

	pw_log_debug("client-stream %p: initialized", &impl->this);

	impl->cnode = pw_node_get_implementation(impl->client_node->node);

	if ((res = spa_node_get_n_ports(impl->cnode,
			     &n_input_ports,
			     &max_input_ports,
			     &n_output_ports,
			     &max_output_ports)) < 0)
		return;

	if (n_input_ports > 0) {
		impl->direction = SPA_DIRECTION_INPUT;
		dir = "Input";
	}
	else {
		impl->direction = SPA_DIRECTION_OUTPUT;
		dir = "Output";
	}

	props = pw_node_get_properties(impl->client_node->node);
	if (props != NULL && (str = pw_properties_get(props, PW_NODE_PROP_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	impl->client_node->node->rt.driver = impl->this.node->rt.driver;

	impl->client_port = pw_node_find_port(impl->client_node->node, impl->direction, 0);
	if (impl->client_port == NULL)
		return;

	if ((res = pw_port_init_mix(impl->client_port, &impl->client_port_mix)) < 0)
		return;

	if ((res = spa_node_port_set_io(impl->client_port->mix,
				impl->direction, 0,
				SPA_IO_Buffers,
				impl->client_port_mix.io,
				sizeof(impl->client_port_mix.io))) < 0)
		return;

	state = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if ((res = spa_node_port_enum_params(impl->cnode,
				impl->direction, 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) <= 0) {
		pw_log_warn("client-stream %p: no format given", &impl->this);
		impl->adapter = impl->cnode;
		impl->adapter_mix = impl->client_port->mix;
		impl->use_converter = false;
		return;
	}

	if (spa_format_parse(format, &media_type, &media_subtype) < 0)
		return;

	pw_log_debug("client-stream %p: %s/%s", &impl->this,
			spa_debug_type_find_name(spa_type_media_type, media_type),
			spa_debug_type_find_name(spa_type_media_subtype, media_subtype));


	if (!exclusive &&
	    media_type == SPA_MEDIA_TYPE_audio &&
	    media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		if ((impl->adapter = pw_load_spa_interface("audioconvert/libspa-audioconvert",
				"audioconvert", SPA_TYPE_INTERFACE_Node, NULL, 0, NULL)) == NULL)
			return;

		impl->adapter_mix = impl->adapter;
		impl->use_converter = true;
	}
	else {
		impl->adapter = impl->cnode;
		impl->adapter_mix = impl->client_port->mix;
		impl->use_converter = false;
	}

	if (impl->use_converter) {
		if ((res = spa_node_port_set_io(impl->adapter_mix,
					SPA_DIRECTION_REVERSE(impl->direction), 0,
					SPA_IO_Buffers,
					impl->client_port_mix.io,
					sizeof(impl->client_port_mix.io))) < 0)
			return;


	}

	if (media_type == SPA_MEDIA_TYPE_audio)
		type = "Audio";
	else if (media_type == SPA_MEDIA_TYPE_video)
		type = "Video";
	else
		type = "Generic";

	snprintf(media_class, sizeof(media_class), "Stream/%s/%s", dir, type);

	pw_node_register(impl->this.node,
			pw_resource_get_client(impl->client_node->resource),
			impl->client_node->parent,
			pw_properties_new(
				"media.class", media_class,
				NULL));

	if (impl->use_converter) {
		struct pw_port *adapter_port;
		adapter_port = pw_node_find_port(impl->this.node, impl->direction, 0);

		if (adapter_port) {
			try_link_controls(impl, impl->client_port, adapter_port);
		}
		else {
			pw_log_warn("client-stream %p: can't link controls", &impl->this);
		}
	}

	pw_log_debug("client-stream %p: activating", &impl->this);

	pw_node_set_active(impl->this.node, true);
}

static void cleanup(struct impl *impl)
{
	if (impl->use_converter) {
		if (impl->adapter)
			pw_unload_spa_interface(impl->adapter);
	}

	if (impl->buffers)
		free(impl->buffers);
	if (impl->mem) {
		pw_memblock_free(impl->mem);
		impl->mem = NULL;
	}
	free(impl);
}

static void client_node_destroy(void *data)
{
	struct impl *impl = data;
	pw_log_debug("client-stream %p: destroy", &impl->this);

	pw_node_set_driver(impl->client_node->node, NULL);

	spa_hook_remove(&impl->client_node_listener);
	spa_hook_remove(&impl->node_listener);
	pw_node_destroy(impl->this.node);
	cleanup(impl);
}

static void client_node_async_complete(void *data, uint32_t seq, int res)
{
	struct impl *impl = data;
	struct node *node = &impl->node;

	pw_log_debug("client-stream %p: async complete %d %d", &impl->this, seq, res);
	node->callbacks->done(node->callbacks_data, seq, res);
}

static void client_node_active_changed(void *data, bool active)
{
	struct impl *impl = data;

	pw_log_debug("client-stream %p: active %d", &impl->this, active);
	pw_node_set_active(impl->this.node, active);
}

static const struct pw_node_events client_node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = client_node_destroy,
	.initialized = client_node_initialized,
	.async_complete = client_node_async_complete,
	.active_changed = client_node_active_changed,
};

static void node_destroy(void *data)
{
	struct impl *impl = data;

	pw_log_debug("client-stream %p: destroy", &impl->this);

	spa_hook_remove(&impl->node_listener);
	spa_hook_remove(&impl->client_node_listener);
	pw_client_node_destroy(impl->client_node);
	cleanup(impl);
}

static void node_initialized(void *data)
{
	struct impl *impl = data;
	pw_client_node_registered(impl->client_node, impl->this.node->global->id);
}

static void node_driver_changed(void *data, struct pw_node *driver)
{
	struct impl *impl = data;
	impl->client_node->node->driver_node = driver;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.initialized = node_initialized,
	.driver_changed = node_driver_changed,
};

/** Create a new client stream
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client stream
 *
 * Create a new \ref pw_stream.
 *
 * \memberof pw_client_stream
 */
struct pw_client_stream *pw_client_stream_new(struct pw_resource *resource,
					  struct pw_global *parent,
					  struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_client_stream *this;
	struct pw_client *client = pw_resource_get_client(resource);
	struct pw_core *core = pw_client_get_core(client);
	const struct spa_support *support;
	uint32_t n_support;
	const char *name;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	impl->core = core;

	pw_log_debug("client-stream %p: new", impl);

	impl->client_node = pw_client_node_new(
			resource,
			parent,
			pw_properties_copy(properties),
			false);
	if (impl->client_node == NULL)
		goto error_no_node;

	support = pw_core_get_support(impl->core, &n_support);

	node_init(&impl->node, NULL, support, n_support);
	impl->node.impl = impl;

	if ((name = pw_properties_get(properties, "node.name")) == NULL)
		name = "client-stream";

	this->node = pw_spa_node_new(core,
				     client,
				     parent,
				     name,
				     PW_SPA_NODE_FLAG_ASYNC,
				     &impl->node.node,
				     NULL,
				     properties, 0);
	if (this->node == NULL)
		goto error_no_node;

	this->node->remote = true;

	pw_node_add_listener(impl->client_node->node,
			     &impl->client_node_listener,
			     &client_node_events, impl);
	pw_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

      error_no_node:
	pw_resource_destroy(resource);
	free(impl);
	return NULL;
}

/** Destroy a client stream
 * \param stream the client stream to destroy
 * \memberof pw_client_stream
 */
void pw_client_stream_destroy(struct pw_client_stream *stream)
{
	struct impl *impl = SPA_CONTAINER_OF(stream, struct impl, this);
	pw_client_node_destroy(impl->client_node);
}
