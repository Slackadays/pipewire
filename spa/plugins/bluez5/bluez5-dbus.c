/* Spa V4l2 dbus
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include "a2dp-codecs.h"
#include "defs.h"

#define NAME "bluez5-monitor"

struct spa_bt_monitor {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_system *main_system;
	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	struct spa_hook_list hooks;

	uint32_t count;
	uint32_t id;

	/*
	 * Lists of BlueZ objects, kept up-to-date by following DBus events
	 * initiated by BlueZ. Object lifetime is also determined by that.
	 */
	struct spa_list adapter_list;
	struct spa_list device_list;
	struct spa_list remote_endpoint_list;
	struct spa_list transport_list;

	unsigned int filters_added:1;
	unsigned int objects_listed:1;

	struct spa_bt_backend *backend_native;
	struct spa_bt_backend *backend_ofono;
	struct spa_bt_backend *backend_hsphfpd;

	struct spa_dict enabled_codecs;

	unsigned int enable_sbc_xq:1;
};

/* Stream endpoints owned by BlueZ for each device */
struct spa_bt_remote_endpoint {
	struct spa_list link;
	struct spa_list device_link;
	struct spa_bt_monitor *monitor;
	char *path;

	char *uuid;
	unsigned int codec;
	struct spa_bt_device *device;
	uint8_t *capabilities;
	int capabilities_len;
	bool delay_reporting;
};

/*
 * Codec switching tries various codec/remote endpoint combinations
 * in order, until an acceptable one is found. This triggers BlueZ
 * to initiate DBus calls that result to the creation of a transport
 * with the desired capabilities.
 * The codec switch struct tracks candidates still to be tried.
 */
struct spa_bt_a2dp_codec_switch {
	struct spa_bt_device *device;
	struct spa_list device_link;

	DBusPendingCall *pending;

	uint32_t profile;

	/*
	 * Called asynchronously, so endpoint paths instead of pointers (which may be
	 * invalidated in the meantime).
	 */
	const struct a2dp_codec **codecs;
	char **paths;

	const struct a2dp_codec **codec_iter;	/**< outer iterator over codecs */
	char **path_iter;			/**< inner iterator over endpoint paths */

	size_t num_paths;
};

/*
 * SCO socket connect may fail with ECONNABORTED if it is done too soon after
 * previous close. To avoid this in cases where nodes are toggled between
 * stopped/started rapidly, postpone release until the transport has remained
 * unused for a time. Since this appears common to multiple SCO backends, we do
 * it for all SCO backends here.
 */
#define SCO_TRANSPORT_RELEASE_TIMEOUT_MSEC 1000
#define SPA_BT_TRANSPORT_IS_SCO(transport) (transport->backend != NULL)

static int spa_bt_transport_stop_release_timer(struct spa_bt_transport *transport);
static int spa_bt_transport_start_release_timer(struct spa_bt_transport *transport);


static inline void add_dict(struct spa_pod_builder *builder, const char *key, const char *val)
{
	spa_pod_builder_string(builder, key);
	spa_pod_builder_string(builder, val);
}

static int a2dp_codec_to_endpoint(const struct a2dp_codec *codec,
				   const char * endpoint,
				   char** object_path)
{
	*object_path = spa_aprintf("%s/%s", endpoint, codec->name);
	if (*object_path == NULL)
		return -errno;
	return 0;
}

static const struct a2dp_codec *a2dp_endpoint_to_codec(const char *endpoint)
{
	const char *codec_name;
	int i;

	if (strstr(endpoint, A2DP_SINK_ENDPOINT "/") == endpoint)
		codec_name = endpoint + strlen(A2DP_SINK_ENDPOINT "/");
	else if (strstr(endpoint, A2DP_SOURCE_ENDPOINT "/") == endpoint)
		codec_name = endpoint + strlen(A2DP_SOURCE_ENDPOINT "/");
	else
		return NULL;

	for (i = 0; a2dp_codecs[i]; i++) {
		const struct a2dp_codec *codec = a2dp_codecs[i];
		if (strcmp(codec->name, codec_name) == 0)
			return codec;
	}
	return NULL;
}

static bool is_a2dp_codec_enabled(struct spa_bt_monitor *monitor, const struct a2dp_codec *codec)
{
	return spa_dict_lookup(&monitor->enabled_codecs, codec->name) != NULL;
}

static DBusHandlerResult endpoint_select_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path;
	uint8_t *cap, config[A2DP_MAX_CAPS_SIZE];
	uint8_t *pconf = (uint8_t *) config;
	DBusMessage *r;
	DBusError err;
	int i, size, res;
	const struct a2dp_codec *codec;

	dbus_error_init(&err);

	path = dbus_message_get_path(m);

	if (!dbus_message_get_args(m, &err, DBUS_TYPE_ARRAY,
				DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Endpoint SelectConfiguration(): %s", err.message);
		dbus_error_free(&err);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	spa_log_info(monitor->log, "%p: %s select conf %d", monitor, path, size);
	for (i = 0; i < size; i++)
		spa_log_debug(monitor->log, "  %d: %02x", i, cap[i]);

	codec = a2dp_endpoint_to_codec(path);
	if (codec != NULL)
		res = codec->select_config(codec, 0, cap, size, NULL, config);
	else
		res = -ENOTSUP;

	if (res < 0 || res != size) {
		spa_log_error(monitor->log, "can't select config: %d (%s)",
				res, spa_strerror(res));
		if ((r = dbus_message_new_error(m, "org.bluez.Error.InvalidArguments",
				"Unable to select configuration")) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		goto exit_send;
	}
	for (i = 0; i < size; i++)
		spa_log_debug(monitor->log, "  %d: %02x", i, pconf[i]);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_message_append_args(r, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &pconf, size, DBUS_TYPE_INVALID))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

      exit_send:
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static struct spa_bt_adapter *adapter_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_adapter *d;
	spa_list_for_each(d, &monitor->adapter_list, link)
		if (strcmp(d->path, path) == 0)
			return d;
	return NULL;
}

static bool check_iter_signature(DBusMessageIter *it, const char *sig)
{
	char *v;
	int res;
	v = dbus_message_iter_get_signature(it);
	res = strcmp(v, sig);
	dbus_free(v);
	return res == 0;
}

static int adapter_update_props(struct spa_bt_adapter *adapter,
				DBusMessageIter *props_iter,
				DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = adapter->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%s", adapter, key, value);

			if (strcmp(key, "Alias") == 0) {
				free(adapter->alias);
				adapter->alias = strdup(value);
			}
			else if (strcmp(key, "Name") == 0) {
				free(adapter->name);
				adapter->name = strdup(value);
			}
			else if (strcmp(key, "Address") == 0) {
				free(adapter->address);
				adapter->address = strdup(value);
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%d", adapter, key, value);

			if (strcmp(key, "Class") == 0)
				adapter->bluetooth_class = value;

		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%d", adapter, key, value);

			if (strcmp(key, "Powered") == 0) {
				adapter->powered = value;
			}
		}
		else if (strcmp(key, "UUIDs") == 0) {
			DBusMessageIter iter;

			if (!check_iter_signature(&it[1], "as"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;
				enum spa_bt_profile profile;

				dbus_message_iter_get_basic(&iter, &uuid);

				profile = spa_bt_profile_from_uuid(uuid);

				if (profile && (adapter->profiles & profile) == 0) {
					spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, uuid);
					adapter->profiles |= profile;
				}
				dbus_message_iter_next(&iter);
			}
		}
		else
			spa_log_debug(monitor->log, "adapter %p: unhandled key %s", adapter, key);

	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static struct spa_bt_adapter *adapter_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_adapter *d;

	d = calloc(1, sizeof(struct spa_bt_adapter));
	if (d == NULL)
		return NULL;

	d->monitor = monitor;
	d->path = strdup(path);

	spa_list_prepend(&monitor->adapter_list, &d->link);

	return d;
}

static void adapter_free(struct spa_bt_adapter *adapter)
{
	struct spa_bt_monitor *monitor = adapter->monitor;
	spa_log_debug(monitor->log, "%p", adapter);

	spa_list_remove(&adapter->link);
	free(adapter->alias);
	free(adapter->name);
	free(adapter->address);
	free(adapter->path);
	free(adapter);
}

struct spa_bt_device *spa_bt_device_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_device *d;
	spa_list_for_each(d, &monitor->device_list, link)
		if (strcmp(d->path, path) == 0)
			return d;
	return NULL;
}

struct spa_bt_device *spa_bt_device_find_by_address(struct spa_bt_monitor *monitor, const char *remote_address, const char *local_address)
{
	struct spa_bt_device *d;
	spa_list_for_each(d, &monitor->device_list, link)
		if (strcmp(d->address, remote_address) == 0 && strcmp(d->adapter->address, local_address) == 0)
			return d;
	return NULL;
}

static struct spa_bt_device *device_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_device *d;

	d = calloc(1, sizeof(struct spa_bt_device));
	if (d == NULL)
		return NULL;

	d->id = monitor->id++;
	d->monitor = monitor;
	d->path = strdup(path);
	spa_list_init(&d->remote_endpoint_list);
	spa_list_init(&d->transport_list);
	spa_list_init(&d->codec_switch_list);

	spa_hook_list_init(&d->listener_list);

	spa_list_prepend(&monitor->device_list, &d->link);

	return d;
}

static int device_stop_timer(struct spa_bt_device *device);

static int device_remove(struct spa_bt_monitor *monitor, struct spa_bt_device *device);

static void a2dp_codec_switch_free(struct spa_bt_a2dp_codec_switch *sw);

static void device_free(struct spa_bt_device *device)
{
	struct spa_bt_remote_endpoint *ep, *tep;
	struct spa_bt_a2dp_codec_switch *sw;
	struct spa_bt_transport *t, *tt;
	struct spa_bt_monitor *monitor = device->monitor;

	spa_log_debug(monitor->log, "%p", device);
	device_stop_timer(device);
	device_remove(monitor, device);

	spa_list_for_each_safe(ep, tep, &device->remote_endpoint_list, device_link) {
		if (ep->device == device) {
			spa_list_remove(&ep->device_link);
			ep->device = NULL;
		}
	}

	spa_list_for_each_safe(t, tt, &device->transport_list, device_link) {
		if (t->device == device) {
			spa_list_remove(&t->device_link);
			t->device = NULL;
		}
	}

	spa_list_consume(sw, &device->codec_switch_list, device_link)
		a2dp_codec_switch_free(sw);

	spa_list_remove(&device->link);
	free(device->path);
	free(device->alias);
	free(device->address);
	free(device->adapter_path);
	free(device->name);
	free(device->icon);
	free(device);
}

static int device_add(struct spa_bt_monitor *monitor, struct spa_bt_device *device)
{
	struct spa_device_object_info info;
	char dev[32], name[128], class[16];
	struct spa_dict_item items[20];
	uint32_t n_items = 0;

	if (device->connected_profiles == 0 || device->added)
		return 0;

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_BLUEZ5_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "bluez5");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	snprintf(name, sizeof(name), "bluez_card.%s", device->address);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, name);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION, device->name);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ALIAS, device->alias);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ICON_NAME, device->icon);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_FORM_FACTOR,
			spa_bt_form_factor_name(
				spa_bt_form_factor_from_class(device->bluetooth_class)));
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PATH, device->path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);
	snprintf(dev, sizeof(dev), "pointer:%p", device);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_DEVICE, dev);
	snprintf(class, sizeof(class), "0x%06x", device->bluetooth_class);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CLASS, class);

	info.props = &SPA_DICT_INIT(items, n_items);

	device->added = true;
	spa_device_emit_object_info(&monitor->hooks, device->id, &info);

	return 0;
}

static int device_remove(struct spa_bt_monitor *monitor, struct spa_bt_device *device)
{
	if (!device->added)
		return 0;

	device->added = false;
	spa_device_emit_object_info(&monitor->hooks, device->id, NULL);

	return 0;
}


#define DEVICE_PROFILE_TIMEOUT_SEC 3

static void device_timer_event(struct spa_source *source)
{
	struct spa_bt_device *device = source->data;
	struct spa_bt_monitor *monitor = device->monitor;
	uint64_t exp;

	if (spa_system_timerfd_read(monitor->main_system, source->fd, &exp) < 0)
                spa_log_warn(monitor->log, "error reading timerfd: %s", strerror(errno));

	spa_log_debug(monitor->log, "device %p: timeout %08x %08x",
			device, device->profiles, device->connected_profiles);

	device_add(device->monitor, device);
}

static int device_start_timer(struct spa_bt_device *device)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct itimerspec ts;

	spa_log_debug(monitor->log, "device %p: start timer", device);
	if (device->timer.data == NULL) {
		device->timer.data = device;
		device->timer.func = device_timer_event;
		device->timer.fd = spa_system_timerfd_create(monitor->main_system,
				CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		device->timer.mask = SPA_IO_IN;
		device->timer.rmask = 0;
		spa_loop_add_source(monitor->main_loop, &device->timer);
	}
	ts.it_value.tv_sec = DEVICE_PROFILE_TIMEOUT_SEC;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, device->timer.fd, 0, &ts, NULL);
	return 0;
}

static int device_stop_timer(struct spa_bt_device *device)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct itimerspec ts;

	if (device->timer.data == NULL)
		return 0;

	spa_log_debug(monitor->log, "device %p: stop timer", device);
	spa_loop_remove_source(monitor->main_loop, &device->timer);
        ts.it_value.tv_sec = 0;
        ts.it_value.tv_nsec = 0;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 0;
        spa_system_timerfd_settime(monitor->main_system, device->timer.fd, 0, &ts, NULL);
	spa_system_close(monitor->main_system, device->timer.fd);
	device->timer.data = NULL;
	return 0;
}

int spa_bt_device_check_profiles(struct spa_bt_device *device, bool force)
{
	struct spa_bt_monitor *monitor = device->monitor;
	uint32_t connected_profiles = device->connected_profiles;

	if (connected_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
		connected_profiles |= SPA_BT_PROFILE_HEADSET_HEAD_UNIT;
	if (connected_profiles & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)
		connected_profiles |= SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY;

	spa_log_debug(monitor->log, "device %p: profiles %08x %08x %d",
			device, device->profiles, connected_profiles, device->added);

	if (connected_profiles == 0 && device->active_codec_switch == NULL) {
		if (device->added) {
			device_stop_timer(device);
			device_remove(monitor, device);
		}
	} else if (force || (device->profiles & connected_profiles) == device->profiles) {
		device_stop_timer(device);
		device_add(monitor, device);
	} else {
		device_start_timer(device);
	}
	return 0;
}

static void device_set_connected(struct spa_bt_device *device, int connected)
{
	struct spa_bt_monitor *monitor = device->monitor;

	if (device->connected && !connected)
		device->connected_profiles = 0;

	device->connected = connected;

	if (connected)
		spa_bt_device_check_profiles(device, false);
	else {
		device_stop_timer(device);
		if (device->added)
			device_remove(monitor, device);
	}
}

int spa_bt_device_connect_profile(struct spa_bt_device *device, enum spa_bt_profile profile)
{
	uint32_t prev_connected = device->connected_profiles;
	device->connected_profiles |= profile;
	spa_bt_device_check_profiles(device, false);
	if (device->connected_profiles != prev_connected)
		spa_bt_device_emit_profiles_changed(device, device->profiles, prev_connected);
	return 0;
}


static int device_update_props(struct spa_bt_device *device,
			       DBusMessageIter *props_iter,
			       DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = device->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%s", device, key, value);

			if (strcmp(key, "Alias") == 0) {
				free(device->alias);
				device->alias = strdup(value);
			}
			else if (strcmp(key, "Name") == 0) {
				free(device->name);
				device->name = strdup(value);
			}
			else if (strcmp(key, "Address") == 0) {
				free(device->address);
				device->address = strdup(value);
			}
			else if (strcmp(key, "Adapter") == 0) {
				free(device->adapter_path);
				device->adapter_path = strdup(value);

				device->adapter = adapter_find(monitor, value);
				if (device->adapter == NULL) {
					spa_log_warn(monitor->log, "unknown adapter %s", value);
				}
			}
			else if (strcmp(key, "Icon") == 0) {
				free(device->icon);
				device->icon = strdup(value);
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%08x", device, key, value);

			if (strcmp(key, "Class") == 0)
				device->bluetooth_class = value;
		}
		else if (type == DBUS_TYPE_UINT16) {
			uint16_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "Appearance") == 0)
				device->appearance = value;
		}
		else if (type == DBUS_TYPE_INT16) {
			int16_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "RSSI") == 0)
				device->RSSI = value;
		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "Paired") == 0) {
				device->paired = value;
			}
			else if (strcmp(key, "Trusted") == 0) {
				device->trusted = value;
			}
			else if (strcmp(key, "Connected") == 0) {
				device_set_connected(device, value);
			}
			else if (strcmp(key, "Blocked") == 0) {
				device->blocked = value;
			}
			else if (strcmp(key, "ServicesResolved") == 0) {
				if (value)
					spa_bt_device_check_profiles(device, false);
			}
		}
		else if (strcmp(key, "UUIDs") == 0) {
			DBusMessageIter iter;
			uint32_t prev_profiles = device->profiles;

			if (!check_iter_signature(&it[1], "as"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;
				enum spa_bt_profile profile;

				dbus_message_iter_get_basic(&iter, &uuid);

				profile = spa_bt_profile_from_uuid(uuid);
				if (profile && (device->profiles & profile) == 0) {
					spa_log_debug(monitor->log, "device %p: add UUID=%s", device, uuid);
					device->profiles |= profile;
				}
				dbus_message_iter_next(&iter);
			}

			if (device->profiles != prev_profiles)
				spa_bt_device_emit_profiles_changed(
					device, prev_profiles, device->connected_profiles);
		}
		else
			spa_log_debug(monitor->log, "device %p: unhandled key %s type %d", device, key, type);

	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static bool device_can_accept_a2dp_codec(struct spa_bt_device *device, const struct a2dp_codec *codec)
{
	struct spa_bt_monitor *monitor = device->monitor;

	/* Device capability checks in addition to A2DP Capabilities. */

	if (!is_a2dp_codec_enabled(device->monitor, codec))
		return false;

	if (codec->feature_flag != NULL && strcmp(codec->feature_flag, "sbc-xq") == 0)
		return monitor->enable_sbc_xq;

	/* The rest is determined by A2DP caps */
	return true;
}

bool spa_bt_device_supports_a2dp_codec(struct spa_bt_device *device, const struct a2dp_codec *codec)
{
	struct spa_bt_remote_endpoint *ep;

	if (!device_can_accept_a2dp_codec(device, codec))
		return false;

	if (!device->adapter->application_registered) {
		/* Codec switching not supported: only plain SBC allowed */
		return (codec->codec_id == A2DP_CODEC_SBC && strcmp(codec->name, "sbc") == 0);
	}

	spa_list_for_each(ep, &device->remote_endpoint_list, device_link) {
		if (a2dp_codec_check_caps(codec, ep->codec, ep->capabilities, ep->capabilities_len))
			return true;
	}

	return false;
}

const struct a2dp_codec **spa_bt_device_get_supported_a2dp_codecs(struct spa_bt_device *device, size_t *count)
{
	const struct a2dp_codec **supported_codecs;
	size_t i, j, size;

	*count = 0;

	size = 8;
	supported_codecs = malloc(size * sizeof(const struct a2dp_codec *));
	if (supported_codecs == NULL)
		return NULL;

	j = 0;
	for (i = 0; a2dp_codecs[i] != NULL; ++i) {
		if (spa_bt_device_supports_a2dp_codec(device, a2dp_codecs[i])) {
			supported_codecs[j] = a2dp_codecs[i];
			++j;
		}

		if (j >= size) {
			const struct a2dp_codec **p;
			size = size * 2;
			p = realloc(supported_codecs, size * sizeof(const struct a2dp_codec *));
			if (p == NULL) {
				free(supported_codecs);
				return NULL;
			}
			supported_codecs = p;
		}
	}

	supported_codecs[j] = NULL;
	*count = j;

	return supported_codecs;
}

static int device_try_connect_profile(struct spa_bt_device *device,
                                      enum spa_bt_profile profile,
                                      const char *profile_uuid)
{
	struct spa_bt_monitor *monitor = device->monitor;
	DBusMessage *m;

	if (!(device->profiles & profile) || (device->connected_profiles & profile))
		return 0;

	spa_log_info(monitor->log, "device %p %s: A2DP profile %s not connected; try ConnectProfile()",
	             device, device->path, profile_uuid);

	/* Call org.bluez.Device1.ConnectProfile() on device, ignoring result */

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
	                                 device->path,
	                                 BLUEZ_DEVICE_INTERFACE,
	                                 "ConnectProfile");
	if (m == NULL)
		return -ENOMEM;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &profile_uuid, DBUS_TYPE_INVALID);
	if (!dbus_connection_send(monitor->conn, m, NULL)) {
		dbus_message_unref(m);
		return -EIO;
	}
	dbus_message_unref(m);

	return 0;
}

static void reconnect_device_profiles(struct spa_bt_monitor *monitor)
{
	struct spa_bt_device *device;

	/* Call org.bluez.Device1.ConnectProfile() on connected devices, it they have A2DP but the
	 * profile is not yet connected.
	 */

	spa_list_for_each(device, &monitor->device_list, link) {
		if (device->connected) {
			device_try_connect_profile(device, SPA_BT_PROFILE_A2DP_SINK, SPA_BT_UUID_A2DP_SINK);
			device_try_connect_profile(device, SPA_BT_PROFILE_A2DP_SOURCE, SPA_BT_UUID_A2DP_SOURCE);
		}
	}
}

static struct spa_bt_remote_endpoint *device_remote_endpoint_find(struct spa_bt_device *device, const char *path)
{
	struct spa_bt_remote_endpoint *ep;
	spa_list_for_each(ep, &device->remote_endpoint_list, device_link)
		if (strcmp(ep->path, path) == 0)
			return ep;
	return NULL;
}

static struct spa_bt_remote_endpoint *remote_endpoint_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_remote_endpoint *ep;
	spa_list_for_each(ep, &monitor->remote_endpoint_list, link)
		if (strcmp(ep->path, path) == 0)
			return ep;
	return NULL;
}

static int remote_endpoint_update_props(struct spa_bt_remote_endpoint *remote_endpoint,
				DBusMessageIter *props_iter,
				DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = remote_endpoint->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%s", remote_endpoint, key, value);

			if (strcmp(key, "UUID") == 0) {
				free(remote_endpoint->uuid);
				remote_endpoint->uuid = strdup(value);
			}
			else if (strcmp(key, "Device") == 0) {
				struct spa_bt_device *device;
				device = spa_bt_device_find(monitor, value);
				spa_log_debug(monitor->log, "remote_endpoint %p: device -> %p", remote_endpoint, device);

				if (remote_endpoint->device != device) {
					if (remote_endpoint->device != NULL)
						spa_list_remove(&remote_endpoint->device_link);
					remote_endpoint->device = device;
					if (device != NULL)
						spa_list_append(&device->remote_endpoint_list, &remote_endpoint->device_link);
				}
			}
		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%d", remote_endpoint, key, value);

			if (strcmp(key, "DelayReporting") == 0) {
				remote_endpoint->delay_reporting = value;
			}
		}
		else if (type == DBUS_TYPE_BYTE) {
			uint8_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%02x", remote_endpoint, key, value);

			if (strcmp(key, "Codec") == 0) {
				remote_endpoint->codec = value;
			}
		}
		else if (strcmp(key, "Capabilities") == 0) {
			DBusMessageIter iter;
			uint8_t *value;
			int i, len;

			if (!check_iter_signature(&it[1], "ay"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);
			dbus_message_iter_get_fixed_array(&iter, &value, &len);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%d", remote_endpoint, key, len);
			for (i = 0; i < len; i++)
				spa_log_debug(monitor->log, "  %d: %02x", i, value[i]);

			free(remote_endpoint->capabilities);
			remote_endpoint->capabilities_len = 0;

			remote_endpoint->capabilities = malloc(len);
			if (remote_endpoint->capabilities) {
				memcpy(remote_endpoint->capabilities, value, len);
				remote_endpoint->capabilities_len = len;
			}
		}
		else
			spa_log_debug(monitor->log, "remote_endpoint %p: unhandled key %s", remote_endpoint, key);

	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static struct spa_bt_remote_endpoint *remote_endpoint_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_remote_endpoint *ep;

	ep = calloc(1, sizeof(struct spa_bt_remote_endpoint));
	if (ep == NULL)
		return NULL;

	ep->monitor = monitor;
	ep->path = strdup(path);

	spa_list_prepend(&monitor->remote_endpoint_list, &ep->link);

	return ep;
}

static void remote_endpoint_free(struct spa_bt_remote_endpoint *remote_endpoint)
{
	struct spa_bt_monitor *monitor = remote_endpoint->monitor;

	spa_log_debug(monitor->log, "remote endpoint %p: free %s",
	              remote_endpoint, remote_endpoint->path);

	if (remote_endpoint->device)
		spa_list_remove(&remote_endpoint->device_link);

	spa_list_remove(&remote_endpoint->link);
	free(remote_endpoint->path);
	free(remote_endpoint->uuid);
	free(remote_endpoint->capabilities);
	free(remote_endpoint);
}

struct spa_bt_transport *spa_bt_transport_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_transport *t;
	spa_list_for_each(t, &monitor->transport_list, link)
		if (strcmp(t->path, path) == 0)
			return t;
	return NULL;
}

struct spa_bt_transport *spa_bt_transport_find_full(struct spa_bt_monitor *monitor,
                                                    bool (*callback) (struct spa_bt_transport *t, const void *data),
                                                    const void *data)
{
	struct spa_bt_transport *t;

	spa_list_for_each(t, &monitor->transport_list, link)
		if (callback(t, data) == true)
			return t;
	return NULL;
}


struct spa_bt_transport *spa_bt_transport_create(struct spa_bt_monitor *monitor, char *path, size_t extra)
{
	struct spa_bt_transport *t;

	t = calloc(1, sizeof(struct spa_bt_transport) + extra);
	if (t == NULL)
		return NULL;

	t->acquire_refcount = 0;
	t->monitor = monitor;
	t->path = path;
	t->fd = -1;
	t->sco_io = NULL;
	t->user_data = SPA_MEMBER(t, sizeof(struct spa_bt_transport), void);
	spa_hook_list_init(&t->listener_list);

	spa_list_append(&monitor->transport_list, &t->link);

	return t;
}
static void transport_set_state(struct spa_bt_transport *transport, enum spa_bt_transport_state state)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	enum spa_bt_transport_state old = transport->state;

	if (old != state) {
		transport->state = state;
		spa_log_debug(monitor->log, "transport %p: %s state changed %d -> %d",
				transport, transport->path, old, state);
		spa_bt_transport_emit_state_changed(transport, old, state);
	}
}

void spa_bt_transport_free(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_device *device = transport->device;
	uint32_t prev_connected = 0;

	spa_log_debug(monitor->log, "transport %p: free %s", transport, transport->path);

	transport_set_state(transport, SPA_BT_TRANSPORT_STATE_IDLE);

	spa_bt_transport_emit_destroy(transport);

	spa_bt_transport_stop_release_timer(transport);

	if (transport->sco_io) {
		spa_bt_sco_io_destroy(transport->sco_io);
		transport->sco_io = NULL;
	}

	spa_bt_transport_destroy(transport);

	if (transport->fd >= 0) {
		shutdown(transport->fd, SHUT_RDWR);
		close(transport->fd);
		transport->fd = -1;
	}

	spa_list_remove(&transport->link);
	if (transport->device) {
		prev_connected = transport->device->connected_profiles;
		transport->device->connected_profiles &= ~transport->profile;
		spa_list_remove(&transport->device_link);
	}
	free(transport->path);
	free(transport);

	if (device && device->connected_profiles != prev_connected)
		spa_bt_device_emit_profiles_changed(device, device->profiles, prev_connected);
}

int spa_bt_transport_acquire(struct spa_bt_transport *transport, bool optional)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	int res;

	if (transport->acquire_refcount > 0) {
		spa_log_debug(monitor->log, "transport %p: incref %s", transport, transport->path);
		transport->acquire_refcount += 1;
		return 0;
	}
	spa_assert(transport->acquire_refcount == 0);

	res = spa_bt_transport_impl(transport, acquire, 0, optional);

	if (res >= 0)
		transport->acquire_refcount = 1;

	return res;
}

int spa_bt_transport_release(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	int res;

	if (transport->acquire_refcount > 1) {
		spa_log_debug(monitor->log, "transport %p: decref %s", transport, transport->path);
		transport->acquire_refcount -= 1;
		return 0;
	}
	else if (transport->acquire_refcount == 0) {
		spa_log_info(monitor->log, "transport %s already released", transport->path);
		return 0;
	}
	spa_assert(transport->acquire_refcount == 1);

	if (SPA_BT_TRANSPORT_IS_SCO(transport)) {
		/* Postpone SCO transport releases, since we might need it again soon */
		res = spa_bt_transport_start_release_timer(transport);
	} else {
		res = spa_bt_transport_impl(transport, release, 0);
		if (res >= 0)
			transport->acquire_refcount = 0;
	}

	return res;
}

static void spa_bt_transport_release_timer_event(struct spa_source *source)
{
	struct spa_bt_transport *transport = source->data;
	struct spa_bt_monitor *monitor = transport->monitor;

	spa_assert(transport->acquire_refcount >= 1);

	spa_bt_transport_stop_release_timer(transport);

	if (transport->acquire_refcount == 1) {
		spa_bt_transport_impl(transport, release, 0);
	} else {
		spa_log_debug(monitor->log, "transport %p: delayed decref %s", transport, transport->path);
	}
	transport->acquire_refcount -= 1;
}

static int spa_bt_transport_start_release_timer(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct itimerspec ts;

	if (transport->release_timer.data == NULL) {
		transport->release_timer.data = transport;
		transport->release_timer.func = spa_bt_transport_release_timer_event;
		transport->release_timer.fd = spa_system_timerfd_create(
			monitor->main_system, CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		transport->release_timer.mask = SPA_IO_IN;
		transport->release_timer.rmask = 0;
		spa_loop_add_source(monitor->main_loop, &transport->release_timer);
	}
	ts.it_value.tv_sec = SCO_TRANSPORT_RELEASE_TIMEOUT_MSEC / SPA_MSEC_PER_SEC;
	ts.it_value.tv_nsec = (SCO_TRANSPORT_RELEASE_TIMEOUT_MSEC % SPA_MSEC_PER_SEC) * SPA_NSEC_PER_MSEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, transport->release_timer.fd, 0, &ts, NULL);
	return 0;
}

static int spa_bt_transport_stop_release_timer(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct itimerspec ts;

	if (transport->release_timer.data == NULL)
		return 0;

	spa_loop_remove_source(monitor->main_loop, &transport->release_timer);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, transport->release_timer.fd, 0, &ts, NULL);
	spa_system_close(monitor->main_system, transport->release_timer.fd);
	transport->release_timer.data = NULL;
	return 0;
}

void spa_bt_transport_ensure_sco_io(struct spa_bt_transport *t, struct spa_loop *data_loop)
{
	if (t->sco_io == NULL) {
		t->sco_io = spa_bt_sco_io_create(data_loop,
						 t->fd,
						 t->read_mtu,
						 t->write_mtu);
	}
}

static int transport_update_props(struct spa_bt_transport *transport,
				  DBusMessageIter *props_iter,
				  DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = transport->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%s", transport, key, value);

			if (strcmp(key, "UUID") == 0) {
				switch (spa_bt_profile_from_uuid(value)) {
				case SPA_BT_PROFILE_A2DP_SOURCE:
					transport->profile = SPA_BT_PROFILE_A2DP_SINK;
					break;
				case SPA_BT_PROFILE_A2DP_SINK:
					transport->profile = SPA_BT_PROFILE_A2DP_SOURCE;
					break;
				default:
					spa_log_warn(monitor->log, "unknown profile %s", value);
					break;
				}
			}
			else if (strcmp(key, "State") == 0) {
				transport_set_state(transport, spa_bt_transport_state_from_string(value));
			}
			else if (strcmp(key, "Device") == 0) {
				transport->device = spa_bt_device_find(monitor, value);
				if (transport->device == NULL)
					spa_log_warn(monitor->log, "could not find device %s", value);
			}
		}
		else if (strcmp(key, "Codec") == 0) {
			uint8_t value;

			if (type != DBUS_TYPE_BYTE)
				goto next;
			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%02x", transport, key, value);

			transport->codec = value;
		}
		else if (strcmp(key, "Configuration") == 0) {
			DBusMessageIter iter;
			uint8_t *value;
			int i, len;

			if (!check_iter_signature(&it[1], "ay"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);
			dbus_message_iter_get_fixed_array(&iter, &value, &len);

			spa_log_debug(monitor->log, "transport %p: %s=%d", transport, key, len);
			for (i = 0; i < len; i++)
				spa_log_debug(monitor->log, "  %d: %02x", i, value[i]);

			free(transport->configuration);
			transport->configuration_len = 0;

			transport->configuration = malloc(len);
			if (transport->configuration) {
				memcpy(transport->configuration, value, len);
				transport->configuration_len = len;
			}
		}
		else if (strcmp(key, "Volume") == 0) {
		}
		else if (strcmp(key, "Delay") == 0) {
			uint16_t value;

			if (type != DBUS_TYPE_UINT16)
				goto next;
			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%02x", transport, key, value);

			transport->delay = value;
		}
	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static int transport_acquire(void *data, bool optional)
{
	struct spa_bt_transport *transport = data;
	struct spa_bt_monitor *monitor = transport->monitor;
	DBusMessage *m, *r;
	DBusError err;
	int ret = 0;
	const char *method = optional ? "TryAcquire" : "Acquire";

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
					 BLUEZ_MEDIA_TRANSPORT_INTERFACE,
					 method);
	if (m == NULL)
		return -ENOMEM;

	dbus_error_init(&err);

	r = dbus_connection_send_with_reply_and_block(monitor->conn, m, -1, &err);
	dbus_message_unref(m);
	m = NULL;

	if (r == NULL) {
		if (optional && strcmp(err.name, "org.bluez.Error.NotAvailable") == 0) {
			spa_log_info(monitor->log, "Failed optional acquire of unavailable transport %s",
					transport->path);
		}
		else {
			spa_log_error(monitor->log, "Transport %s() failed for transport %s (%s)",
					method, transport->path, err.message);
		}
		dbus_error_free(&err);
		return -EIO;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "%s returned error: %s", method, dbus_message_get_error_name(r));
		ret = -EIO;
		goto finish;
	}

	if (!dbus_message_get_args(r, &err,
				   DBUS_TYPE_UNIX_FD, &transport->fd,
				   DBUS_TYPE_UINT16, &transport->read_mtu,
				   DBUS_TYPE_UINT16, &transport->write_mtu,
				   DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Failed to parse %s() reply: %s", method, err.message);
		dbus_error_free(&err);
		ret = -EIO;
		goto finish;
	}
	spa_log_debug(monitor->log, "transport %p: %s %s, fd %d MTU %d:%d", transport, method,
			transport->path, transport->fd, transport->read_mtu, transport->write_mtu);

finish:
	dbus_message_unref(r);
	return ret;
}

static int transport_release(void *data)
{
	struct spa_bt_transport *transport = data;
	struct spa_bt_monitor *monitor = transport->monitor;
	DBusMessage *m, *r;
	DBusError err;
	bool is_idle = (transport->state == SPA_BT_TRANSPORT_STATE_IDLE);

	spa_log_debug(monitor->log, NAME": transport %p: Release %s",
			transport, transport->path);

	close(transport->fd);
	transport->fd = -1;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
					 BLUEZ_MEDIA_TRANSPORT_INTERFACE,
					 "Release");
	if (m == NULL)
		return -ENOMEM;

	dbus_error_init(&err);

	r = dbus_connection_send_with_reply_and_block(monitor->conn, m, -1, &err);
	dbus_message_unref(m);
	m = NULL;

	if (r != NULL)
		dbus_message_unref(r);

	if (dbus_error_is_set(&err)) {
		if (is_idle) {
			/* XXX: The fd always needs to be closed. However, Release()
			 * XXX: apparently doesn't need to be called on idle transports
			 * XXX: and fails. We call it just to be sure (e.g. in case
			 * XXX: there's a race with updating the property), but tone down the error.
			 */
			spa_log_debug(monitor->log, "Failed to release idle transport %s: %s",
			              transport->path, err.message);
		} else {
			spa_log_error(monitor->log, "Failed to release transport %s: %s",
			              transport->path, err.message);
		}
		dbus_error_free(&err);
	}
	else
		spa_log_info(monitor->log, "Transport %s released", transport->path);

	return 0;
}

static const struct spa_bt_transport_implementation transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = transport_acquire,
	.release = transport_release,
};

static void append_basic_array_variant_dict_entry(DBusMessageIter *dict, int key_type_int, void* key, const char* variant_type_str, const char* array_type_str, int array_type_int, void* data, int data_size);

static void a2dp_codec_switch_reply(DBusPendingCall *pending, void *userdata);

static int a2dp_codec_switch_cmp(const void *a, const void *b);

static struct spa_bt_a2dp_codec_switch *a2dp_codec_switch_cmp_sw;  /* global for qsort */

static void a2dp_codec_switch_free(struct spa_bt_a2dp_codec_switch *sw)
{
	char **p;

	if (sw->pending != NULL) {
		dbus_pending_call_cancel(sw->pending);
		dbus_pending_call_unref(sw->pending);
	}

	if (sw->device != NULL)
		spa_list_remove(&sw->device_link);

	if (sw->paths != NULL)
		for (p = sw->paths; *p != NULL; ++p)
			free(*p);

	free(sw->paths);
	free(sw->codecs);
	free(sw);
}

static void a2dp_codec_switch_next(struct spa_bt_a2dp_codec_switch *sw)
{
	spa_assert(*sw->codec_iter != NULL && *sw->path_iter != NULL);

	++sw->path_iter;
	if (*sw->path_iter == NULL) {
		++sw->codec_iter;
		sw->path_iter = sw->paths;
	}
}

static bool a2dp_codec_switch_process_current(struct spa_bt_a2dp_codec_switch *sw)
{
	struct spa_bt_remote_endpoint *ep;
	const struct a2dp_codec *codec;
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	char *local_endpoint_base;
	char *local_endpoint = NULL;
	int res, config_size;
	dbus_bool_t dbus_ret;
	const char *str;
	DBusMessage *m;
	DBusMessageIter iter, d;
	int i;

	/* Try setting configuration for current codec on current endpoint in list */

	codec = *sw->codec_iter;

	spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: consider codec %s for remote endpoint %s",
	              sw, (*sw->codec_iter)->name, *sw->path_iter);

	ep = device_remote_endpoint_find(sw->device, *sw->path_iter);

	if (ep == NULL || ep->capabilities == NULL || ep->uuid == NULL) {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: endpoint %s not valid, try next",
		              sw, *sw->path_iter);
		goto next;
	}

	/* Setup and check compatible configuration */
	if (ep->codec != codec->codec_id) {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: different codec, try next", sw);
		goto next;
	}

	if (!(sw->profile & spa_bt_profile_from_uuid(ep->uuid))) {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: wrong uuid (%s) for profile, try next",
		              sw, ep->uuid);
		goto next;
	}

	if (sw->profile & SPA_BT_PROFILE_A2DP_SINK) {
		local_endpoint_base = A2DP_SOURCE_ENDPOINT;
	} else if (sw->profile & SPA_BT_PROFILE_A2DP_SOURCE) {
		local_endpoint_base = A2DP_SINK_ENDPOINT;
	} else {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: bad profile (%d), try next",
		              sw, sw->profile);
		goto next;
	}

	if (a2dp_codec_to_endpoint(codec, local_endpoint_base, &local_endpoint) < 0) {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: no endpoint for codec %s, try next",
		              sw, codec->name);
		goto next;
	}

	res = codec->select_config(codec, 0, ep->capabilities, ep->capabilities_len, NULL, config);
	if (res < 0) {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: incompatible capabilities (%d), try next",
		              sw, res);
		goto next;
	}
	config_size = res;

	spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: configuration %d", sw, config_size);
	for (i = 0; i < config_size; i++)
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p:     %d: %02x", sw, i, config[i]);

	/* org.bluez.MediaEndpoint1.SetConfiguration on remote endpoint */
	m = dbus_message_new_method_call(BLUEZ_SERVICE, ep->path, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration");
	if (m == NULL) {
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: dbus allocation failure, try next", sw);
		goto next;
	}

	spa_log_info(sw->device->monitor->log, NAME": a2dp codec switch %p: trying codec %s for endpoint %s, local endpoint %s",
	             sw, codec->name, ep->path, local_endpoint);

	dbus_message_iter_init_append(m, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &local_endpoint);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &d);
	str = "Capabilities";
	append_basic_array_variant_dict_entry(&d, DBUS_TYPE_STRING, &str, "ay", "y", DBUS_TYPE_BYTE, config, config_size);
	dbus_message_iter_close_container(&iter, &d);

	spa_assert(sw->pending == NULL);
	dbus_ret = dbus_connection_send_with_reply(sw->device->monitor->conn, m, &sw->pending, -1);

	if (!dbus_ret || sw->pending == NULL) {
		spa_log_error(sw->device->monitor->log, NAME": a2dp codec switch %p: dbus call failure, try next", sw);
		dbus_message_unref(m);
		goto next;
	}

	dbus_ret = dbus_pending_call_set_notify(sw->pending, a2dp_codec_switch_reply, sw, NULL);
	dbus_message_unref(m);

	if (!dbus_ret) {
		spa_log_error(sw->device->monitor->log, NAME": a2dp codec switch %p: dbus set notify failure", sw);
		goto next;
	}

	free(local_endpoint);
	return true;

next:
	free(local_endpoint);
	return false;
}

static void a2dp_codec_switch_process(struct spa_bt_a2dp_codec_switch *sw)
{
	while (*sw->codec_iter != NULL && *sw->path_iter != NULL) {
		if (sw->path_iter == sw->paths && (*sw->codec_iter)->caps_preference_cmp) {
			/* Sort endpoints according to codec preference, when at a new codec. */
			a2dp_codec_switch_cmp_sw = sw;
			qsort(sw->paths, sw->num_paths, sizeof(char *), a2dp_codec_switch_cmp);
		}

		if (a2dp_codec_switch_process_current(sw)) {
			/* Wait for dbus reply */
			return;
		}

		a2dp_codec_switch_next(sw);
	};

	/* Didn't find any suitable endpoint. Report failure. */
	spa_log_info(sw->device->monitor->log, NAME": a2dp codec switch %p: failed to get an endpoint", sw);
	sw->device->active_codec_switch = NULL;
	spa_bt_device_emit_codec_switched(sw->device, -ENODEV);
	spa_bt_device_check_profiles(sw->device, false);
	a2dp_codec_switch_free(sw);
}

static void a2dp_codec_switch_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_a2dp_codec_switch *sw = user_data;
	struct spa_bt_device *device = sw->device;
	DBusMessage *r;

	r = dbus_pending_call_steal_reply(pending);

	spa_assert(sw->pending == pending);
	dbus_pending_call_unref(pending);
	sw->pending = NULL;

	if (sw->device->active_codec_switch != sw) {
		/* This codec switch has been canceled. Switch to the new one. */
		spa_log_debug(sw->device->monitor->log, NAME": a2dp codec switch %p: canceled, go to new switch", sw);
		a2dp_codec_switch_free(sw);

		if (r != NULL)
			dbus_message_unref(r);

		spa_assert(device->active_codec_switch != NULL);
		a2dp_codec_switch_process(device->active_codec_switch);
		return;
	}

	if (r == NULL) {
		spa_log_error(sw->device->monitor->log,
		              NAME": a2dp codec switch %p: empty reply from dbus, trying next",
		              sw);
		goto next;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_debug(sw->device->monitor->log,
		              NAME": a2dp codec switch %p: failed (%s), trying next",
		              sw, dbus_message_get_error_name(r));
		dbus_message_unref(r);
		goto next;
	}

	dbus_message_unref(r);

	/* Success */
	spa_log_info(sw->device->monitor->log, NAME": a2dp codec switch %p: success", sw);
	device->active_codec_switch = NULL;
	spa_bt_device_emit_codec_switched(sw->device, 0);
	spa_bt_device_check_profiles(sw->device, false);
	a2dp_codec_switch_free(sw);
	return;

next:
	a2dp_codec_switch_next(sw);
	a2dp_codec_switch_process(sw);
	return;
}

static int a2dp_codec_switch_cmp(const void *a, const void *b)
{
	struct spa_bt_a2dp_codec_switch *sw = a2dp_codec_switch_cmp_sw;
	const struct a2dp_codec *codec = *sw->codec_iter;
	const char *path1 = *(char **)a, *path2 = *(char **)b;
	struct spa_bt_remote_endpoint *ep1, *ep2;

	ep1 = device_remote_endpoint_find(sw->device, path1);
	ep2 = device_remote_endpoint_find(sw->device, path2);

	if (ep1 != NULL && (ep1->uuid == NULL || ep1->codec != codec->codec_id || ep1->capabilities == NULL))
		ep1 = NULL;
	if (ep2 != NULL && (ep2->uuid == NULL || ep2->codec != codec->codec_id || ep2->capabilities == NULL))
		ep2 = NULL;

	if (ep1 == NULL && ep2 == NULL)
		return 0;
	else if (ep1 == NULL)
		return 1;
	else if (ep2 == NULL)
		return -1;

	return codec->caps_preference_cmp(codec, ep1->capabilities, ep1->capabilities_len,
			ep2->capabilities, ep2->capabilities_len);
}

/* Ensure there's a transport for at least one of the listed codecs */
int spa_bt_device_ensure_a2dp_codec(struct spa_bt_device *device, const struct a2dp_codec **codecs)
{
	struct spa_bt_a2dp_codec_switch *sw;
	struct spa_bt_remote_endpoint *ep;
	struct spa_bt_transport *t;
	const struct a2dp_codec *preferred_codec = NULL;
	size_t i, j, num_codecs, num_eps;

	if (!device->adapter->application_registered) {
		/* Codec switching not supported */
		return -ENOTSUP;
	}

	for (i = 0; codecs[i] != NULL; ++i) {
		if (spa_bt_device_supports_a2dp_codec(device, codecs[i])) {
			preferred_codec = codecs[i];
			break;
		}
	}

	/* Check if we already have an enabled transport for the most preferred codec.
	 * However, if there already was a codec switch running, these transports may
	 * disapper soon. In that case, we have to do the full thing.
	 */
	if (device->active_codec_switch == NULL && preferred_codec != NULL) {
		spa_list_for_each(t, &device->transport_list, device_link) {
			if (t->a2dp_codec != preferred_codec || !t->enabled)
				continue;

			if ((device->connected_profiles & t->profile) != t->profile)
				continue;

			spa_bt_device_emit_codec_switched(device, 0);
			return 0;
		}
	}

	/* Setup and start iteration */

	sw = calloc(1, sizeof(struct spa_bt_a2dp_codec_switch));
	if (sw == NULL)
		return -ENOMEM;

	num_eps = 0;
	spa_list_for_each(ep, &device->remote_endpoint_list, device_link)
		++num_eps;

	num_codecs = 0;
	while (codecs[num_codecs] != NULL)
		++num_codecs;

	sw->codecs = calloc(num_codecs + 1, sizeof(const struct a2dp_codec *));
	sw->paths = calloc(num_eps + 1, sizeof(char *));
	sw->num_paths = num_eps;

	if (sw->codecs == NULL || sw->paths == NULL) {
		a2dp_codec_switch_free(sw);
		return -ENOMEM;
	}

	for (i = 0, j = 0; i < num_codecs; ++i) {
		if (device_can_accept_a2dp_codec(device, codecs[i])) {
			sw->codecs[j] = codecs[i];
			++j;
		}
	}
	sw->codecs[j] = NULL;

	i = 0;
	spa_list_for_each(ep, &device->remote_endpoint_list, device_link) {
		sw->paths[i] = strdup(ep->path);
		if (sw->paths[i] == NULL) {
			a2dp_codec_switch_free(sw);
			return -ENOMEM;
		}
		++i;
	}
	sw->paths[i] = NULL;

	sw->codec_iter = sw->codecs;
	sw->path_iter = sw->paths;

	sw->profile = device->connected_profiles;

	sw->device = device;
	spa_list_append(&device->codec_switch_list, &sw->device_link);

	if (device->active_codec_switch != NULL) {
		/*
		 * There's a codec switch already running.  BlueZ does not appear to allow
		 * calling dbus_pending_call_cancel on an active request, so we have to
		 * wait for the reply to arrive first, and only then start processing this
		 * request.
		 */
		spa_log_debug(sw->device->monitor->log,
		             NAME": a2dp codec switch: already in progress, canceling previous");
		spa_assert(device->active_codec_switch->pending != NULL);
		device->active_codec_switch = sw;
	} else {
		device->active_codec_switch = sw;
		a2dp_codec_switch_process(sw);
	}

	return 0;
}

static DBusHandlerResult endpoint_set_configuration(DBusConnection *conn,
		const char *path, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *transport_path, *endpoint;
	DBusMessageIter it[2];
	DBusMessage *r;
	struct spa_bt_transport *transport;
	bool is_new = false;
	const struct a2dp_codec *codec;

	if (!dbus_message_has_signature(m, "oa{sv}")) {
		spa_log_warn(monitor->log, "invalid SetConfiguration() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	endpoint = dbus_message_get_path(m);

	codec = a2dp_endpoint_to_codec(endpoint);
	if (codec == NULL) {
		spa_log_warn(monitor->log, "unknown SetConfiguration() codec");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &transport_path);
	dbus_message_iter_next(&it[0]);
	dbus_message_iter_recurse(&it[0], &it[1]);

	transport = spa_bt_transport_find(monitor, transport_path);
	is_new = transport == NULL;

	if (is_new) {
		transport = spa_bt_transport_create(monitor, strdup(transport_path), 0);
		if (transport == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		spa_bt_transport_set_implementation(transport, &transport_impl, transport);
	}
	transport->a2dp_codec = codec;
	transport_update_props(transport, &it[1], NULL);

	if (transport->device == NULL) {
		spa_log_warn(monitor->log, "no device found for transport");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (is_new)
		spa_list_append(&transport->device->transport_list, &transport->device_link);

	if (codec->validate_config) {
		struct spa_audio_info info;
		if (codec->validate_config(codec, 0,
					transport->configuration, transport->configuration_len,
					&info) < 0) {
			spa_log_error(monitor->log, "invalid transport configuration");
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		transport->n_channels = info.info.raw.channels;
		memcpy(transport->channels, info.info.raw.position,
				transport->n_channels * sizeof(uint32_t));
	} else {
		transport->n_channels = 2;
		transport->channels[0] = SPA_AUDIO_CHANNEL_FL;
		transport->channels[1] = SPA_AUDIO_CHANNEL_FR;
	}
	spa_log_info(monitor->log, "%p: %s validate conf channels:%d",
			monitor, path, transport->n_channels);

	/*
	 * Per-device determination of which codecs are allowed needs to be done also
	 * here. The A2DP codec switching process does this filtering, but before that
	 * BlueZ may autoconnect to any endpoint with compatible caps.  In case it got it
	 * wrong, mark the transport disabled, and we'll switch to a better one later when
	 * needed. (We don't want to fail the connection, because we want to keep the
	 * profile connected.)
	 */
	transport->enabled = device_can_accept_a2dp_codec(transport->device, codec);

	spa_bt_device_connect_profile(transport->device, transport->profile);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_clear_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	DBusError err;
	DBusMessage *r;
	const char *transport_path;
	struct spa_bt_transport *transport;

	dbus_error_init(&err);

	if (!dbus_message_get_args(m, &err,
				   DBUS_TYPE_OBJECT_PATH, &transport_path,
				   DBUS_TYPE_INVALID)) {
		spa_log_warn(monitor->log, "Bad ClearConfiguration method call: %s",
			err.message);
		dbus_error_free(&err);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	transport = spa_bt_transport_find(monitor, transport_path);

	if (transport != NULL) {
		struct spa_bt_device *device = transport->device;

		spa_log_debug(monitor->log, "transport %p: free %s",
			transport, transport->path);

		spa_bt_transport_free(transport);
		if (device != NULL)
			spa_bt_device_check_profiles(device, false);
	}

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	DBusMessage *r;

	r = dbus_message_new_error(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE ".Error.NotImplemented",
                                            "Method not implemented");
	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path, *interface, *member;
	DBusMessage *r;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(monitor->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = ENDPOINT_INTROSPECT_XML;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_unref(r);
		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration"))
		res = endpoint_set_configuration(c, path, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SelectConfiguration"))
		res = endpoint_select_configuration(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "ClearConfiguration"))
		res = endpoint_clear_configuration(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "Release"))
		res = endpoint_release(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void bluez_register_endpoint_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusMessage *r;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "BlueZ D-Bus ObjectManager not available");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterEndpoint() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

	finish:
	dbus_message_unref(r);
	dbus_pending_call_unref(pending);
}

static void append_basic_variant_dict_entry(DBusMessageIter *dict, int key_type_int, void* key, int variant_type_int, const char* variant_type_str, void* variant) {
	DBusMessageIter dict_entry_it, variant_it;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry_it);
	dbus_message_iter_append_basic(&dict_entry_it, key_type_int, key);

	dbus_message_iter_open_container(&dict_entry_it, DBUS_TYPE_VARIANT, variant_type_str, &variant_it);
	dbus_message_iter_append_basic(&variant_it, variant_type_int, variant);
	dbus_message_iter_close_container(&dict_entry_it, &variant_it);
	dbus_message_iter_close_container(dict, &dict_entry_it);
}

static void append_basic_array_variant_dict_entry(DBusMessageIter *dict, int key_type_int, void* key, const char* variant_type_str, const char* array_type_str, int array_type_int, void* data, int data_size) {
	DBusMessageIter dict_entry_it, variant_it, array_it;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry_it);
	dbus_message_iter_append_basic(&dict_entry_it, key_type_int, key);

	dbus_message_iter_open_container(&dict_entry_it, DBUS_TYPE_VARIANT, variant_type_str, &variant_it);
	dbus_message_iter_open_container(&variant_it, DBUS_TYPE_ARRAY, array_type_str, &array_it);
	dbus_message_iter_append_fixed_array (&array_it, array_type_int, &data, data_size);
	dbus_message_iter_close_container(&variant_it, &array_it);
	dbus_message_iter_close_container(&dict_entry_it, &variant_it);
	dbus_message_iter_close_container(dict, &dict_entry_it);
}

static int bluez_register_endpoint(struct spa_bt_monitor *monitor,
                             const char *path, const char *endpoint,
			     const char *uuid, const struct a2dp_codec *codec) {
	char *str, *object_path = NULL;
	DBusMessage *m;
	DBusMessageIter object_it, dict_it;
	DBusPendingCall *call;
	uint8_t caps[A2DP_MAX_CAPS_SIZE];
	int ret, caps_size;
	uint16_t codec_id = codec->codec_id;

	ret = a2dp_codec_to_endpoint(codec, endpoint, &object_path);
	if (ret < 0)
		return ret;

	caps_size = codec->fill_caps(codec, 0, caps);
	if (caps_size < 0)
		return caps_size;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
	                                 path,
	                                 BLUEZ_MEDIA_INTERFACE,
	                                 "RegisterEndpoint");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &object_it);
	dbus_message_iter_append_basic(&object_it, DBUS_TYPE_OBJECT_PATH, &object_path);

	dbus_message_iter_open_container(&object_it, DBUS_TYPE_ARRAY, "{sv}", &dict_it);

	str = "UUID";
	append_basic_variant_dict_entry(&dict_it, DBUS_TYPE_STRING, &str, DBUS_TYPE_STRING, "s", &uuid);
	str = "Codec";
	append_basic_variant_dict_entry(&dict_it, DBUS_TYPE_STRING, &str, DBUS_TYPE_BYTE, "y", &codec_id);
	str = "Capabilities";
	append_basic_array_variant_dict_entry(&dict_it, DBUS_TYPE_STRING, &str, "ay", "y", DBUS_TYPE_BYTE, caps, caps_size);

	dbus_message_iter_close_container(&object_it, &dict_it);

	dbus_connection_send_with_reply(monitor->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, bluez_register_endpoint_reply, monitor, NULL);
	dbus_message_unref(m);

	free(object_path);

	return 0;
}

static int register_a2dp_endpoint(struct spa_bt_monitor *monitor,
		const struct a2dp_codec *codec, const char *endpoint)
{
	int ret;
	char* object_path = NULL;
	const DBusObjectPathVTable vtable_endpoint = {
		.message_function = endpoint_handler,
	};

	ret = a2dp_codec_to_endpoint(codec, endpoint, &object_path);
	if (ret < 0)
		return ret;

	spa_log_info(monitor->log, "Registering endpoint: %s", object_path);

	if (!dbus_connection_register_object_path(monitor->conn,
	                                          object_path,
	                                          &vtable_endpoint, monitor)) {
		free(object_path);
		return -EIO;
	}

	free(object_path);
	return 0;

}

static int adapter_register_endpoints(struct spa_bt_adapter *a)
{
	struct spa_bt_monitor *monitor = a->monitor;
	int i;
	int err = 0;

	if (a->endpoints_registered)
	    return err;

	/* The legacy bluez5 api doesn't support codec switching
	 * It doesn't make sense to register codecs other than SBC
	 * as bluez5 will probably use SBC anyway and we have no control over it
	 * let's incentivize users to upgrade their bluez5 daemon
	 * if they want proper a2dp codec support
	 * */
	spa_log_warn(monitor->log, "Using legacy bluez5 API for A2DP - only SBC will be supported. "
                               "Please upgrade bluez5.");

	for (i = 0; a2dp_codecs[i]; i++) {
		const struct a2dp_codec *codec = a2dp_codecs[i];

		if (!is_a2dp_codec_enabled(monitor, codec))
			continue;

		if (!(codec->codec_id == A2DP_CODEC_SBC && strcmp(codec->name, "sbc") == 0))
			continue;

		if ((err = bluez_register_endpoint(monitor, a->path,
						   A2DP_SOURCE_ENDPOINT,
		                                   SPA_BT_UUID_A2DP_SOURCE,
		                                   codec)))
			goto out;

		if ((err = bluez_register_endpoint(monitor, a->path,
						   A2DP_SINK_ENDPOINT,
		                                   SPA_BT_UUID_A2DP_SINK,
		                                   codec)))
			goto out;

		a->endpoints_registered = true;
		break;
	}

	if (!a->endpoints_registered) {
		/* Should never happen as SBC support is always enabled */
		spa_log_error(monitor->log, "Broken Pipewire build - unable to locate SBC codec");
		err = -ENOSYS;
	}

	out:
	if (err) {
		spa_log_error(monitor->log, "Failed to register bluez5 endpoints");
	}
	return err;
}

static void append_a2dp_object(DBusMessageIter *iter, const char *endpoint,
		const char *uuid, uint8_t codec_id, uint8_t *caps, size_t caps_size)
{
	char* str;
	const char *interface_name = BLUEZ_MEDIA_ENDPOINT_INTERFACE;
	DBusMessageIter object, array, entry, dict;
	dbus_bool_t delay_reporting;

	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &object);
	dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH, &endpoint);

	dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sa{sv}}", &array);

	dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface_name);

	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &dict);

	str = "UUID";
	append_basic_variant_dict_entry(&dict, DBUS_TYPE_STRING, &str, DBUS_TYPE_STRING, "s", &uuid);
	str = "Codec";
	append_basic_variant_dict_entry(&dict, DBUS_TYPE_STRING, &str, DBUS_TYPE_BYTE, "y", &codec_id);
	str = "Capabilities";
	append_basic_array_variant_dict_entry(&dict, DBUS_TYPE_STRING, &str, "ay", "y", DBUS_TYPE_BYTE, caps, caps_size);
	if (spa_bt_profile_from_uuid(uuid) & SPA_BT_PROFILE_A2DP_SOURCE) {
		str = "DelayReporting";
		delay_reporting = TRUE;
		append_basic_variant_dict_entry(&dict, DBUS_TYPE_STRING, &str, DBUS_TYPE_BOOLEAN, "b", &delay_reporting);
	}

	dbus_message_iter_close_container(&entry, &dict);
	dbus_message_iter_close_container(&array, &entry);
	dbus_message_iter_close_container(&object, &array);
	dbus_message_iter_close_container(iter, &object);
}

static DBusHandlerResult object_manager_handler(DBusConnection *c, DBusMessage *m, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	const char *path, *interface, *member;
	char *endpoint;
	DBusMessage *r;
	DBusMessageIter iter, array;
	DBusHandlerResult res;
	int i;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(monitor->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = OBJECT_MANAGER_INTROSPECT_XML;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_unref(r);
		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")) {
		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_iter_init_append(r, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &array);

		for (i = 0; a2dp_codecs[i]; i++) {
			const struct a2dp_codec *codec = a2dp_codecs[i];
			uint8_t caps[A2DP_MAX_CAPS_SIZE];
			int caps_size, ret;
			uint16_t codec_id = codec->codec_id;

			if (!is_a2dp_codec_enabled(monitor, codec))
				continue;

			caps_size = codec->fill_caps(codec, 0, caps);
			if (caps_size < 0)
				continue;

			if (codec->decode != NULL) {
				ret = a2dp_codec_to_endpoint(codec, A2DP_SINK_ENDPOINT, &endpoint);
				if (ret == 0) {
					spa_log_info(monitor->log, "register A2DP sink codec %s: %s", a2dp_codecs[i]->name, endpoint);
					append_a2dp_object(&array, endpoint, SPA_BT_UUID_A2DP_SINK,
							codec_id, caps, caps_size);
					free(endpoint);
				}
			}

			if (codec->encode != NULL) {
				ret = a2dp_codec_to_endpoint(codec, A2DP_SOURCE_ENDPOINT, &endpoint);
				if (ret == 0) {
					spa_log_info(monitor->log, "register A2DP source codec %s: %s", a2dp_codecs[i]->name, endpoint);
					append_a2dp_object(&array, endpoint, SPA_BT_UUID_A2DP_SOURCE,
							codec_id, caps, caps_size);
					free(endpoint);
				}
			}
		}

		dbus_message_iter_close_container(&iter, &array);
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		res = DBUS_HANDLER_RESULT_HANDLED;

		/* Reconnect A2DP profiles for existing connected devices */
		reconnect_device_profiles(monitor);
	}
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void bluez_register_application_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_adapter *adapter = user_data;
	struct spa_bt_monitor *monitor = adapter->monitor;
	DBusMessage *r;
	bool fallback = true;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
		spa_log_warn(monitor->log, "Registering media applications for adapter %s is disabled in bluez5", adapter->path);
		goto finish;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterApplication() failed: %s",
		        dbus_message_get_error_name(r));
		goto finish;
	}

	fallback = false;
	adapter->application_registered = true;

finish:
	dbus_message_unref(r);
	dbus_pending_call_unref(pending);

	if (fallback)
		adapter_register_endpoints(adapter);
}

static int register_media_application(struct spa_bt_monitor * monitor)
{
	const DBusObjectPathVTable vtable_object_manager = {
		.message_function = object_manager_handler,
	};

	spa_log_info(monitor->log, "Registering media application object: " A2DP_OBJECT_MANAGER_PATH);

	if (!dbus_connection_register_object_path(monitor->conn,
	                                          A2DP_OBJECT_MANAGER_PATH,
	                                          &vtable_object_manager, monitor))
		return -EIO;

	for (int i = 0; a2dp_codecs[i]; i++) {
		const struct a2dp_codec *codec = a2dp_codecs[i];

		if (!is_a2dp_codec_enabled(monitor, codec))
			continue;

		register_a2dp_endpoint(monitor, codec, A2DP_SOURCE_ENDPOINT);
		register_a2dp_endpoint(monitor, codec, A2DP_SINK_ENDPOINT);
	}

	return 0;
}

static void unregister_media_application(struct spa_bt_monitor * monitor)
{
	int ret;
	char *object_path = NULL;
	dbus_connection_unregister_object_path(monitor->conn, A2DP_OBJECT_MANAGER_PATH);

	for (int i = 0; a2dp_codecs[i]; i++) {
		const struct a2dp_codec *codec = a2dp_codecs[i];

		if (!is_a2dp_codec_enabled(monitor, codec))
			continue;

		ret = a2dp_codec_to_endpoint(codec, A2DP_SOURCE_ENDPOINT, &object_path);
		if (ret == 0) {
			dbus_connection_unregister_object_path(monitor->conn, object_path);
			free(object_path);
		}

		ret = a2dp_codec_to_endpoint(codec, A2DP_SINK_ENDPOINT, &object_path);
		if (ret == 0) {
			dbus_connection_unregister_object_path(monitor->conn, object_path);
			free(object_path);
		}
	}
}

static int adapter_register_application(struct spa_bt_adapter *a) {
	const char *object_manager_path = A2DP_OBJECT_MANAGER_PATH;
	struct spa_bt_monitor *monitor = a->monitor;
	DBusMessage *m;
	DBusMessageIter i, d;
	DBusPendingCall *call;

	if (a->application_registered)
		return 0;

	spa_log_debug(monitor->log, "Registering bluez5 media application on adapter %s", a->path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
	                                 a->path,
	                                 BLUEZ_MEDIA_INTERFACE,
	                                 "RegisterApplication");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &object_manager_path);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &d);
	dbus_message_iter_close_container(&i, &d);

	dbus_connection_send_with_reply(monitor->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, bluez_register_application_reply, a, NULL);
	dbus_message_unref(m);

	return 0;
}

static void interface_added(struct spa_bt_monitor *monitor,
			    DBusConnection *conn,
			    const char *object_path,
			    const char *interface_name,
			    DBusMessageIter *props_iter)
{
	spa_log_debug(monitor->log, "Found object %s, interface %s", object_path, interface_name);

	if (strcmp(interface_name, BLUEZ_ADAPTER_INTERFACE) == 0) {
		struct spa_bt_adapter *a;

		a = adapter_find(monitor, object_path);
		if (a == NULL) {
			a = adapter_create(monitor, object_path);
			if (a == NULL) {
				spa_log_warn(monitor->log, "can't create adapter: %m");
				return;
			}
		}
		adapter_update_props(a, props_iter, NULL);
		adapter_register_application(a);
	}
	else if (strcmp(interface_name, BLUEZ_PROFILE_MANAGER_INTERFACE) == 0) {
		backend_native_register_profiles(monitor->backend_native);
	}
	else if (strcmp(interface_name, BLUEZ_DEVICE_INTERFACE) == 0) {
		struct spa_bt_device *d;

		d = spa_bt_device_find(monitor, object_path);
		if (d == NULL) {
			d = device_create(monitor, object_path);
			if (d == NULL) {
				spa_log_warn(monitor->log, "can't create Bluetooth device %s: %m",
						object_path);
				return;
			}
		}
		device_update_props(d, props_iter, NULL);
	}
	else if (strcmp(interface_name, BLUEZ_MEDIA_ENDPOINT_INTERFACE) == 0) {
		struct spa_bt_remote_endpoint *ep;

		ep = remote_endpoint_find(monitor, object_path);
		if (ep == NULL) {
			ep = remote_endpoint_create(monitor, object_path);
			if (ep == NULL) {
				spa_log_warn(monitor->log, "can't create Bluetooth remote endpoint %s: %m",
				             object_path);
				return;
			}
		}
		remote_endpoint_update_props(ep, props_iter, NULL);
	}
}

static void interfaces_added(struct spa_bt_monitor *monitor, DBusMessageIter *arg_iter)
{
	DBusMessageIter it[3];
	const char *object_path;

	dbus_message_iter_get_basic(arg_iter, &object_path);
	dbus_message_iter_next(arg_iter);
	dbus_message_iter_recurse(arg_iter, &it[0]);

	while (dbus_message_iter_get_arg_type(&it[0]) != DBUS_TYPE_INVALID) {
		const char *interface_name;

		dbus_message_iter_recurse(&it[0], &it[1]);
		dbus_message_iter_get_basic(&it[1], &interface_name);
		dbus_message_iter_next(&it[1]);
		dbus_message_iter_recurse(&it[1], &it[2]);

		interface_added(monitor, monitor->conn,
				object_path, interface_name,
				&it[2]);

		dbus_message_iter_next(&it[0]);
	}
}

static void interfaces_removed(struct spa_bt_monitor *monitor, DBusMessageIter *arg_iter)
{
	const char *object_path;
	DBusMessageIter it;

	dbus_message_iter_get_basic(arg_iter, &object_path);
	dbus_message_iter_next(arg_iter);
	dbus_message_iter_recurse(arg_iter, &it);

	while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
		const char *interface_name;

		dbus_message_iter_get_basic(&it, &interface_name);

		spa_log_debug(monitor->log, "Found object %s, interface %s", object_path, interface_name);

		if (strcmp(interface_name, BLUEZ_DEVICE_INTERFACE) == 0) {
			struct spa_bt_device *d;
			d = spa_bt_device_find(monitor, object_path);
			if (d != NULL)
				device_free(d);
		} else if (strcmp(interface_name, BLUEZ_ADAPTER_INTERFACE) == 0) {
			struct spa_bt_adapter *a;
			a = adapter_find(monitor, object_path);
			if (a != NULL)
				adapter_free(a);
		} else if (strcmp(interface_name, BLUEZ_MEDIA_ENDPOINT_INTERFACE) == 0) {
			struct spa_bt_remote_endpoint *ep;
			ep = remote_endpoint_find(monitor, object_path);
			if (ep != NULL)
				remote_endpoint_free(ep);
		}

		dbus_message_iter_next(&it);
	}
}

static void get_managed_objects_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusMessage *r;
	DBusMessageIter it[6];

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "BlueZ D-Bus ObjectManager not available");
		goto finish;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "GetManagedObjects() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

	if (!dbus_message_iter_init(r, &it[0]) ||
	    strcmp(dbus_message_get_signature(r), "a{oa{sa{sv}}}") != 0) {
		spa_log_error(monitor->log, "Invalid reply signature for GetManagedObjects()");
		goto finish;
	}

	dbus_message_iter_recurse(&it[0], &it[1]);

	while (dbus_message_iter_get_arg_type(&it[1]) != DBUS_TYPE_INVALID) {
		dbus_message_iter_recurse(&it[1], &it[2]);

		interfaces_added(monitor, &it[2]);

		dbus_message_iter_next(&it[1]);
	}

	monitor->objects_listed = true;

      finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
	return;
}

static void get_managed_objects(struct spa_bt_monitor *monitor)
{
	DBusMessage *m;
	DBusPendingCall *call;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 "/",
					 "org.freedesktop.DBus.ObjectManager",
					 "GetManagedObjects");

	dbus_connection_send_with_reply(monitor->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, get_managed_objects_reply, monitor, NULL);
        dbus_message_unref(m);
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusError err;

	dbus_error_init(&err);

	if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;

		spa_log_debug(monitor->log, "Name owner changed %s", dbus_message_get_path(m));

		if (!dbus_message_get_args(m, &err,
					   DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_STRING, &old_owner,
					   DBUS_TYPE_STRING, &new_owner,
					   DBUS_TYPE_INVALID)) {
			spa_log_error(monitor->log, NAME": Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
			goto fail;
		}

		if (strcmp(name, BLUEZ_SERVICE) == 0) {
			if (old_owner && *old_owner) {
				struct spa_bt_adapter *a;
				struct spa_bt_device *d;
				struct spa_bt_remote_endpoint *ep;
				struct spa_bt_transport *t;

				spa_log_debug(monitor->log, "Bluetooth daemon disappeared");
				monitor->objects_listed = false;

				spa_list_consume(t, &monitor->transport_list, link)
					spa_bt_transport_free(t);
				spa_list_consume(ep, &monitor->remote_endpoint_list, link)
					remote_endpoint_free(ep);
				spa_list_consume(d, &monitor->device_list, link)
					device_free(d);
				spa_list_consume(a, &monitor->adapter_list, link)
					adapter_free(a);
			}

			if (new_owner && *new_owner) {
				spa_log_debug(monitor->log, "Bluetooth daemon appeared");
				get_managed_objects(monitor);
			}
		}
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded")) {
		DBusMessageIter it;

		spa_log_debug(monitor->log, "interfaces added %s", dbus_message_get_path(m));

		if (!monitor->objects_listed)
			goto finish;

		if (!dbus_message_iter_init(m, &it) || strcmp(dbus_message_get_signature(m), "oa{sa{sv}}") != 0) {
			spa_log_error(monitor->log, NAME": Invalid signature found in InterfacesAdded");
			goto finish;
		}

		interfaces_added(monitor, &it);
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
		DBusMessageIter it;

		spa_log_debug(monitor->log, "interfaces removed %s", dbus_message_get_path(m));

		if (!monitor->objects_listed)
			goto finish;

		if (!dbus_message_iter_init(m, &it) || strcmp(dbus_message_get_signature(m), "oas") != 0) {
			spa_log_error(monitor->log, NAME": Invalid signature found in InterfacesRemoved");
			goto finish;
		}

		interfaces_removed(monitor, &it);
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
		DBusMessageIter it[2];
		const char *iface, *path;

		if (!monitor->objects_listed)
			goto finish;

		if (!dbus_message_iter_init(m, &it[0]) ||
		    strcmp(dbus_message_get_signature(m), "sa{sv}as") != 0) {
			spa_log_error(monitor->log, "Invalid signature found in PropertiesChanged");
			goto finish;
		}
		path = dbus_message_get_path(m);

		dbus_message_iter_get_basic(&it[0], &iface);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		if (strcmp(iface, BLUEZ_ADAPTER_INTERFACE) == 0) {
			struct spa_bt_adapter *a;

			a = adapter_find(monitor, path);
			if (a == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown adapter %s", path);
				goto finish;
			}
			spa_log_debug(monitor->log, "Properties changed in adapter %s", path);

			adapter_update_props(a, &it[1], NULL);
		}
		else if (strcmp(iface, BLUEZ_DEVICE_INTERFACE) == 0) {
			struct spa_bt_device *d;

			d = spa_bt_device_find(monitor, path);
			if (d == NULL) {
				spa_log_debug(monitor->log,
						"Properties changed in unknown device %s", path);
				goto finish;
			}
			spa_log_debug(monitor->log, "Properties changed in device %s", path);

			device_update_props(d, &it[1], NULL);
		}
		else if (strcmp(iface, BLUEZ_MEDIA_ENDPOINT_INTERFACE) == 0) {
			struct spa_bt_remote_endpoint *ep;

			ep = remote_endpoint_find(monitor, path);
			if (ep == NULL) {
				spa_log_debug(monitor->log,
						"Properties changed in unknown remote endpoint %s", path);
				goto finish;
			}
			spa_log_debug(monitor->log, "Properties changed in remote endpoint %s", path);

			remote_endpoint_update_props(ep, &it[1], NULL);
		}
		else if (strcmp(iface, BLUEZ_MEDIA_TRANSPORT_INTERFACE) == 0) {
			struct spa_bt_transport *transport;

			transport = spa_bt_transport_find(monitor, path);
			if (transport == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown transport %s", path);
				goto finish;
			}

			spa_log_debug(monitor->log, "Properties changed in transport %s", path);

			transport_update_props(transport, &it[1], NULL);
		}
        }

fail:
	dbus_error_free(&err);
finish:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void add_filters(struct spa_bt_monitor *this)
{
	DBusError err;

	if (this->filters_added)
		return;

	dbus_error_init(&err);

	if (!dbus_connection_add_filter(this->conn, filter_cb, this, NULL)) {
		spa_log_error(this->log, "failed to add filter function");
		goto fail;
	}

	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" BLUEZ_SERVICE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_ADAPTER_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_DEVICE_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_MEDIA_ENDPOINT_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_MEDIA_TRANSPORT_INTERFACE "'", &err);

	this->filters_added = true;

	return;

fail:
	dbus_error_free(&err);
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct spa_bt_monitor *this = object;
        struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

        spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	add_filters(this);
	get_managed_objects(this);

	if (this->backend_ofono)
		backend_ofono_add_filters(this->backend_ofono);

	if (this->backend_hsphfpd)
		backend_hsphfpd_add_filters(this->backend_hsphfpd);

        spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct spa_bt_monitor *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct spa_bt_monitor *) handle;

	if (strcmp(type, SPA_TYPE_INTERFACE_Device) == 0)
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct spa_bt_monitor *monitor;
	struct spa_bt_adapter *a;
	struct spa_bt_device *d;
	struct spa_bt_remote_endpoint *ep;
	struct spa_bt_transport *t;

	monitor = (struct spa_bt_monitor *) handle;

	unregister_media_application(monitor);

	spa_list_consume(t, &monitor->transport_list, link)
		spa_bt_transport_free(t);
	spa_list_consume(ep, &monitor->remote_endpoint_list, link)
		remote_endpoint_free(ep);
	spa_list_consume(d, &monitor->device_list, link)
		device_free(d);
	spa_list_consume(a, &monitor->adapter_list, link)
		adapter_free(a);

	if (monitor->backend_native) {
		backend_native_free(monitor->backend_native);
		monitor->backend_native = NULL;
	}

	if (monitor->backend_ofono) {
		backend_ofono_free(monitor->backend_ofono);
		monitor->backend_ofono = NULL;
	}

	if (monitor->backend_hsphfpd) {
		backend_hsphfpd_free(monitor->backend_hsphfpd);
		monitor->backend_hsphfpd = NULL;
	}

	free((void*)monitor->enabled_codecs.items);
	spa_zero(monitor->enabled_codecs);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct spa_bt_monitor);
}

static int parse_codec_array(struct spa_bt_monitor *this, const struct spa_dict *info)
{
	const char *str;
	struct spa_dict_item *codecs;
	struct spa_json it, it_array;
	char codec_name[256];
	size_t num_codecs;
	int i;

	/* Parse bluez5.codecs property to a dict of enabled codecs */

	num_codecs = 0;
	while (a2dp_codecs[num_codecs])
		++num_codecs;

	codecs = calloc(num_codecs, sizeof(struct spa_dict_item));
	if (codecs == NULL)
		return -ENOMEM;

	if ((str = spa_dict_lookup(info, "bluez5.codecs")) == NULL)
		goto fallback;

	spa_json_init(&it, str, strlen(str));

	if (spa_json_enter_array(&it, &it_array) <= 0) {
		spa_log_error(this->log, NAME": property bluez5.codecs '%s' is not an array", str);
		goto fallback;
	}

	this->enabled_codecs = SPA_DICT_INIT(codecs, 0);

	while (spa_json_get_string(&it_array, codec_name, sizeof(codec_name)) > 0) {
		int i;

		for (i = 0; a2dp_codecs[i]; ++i) {
			const struct a2dp_codec *codec = a2dp_codecs[i];

			if (strcmp(codec->name, codec_name) != 0)
				continue;

			if (spa_dict_lookup_item(&this->enabled_codecs, codec->name) != NULL)
				continue;

			spa_log_debug(this->log, NAME": enabling codec %s", codec->name);

			spa_assert(this->enabled_codecs.n_items < num_codecs);

			codecs[this->enabled_codecs.n_items].key = codec->name;
			codecs[this->enabled_codecs.n_items].value = "true";
			++this->enabled_codecs.n_items;

			break;
		}
	}

	spa_dict_qsort(&this->enabled_codecs);

	for (i = 0; a2dp_codecs[i]; ++i) {
		const struct a2dp_codec *codec = a2dp_codecs[i];
		if (!is_a2dp_codec_enabled(this, codec))
			spa_log_debug(this->log, NAME": disabling codec %s", codec->name);
	}
	return 0;

fallback:
	for (i = 0; a2dp_codecs[i]; ++i) {
		const struct a2dp_codec *codec = a2dp_codecs[i];
		spa_log_debug(this->log, NAME": enabling codec %s", codec->name);
		codecs[i].key = codec->name;
		codecs[i].value = "true";
	}
	this->enabled_codecs = SPA_DICT_INIT(codecs, i);
	spa_dict_qsort(&this->enabled_codecs);
	return 0;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_monitor *this;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct spa_bt_monitor *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->main_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);

	if (this->dbus == NULL) {
		spa_log_error(this->log, "a dbus is needed");
		return -EINVAL;
	}

	this->dbus_connection = spa_dbus_get_connection(this->dbus, SPA_DBUS_TYPE_SYSTEM);
	if (this->dbus_connection == NULL) {
		spa_log_error(this->log, "no dbus connection");
		return -EIO;
	}
	this->conn = spa_dbus_connection_get(this->dbus_connection);

	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	spa_list_init(&this->adapter_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->remote_endpoint_list);
	spa_list_init(&this->transport_list);

	if ((res = parse_codec_array(this, info)) < 0)
		return res;

	register_media_application(this);

	if (info) {
		const char *str;

		if ((str = spa_dict_lookup(info, "bluez5.sbc-xq-support")) != NULL &&
		    (strcmp(str, "true") == 0 || atoi(str)))
			this->enable_sbc_xq = true;
	}

	this->backend_native = backend_native_new(this, this->conn, info, support, n_support);
	this->backend_ofono = backend_ofono_new(this, this->conn, info, support, n_support);
	this->backend_hsphfpd = backend_hsphfpd_new(this, this->conn, info, support, n_support);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];

	return 1;
}

const struct spa_handle_factory spa_bluez5_dbus_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_ENUM_DBUS,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
