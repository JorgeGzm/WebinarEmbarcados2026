#include "network_app.h"
#include "web_app.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <gzm/wifi_mgr.h>
#include <gzm/rtc_mgr.h>

LOG_MODULE_REGISTER(network_app, CONFIG_LOG_DEFAULT_LEVEL);

#define WIFI_SSID                 "YOUR_WIFI_SSID"
#define WIFI_PSK                  "YOUR_WIFI_PASSWORD"
#define SNTP_PORT                 123
#define SNTP_SERVER_ADDR          "216.239.35.0"  /* time.google.com */
#define LOCAL_TIMEZONE_HOURS      (1)             /* Budapest CET (UTC+1) */
#define WIFI_SETTLE_DELAY_MS      2000
#define RECONNECT_DELAY_SECONDS   10
#define EVENT_POLL_TIMEOUT_S      3
#define IDLE_FALLBACK_SLEEP_S     1

#define NETWORK_TASK_STACK_SIZE   2048
#define NETWORK_TASK_PRIORITY     7

enum network_state {
	NETWORK_STATE_IDLE,
	NETWORK_STATE_OFFLINE,
	NETWORK_STATE_CONNECTING,
	NETWORK_STATE_RUNNING,
};

static K_THREAD_STACK_DEFINE(g_stack, NETWORK_TASK_STACK_SIZE);
static struct k_thread g_thread;
static k_tid_t g_tid;
static struct net_if *g_iface;
static bool g_initialized;
static bool g_web_app_started;

static enum network_state g_state = NETWORK_STATE_IDLE;

static const char *state_name(enum network_state s)
{
	switch (s) {
	case NETWORK_STATE_IDLE:       return "IDLE";
	case NETWORK_STATE_OFFLINE:    return "OFFLINE";
	case NETWORK_STATE_CONNECTING: return "CONNECTING";
	case NETWORK_STATE_RUNNING:    return "RUNNING";
	}
	return "?";
}

static void transition_to(enum network_state new_state)
{
	LOG_INF("state: %s → %s", state_name(g_state), state_name(new_state));
	g_state = new_state;
}

/* Process a single wifi_mgr event in the RUNNING state. Returns the
 * next state: stays in RUNNING for benign events, transitions to OFFLINE
 * on disconnect.
 */
static enum network_state handle_event_running(const struct wifi_mgr_event_data *evt)
{
	switch (evt->type) {
	case WIFI_MGR_EVENT_DISCONNECTED:
		LOG_WRN("event: wifi disconnected");
		return NETWORK_STATE_OFFLINE;

	case WIFI_MGR_EVENT_IP_OBTAINED:
		LOG_INF("event: IPv4 renewed → ip=%s gw=%s", evt->ip, evt->gw);
		return NETWORK_STATE_RUNNING;

	case WIFI_MGR_EVENT_CONNECTED:
		LOG_INF("event: wifi (re)connected");
		return NETWORK_STATE_RUNNING;

	default:
		return NETWORK_STATE_RUNNING;
	}
}

/* Full bring-up sequence used when entering CONNECTING. Returns 0 on
 * success or negative errno on any failure (caller goes back to OFFLINE).
 */
static int network_bringup(void)
{
	int ret;
	uint32_t timestamp;
	struct rtc_time datetime;

	ret = wifi_mgr_connect(WIFI_SSID, WIFI_PSK);
	if (ret < 0) {
		LOG_ERR("WiFi connect failed (%d)", ret);
		return ret;
	}

	ret = wifi_mgr_wait_for_ip();
	if (ret < 0) {
		LOG_ERR("DHCP failed (%d)", ret);
		return ret;
	}

	k_msleep(WIFI_SETTLE_DELAY_MS);

	timestamp = wifi_mgr_sntp(SNTP_SERVER_ADDR, SNTP_PORT);
	if (timestamp > 0) {
		rtc_mgr_set_timestamp(timestamp);
		rtc_mgr_set_timezone(LOCAL_TIMEZONE_HOURS);
		rtc_mgr_get_local(&datetime);
		LOG_INF("RTC synced (local UTC%+d): %.4d-%.2d-%.2d %.2d:%.2d:%.2d",
			LOCAL_TIMEZONE_HOURS,
			datetime.tm_year, datetime.tm_mon, datetime.tm_mday,
			datetime.tm_hour, datetime.tm_min, datetime.tm_sec);
	} else {
		LOG_WRN("Failed to sync RTC with SNTP");
	}

	/* web_app_start is one-shot, Zephyr's HTTP server keeps running
	 * across wifi disconnect/reconnect, sockets are recovered by the
	 * stack on its own.
	 */
	if (!g_web_app_started) {
		ret = web_app_start();
		if (ret < 0) {
			LOG_ERR("web_app_start failed (%d)", ret);
			return ret;
		}
		g_web_app_started = true;
	}

	return 0;
}

static void network_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct wifi_mgr_event_data evt;
	
	wifi_mgr_init(g_iface);

	transition_to(NETWORK_STATE_OFFLINE);

	while (1) {
		switch (g_state) {
		case NETWORK_STATE_OFFLINE:
			/* Drop any stale events from a previous session so
			 * the next RUNNING phase is not woken by old data.
			 */
			wifi_mgr_event_drain();
			transition_to(NETWORK_STATE_CONNECTING);
			break;

		case NETWORK_STATE_CONNECTING:
			if (network_bringup() == 0) {
				transition_to(NETWORK_STATE_RUNNING);
			} else {
				LOG_WRN("bring-up failed, retrying in %d s",
					RECONNECT_DELAY_SECONDS);
				k_sleep(K_SECONDS(RECONNECT_DELAY_SECONDS));
				transition_to(NETWORK_STATE_OFFLINE);
			}
			break;

		case NETWORK_STATE_RUNNING: {
			/* Idle: poll the wifi_mgr event queue with a 3 s timeout
			 * (-EAGAIN on timeout is silently ignored, we just loop
			 * back to RUNNING). Avoids a forever-block in case an
			 * event is ever lost upstream.
			 */
			if (wifi_mgr_event_get(&evt, K_SECONDS(EVENT_POLL_TIMEOUT_S)) == 0) {
				enum network_state next = handle_event_running(&evt);

				if (next != g_state) {
					transition_to(next);
				}
			}
			break;
		}

		case NETWORK_STATE_IDLE:
		default:
			/* Should never happen after entering the loop. */
			k_sleep(K_SECONDS(IDLE_FALLBACK_SLEEP_S));
			break;
		}
	}
}

int network_task_init(struct net_if *iface)
{
	if (iface == NULL) {
		LOG_ERR("network_task_init: NULL iface");
		return -EINVAL;
	}

	if (g_initialized) {
		LOG_WRN("network_task_init: already initialized, call ignored");
		return -EALREADY;
	}

	g_iface = iface;
	g_initialized = true;

	g_tid = k_thread_create(&g_thread, g_stack,
				K_THREAD_STACK_SIZEOF(g_stack),
				network_thread, NULL, NULL, NULL,
				NETWORK_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(g_tid, "network");

	LOG_INF("network_task_init: worker spawned");
	return 0;
}

int network_task_deinit(void)
{
	if (!g_initialized) {
		LOG_WRN("network_task_deinit: not initialized");
		return -EALREADY;
	}

	if (g_tid != NULL) {
		k_thread_abort(g_tid);
		g_tid = NULL;
	}

	wifi_mgr_event_drain();

	g_state = NETWORK_STATE_IDLE;
	g_iface = NULL;
	g_initialized = false;

	LOG_INF("network_task_deinit: stopped");
	return 0;
}
