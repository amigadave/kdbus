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
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/init.h>

#include "kdbus.h"

#include "kdbus_internal.h"

/* global list of all namespaces */
static LIST_HEAD(namespace_list);

/* namespace list lock */
DEFINE_MUTEX(kdbus_subsys_lock);

/* next namespace id sequence number */
static u64 kdbus_ns_id_next;

/* control nodes are world accessible */
static char *kdbus_devnode_control(struct device *dev, umode_t *mode
#ifdef DRIVER_CORE_DEVICE_TYPE_DEVNODE_UID
		, uid_t *uid, gid_t *gid)
#else
		)
#endif
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static struct device_type kdbus_devtype_control = {
	.name		= "control",
	.release	= kdbus_release,
	.devnode	= kdbus_devnode_control,
};

/* kdbus namespace */
struct kdbus_ns *kdbus_ns_ref(struct kdbus_ns *ns)
{
	kref_get(&ns->kref);
	return ns;
}

void kdbus_ns_disconnect(struct kdbus_ns *ns)
{
	if (ns->disconnected)
		return;
	ns->disconnected = true;
	list_del(&ns->ns_entry);

	if (ns->dev) {
		device_unregister(ns->dev);
		ns->dev = NULL;
	}
	if (ns->major > 0) {
		idr_remove(&kdbus_ns_major_idr, ns->major);
		unregister_chrdev(ns->major, "kdbus");
		ns->major = 0;
	}
	pr_info("closing namespace %s\n", ns->devpath);
}

static void __kdbus_ns_free(struct kref *kref)
{
	struct kdbus_ns *ns = container_of(kref, struct kdbus_ns, kref);

	kdbus_ns_disconnect(ns);
	pr_info("clean up namespace %s\n", ns->devpath);
	kfree(ns->name);
	kfree(ns->devpath);
	kfree(ns);
}

void kdbus_ns_unref(struct kdbus_ns *ns)
{
	kref_put(&ns->kref, __kdbus_ns_free);
}

static struct kdbus_ns *kdbus_ns_find(struct kdbus_ns const *parent, const char *name)
{
	struct kdbus_ns *ns = NULL;
	struct kdbus_ns *n;

	mutex_lock(&kdbus_subsys_lock);
	list_for_each_entry(n, &namespace_list, ns_entry) {
		if (n->parent != parent)
			continue;
		if (strcmp(n->name, name))
			continue;

		ns = kdbus_ns_ref(n);
		break;
	}

	mutex_unlock(&kdbus_subsys_lock);
	return ns;
}

int kdbus_ns_new(struct kdbus_ns *parent, const char *name, umode_t mode, struct kdbus_ns **ns)
{
	struct kdbus_ns *n;
	const char *ns_name = NULL;
	int i;
	int ret;

	pr_info("%s: %s\n", __func__, name ? name : "init");

	if ((parent && !name) || (!parent && name))
		return -EINVAL;

	n = kdbus_ns_find(parent, name);
	if (n) {
		kdbus_ns_unref(n);
		return -EEXIST;
	}

	n = kzalloc(sizeof(struct kdbus_ns), GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	if (name) {
		ns_name = kstrdup(name, GFP_KERNEL);
		if (!ns_name) {
			kfree(n);
			return -ENOMEM;
		}
	}

	INIT_LIST_HEAD(&n->bus_list);
	kref_init(&n->kref);
	idr_init(&n->idr);
	mutex_init(&n->lock);

	/* compose name and path of base directory in /dev */
	if (!parent) {
		/* initial namespace */
		n->devpath = kstrdup("kdbus", GFP_KERNEL);
		if (!n->devpath) {
			ret = -ENOMEM;
			goto ret;
		}

		/* register static major to support module auto-loading */
		ret = register_chrdev(KDBUS_CHAR_MAJOR, "kdbus", &kdbus_device_ops);
		if (ret)
			goto ret;
		n->major = KDBUS_CHAR_MAJOR;
	} else {
		n->parent = parent;
		n->devpath = kasprintf(GFP_KERNEL, "kdbus/ns/%s/%s", parent->devpath, name);
		if (!n->devpath) {
			ret = -ENOMEM;
			goto ret;
		}

		/* get dynamic major */
		n->major = register_chrdev(0, "kdbus", &kdbus_device_ops);
		if (n->major < 0) {
			ret = n->major;
			goto ret;
		}
		n->name = ns_name;
	}

	mutex_lock(&kdbus_subsys_lock);

	/* kdbus_device_ops' dev_t finds the namespace in the major map,
	 * and the bus in the minor map of that namespace */
	i = idr_alloc(&kdbus_ns_major_idr, n, n->major, 0, GFP_KERNEL);
	if (i <= 0) {
		ret = -EEXIST;
		goto err_unlock;
	}

	/* get id for this namespace */
	n->id = kdbus_ns_id_next++;

	/* register control device for this namespace */
	n->dev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!n->dev)
		goto err_unlock;
	dev_set_name(n->dev, "%s/%s", n->devpath, "control");
	n->dev->bus = &kdbus_subsys;
	n->dev->type = &kdbus_devtype_control;
	n->dev->devt = MKDEV(n->major, 0);
	dev_set_drvdata(n->dev, n);
	ret = device_register(n->dev);
	if (ret < 0) {
		put_device(n->dev);
		n->dev = NULL;
		goto err_unlock;
	}

	list_add_tail(&n->ns_entry, &namespace_list);

	mutex_unlock(&kdbus_subsys_lock);

	*ns = n;
	pr_info("created namespace %llu '%s/'\n",
		(unsigned long long)n->id, n->devpath);
	return 0;

err_unlock:
	mutex_unlock(&kdbus_subsys_lock);
ret:
	kdbus_ns_unref(n);
	return ret;
}
