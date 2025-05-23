// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB Super Speed (Plus) redriver core module
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "redriver-core: " fmt

#include <linux/usb/redriver.h>
#include <linux/module.h>

static LIST_HEAD(usb_redriver_list);
static DEFINE_SPINLOCK(usb_rediver_lock);

/**
 * usb_add_redriver, register a redriver from a specific chip driver.
 * @redriver, redriver allocated by specific chip driver.
 *
 * if of node not exist, return -EINVAL,
 * if exist, add redriver to global list.
 */
int usb_add_redriver(struct usb_redriver *redriver)
{
	struct usb_redriver *iter;

	if (!redriver->of_node ||  redriver->bounded ||
	    (redriver->has_orientation && !redriver->get_orientation))
		return -EINVAL;

	spin_lock(&usb_rediver_lock);

	list_for_each_entry(iter, &usb_redriver_list, list) {
		if (iter == redriver) {
			spin_unlock(&usb_rediver_lock);
			return -EEXIST;
		}
	}

	pr_err("add redriver %s\n", of_node_full_name(redriver->of_node));
	list_add_tail(&redriver->list, &usb_redriver_list);
	spin_unlock(&usb_rediver_lock);

	return 0;
}
EXPORT_SYMBOL(usb_add_redriver);

/**
 * usb_remove_redriver, remove a redriver from a specific chip driver.
 * @redriver, redriver allocated by specific chip driver.
 *
 * remove redriver from global list.
 * if redriver rmmod, it is better change to default state inside it's driver,
 * no unbind operation here.
 */
int usb_remove_redriver(struct usb_redriver *redriver)
{
	spin_lock(&usb_rediver_lock);

	if (redriver->bounded) {
		spin_unlock(&usb_rediver_lock);
		return -EINVAL;
	}

	pr_debug("remove redriver %s\n", of_node_full_name(redriver->of_node));
	list_del(&redriver->list);

	spin_unlock(&usb_rediver_lock);

	return 0;
}
EXPORT_SYMBOL(usb_remove_redriver);

/**
 * usb_get_redriver_by_phandle, find redriver to be used.
 * @np, device node of device which use the redriver
 * @phandle_name, phandle name which refer to the redriver
 * @index, phandle index which refer to the redriver
 *
 * if no phandle or redriver device tree status is disabled, return NULL.
 * if redriver is not registered, return -EPROBE_DEFER.
 * if redriver registered, return it.
 */
static int retry_redriver_count = 0;
struct usb_redriver *usb_get_redriver_by_phandle(const struct device_node *np,
		const char *phandle_name, int index)
{
	struct usb_redriver *redriver;
	struct device_node *node;
	bool found = false;

	node = of_parse_phandle(np, phandle_name, index);
	if (!of_device_is_available(node)) {
		of_node_put(node);
		return NULL;
	}

	spin_lock(&usb_rediver_lock);
	list_for_each_entry(redriver, &usb_redriver_list, list) {
		if (redriver->of_node == node) {
			found = true;
			break;
		}
	}

	if (!found) {
		of_node_put(node);
		spin_unlock(&usb_rediver_lock);
		if (retry_redriver_count < 5) {
			pr_err("get redriver err, retry %d.\n", retry_redriver_count);
			retry_redriver_count++;
			return ERR_PTR(-EPROBE_DEFER);
		} else {
			retry_redriver_count = 0;
			pr_err("get redriver err, end.\n");
			return NULL;
		}
	}

	pr_debug("get redriver %s\n", of_node_full_name(redriver->of_node));
	redriver->bounded = true;

	spin_unlock(&usb_rediver_lock);

	return redriver;
}
EXPORT_SYMBOL(usb_get_redriver_by_phandle);

/**
 * usb_put_redriver, redriver will not be used.
 * @redriver, redriver allocated by specific chip driver.
 *
 * when user module exit, unbind redriver.
 */
void usb_put_redriver(struct usb_redriver *redriver)
{
	if (!redriver)
		return;

	spin_lock(&usb_rediver_lock);
	of_node_put(redriver->of_node);
	pr_debug("put redriver %s\n", of_node_full_name(redriver->of_node));
	redriver->bounded = false;
	spin_unlock(&usb_rediver_lock);

	if (redriver->unbind)
		redriver->unbind(redriver);
}
EXPORT_SYMBOL(usb_put_redriver);


/* note: following exported symbol can be inlined in header file,
 * export here to avoid unexpected CFI(Clang Control Flow Integrity) issue.
 */
void usb_redriver_release_lanes(struct usb_redriver *ur, int num)
{
	if (ur && ur->release_usb_lanes)
		ur->release_usb_lanes(ur, num);
}
EXPORT_SYMBOL(usb_redriver_release_lanes);

void usb_redriver_notify_connect(struct usb_redriver *ur, int ort)
{
	if (ur && ur->notify_connect)
		ur->notify_connect(ur, ort);
}
EXPORT_SYMBOL(usb_redriver_notify_connect);

void usb_redriver_notify_disconnect(struct usb_redriver *ur)
{
	if (ur && ur->notify_disconnect)
		ur->notify_disconnect(ur);
}
EXPORT_SYMBOL(usb_redriver_notify_disconnect);

int usb_redriver_get_orientation(struct usb_redriver *ur)
{
	if (ur && ur->has_orientation)
		return ur->get_orientation(ur);

	return -1;
}
EXPORT_SYMBOL(usb_redriver_get_orientation);

void usb_redriver_gadget_pullup_enter(struct usb_redriver *ur,
					int is_on)
{
	if (ur && ur->gadget_pullup_enter)
		ur->gadget_pullup_enter(ur, is_on);
}
EXPORT_SYMBOL(usb_redriver_gadget_pullup_enter);

void usb_redriver_gadget_pullup_exit(struct usb_redriver *ur,
		int is_on)
{
	if (ur && ur->gadget_pullup_exit)
		ur->gadget_pullup_exit(ur, is_on);
}
EXPORT_SYMBOL(usb_redriver_gadget_pullup_exit);

void usb_redriver_host_powercycle(struct usb_redriver *ur)
{
	if (ur && ur->host_powercycle)
		ur->host_powercycle(ur);
}
EXPORT_SYMBOL(usb_redriver_host_powercycle);


static int __init usb_redriver_init(void)
{
	pr_debug("module init\n");
	return 0;
}
module_init(usb_redriver_init);

static void __exit usb_redriver_exit(void)
{
	pr_debug("module exit\n");
}
module_exit(usb_redriver_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Super Speed (Plus) redriver core module");
