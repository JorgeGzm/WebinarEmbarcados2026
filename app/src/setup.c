#include "setup.h"
#include "network_app.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_if.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include <gzm/rtc_mgr.h>
#include <gzm/fs_mgr.h>
#include <gzm/ringtone.h>

LOG_MODULE_REGISTER(setup, CONFIG_LOG_DEFAULT_LEVEL);

/* LittleFS mount point - usando a partição storage definida no overlay */
#define STORAGE_PARTITION_ID PARTITION_ID(storage_partition)

/* Configuração otimizada para RP2040 (PAGE_SIZE=256, SECTOR_SIZE=4096) */
static struct fs_littlefs lfs_data = {
    .cfg = {
        .read_size = 16,
        .prog_size = 256,
        .cache_size = 256,
        .lookahead_size = 128,
        .block_cycles = 512,
    },
};

static struct fs_mount_t lfs_storage_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_data,
    .storage_dev = (void *)STORAGE_PARTITION_ID,
    .mnt_point = "/lfs1",
};

/* Ringtone PWM buzzer, resolved here so the lib stays portable */
static const struct pwm_dt_spec buzzer_pwm =
	PWM_DT_SPEC_GET(DT_ALIAS(pwm_buzzer0));

/* Watchdog: 5 s timeout, fed from main()'s idle loop via setup_wdt_feed(). */
#define WDT_TIMEOUT_MS 5000

static const struct device *const g_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int g_wdt_channel = -1;
static bool g_wdt_feed_enabled = true;

/* Human-readable names for the RESET_* flags defined in <drivers/hwinfo.h>.
 * Each SoC reports only the subset of flags supported by its hardware.
 *
 * is_error = true → unexpected/abnormal reset (logs as LOG_ERR).
 * is_error = false → planned/benign reset (logs as LOG_INF).
 */
struct reset_cause {
	uint32_t flag;
	const char *name;
	bool is_error;
};

static const struct reset_cause reset_cause_names[] = {
	{ RESET_PIN,            "PIN (NRST)",                   false },
	{ RESET_SOFTWARE,       "SOFTWARE (NVIC_SystemReset)",  false },
	{ RESET_BROWNOUT,       "BROWNOUT (low voltage)",       true  },
	{ RESET_POR,            "POR (power-on)",               false },
	{ RESET_WATCHDOG,       "WATCHDOG (firmware hang)",     true  },
	{ RESET_DEBUG,          "DEBUG (debugger)",             false },
	{ RESET_SECURITY,       "SECURITY (violation)",         true  },
	{ RESET_LOW_POWER_WAKE, "LOW_POWER_WAKE (unexpected)",  true  },
	{ RESET_CPU_LOCKUP,     "CPU_LOCKUP (hard fault)",      true  },
	{ RESET_PARITY,         "PARITY (memory error)",        true  },
	{ RESET_PLL,            "PLL (clock fault)",            true  },
	{ RESET_CLOCK,          "CLOCK (clock fault)",          true  },
	{ RESET_HARDWARE,       "HARDWARE (generic fault)",     true  },
	{ RESET_USER,           "USER (app-defined)",           true  },
	{ RESET_TEMPERATURE,    "TEMPERATURE (overheat)",       true  },
};

static int  setup_init_middlware(void);
static void setup_init_sanity(void);
static int  setup_init_database(void);
static int  setup_init_tasks(void);
static int  setup_fatal_error(int error);
static int  setup_init_watchdog(void);

static int setup_init_middlware(void)
{
	int ret;

	LOG_INF("  [1/4] middlware");

	setup_init_watchdog();

	ret = rtc_mgr_init(DEVICE_DT_GET(DT_NODELABEL(rtc)));
	if (ret < 0) {
		LOG_ERR("RTC init failed (%d)", ret);
	}

	ret = fs_mgr_init(&lfs_storage_mnt);
	if (ret < 0) {
		LOG_ERR("FS init failed (%d)", ret);
	}

	ringtone_init(&buzzer_pwm);

	return 0;
}

static int setup_init_watchdog(void)
{
	struct wdt_timeout_cfg cfg = {
		.window.min = 0U,
		.window.max = WDT_TIMEOUT_MS,
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ret;

	if (!device_is_ready(g_wdt)) {
		LOG_ERR("watchdog device not ready");
		return -ENODEV;
	}

	ret = wdt_install_timeout(g_wdt, &cfg);
	if (ret < 0) {
		LOG_ERR("wdt_install_timeout failed: %d", ret);
		return ret;
	}
	g_wdt_channel = ret;

	ret = wdt_setup(g_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret < 0) {
		LOG_ERR("wdt_setup failed: %d", ret);
		g_wdt_channel = -1;
		return ret;
	}

	LOG_INF("watchdog armed: %d ms timeout, channel %d",
		WDT_TIMEOUT_MS, g_wdt_channel);
	return 0;
}

int setup_wdt_feed(void)
{
	if (g_wdt_channel < 0) {
		return -ENODEV;
	}
	if (!g_wdt_feed_enabled) {
		return 0;
	}
	return wdt_feed(g_wdt, g_wdt_channel);
}

static int cmd_watchdog_test(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (g_wdt_channel < 0) {
		shell_error(sh, "Watchdog is not armed");
		return -ENODEV;
	}

	shell_warn(sh, "Stopping watchdog feed; reset expected in about 5 seconds");
	g_wdt_feed_enabled = false;
	return 0;
}

SHELL_CMD_REGISTER(watchdog_test, NULL,
		   "Stop feeding the watchdog and verify automatic reset.",
		   cmd_watchdog_test);

static void setup_init_sanity(void)
{
	uint32_t cause = 0;
	int ret;
	size_t i;

	LOG_INF("  [2/4] sanity");

	ret = hwinfo_get_reset_cause(&cause);
	if (ret < 0) {
		LOG_WRN("    reset cause unavailable: %d", ret);
		return;
	}

	if (cause == 0) {
		LOG_INF("    last reset: (no flags reported by hardware)");
	} else {
		LOG_INF("    last reset cause: 0x%08x", cause);
		for (i = 0; i < ARRAY_SIZE(reset_cause_names); i++) {
			if (cause & reset_cause_names[i].flag) {
				if (reset_cause_names[i].is_error) {
					LOG_ERR("      - %s", reset_cause_names[i].name);
				} else {
					LOG_INF("      - %s", reset_cause_names[i].name);
				}
			}
		}
	}

	/* Clear flags so the next boot only reports its own cause. */
	(void)hwinfo_clear_reset_cause();
}

static int setup_init_database(void)
{
	LOG_INF("  [3/4] database");
	return 0;
}

static int setup_init_tasks(void)
{
	int err;
	LOG_INF("  [4/4] tasks");

	err = network_task_init(net_if_get_default());

	return err;
}

static int setup_fatal_error(int error)
{
	LOG_ERR("setup_fatal_error: phase failed with %d", error);
	return error;
}

int setup_init(void)
{
	int err;

	LOG_INF("setup_init: starting");

	setup_init_sanity();
	
	err = setup_init_middlware();
	if (err) {
		return setup_fatal_error(err);
	}

	err = setup_init_database();
	if (err) {
		return setup_fatal_error(err);
	}

	err = setup_init_tasks();
	if (err) {
		return setup_fatal_error(err);
	}

	LOG_INF("setup_init: done");
	return 0;
}
