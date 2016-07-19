/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include "test.h"

/* make sure /dev/busX exists, is a cdev and accessible */
static void test_api_cdev(void)
{
	struct stat st;
	int r;

	r = access(test_path, F_OK);
	assert(r >= 0);

	r = stat(test_path, &st);
	assert(r >= 0);
	assert((st.st_mode & S_IFMT) == S_IFCHR);

	r = open(test_path, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
	assert(r >= 0);
	close(r);
}

/* make sure we can open and use /dev/busX via bus1_client */
static void test_api_client(void)
{
	struct bus1_client *c;
	int r, fd;

	r = bus1_client_new_from_path(&c, test_path);
	assert(r >= 0);

	c = bus1_client_free(c);
	assert(!c);

	c = bus1_client_free(NULL);
	assert(!c);

	fd = open(test_path, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
	assert(fd >= 0);

	r = bus1_client_new_from_fd(&c, fd); /* consumes @fd on success */
	assert(r >= 0);

	c = bus1_client_free(c);
	assert(!c);
}

/* make sure basic connect + clone works */
static void test_api_connect(void)
{
	struct bus1_cmd_peer_init query;
	struct bus1_client *c1, *c2;
	uint64_t node, handle;
	int r, fd;

	r = bus1_client_new_from_path(&c1, test_path);
	assert(r >= 0);

	/* verify clone fails if origin is unconnected */

	node = BUS1_NODE_FLAG_MANAGED | BUS1_NODE_FLAG_ALLOCATE;
	handle = BUS1_HANDLE_INVALID;
	fd = -1;
	r = bus1_client_clone(c1, &node, &handle, &fd, BUS1_CLIENT_POOL_SIZE);
	assert(r < 0);
	assert(node == (BUS1_NODE_FLAG_MANAGED | BUS1_NODE_FLAG_ALLOCATE));
	assert(handle == BUS1_HANDLE_INVALID);
	assert(fd == -1);

	memset(&query, 0, sizeof(query));
	r = bus1_client_ioctl(c1, BUS1_CMD_PEER_QUERY, &query);
	assert(r == -ENOTCONN);

	/* connect @c1 properly */

	r = bus1_client_init(c1, BUS1_CLIENT_POOL_SIZE);
	assert(r >= 0);

	memset(&query, 0, sizeof(query));
	r = bus1_client_ioctl(c1, BUS1_CMD_PEER_QUERY, &query);
	assert(r >= 0);
	assert(query.max_bytes == BUS1_CLIENT_POOL_SIZE);

	/* disconnect and reconnect @c1 */

	c1 = bus1_client_free(c1);
	assert(!c1);

	r = bus1_client_new_from_path(&c1, test_path);
	assert(r >= 0);

	r = bus1_client_init(c1, BUS1_CLIENT_POOL_SIZE);
	assert(r >= 0);

	/* clone new peer from @c1 and create @c2 from it */

	r = bus1_client_clone(c1, &node, &handle, &fd, BUS1_CLIENT_POOL_SIZE);
	assert(r >= 0);
	assert(node != BUS1_HANDLE_INVALID);
	assert(handle != BUS1_HANDLE_INVALID);
	assert(fd >= 0);

	r = bus1_client_new_from_fd(&c2, fd);
	assert(r >= 0);

	memset(&query, 0, sizeof(query));
	r = bus1_client_ioctl(c2, BUS1_CMD_PEER_QUERY, &query);
	assert(r >= 0);
	assert(query.max_bytes == BUS1_CLIENT_POOL_SIZE);

	c2 = bus1_client_free(c2);
	assert(!c2);

	/* drop @c1 */

	c1 = bus1_client_free(c1);
	assert(!c1);
}

/* make sure basic handle-release/destroy (with notifications) works */
static void test_api_handle(void)
{
	struct bus1_cmd_recv recv;
	struct bus1_client *c1, *c2;
	uint64_t node, handle;
	int r, fd;

	/* create new peer and one clone */

	r = bus1_client_new_from_path(&c1, test_path);
	assert(r >= 0);
	r = bus1_client_init(c1, BUS1_CLIENT_POOL_SIZE);
	assert(r >= 0);

	node = BUS1_NODE_FLAG_MANAGED | BUS1_NODE_FLAG_ALLOCATE;
	handle = BUS1_HANDLE_INVALID;
	fd = -1;
	r = bus1_client_clone(c1, &node, &handle, &fd, BUS1_CLIENT_POOL_SIZE);
	assert(r >= 0);
	assert(node != (BUS1_NODE_FLAG_MANAGED | BUS1_NODE_FLAG_ALLOCATE));
	assert(handle != BUS1_HANDLE_INVALID);
	assert(fd >= 0);

	r = bus1_client_new_from_fd(&c2, fd);
	assert(r >= 0);

	/* verify clone-handle has no DESTROY access */

	r = bus1_client_node_destroy(c2, handle);
	assert(r < 0);
	assert(r == -ENXIO);

	/* verify clone-handle can release its handle exactly once */

	r = bus1_client_handle_release(c2, handle);
	assert(r >= 0);
	r = bus1_client_handle_release(c2, handle);
	assert(r < 0);
	assert(r == -ENXIO);

	/* verify that no notification has been queued, yet */

	recv = (struct bus1_cmd_recv){};
	r = bus1_client_recv(c1, &recv);
	assert(r == -EAGAIN);
	r = bus1_client_recv(c2, &recv);
	assert(r == -EAGAIN);

	/* verify that the owner can release its handle exactly once */

	r = bus1_client_handle_release(c1, handle);
	assert(r >= 0);
	r = bus1_client_handle_release(c1, handle);
	assert(r < 0);
	assert(r == -ENXIO);

	/* verify that a release notification was queued */

	recv = (struct bus1_cmd_recv){};
	r = bus1_client_recv(c1, &recv);
	assert(r >= 0);
	assert(recv.type == BUS1_MSG_NODE_RELEASE);

	/* verify that the owner can destroy its handle exactly once */

	r = bus1_client_node_destroy(c1, handle);
	assert(r >= 0);
	r = bus1_client_node_destroy(c1, handle);
	assert(r < 0);
	assert(r == -ENXIO);

	/* verify that a destruction notification was queued */

	recv = (struct bus1_cmd_recv){};
	r = bus1_client_recv(c1, &recv);
	assert(r >= 0);
	assert(recv.type == BUS1_MSG_NODE_DESTROY);

	/* verify that both queues are empty (no unexpected notifications) */

	recv = (struct bus1_cmd_recv){};
	r = bus1_client_recv(c1, &recv);
	assert(r == -EAGAIN);
	r = bus1_client_recv(c2, &recv);
	assert(r == -EAGAIN);

	/* drop peers */

	c2 = bus1_client_free(c2);
	assert(!c2);

	c1 = bus1_client_free(c1);
	assert(!c1);
}

/* make sure we can set + get seed */
static void test_api_seed(void)
{
	struct bus1_client *client;
	char *payload = "WOOF";
	struct iovec vecs[] = {
		{
			.iov_base = payload,
			.iov_len = strlen(payload) + 1,
		},
	};
	uint64_t handles[] = {
		BUS1_NODE_FLAG_MANAGED | BUS1_NODE_FLAG_ALLOCATE,
	};
	struct bus1_cmd_send send = {
		.flags = BUS1_SEND_FLAG_SEED,
		.ptr_vecs = (uintptr_t)vecs,
		.n_vecs = sizeof(vecs) / sizeof(*vecs),
		.ptr_handles = (uintptr_t)handles,
		.n_handles = sizeof(handles) / sizeof(*handles),
	};
	struct bus1_cmd_recv recv = {
		.flags = BUS1_RECV_FLAG_SEED,
	};
	const void *slice;
	uint64_t handle_id;
	int r;

	/* setup default client */

	r = bus1_client_new_from_path(&client, test_path);
	assert(r >= 0);
	r = bus1_client_init(client, BUS1_CLIENT_POOL_SIZE);
	assert(r >= 0);
	r = bus1_client_mmap(client);
	assert(r >= 0);

	/* set SEED on @client and verify that nodes were properly created */

	r = bus1_client_send(client, &send);
	assert(r >= 0);

	assert(handles[0] != 0);
	assert(handles[0] != BUS1_HANDLE_INVALID);
	assert(!(handles[0] & BUS1_NODE_FLAG_ALLOCATE));

	/* verify that we can replace a SEED */

	r = bus1_client_send(client, &send);
	assert(r >= 0);

	/* retrieve SEED and verify its content */

	r = bus1_client_recv(client, &recv);
	assert(r >= 0);
	assert(recv.type == BUS1_MSG_DATA);

	slice = bus1_client_slice_from_offset(client, recv.data.offset);
	assert(slice);

	assert(recv.data.n_bytes == strlen(payload) + 1);
	assert(strncmp(slice, payload, recv.data.n_bytes) == 0);
	handle_id = *(uint64_t*)((uint8_t*)slice + ((recv.data.n_bytes + 7) & ~(7ULL)));
	assert(handle_id != 0);
	assert(handle_id != BUS1_HANDLE_INVALID);
	assert(!(handle_id & BUS1_NODE_FLAG_ALLOCATE));

	client = bus1_client_free(client);
	assert(!client);
}

int test_api(void)
{
	test_api_cdev();
	test_api_client();
	test_api_connect();
	test_api_handle();
	test_api_seed();
	return TEST_OK;
}
