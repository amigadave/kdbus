/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/hashtable.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "names.h"
#include "ep.h"
#include "ns.h"

void kdbus_release(struct device *dev)
{
	kfree(dev);
}


struct kdbus_bus *kdbus_bus_ref(struct kdbus_bus *bus)
{
	kref_get(&bus->kref);
	return bus;
}

static void __kdbus_bus_free(struct kref *kref)
{
	struct kdbus_bus *bus = container_of(kref, struct kdbus_bus, kref);

	kdbus_name_registry_unref(bus->name_registry);
	kdbus_bus_disconnect(bus);
	pr_info("clean up bus %s/%s\n",
		bus->ns->devpath, bus->name);

	kfree(bus->name);
	kfree(bus);
}

void kdbus_bus_unref(struct kdbus_bus *bus)
{
	kref_put(&bus->kref, __kdbus_bus_free);
}

struct kdbus_conn *kdbus_bus_find_conn_by_id(struct kdbus_bus *bus, u64 id)
{
	struct kdbus_conn *conn;

	hash_for_each_possible(bus->conn_hash, conn, hentry, id)
		if (conn->id == id)
			return conn;

	return NULL;
}

void kdbus_bus_disconnect(struct kdbus_bus *bus)
{
	struct kdbus_ep *ep, *tmp;

	if (bus->disconnected)
		return;
	bus->disconnected = true;
	list_del(&bus->bus_entry);

	/* remove any endpoints attached to this bus */
	list_for_each_entry_safe(ep, tmp, &bus->ep_list, bus_entry) {
		kdbus_ep_disconnect(ep);
		kdbus_ep_unref(ep);
	}

	pr_info("closing bus %s/%s\n", bus->ns->devpath, bus->name);
}

static struct kdbus_bus *kdbus_bus_find(struct kdbus_ns *ns, const char *name)
{
	struct kdbus_bus *bus = NULL;
	struct kdbus_bus *b;

	mutex_lock(&ns->lock);
	list_for_each_entry(b, &ns->bus_list, bus_entry) {
		if (strcmp(b->name, name))
			continue;

		bus = kdbus_bus_ref(b);
		break;
	}

	mutex_unlock(&ns->lock);
	return bus;
}

int kdbus_bus_new(struct kdbus_ns *ns, struct kdbus_cmd_bus_kmake *bus_kmake,
		  umode_t mode, kuid_t uid, kgid_t gid, struct kdbus_bus **bus)
{
	char prefix[16];
	struct kdbus_bus *b;
	int ret;

	/* enforce "$UID-" prefix */
	snprintf(prefix, sizeof(prefix), "%u-", from_kuid(current_user_ns(), uid));
	if (strncmp(bus_kmake->name, prefix, strlen(prefix) != 0))
		return -EPERM;

	b = kdbus_bus_find(ns, bus_kmake->name);
	if (b) {
		kdbus_bus_unref(b);
		return -EEXIST;
	}

	b = kzalloc(sizeof(struct kdbus_bus), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	kref_init(&b->kref);
	b->ns = ns;
	b->bus_flags = bus_kmake->make.flags;
	b->bloom_size = bus_kmake->make.bloom_size;
	b->cgroup_id = bus_kmake->cgroup_id;
	b->conn_id_next = 1; /* connection 0 == kernel */
	mutex_init(&b->lock);
	hash_init(b->conn_hash);
	INIT_LIST_HEAD(&b->ep_list);

	b->name = kstrdup(bus_kmake->name, GFP_KERNEL);
	if (!b->name) {
		ret = -ENOMEM;
		goto ret;
	}

	b->name_registry = kdbus_name_registry_new();
	if (!b->name_registry) {
		ret = -ENOMEM;
		goto ret;
	}

	ret = kdbus_ep_new(b, "bus", mode, uid, gid,
			   b->bus_flags & KDBUS_POLICY_OPEN);
	if (ret < 0)
		goto ret;

	mutex_lock(&ns->lock);
	b->id = ns->bus_id_next++;

	list_add_tail(&b->bus_entry, &ns->bus_list);
	mutex_unlock(&ns->lock);

	*bus = b;
	pr_info("created bus %llu '%s/%s'\n",
		(unsigned long long)b->id, ns->devpath, b->name);
	return 0;
ret:
	kdbus_bus_unref(b);
	return ret;
}

int kdbus_bus_make_user(void __user *buf, struct kdbus_cmd_bus_kmake **kmake)
{
	u64 size;
	struct kdbus_cmd_bus_kmake *km;
	const struct kdbus_cmd_make_item *item;
	int ret;

	if (kdbus_size_get_user(size, buf, struct kdbus_cmd_bus_make))
		return -EFAULT;

	if (size < sizeof(struct kdbus_cmd_bus_make) || size > 0xffff)
		return -EMSGSIZE;

	km = kmalloc(sizeof(struct kdbus_cmd_bus_kmake) + size, GFP_KERNEL);
	if (!km)
		return -ENOMEM;

	memset(km, 0, offsetof(struct kdbus_cmd_bus_kmake, make));
	if (copy_from_user(&km->make, buf, size)) {
		ret = -EFAULT;
		goto out_err;
	}

	KDBUS_ITEM_FOREACH(item, &km->make) {
		/* empty data records are invalid */
		if (item->size <= KDBUS_ITEM_HEADER_SIZE) {
			ret = -EINVAL;
			goto out_err;
		}

		switch (item->type) {
		case KDBUS_CMD_MAKE_NAME:
			if (km->name) {
				ret = -EEXIST;
				goto out_err;
			}

			if (item->size < KDBUS_ITEM_HEADER_SIZE + 2) {
				ret = -EINVAL;
				goto out_err;
			}

			if (item->size > KDBUS_ITEM_HEADER_SIZE + 64) {
				ret = -ENAMETOOLONG;
				goto out_err;
			}

			if (!kdbus_validate_nul(item->str,
					item->size - KDBUS_ITEM_HEADER_SIZE)) {
				ret = -EINVAL;
				goto out_err;
			}

			km->name = item->str;
			continue;

		case KDBUS_CMD_MAKE_CGROUP:
			if (km->cgroup_id) {
				ret = -EEXIST;
				goto out_err;
			}

			km->cgroup_id = item->data64[0];
			continue;

		default:
			ret = -ENOTSUPP;
			goto out_err;
		}
	}

	/* expect correct padding and size values */
	if ((char *)item - ((char *)&km->make + km->make.size) >= 8)
		return EINVAL;

	if (!km->name) {
		ret = -EBADMSG;
		goto out_err;
	}

	if (!KDBUS_IS_ALIGNED8(km->make.bloom_size)) {
		return -EINVAL;
		goto out_err;
	}

	if (km->make.bloom_size < 8 || km->make.bloom_size > 16 * 1024) {
		ret = -EINVAL;
		goto out_err;
	}

	*kmake = km;
	return 0;

out_err:
	return ret;
}
