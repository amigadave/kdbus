/*
 * Copyright (C) 2013-2014 Kay Sievers
 * Copyright (C) 2013-2014 Daniel Mack
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */
#pragma once

#define BIT(X) (1 << (X))

#include "../kdbus.h"

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)
#define ELEMENTSOF(x) (sizeof(x)/sizeof((x)[0]))

#define KDBUS_PTR(addr) ((void *)(uintptr_t)(addr))

#define KDBUS_ALIGN8(l) (((l) + 7) & ~7)
#define KDBUS_ITEM_HEADER_SIZE offsetof(struct kdbus_item, data)
#define KDBUS_ITEM_SIZE(s) KDBUS_ALIGN8((s) + KDBUS_ITEM_HEADER_SIZE)

#define KDBUS_ITEM_NEXT(item) \
	(typeof(item))(((uint8_t *)item) + KDBUS_ALIGN8((item)->size))
#define KDBUS_ITEM_FOREACH(item, head, first)				\
	for (item = (head)->first;					\
	     (uint8_t *)(item) < (uint8_t *)(head) + (head)->size;	\
	     item = KDBUS_ITEM_NEXT(item))

#define POOL_SIZE (16 * 1024LU * 1024LU)

/* Dump as user of process, useful for user namespace testing */
#define SUID_DUMP_USER	1

extern int kdbus_util_verbose;

#define kdbus_printf(X...) \
	if (kdbus_util_verbose) \
		printf(X)

struct kdbus_conn {
	int fd;
	uint64_t id;
	void *buf;
};

int sys_memfd_create(const char *name, __u64 size);
int sys_memfd_seal_set(int fd);
off_t sys_memfd_get_size(int fd, off_t *size);

int kdbus_name_list(struct kdbus_conn *conn, uint64_t flags);
int kdbus_name_release(struct kdbus_conn *conn, const char *name);
int kdbus_name_acquire(struct kdbus_conn *conn, const char *name,
		       uint64_t flags);
void kdbus_msg_free(struct kdbus_msg *msg);
int kdbus_msg_recv(struct kdbus_conn *conn,
		   struct kdbus_msg **msg, uint64_t *offset);
int kdbus_msg_recv_poll(struct kdbus_conn *conn, unsigned int timeout_ms,
			struct kdbus_msg **msg_out, uint64_t *offset);
int kdbus_free(const struct kdbus_conn *conn, uint64_t offset);
void kdbus_msg_dump(const struct kdbus_conn *conn,
		    const struct kdbus_msg *msg);
int kdbus_create_bus(int control_fd, const char *name, char **path);
int kdbus_msg_send(const struct kdbus_conn *conn, const char *name,
		   uint64_t cookie, uint64_t flags, uint64_t timeout,
		   int64_t priority, uint64_t dst_id);
struct kdbus_conn *kdbus_hello(const char *path, uint64_t hello_flags,
			       const struct kdbus_item *item,
			       size_t item_size);
struct kdbus_conn *kdbus_hello_registrar(const char *path, const char *name,
					 const struct kdbus_policy_access *access,
					 size_t num_access, uint64_t flags);
struct kdbus_conn *kdbus_hello_activator(const char *path, const char *name,
					 const struct kdbus_policy_access *access,
					 size_t num_access);
void kdbus_conn_free(struct kdbus_conn *conn);
int kdbus_conn_update_attach_flags(struct kdbus_conn *conn, uint64_t flags);
int kdbus_conn_update_policy(struct kdbus_conn *conn, const char *name,
			     const struct kdbus_policy_access *access,
			     size_t num_access);

int kdbus_add_match_empty(struct kdbus_conn *conn);

int drop_privileges(uid_t uid, gid_t gid);

int userns_map_uid_gid(pid_t pid,
		       const char *map_uid,
		       const char *map_gid);
