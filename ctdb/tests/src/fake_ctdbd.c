/*
   Fake CTDB server for testing

   Copyright (C) Amitay Isaacs  2016

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/network.h"
#include "system/time.h"

#include <popt.h>
#include <talloc.h>
#include <tevent.h>
#include <tdb.h>

#include "lib/util/dlinklist.h"
#include "lib/util/tevent_unix.h"
#include "lib/util/debug.h"
#include "lib/util/samba_util.h"
#include "lib/async_req/async_sock.h"

#include "protocol/protocol.h"
#include "protocol/protocol_api.h"

#include "common/comm.h"
#include "common/system.h"
#include "common/logging.h"


#define CTDB_PORT 4379

/* A fake flag that is only supported by some functions */
#define NODE_FLAGS_FAKE_TIMEOUT 0x80000000

struct node {
	ctdb_sock_addr addr;
	uint32_t pnn;
	uint32_t flags;
	uint32_t capabilities;
	bool recovery_disabled;
	void *recovery_substate;
};

struct node_map {
	uint32_t num_nodes;
	struct node *node;
	uint32_t pnn;
	uint32_t recmaster;
};

struct interface {
	const char *name;
	bool link_up;
	uint32_t references;
};

struct interface_map {
	int num;
	struct interface *iface;
};

struct vnn_map {
	uint32_t recmode;
	uint32_t generation;
	uint32_t size;
	uint32_t *map;
};

struct srvid_register_state {
	struct srvid_register_state *prev, *next;
	struct ctdbd_context *ctdb;
	uint64_t srvid;
};

struct ctdbd_context {
	struct node_map *node_map;
	struct interface_map *iface_map;
	struct vnn_map *vnn_map;
	struct srvid_register_state *rstate;
	int num_clients;
	struct timeval start_time;
	struct timeval recovery_start_time;
	struct timeval recovery_end_time;
	bool takeover_disabled;
};

/*
 * Parse routines
 */

static struct node_map *nodemap_init(TALLOC_CTX *mem_ctx)
{
	struct node_map *node_map;

	node_map = talloc_zero(mem_ctx, struct node_map);
	if (node_map == NULL) {
		return NULL;
	}

	node_map->pnn = CTDB_UNKNOWN_PNN;
	node_map->recmaster = CTDB_UNKNOWN_PNN;

	return node_map;
}

/* Read a nodemap from stdin.  Each line looks like:
 *  <PNN> <FLAGS> [RECMASTER] [CURRENT] [CAPABILITIES]
 * EOF or a blank line terminates input.
 *
 * By default, capablities for each node are
 * CTDB_CAP_RECMASTER|CTDB_CAP_LMASTER.  These 2
 * capabilities can be faked off by adding, for example,
 * -CTDB_CAP_RECMASTER.
 */

static bool nodemap_parse(struct node_map *node_map)
{
	char line[1024];

	while ((fgets(line, sizeof(line), stdin) != NULL)) {
		uint32_t pnn, flags, capabilities;
		char *tok, *t;
		char *ip;
		ctdb_sock_addr saddr;
		struct node *node;

		if (line[0] == '\n') {
			break;
		}

		/* Get rid of pesky newline */
		if ((t = strchr(line, '\n')) != NULL) {
			*t = '\0';
		}

		/* Get PNN */
		tok = strtok(line, " \t");
		if (tok == NULL) {
			fprintf(stderr, "bad line (%s) - missing PNN\n", line);
			continue;
		}
		pnn = (uint32_t)strtoul(tok, NULL, 0);

		/* Get IP */
		tok = strtok(NULL, " \t");
		if (tok == NULL) {
			fprintf(stderr, "bad line (%s) - missing IP\n", line);
			continue;
		}
		if (!parse_ip(tok, NULL, CTDB_PORT, &saddr)) {
			fprintf(stderr, "bad line (%s) - invalid IP\n", line);
			continue;
		}
		ip = talloc_strdup(node_map, tok);
		if (ip == NULL) {
			goto fail;
		}

		/* Get flags */
		tok = strtok(NULL, " \t");
		if (tok == NULL) {
			fprintf(stderr, "bad line (%s) - missing flags\n",
				line);
			continue;
		}
		flags = (uint32_t)strtoul(tok, NULL, 0);
		/* Handle deleted nodes */
		if (flags & NODE_FLAGS_DELETED) {
			talloc_free(ip);
			ip = talloc_strdup(node_map, "0.0.0.0");
			if (ip == NULL) {
				goto fail;
			}
		}
		capabilities = CTDB_CAP_RECMASTER|CTDB_CAP_LMASTER;

		tok = strtok(NULL, " \t");
		while (tok != NULL) {
			if (strcmp(tok, "CURRENT") == 0) {
				node_map->pnn = pnn;
			} else if (strcmp(tok, "RECMASTER") == 0) {
				node_map->recmaster = pnn;
			} else if (strcmp(tok, "-CTDB_CAP_RECMASTER") == 0) {
				capabilities &= ~CTDB_CAP_RECMASTER;
			} else if (strcmp(tok, "-CTDB_CAP_LMASTER") == 0) {
				capabilities &= ~CTDB_CAP_LMASTER;
			} else if (strcmp(tok, "TIMEOUT") == 0) {
				/* This can be done with just a flag
				 * value but it is probably clearer
				 * and less error-prone to fake this
				 * with an explicit token */
				flags |= NODE_FLAGS_FAKE_TIMEOUT;
			}
			tok = strtok(NULL, " \t");
		}

		node_map->node = talloc_realloc(node_map, node_map->node,
						struct node,
						node_map->num_nodes + 1);
		if (node_map->node == NULL) {
			goto fail;
		}
		node = &node_map->node[node_map->num_nodes];

		parse_ip(ip, NULL, CTDB_PORT, &node->addr);
		node->pnn = pnn;
		node->flags = flags;
		node->capabilities = capabilities;
		node->recovery_disabled = false;
		node->recovery_substate = NULL;

		node_map->num_nodes += 1;
	}

	DEBUG(DEBUG_INFO, ("Parsing nodemap done\n"));
	return true;

fail:
	DEBUG(DEBUG_INFO, ("Parsing nodemap failed\n"));
	return false;

}

/* Append a node to a node map with given address and flags */
static bool node_map_add(struct ctdb_node_map *nodemap,
			 const char *nstr, uint32_t flags)
{
	ctdb_sock_addr addr;
	uint32_t num;
	struct ctdb_node_and_flags *n;

	if (! parse_ip(nstr, NULL, CTDB_PORT, &addr)) {
		fprintf(stderr, "Invalid IP address %s\n", nstr);
		return false;
	}

	num = nodemap->num;
	nodemap->node = talloc_realloc(nodemap, nodemap->node,
				       struct ctdb_node_and_flags, num+1);
	if (nodemap->node == NULL) {
		return false;
	}

	n = &nodemap->node[num];
	n->addr = addr;
	n->pnn = num;
	n->flags = flags;

	nodemap->num = num+1;
	return true;
}

/* Read a nodes file into a node map */
static struct ctdb_node_map *ctdb_read_nodes_file(TALLOC_CTX *mem_ctx,
						  const char *nlist)
{
	char **lines;
	int nlines;
	int i;
	struct ctdb_node_map *nodemap;

	nodemap = talloc_zero(mem_ctx, struct ctdb_node_map);
	if (nodemap == NULL) {
		return NULL;
	}

	lines = file_lines_load(nlist, &nlines, 0, mem_ctx);
	if (lines == NULL) {
		return NULL;
	}

	while (nlines > 0 && strcmp(lines[nlines-1], "") == 0) {
		nlines--;
	}

	for (i=0; i<nlines; i++) {
		char *node;
		uint32_t flags;
		size_t len;

		node = lines[i];
		/* strip leading spaces */
		while((*node == ' ') || (*node == '\t')) {
			node++;
		}

		len = strlen(node);

		/* strip trailing spaces */
		while ((len > 1) &&
		       ((node[len-1] == ' ') || (node[len-1] == '\t')))
		{
			node[len-1] = '\0';
			len--;
		}

		if (len == 0) {
			continue;
		}
		if (*node == '#') {
			/* A "deleted" node is a node that is
			   commented out in the nodes file.  This is
			   used instead of removing a line, which
			   would cause subsequent nodes to change
			   their PNN. */
			flags = NODE_FLAGS_DELETED;
			node = discard_const("0.0.0.0");
		} else {
			flags = 0;
		}
		if (! node_map_add(nodemap, node, flags)) {
			talloc_free(lines);
			TALLOC_FREE(nodemap);
			return NULL;
		}
	}

	talloc_free(lines);
	return nodemap;
}

static struct ctdb_node_map *read_nodes_file(TALLOC_CTX *mem_ctx,
					     uint32_t pnn)
{
	struct ctdb_node_map *nodemap;
	char nodepath[PATH_MAX];
	const char *nodes_list;

	/* read the nodes file */
	sprintf(nodepath, "CTDB_NODES_%u", pnn);
	nodes_list = getenv(nodepath);
	if (nodes_list == NULL) {
		nodes_list = getenv("CTDB_NODES");
		if (nodes_list == NULL) {
			DEBUG(DEBUG_INFO, ("Nodes file not defined\n"));
			return NULL;
		}
	}

	nodemap = ctdb_read_nodes_file(mem_ctx, nodes_list);
	if (nodemap == NULL) {
		DEBUG(DEBUG_INFO, ("Failed to read nodes file \"%s\"\n",
				   nodes_list));
		return NULL;
	}

	return nodemap;
}

static struct interface_map *interfaces_init(TALLOC_CTX *mem_ctx)
{
	struct interface_map *iface_map;

	iface_map = talloc_zero(mem_ctx, struct interface_map);
	if (iface_map == NULL) {
		return NULL;
	}

	return iface_map;
}

/* Read interfaces information.  Same format as "ctdb ifaces -Y"
 * output:
 *   :Name:LinkStatus:References:
 *   :eth2:1:4294967294
 *   :eth1:1:4294967292
 */

static bool interfaces_parse(struct interface_map *iface_map)
{
	char line[1024];

	while ((fgets(line, sizeof(line), stdin) != NULL)) {
		uint16_t link_state;
		uint32_t references;
		char *tok, *t, *name;
		struct interface *iface;

		if (line[0] == '\n') {
			break;
		}

		/* Get rid of pesky newline */
		if ((t = strchr(line, '\n')) != NULL) {
			*t = '\0';
		}

		if (strcmp(line, ":Name:LinkStatus:References:") == 0) {
			continue;
		}

		/* Leading colon... */
		// tok = strtok(line, ":");

		/* name */
		tok = strtok(line, ":");
		if (tok == NULL) {
			fprintf(stderr, "bad line (%s) - missing name\n", line);
			continue;
		}
		name = tok;

		/* link_state */
		tok = strtok(NULL, ":");
		if (tok == NULL) {
			fprintf(stderr, "bad line (%s) - missing link state\n",
				line);
			continue;
		}
		link_state = (uint16_t)strtoul(tok, NULL, 0);

		/* references... */
		tok = strtok(NULL, ":");
		if (tok == NULL) {
			fprintf(stderr, "bad line (%s) - missing references\n",
				line);
			continue;
		}
		references = (uint32_t)strtoul(tok, NULL, 0);

		iface_map->iface = talloc_realloc(iface_map, iface_map->iface,
						  struct interface,
						  iface_map->num + 1);
		if (iface_map->iface == NULL) {
			goto fail;
		}

		iface = &iface_map->iface[iface_map->num];

		iface->name = talloc_strdup(iface_map, name);
		if (iface->name == NULL) {
			goto fail;
		}
		iface->link_up = link_state;
		iface->references = references;

		iface_map->num += 1;
	}

	DEBUG(DEBUG_INFO, ("Parsing interfaces done\n"));
	return true;

fail:
	fprintf(stderr, "Parsing interfaces failed\n");
	return false;
}

static struct vnn_map *vnnmap_init(TALLOC_CTX *mem_ctx)
{
	struct vnn_map *vnn_map;

	vnn_map = talloc_zero(mem_ctx, struct vnn_map);
	if (vnn_map == NULL) {
		fprintf(stderr, "Memory error\n");
		return NULL;
	}
	vnn_map->recmode = CTDB_RECOVERY_ACTIVE;
	vnn_map->generation = INVALID_GENERATION;

	return vnn_map;
}

/* Read vnn map.
 * output:
 *   <GENERATION>
 *   <LMASTER0>
 *   <LMASTER1>
 *   ...
 */

static bool vnnmap_parse(struct vnn_map *vnn_map)
{
	char line[1024];

	while (fgets(line, sizeof(line), stdin) != NULL) {
		uint32_t n;
		char *t;

		if (line[0] == '\n') {
			break;
		}

		/* Get rid of pesky newline */
		if ((t = strchr(line, '\n')) != NULL) {
			*t = '\0';
		}

		n = (uint32_t) strtol(line, NULL, 0);

		/* generation */
		if (vnn_map->generation == INVALID_GENERATION) {
			vnn_map->generation = n;
			continue;
		}

		vnn_map->map = talloc_realloc(vnn_map, vnn_map->map, uint32_t,
					      vnn_map->size + 1);
		if (vnn_map->map == NULL) {
			fprintf(stderr, "Memory error\n");
			goto fail;
		}

		vnn_map->map[vnn_map->size] = n;
		vnn_map->size += 1;
	}

	DEBUG(DEBUG_INFO, ("Parsing vnnmap done\n"));
	return true;

fail:
	fprintf(stderr, "Parsing vnnmap failed\n");
	return false;
}

/*
 * CTDB context setup
 */

static uint32_t new_generation(uint32_t old_generation)
{
	uint32_t generation;

	while (1) {
		generation = random();
		if (generation != INVALID_GENERATION &&
		    generation != old_generation) {
			break;
		}
	}

	return generation;
}

static struct ctdbd_context *ctdbd_setup(TALLOC_CTX *mem_ctx)
{
	struct ctdbd_context *ctdb;
	char line[1024];
	bool status;

	ctdb = talloc_zero(mem_ctx, struct ctdbd_context);
	if (ctdb == NULL) {
		return NULL;
	}

	ctdb->node_map = nodemap_init(ctdb);
	if (ctdb->node_map == NULL) {
		goto fail;
	}

	ctdb->iface_map = interfaces_init(ctdb);
	if (ctdb->iface_map == NULL) {
		goto fail;
	}

	ctdb->vnn_map = vnnmap_init(ctdb);
	if (ctdb->vnn_map == NULL) {
		goto fail;
	}

	while (fgets(line, sizeof(line), stdin) != NULL) {
		char *t;

		if ((t = strchr(line, '\n')) != NULL) {
			*t = '\0';
		}

		if (strcmp(line, "NODEMAP") == 0) {
			status = nodemap_parse(ctdb->node_map);
		} else if (strcmp(line, "IFACES") == 0) {
			status = interfaces_parse(ctdb->iface_map);
		} else if (strcmp(line, "VNNMAP") == 0) {
			status = vnnmap_parse(ctdb->vnn_map);
		} else {
			fprintf(stderr, "Unknown line %s\n", line);
			status = false;
		}

		if (! status) {
			goto fail;
		}
	}

	ctdb->start_time = tevent_timeval_current();
	ctdb->recovery_start_time = tevent_timeval_current();
	ctdb->vnn_map->recmode = CTDB_RECOVERY_NORMAL;
	if (ctdb->vnn_map->generation == INVALID_GENERATION) {
		ctdb->vnn_map->generation =
			new_generation(ctdb->vnn_map->generation);
	}
	ctdb->recovery_end_time = tevent_timeval_current();

	return ctdb;

fail:
	TALLOC_FREE(ctdb);
	return NULL;
}

static bool ctdbd_verify(struct ctdbd_context *ctdb)
{
	struct node *node;
	int i;

	if (ctdb->node_map->num_nodes == 0) {
		return true;
	}

	/* Make sure all the nodes are in order */
	for (i=0; i<ctdb->node_map->num_nodes; i++) {
		node = &ctdb->node_map->node[i];
		if (node->pnn != i) {
			fprintf(stderr, "Expected node %u, found %u\n",
				i, node->pnn);
			return false;
		}
	}

	node = &ctdb->node_map->node[ctdb->node_map->pnn];
	if (node->flags & NODE_FLAGS_DISCONNECTED) {
		DEBUG(DEBUG_INFO, ("Node disconnected, exiting\n"));
		exit(0);
	}

	return true;
}

/*
 * Doing a recovery
 */

struct recover_state {
	struct tevent_context *ev;
	struct ctdbd_context *ctdb;
};

static int recover_check(struct tevent_req *req);
static void recover_wait_done(struct tevent_req *subreq);
static void recover_done(struct tevent_req *subreq);

static struct tevent_req *recover_send(TALLOC_CTX *mem_ctx,
				       struct tevent_context *ev,
				       struct ctdbd_context *ctdb)
{
	struct tevent_req *req;
	struct recover_state *state;
	int ret;

	req = tevent_req_create(mem_ctx, &state, struct recover_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->ctdb = ctdb;

	ret = recover_check(req);
	if (ret != 0) {
		tevent_req_error(req, ret);
		return tevent_req_post(req, ev);
	}

	return req;
}

static int recover_check(struct tevent_req *req)
{
	struct recover_state *state = tevent_req_data(
		req, struct recover_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct tevent_req *subreq;
	bool recovery_disabled;
	int i;

	recovery_disabled = false;
	for (i=0; i<ctdb->node_map->num_nodes; i++) {
		if (ctdb->node_map->node[i].recovery_disabled) {
			recovery_disabled = true;
			break;
		}
	}

	subreq = tevent_wakeup_send(state, state->ev,
				    tevent_timeval_current_ofs(1, 0));
	if (subreq == NULL) {
		return ENOMEM;
	}

	if (recovery_disabled) {
		tevent_req_set_callback(subreq, recover_wait_done, req);
	} else {
		ctdb->recovery_start_time = tevent_timeval_current();
		tevent_req_set_callback(subreq, recover_done, req);
	}

	return 0;
}

static void recover_wait_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	int ret;
	bool status;

	status = tevent_wakeup_recv(subreq);
	TALLOC_FREE(subreq);
	if (! status) {
		tevent_req_error(req, EIO);
		return;
	}

	ret = recover_check(req);
	if (ret != 0) {
		tevent_req_error(req, ret);
	}
}

static void recover_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct recover_state *state = tevent_req_data(
		req, struct recover_state);
	struct ctdbd_context *ctdb = state->ctdb;
	bool status;

	status = tevent_wakeup_recv(subreq);
	TALLOC_FREE(subreq);
	if (! status) {
		tevent_req_error(req, EIO);
		return;
	}

	ctdb->vnn_map->recmode = CTDB_RECOVERY_NORMAL;
	ctdb->recovery_end_time = tevent_timeval_current();
	ctdb->vnn_map->generation = new_generation(ctdb->vnn_map->generation);

	tevent_req_done(req);
}

static bool recover_recv(struct tevent_req *req, int *perr)
{
	int err;

	if (tevent_req_is_unix_error(req, &err)) {
		if (perr != NULL) {
			*perr = err;
		}
		return false;
	}

	return true;
}

/*
 * Routines for ctdb_req_header
 */

static void header_fix_pnn(struct ctdb_req_header *header,
			   struct ctdbd_context *ctdb)
{
	if (header->srcnode == CTDB_CURRENT_NODE) {
		header->srcnode = ctdb->node_map->pnn;
	}

	if (header->destnode == CTDB_CURRENT_NODE) {
		header->destnode = ctdb->node_map->pnn;
	}
}

static struct ctdb_req_header header_reply_control(
					struct ctdb_req_header *header,
					struct ctdbd_context *ctdb)
{
	struct ctdb_req_header reply_header;

	reply_header = (struct ctdb_req_header) {
		.ctdb_magic = CTDB_MAGIC,
		.ctdb_version = CTDB_PROTOCOL,
		.generation = ctdb->vnn_map->generation,
		.operation = CTDB_REPLY_CONTROL,
		.destnode = header->srcnode,
		.srcnode = header->destnode,
		.reqid = header->reqid,
	};

	return reply_header;
}

static struct ctdb_req_header header_reply_message(
					struct ctdb_req_header *header,
					struct ctdbd_context *ctdb)
{
	struct ctdb_req_header reply_header;

	reply_header = (struct ctdb_req_header) {
		.ctdb_magic = CTDB_MAGIC,
		.ctdb_version = CTDB_PROTOCOL,
		.generation = ctdb->vnn_map->generation,
		.operation = CTDB_REQ_MESSAGE,
		.destnode = header->srcnode,
		.srcnode = header->destnode,
		.reqid = 0,
	};

	return reply_header;
}

/*
 * Client state
 */

struct client_state {
	struct tevent_context *ev;
	int fd;
	struct ctdbd_context *ctdb;
	int pnn;
	struct comm_context *comm;
	struct srvid_register_state *rstate;
	int status;
};

/*
 * Send replies to controls and messages
 */

static void client_reply_done(struct tevent_req *subreq);

static void client_send_message(struct tevent_req *req,
				struct ctdb_req_header *header,
				struct ctdb_req_message_data *message)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct tevent_req *subreq;
	struct ctdb_req_header reply_header;
	uint8_t *buf;
	size_t datalen, buflen;
	int ret;

	reply_header = header_reply_message(header, ctdb);

	datalen = ctdb_req_message_data_len(&reply_header, message);
	ret = ctdb_allocate_pkt(state, datalen, &buf, &buflen);
	if (ret != 0) {
		tevent_req_error(req, ret);
		return;
	}

	ret = ctdb_req_message_data_push(&reply_header, message,
					 buf, &buflen);
	if (ret != 0) {
		tevent_req_error(req, ret);
		return;
	}

	DEBUG(DEBUG_INFO, ("message srvid = 0x%"PRIx64"\n", message->srvid));

	subreq = comm_write_send(state, state->ev, state->comm, buf, buflen);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, client_reply_done, req);

	talloc_steal(subreq, buf);
}

static void client_send_control(struct tevent_req *req,
				struct ctdb_req_header *header,
				struct ctdb_reply_control *reply)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct tevent_req *subreq;
	struct ctdb_req_header reply_header;
	uint8_t *buf;
	size_t datalen, buflen;
	int ret;

	reply_header = header_reply_control(header, ctdb);

	datalen = ctdb_reply_control_len(&reply_header, reply);
	ret = ctdb_allocate_pkt(state, datalen, &buf, &buflen);
	if (ret != 0) {
		tevent_req_error(req, ret);
		return;
	}

	ret = ctdb_reply_control_push(&reply_header, reply, buf, &buflen);
	if (ret != 0) {
		tevent_req_error(req, ret);
		return;
	}

	DEBUG(DEBUG_INFO, ("reply opcode = %u\n", reply->rdata.opcode));

	subreq = comm_write_send(state, state->ev, state->comm, buf, buflen);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, client_reply_done, req);

	talloc_steal(subreq, buf);
}

static void client_reply_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	int ret;
	bool status;

	status = comm_write_recv(subreq, &ret);
	TALLOC_FREE(subreq);
	if (! status) {
		tevent_req_error(req, ret);
	}
}

/*
 * Handling protocol - controls
 */

static void control_process_exists(TALLOC_CTX *mem_ctx,
				   struct tevent_req *req,
				   struct ctdb_req_header *header,
				   struct ctdb_req_control *request)
{
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = kill(request->rdata.data.pid, 0);
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

static void control_ping(TALLOC_CTX *mem_ctx,
			 struct tevent_req *req,
			 struct ctdb_req_header *header,
			 struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = ctdb->num_clients;
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

static void control_getvnnmap(TALLOC_CTX *mem_ctx,
			      struct tevent_req *req,
			      struct ctdb_req_header *header,
			      struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct ctdb_vnn_map *vnnmap;

	reply.rdata.opcode = request->opcode;

	vnnmap = talloc_zero(mem_ctx, struct ctdb_vnn_map);
	if (vnnmap == NULL) {
		reply.status = ENOMEM;
		reply.errmsg = "Memory error";
	} else {
		vnnmap->generation = ctdb->vnn_map->generation;
		vnnmap->size = ctdb->vnn_map->size;
		vnnmap->map = ctdb->vnn_map->map;

		reply.rdata.data.vnnmap = vnnmap;
		reply.status = 0;
		reply.errmsg = NULL;
	}

	client_send_control(req, header, &reply);
}

static void control_get_recmode(TALLOC_CTX *mem_ctx,
				struct tevent_req *req,
				struct ctdb_req_header *header,
				struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = ctdb->vnn_map->recmode;
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

struct set_recmode_state {
	struct tevent_req *req;
	struct ctdbd_context *ctdb;
	struct ctdb_req_header header;
	struct ctdb_reply_control reply;
};

static void set_recmode_callback(struct tevent_req *subreq)
{
	struct set_recmode_state *substate = tevent_req_callback_data(
		subreq, struct set_recmode_state);
	bool status;
	int ret;

	status = recover_recv(subreq, &ret);
	TALLOC_FREE(subreq);
	if (! status) {
		substate->reply.status = ret;
		substate->reply.errmsg = "recovery failed";
	} else {
		substate->reply.status = 0;
		substate->reply.errmsg = NULL;
	}

	client_send_control(substate->req, &substate->header, &substate->reply);
	talloc_free(substate);
}

static void control_set_recmode(TALLOC_CTX *mem_ctx,
				struct tevent_req *req,
				struct ctdb_req_header *header,
				struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct tevent_req *subreq;
	struct ctdbd_context *ctdb = state->ctdb;
	struct set_recmode_state *substate;
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;

	if (request->rdata.data.recmode == CTDB_RECOVERY_NORMAL) {
		reply.status = -1;
		reply.errmsg = "Client cannot set recmode to NORMAL";
		goto fail;
	}

	substate = talloc_zero(ctdb, struct set_recmode_state);
	if (substate == NULL) {
		reply.status = -1;
		reply.errmsg = "Memory error";
		goto fail;
	}

	substate->req = req;
	substate->ctdb = ctdb;
	substate->header = *header;
	substate->reply.rdata.opcode = request->opcode;

	subreq = recover_send(substate, state->ev, state->ctdb);
	if (subreq == NULL) {
		talloc_free(substate);
		goto fail;
	}
	tevent_req_set_callback(subreq, set_recmode_callback, substate);

	ctdb->vnn_map->recmode = CTDB_RECOVERY_ACTIVE;
	return;

fail:
	client_send_control(req, header, &reply);

}

static int srvid_register_state_destructor(struct srvid_register_state *rstate)
{
	DLIST_REMOVE(rstate->ctdb->rstate, rstate);
	return 0;
}

static void control_register_srvid(TALLOC_CTX *mem_ctx,
				   struct tevent_req *req,
				   struct ctdb_req_header *header,
				   struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct srvid_register_state *rstate;

	reply.rdata.opcode = request->opcode;

	rstate = talloc_zero(ctdb, struct srvid_register_state);
	if (rstate == NULL) {
		reply.status = -1;
		reply.errmsg = "Memory error";
		goto fail;
	}
	rstate->ctdb = ctdb;
	rstate->srvid = request->srvid;

	talloc_set_destructor(rstate, srvid_register_state_destructor);

	DLIST_ADD_END(ctdb->rstate, rstate);

	DEBUG(DEBUG_INFO, ("Register srvid 0x%"PRIx64"\n", rstate->srvid));

	reply.status = 0;
	reply.errmsg = NULL;

fail:
	client_send_control(req, header, &reply);
}

static void control_deregister_srvid(TALLOC_CTX *mem_ctx,
				     struct tevent_req *req,
				     struct ctdb_req_header *header,
				     struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct srvid_register_state *rstate = NULL;

	reply.rdata.opcode = request->opcode;

	for (rstate = ctdb->rstate; rstate != NULL; rstate = rstate->next) {
		if (rstate->srvid == request->srvid) {
			break;
		}
	}

	if (rstate == NULL) {
		reply.status = -1;
		reply.errmsg = "srvid not registered";
		goto fail;
	}

	DEBUG(DEBUG_INFO, ("Deregister srvid 0x%"PRIx64"\n", rstate->srvid));
	talloc_free(rstate);

	reply.status = 0;
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
	return;

fail:
	TALLOC_FREE(rstate);
	client_send_control(req, header, &reply);
}

static void control_get_pid(TALLOC_CTX *mem_ctx,
			    struct tevent_req *req,
			    struct ctdb_req_header *header,
			    struct ctdb_req_control *request)
{
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = getpid();
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

static void control_get_recmaster(TALLOC_CTX *mem_ctx,
				  struct tevent_req *req,
				  struct ctdb_req_header *header,
				  struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = ctdb->node_map->recmaster;
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

static void control_get_pnn(TALLOC_CTX *mem_ctx,
			    struct tevent_req *req,
			    struct ctdb_req_header *header,
			    struct ctdb_req_control *request)
{
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = header->destnode;
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

static void control_shutdown(TALLOC_CTX *mem_ctx,
			     struct tevent_req *req,
			     struct ctdb_req_header *hdr,
			     struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);

	state->status = 99;
}

static void control_uptime(TALLOC_CTX *mem_ctx,
			   struct tevent_req *req,
			   struct ctdb_req_header *header,
			   struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct ctdb_uptime *uptime;;

	reply.rdata.opcode = request->opcode;

	uptime = talloc_zero(mem_ctx, struct ctdb_uptime);
	if (uptime == NULL) {
		goto fail;
	}

	uptime->current_time = tevent_timeval_current();
	uptime->ctdbd_start_time = ctdb->start_time;
	uptime->last_recovery_started = ctdb->recovery_start_time;
	uptime->last_recovery_finished = ctdb->recovery_end_time;

	reply.rdata.data.uptime = uptime;
	reply.status = 0;
	reply.errmsg = NULL;
	client_send_control(req, header, &reply);
	return;

fail:
	reply.status = -1;
	reply.errmsg = "Memory error";
	client_send_control(req, header, &reply);
}

static void control_reload_nodes_file(TALLOC_CTX *mem_ctx,
				      struct tevent_req *req,
				      struct ctdb_req_header *header,
				      struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct ctdb_node_map *nodemap;
	struct node_map *node_map = ctdb->node_map;
	int i;

	reply.rdata.opcode = request->opcode;

	nodemap = read_nodes_file(mem_ctx, header->destnode);
	if (nodemap == NULL) {
		goto fail;
	}

	for (i=0; i<nodemap->num; i++) {
		struct node *node;

		if (i < node_map->num_nodes &&
		    ctdb_sock_addr_same(&nodemap->node[i].addr,
					&node_map->node[i].addr)) {
			continue;
		}

		if (nodemap->node[i].flags & NODE_FLAGS_DELETED) {
			node = &node_map->node[i];

			node->flags |= NODE_FLAGS_DELETED;
			parse_ip("0.0.0.0", NULL, 0, &node->addr);

			continue;
		}

		if (i < node_map->num_nodes &&
		    node_map->node[i].flags & NODE_FLAGS_DELETED) {
			node = &node_map->node[i];

			node->flags &= ~NODE_FLAGS_DELETED;
			node->addr = nodemap->node[i].addr;

			continue;
		}

		node_map->node = talloc_realloc(node_map, node_map->node,
						struct node,
						node_map->num_nodes+1);
		if (node_map->node == NULL) {
			goto fail;
		}
		node = &node_map->node[node_map->num_nodes];

		node->addr = nodemap->node[i].addr;
		node->pnn = nodemap->node[i].pnn;
		node->flags = 0;
		node->capabilities = CTDB_CAP_DEFAULT;
		node->recovery_disabled = false;
		node->recovery_substate = NULL;

		node_map->num_nodes += 1;
	}

	talloc_free(nodemap);

	reply.status = 0;
	reply.errmsg = NULL;
	client_send_control(req, header, &reply);
	return;

fail:
	reply.status = -1;
	reply.errmsg = "Memory error";
	client_send_control(req, header, &reply);
}

static void control_get_capabilities(TALLOC_CTX *mem_ctx,
				     struct tevent_req *req,
				     struct ctdb_req_header *header,
				     struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct node *node;
	uint32_t caps = 0;

	reply.rdata.opcode = request->opcode;

	node = &ctdb->node_map->node[header->destnode];
	caps = node->capabilities;

	if (node->flags & NODE_FLAGS_FAKE_TIMEOUT) {
		/* Don't send reply */
		return;
	}

	reply.rdata.data.caps = caps;
	reply.status = 0;
	reply.errmsg = NULL;

	client_send_control(req, header, &reply);
}

static void control_get_nodemap(TALLOC_CTX *mem_ctx,
				struct tevent_req *req,
				struct ctdb_req_header *header,
				struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct ctdb_node_map *nodemap;
	struct node *node;
	int i;

	reply.rdata.opcode = request->opcode;

	nodemap = talloc_zero(mem_ctx, struct ctdb_node_map);
	if (nodemap == NULL) {
		goto fail;
	}

	nodemap->num = ctdb->node_map->num_nodes;
	nodemap->node = talloc_array(nodemap, struct ctdb_node_and_flags,
				     nodemap->num);
	if (nodemap->node == NULL) {
		goto fail;
	}

	for (i=0; i<nodemap->num; i++) {
		node = &ctdb->node_map->node[i];
		nodemap->node[i] = (struct ctdb_node_and_flags) {
			.pnn = node->pnn,
			.flags = node->flags,
			.addr = node->addr,
		};
	}

	reply.rdata.data.nodemap = nodemap;
	reply.status = 0;
	reply.errmsg = NULL;
	client_send_control(req, header, &reply);
	return;

fail:
	reply.status = -1;
	reply.errmsg = "Memory error";
	client_send_control(req, header, &reply);
}

static void control_get_ifaces(TALLOC_CTX *mem_ctx,
			       struct tevent_req *req,
			       struct ctdb_req_header *header,
			       struct ctdb_req_control *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_reply_control reply;
	struct ctdb_iface_list *iface_list;
	struct interface *iface;
	int i;

	reply.rdata.opcode = request->opcode;

	iface_list = talloc_zero(mem_ctx, struct ctdb_iface_list);
	if (iface_list == NULL) {
		goto fail;
	}

	iface_list->num = ctdb->iface_map->num;
	iface_list->iface = talloc_array(iface_list, struct ctdb_iface,
					 iface_list->num);
	if (iface_list->iface == NULL) {
		goto fail;
	}

	for (i=0; i<iface_list->num; i++) {
		iface = &ctdb->iface_map->iface[i];
		iface_list->iface[i] = (struct ctdb_iface) {
			.link_state = iface->link_up,
			.references = iface->references,
		};
		strncpy(iface_list->iface[i].name, iface->name,
			CTDB_IFACE_SIZE+2);
	}

	reply.rdata.data.iface_list = iface_list;
	reply.status = 0;
	reply.errmsg = NULL;
	client_send_control(req, header, &reply);
	return;

fail:
	reply.status = -1;
	reply.errmsg = "Memory error";
	client_send_control(req, header, &reply);
}

static void control_get_nodes_file(TALLOC_CTX *mem_ctx,
				   struct tevent_req *req,
				   struct ctdb_req_header *header,
				   struct ctdb_req_control *request)
{
	struct ctdb_reply_control reply;
	struct ctdb_node_map *nodemap;

	reply.rdata.opcode = request->opcode;

	nodemap = read_nodes_file(mem_ctx, header->destnode);
	if (nodemap == NULL) {
		goto fail;
	}

	reply.rdata.data.nodemap = nodemap;
	reply.status = 0;
	reply.errmsg = NULL;
	client_send_control(req, header, &reply);
	return;

fail:
	reply.status = -1;
	reply.errmsg = "Failed to read nodes file";
	client_send_control(req, header, &reply);
}

static void control_error(TALLOC_CTX *mem_ctx,
			  struct tevent_req *req,
			  struct ctdb_req_header *header,
			  struct ctdb_req_control *request)
{
	struct ctdb_reply_control reply;

	reply.rdata.opcode = request->opcode;
	reply.status = -1;
	reply.errmsg = "Not implemented";

	client_send_control(req, header, &reply);
}

/*
 * Handling protocol - messages
 */

struct disable_recoveries_state {
	struct node *node;
};

static void disable_recoveries_callback(struct tevent_req *subreq)
{
	struct disable_recoveries_state *substate = tevent_req_callback_data(
		subreq, struct disable_recoveries_state);
	bool status;

	status = tevent_wakeup_recv(subreq);
	TALLOC_FREE(subreq);
	if (! status) {
		DEBUG(DEBUG_INFO, ("tevent_wakeup_recv failed\n"));
	}

	substate->node->recovery_disabled = false;
	TALLOC_FREE(substate->node->recovery_substate);
}

static void message_disable_recoveries(TALLOC_CTX *mem_ctx,
				       struct tevent_req *req,
				       struct ctdb_req_header *header,
				       struct ctdb_req_message *request)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct tevent_req *subreq;
	struct ctdbd_context *ctdb = state->ctdb;
	struct disable_recoveries_state *substate;
	struct ctdb_disable_message *disable = request->data.disable;
	struct ctdb_req_message_data reply;
	struct node *node;
	int ret = -1;
	TDB_DATA data;

	node = &ctdb->node_map->node[header->destnode];

	if (disable->timeout == 0) {
		TALLOC_FREE(node->recovery_substate);
		node->recovery_disabled = false;
		DEBUG(DEBUG_INFO, ("Enabled recoveries on node %u\n",
				   header->destnode));
		goto done;
	}

	substate = talloc_zero(ctdb->node_map,
			       struct disable_recoveries_state);
	if (substate == NULL) {
		goto fail;
	}

	substate->node = node;

	subreq = tevent_wakeup_send(substate, state->ev,
				    tevent_timeval_current_ofs(
					    disable->timeout, 0));
	if (subreq == NULL) {
		talloc_free(substate);
		goto fail;
	}
	tevent_req_set_callback(subreq, disable_recoveries_callback, substate);

	DEBUG(DEBUG_INFO, ("Disabled recoveries for %d seconds on node %u\n",
			   disable->timeout, header->destnode));
	node->recovery_substate = substate;
	node->recovery_disabled = true;

done:
	ret = header->destnode;

fail:
	reply.srvid = disable->srvid;
	data.dptr = (uint8_t *)&ret;
	data.dsize = sizeof(int);
	reply.data = data;

	client_send_message(req, header, &reply);
}

/*
 * Handle a single client
 */

static void client_read_handler(uint8_t *buf, size_t buflen,
				void *private_data);
static void client_dead_handler(void *private_data);
static void client_process_packet(struct tevent_req *req,
				  uint8_t *buf, size_t buflen);
static void client_process_message(struct tevent_req *req,
				   uint8_t *buf, size_t buflen);
static void client_process_control(struct tevent_req *req,
				   uint8_t *buf, size_t buflen);
static void client_reply_done(struct tevent_req *subreq);

static struct tevent_req *client_send(TALLOC_CTX *mem_ctx,
				      struct tevent_context *ev,
				      int fd, struct ctdbd_context *ctdb,
				      int pnn)
{
	struct tevent_req *req;
	struct client_state *state;
	int ret;

	req = tevent_req_create(mem_ctx, &state, struct client_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->fd = fd;
	state->ctdb = ctdb;
	state->pnn = pnn;

	ret = comm_setup(state, ev, fd, client_read_handler, req,
			 client_dead_handler, req, &state->comm);
	if (ret != 0) {
		tevent_req_error(req, ret);
		return tevent_req_post(req, ev);
	}

	DEBUG(DEBUG_INFO, ("New client fd=%d\n", fd));

	return req;
}

static void client_read_handler(uint8_t *buf, size_t buflen,
				void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	struct ctdb_req_header header;
	int ret, i;

	ret = ctdb_req_header_pull(buf, buflen, &header);
	if (ret != 0) {
		return;
	}

	if (buflen != header.length) {
		return;
	}

	ret = ctdb_req_header_verify(&header, 0);
	if (ret != 0) {
		return;
	}

	header_fix_pnn(&header, ctdb);

	if (header.destnode == CTDB_BROADCAST_ALL) {
		for (i=0; i<ctdb->node_map->num_nodes; i++) {
			header.destnode = i;

			ctdb_req_header_push(&header, buf);
			client_process_packet(req, buf, buflen);
		}
		return;
	}

	if (header.destnode == CTDB_BROADCAST_CONNECTED) {
		for (i=0; i<ctdb->node_map->num_nodes; i++) {
			if (ctdb->node_map->node[i].flags &
			    NODE_FLAGS_DISCONNECTED) {
				continue;
			}

			header.destnode = i;

			ctdb_req_header_push(&header, buf);
			client_process_packet(req, buf, buflen);
		}
		return;
	}

	if (header.destnode > ctdb->node_map->num_nodes) {
		fprintf(stderr, "Invalid destination pnn 0x%x\n",
			header.destnode);
		return;
	}


	if (ctdb->node_map->node[header.destnode].flags & NODE_FLAGS_DISCONNECTED) {
		fprintf(stderr, "Packet for disconnected node pnn %u\n",
			header.destnode);
		return;
	}

	ctdb_req_header_push(&header, buf);
	client_process_packet(req, buf, buflen);
}

static void client_dead_handler(void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);

	tevent_req_done(req);
}

static void client_process_packet(struct tevent_req *req,
				  uint8_t *buf, size_t buflen)
{
	struct ctdb_req_header header;
	int ret;

	ret = ctdb_req_header_pull(buf, buflen, &header);
	if (ret != 0) {
		return;
	}

	switch (header.operation) {
	case CTDB_REQ_MESSAGE:
		client_process_message(req, buf, buflen);
		break;

	case CTDB_REQ_CONTROL:
		client_process_control(req, buf, buflen);
		break;

	default:
		break;
	}
}

static void client_process_message(struct tevent_req *req,
				   uint8_t *buf, size_t buflen)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	TALLOC_CTX *mem_ctx;
	struct ctdb_req_header header;
	struct ctdb_req_message request;
	uint64_t srvid;
	int ret;

	mem_ctx = talloc_new(state);
	if (tevent_req_nomem(mem_ctx, req)) {
		return;
	}

	ret = ctdb_req_message_pull(buf, buflen, &header, mem_ctx, &request);
	if (ret != 0) {
		talloc_free(mem_ctx);
		tevent_req_error(req, ret);
		return;
	}

	header_fix_pnn(&header, ctdb);

	srvid = request.srvid;
	DEBUG(DEBUG_INFO, ("request srvid = 0x%"PRIx64"\n", srvid));

	if (srvid == CTDB_SRVID_DISABLE_RECOVERIES) {
		message_disable_recoveries(mem_ctx, req, &header, &request);
	}

	/* check srvid */
	talloc_free(mem_ctx);
}

static void client_process_control(struct tevent_req *req,
				   uint8_t *buf, size_t buflen)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	struct ctdbd_context *ctdb = state->ctdb;
	TALLOC_CTX *mem_ctx;
	struct ctdb_req_header header;
	struct ctdb_req_control request;
	int ret;

	mem_ctx = talloc_new(state);
	if (tevent_req_nomem(mem_ctx, req)) {
		return;
	}

	ret = ctdb_req_control_pull(buf, buflen, &header, mem_ctx, &request);
	if (ret != 0) {
		talloc_free(mem_ctx);
		tevent_req_error(req, ret);
		return;
	}

	header_fix_pnn(&header, ctdb);

	DEBUG(DEBUG_INFO, ("request opcode = %u\n", request.opcode));

	switch (request.opcode) {
	case CTDB_CONTROL_PROCESS_EXISTS:
		control_process_exists(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_PING:
		control_ping(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GETVNNMAP:
		control_getvnnmap(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_RECMODE:
		control_get_recmode(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_SET_RECMODE:
		control_set_recmode(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_REGISTER_SRVID:
		control_register_srvid(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_DEREGISTER_SRVID:
		control_deregister_srvid(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_PID:
		control_get_pid(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_RECMASTER:
		control_get_recmaster(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_PNN:
		control_get_pnn(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_SHUTDOWN:
		control_shutdown(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_UPTIME:
		control_uptime(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_RELOAD_NODES_FILE:
		control_reload_nodes_file(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_CAPABILITIES:
		control_get_capabilities(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_NODEMAP:
		control_get_nodemap(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_IFACES:
		control_get_ifaces(mem_ctx, req, &header, &request);
		break;

	case CTDB_CONTROL_GET_NODES_FILE:
		control_get_nodes_file(mem_ctx, req, &header, &request);
		break;

	default:
		if (! (request.flags & CTDB_CTRL_FLAG_NOREPLY)) {
			control_error(mem_ctx, req, &header, &request);
		}
		break;
	}

	talloc_free(mem_ctx);
}

static int client_recv(struct tevent_req *req, int *perr)
{
	struct client_state *state = tevent_req_data(
		req, struct client_state);
	int err;

	DEBUG(DEBUG_INFO, ("Client done fd=%d\n", state->fd));
	close(state->fd);

	if (tevent_req_is_unix_error(req, &err)) {
		if (perr != NULL) {
			*perr = err;
		}
		return -1;
	}

	return state->status;
}

/*
 * Fake CTDB server
 */

struct server_state {
	struct tevent_context *ev;
	struct ctdbd_context *ctdb;
	int fd;
};

static void server_new_client(struct tevent_req *subreq);
static void server_client_done(struct tevent_req *subreq);

static struct tevent_req *server_send(TALLOC_CTX *mem_ctx,
				      struct tevent_context *ev,
				      struct ctdbd_context *ctdb,
				      int fd)
{
	struct tevent_req *req, *subreq;
	struct server_state *state;

	req = tevent_req_create(mem_ctx, &state, struct server_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->ctdb = ctdb;
	state->fd = fd;

	subreq = accept_send(state, ev, fd);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, server_new_client, req);

	return req;
}

static void server_new_client(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct server_state *state = tevent_req_data(
		req, struct server_state);
	struct ctdbd_context *ctdb = state->ctdb;
	int client_fd;
	int ret = 0;

	client_fd = accept_recv(subreq, NULL, NULL, &ret);
	TALLOC_FREE(subreq);
	if (client_fd == -1) {
		tevent_req_error(req, ret);
		return;
	}

	subreq = client_send(state, state->ev, client_fd,
			     ctdb, ctdb->node_map->pnn);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, server_client_done, req);

	ctdb->num_clients += 1;

	subreq = accept_send(state, state->ev, state->fd);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, server_new_client, req);
}

static void server_client_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct server_state *state = tevent_req_data(
		req, struct server_state);
	struct ctdbd_context *ctdb = state->ctdb;
	int ret = 0;
	int status;

	status = client_recv(subreq, &ret);
	TALLOC_FREE(subreq);
	if (status < 0) {
		tevent_req_error(req, ret);
		return;
	}

	ctdb->num_clients -= 1;

	if (status == 99) {
		/* Special status, to shutdown server */
		DEBUG(DEBUG_INFO, ("Shutting down server\n"));
		tevent_req_done(req);
	}
}

static bool server_recv(struct tevent_req *req, int *perr)
{
	int err;

	if (tevent_req_is_unix_error(req, &err)) {
		if (perr != NULL) {
			*perr = err;
		}
		return false;
	}
	return true;
}

/*
 * Main functions
 */

static int socket_init(const char *sockpath)
{
	struct sockaddr_un addr;
	size_t len;
	int ret, fd;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	len = strlcpy(addr.sun_path, sockpath, sizeof(addr.sun_path));
	if (len >= sizeof(addr.sun_path)) {
		fprintf(stderr, "path too long: %s\n", sockpath);
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		fprintf(stderr, "socket failed - %s\n", sockpath);
		return -1;
	}

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret != 0) {
		fprintf(stderr, "bind failed - %s\n", sockpath);
		goto fail;
	}

	ret = listen(fd, 10);
	if (ret != 0) {
		fprintf(stderr, "listen failed\n");
		goto fail;
	}

	DEBUG(DEBUG_INFO, ("Socket init done\n"));

	return fd;

fail:
	if (fd != -1) {
		close(fd);
	}
	return -1;
}

static struct options {
	const char *sockpath;
	const char *pidfile;
	const char *debuglevel;
} options;

static struct poptOption cmdline_options[] = {
	{ "socket", 's', POPT_ARG_STRING, &options.sockpath, 0,
		"Unix domain socket path", "filename" },
	{ "pidfile", 'p', POPT_ARG_STRING, &options.pidfile, 0,
		"pid file", "filename" } ,
	{ "debug", 'd', POPT_ARG_STRING, &options.debuglevel, 0,
		"debug level", "ERR|WARNING|NOTICE|INFO|DEBUG" } ,
};

static void cleanup(void)
{
	unlink(options.sockpath);
	unlink(options.pidfile);
}

static void signal_handler(int sig)
{
	cleanup();
	exit(0);
}

static void start_server(TALLOC_CTX *mem_ctx, struct tevent_context *ev,
			 struct ctdbd_context *ctdb, int fd, int pfd)
{
	struct tevent_req *req;
	int ret = 0;
	ssize_t len;

	atexit(cleanup);
	signal(SIGTERM, signal_handler);

	req = server_send(mem_ctx, ev, ctdb, fd);
	if (req == NULL) {
		fprintf(stderr, "Memory error\n");
		exit(1);
	}

	len = write(pfd, &ret, sizeof(ret));
	if (len != sizeof(ret)) {
		fprintf(stderr, "Failed to send message to parent\n");
		exit(1);
	}
	close(pfd);

	tevent_req_poll(req, ev);

	server_recv(req, &ret);
	if (ret != 0) {
		exit(1);
	}
}

int main(int argc, const char *argv[])
{
	TALLOC_CTX *mem_ctx;
	struct ctdbd_context *ctdb;
	struct tevent_context *ev;
	enum debug_level debug_level;
	poptContext pc;
	int opt, fd, ret, pfd[2];
	ssize_t len;
	pid_t pid;
	FILE *fp;

	pc = poptGetContext(argv[0], argc, argv, cmdline_options,
			    POPT_CONTEXT_KEEP_FIRST);
	while ((opt = poptGetNextOpt(pc)) != -1) {
		fprintf(stderr, "Invalid option %s\n", poptBadOption(pc, 0));
		exit(1);
	}

	if (options.sockpath == NULL) {
		fprintf(stderr, "Please specify socket path\n");
		poptPrintHelp(pc, stdout, 0);
		exit(1);
	}

	if (options.pidfile == NULL) {
		fprintf(stderr, "Please specify pid file\n");
		poptPrintHelp(pc, stdout, 0);
		exit(1);
	}

	if (options.debuglevel == NULL) {
		DEBUGLEVEL = debug_level_to_int(DEBUG_ERR);
	} else {
		if (debug_level_parse(options.debuglevel, &debug_level)) {
			DEBUGLEVEL = debug_level_to_int(debug_level);
		} else {
			fprintf(stderr, "Invalid debug level\n");
			poptPrintHelp(pc, stdout, 0);
			exit(1);
		}
	}

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		fprintf(stderr, "Memory error\n");
		exit(1);
	}

	ctdb = ctdbd_setup(mem_ctx);
	if (ctdb == NULL) {
		exit(1);
	}

	if (! ctdbd_verify(ctdb)) {
		exit(1);
	}

	ev = tevent_context_init(mem_ctx);
	if (ev == NULL) {
		fprintf(stderr, "Memory error\n");
		exit(1);
	}

	fd = socket_init(options.sockpath);
	if (fd == -1) {
		exit(1);
	}

	ret = pipe(pfd);
	if (ret != 0) {
		fprintf(stderr, "Failed to create pipe\n");
		cleanup();
		exit(1);
	}

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Failed to fork\n");
		cleanup();
		exit(1);
	}

	if (pid == 0) {
		/* Child */
		close(pfd[0]);
		start_server(mem_ctx, ev, ctdb, fd, pfd[1]);
		exit(1);
	}

	/* Parent */
	close(pfd[1]);

	len = read(pfd[0], &ret, sizeof(ret));
	close(pfd[0]);
	if (len != sizeof(ret)) {
		fprintf(stderr, "len = %zi\n", len);
		fprintf(stderr, "Failed to get message from child\n");
		kill(pid, SIGTERM);
		exit(1);
	}

	fp = fopen(options.pidfile, "w");
	if (fp == NULL) {
		fprintf(stderr, "Failed to open pid file %s\n",
			options.pidfile);
		kill(pid, SIGTERM);
		exit(1);
	}
	fprintf(fp, "%d\n", pid);
	fclose(fp);

	return 0;
}