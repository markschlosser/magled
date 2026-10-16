// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Schlosser
//
// magled: keep the LED on the MagSafe 3 connector off (or at a chosen colour).
//
// Runs until stopped, writing the AppleSMC ACLC key whenever macOS changes it.
// Purely event-driven. On exit the LED goes back to macOS. Needs root.
//
// Build: clang -O2 -Wall -o magled magled.c -framework IOKit

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAGLED_VERSION "0.1.2"

#define LED_KEY "ACLC"
#define LED_SYSTEM 0x00 // macOS controls the LED (default behaviour)
#define LED_OFF 0x01

#define LED_GREEN 0x03

// AppleSMC user-client protocol: one struct in, one struct out.
enum {
    SMC_SELECTOR = 2,
    SMC_CMD_READ_BYTES = 5,
    SMC_CMD_WRITE_BYTES = 6,
    SMC_CMD_READ_KEYINFO = 9,
};

typedef struct {
    uint8_t major, minor, build, reserved;
    uint16_t release;
} smc_vers_t;

typedef struct {
    uint16_t version, length;
    uint32_t cpu_limit, gpu_limit, mem_limit;
} smc_plimit_t;

typedef struct {
    uint32_t size;
    uint32_t type;
    uint8_t attributes;
} smc_keyinfo_t;

typedef struct {
    uint32_t key;
    smc_vers_t vers;
    smc_plimit_t plimit;
    smc_keyinfo_t info;
    uint8_t result;
    uint8_t status;
    uint8_t cmd;
    uint32_t data32;
    uint8_t bytes[32];
} smc_msg_t;

_Static_assert(sizeof(smc_msg_t) == 80, "AppleSMC expects an 80-byte struct");

static io_connect_t smc_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) {
        fprintf(stderr, "magled: AppleSMC service not found\n");
        exit(1);
    }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "magled: cannot open AppleSMC (0x%x)\n", kr);
        exit(1);
    }
    return conn;
}

static uint32_t key_code(const char *k) {
    return (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | (uint32_t)k[3];
}

// Sends one command; returns 0 on success, otherwise prints why and returns -1.
static int smc_call(io_connect_t conn, smc_msg_t *in, smc_msg_t *out) {
    size_t out_size = sizeof(*out);
    memset(out, 0, sizeof(*out));
    kern_return_t kr = IOConnectCallStructMethod(conn, SMC_SELECTOR, in, sizeof(*in), out, &out_size);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "magled: SMC call failed (0x%x)%s\n", kr,
                kr == kIOReturnNotPrivileged ? " - try sudo" : "");
        return -1;
    }
    if (out->result != 0) {
        fprintf(stderr, "magled: SMC returned error %u%s\n", out->result,
                out->result == 132 ? " (key " LED_KEY " not found on this Mac)" : "");
        return -1;
    }
    return 0;
}

static int smc_keyinfo(io_connect_t conn, const char *key, smc_keyinfo_t *info) {
    smc_msg_t in = {0}, out;
    in.key = key_code(key);
    in.cmd = SMC_CMD_READ_KEYINFO;
    if (smc_call(conn, &in, &out)) return -1;
    *info = out.info;
    return 0;
}

static int smc_read_u8(io_connect_t conn, const char *key, uint8_t *value) {
    smc_keyinfo_t info;
    if (smc_keyinfo(conn, key, &info)) return -1;
    smc_msg_t in = {0}, out;
    in.key = key_code(key);
    in.info.size = info.size;
    in.cmd = SMC_CMD_READ_BYTES;
    if (smc_call(conn, &in, &out)) return -1;
    *value = out.bytes[0];
    return 0;
}

static int smc_write_u8(io_connect_t conn, const char *key, uint8_t value) {
    smc_keyinfo_t info;
    if (smc_keyinfo(conn, key, &info)) return -1;
    if (info.size != 1) {
        fprintf(stderr, "magled: %s is %u bytes, expected 1\n", key, info.size);
        return -1;
    }
    smc_msg_t in = {0}, out;
    in.key = key_code(key);
    in.info.size = 1;
    in.cmd = SMC_CMD_WRITE_BYTES;
    in.bytes[0] = value;
    return smc_call(conn, &in, &out);
}

// macOS rewrites ACLC on power events, so keep putting it back.

static io_connect_t g_conn;
static io_connect_t g_pm_root;

static uint8_t g_target;
static uint8_t g_macos_value = LED_SYSTEM; // last value macOS wrote; restored on exit

static void log_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

static void enforce(void) {
    uint8_t v;
    if (smc_read_u8(g_conn, LED_KEY, &v) == 0 && v != g_target) {
        g_macos_value = v;
        log_time();
        fprintf(stderr, "found " LED_KEY "=0x%02x, setting 0x%02x\n", v, g_target);
        smc_write_u8(g_conn, LED_KEY, g_target);
    }
}

// After plugging in, the SMC forces the LED green in a couple of short bursts
// and ignores writes during them, so polling faster than this doesn't help.
// Watch for a few seconds; a new event restarts the window.
#define WATCH_STEP_MS 250
#define WATCH_STEPS 20

static int g_watch_left;

static void watch_tick(void) {
    enforce();
    if (--g_watch_left > 0)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, WATCH_STEP_MS * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{ watch_tick(); });
}

static void enforce_soon(void) {
    int idle = g_watch_left <= 0;
    g_watch_left = WATCH_STEPS;
    if (idle) watch_tick();
}

static void power_cb(void *ref, io_service_t svc, natural_t type, void *arg) {
    (void)ref, (void)svc;
    switch (type) {
    case kIOMessageCanSystemSleep:
    case kIOMessageSystemWillSleep:
        IOAllowPowerChange(g_pm_root, (long)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        enforce_soon();
        break;
    }
}

__attribute__((noreturn)) static void run_daemon(io_connect_t conn, uint8_t target) {
    g_conn = conn;
    g_target = target;
    dispatch_queue_t q = dispatch_get_main_queue();

    // Purely event-driven: nothing runs between events.

    // Plugging in or unplugging: macOS rewrites the LED in bursts, so watch a while.
    int source_token, level_token;
    notify_register_dispatch(kIOPSNotifyPowerSource, &source_token, q, ^(int t) {
        (void)t;
        enforce_soon();
    });

    // Charging state and battery level changes (e.g. finishing charging): one check.
    notify_register_dispatch(kIOPSNotifyAnyPowerSource, &level_token, q, ^(int t) {
        (void)t;
        enforce();
    });

    // Wake from sleep.
    IONotificationPortRef port;
    io_object_t notifier;
    g_pm_root = IORegisterForSystemPower(NULL, &port, power_cb, &notifier);
    if (g_pm_root) IONotificationPortSetDispatchQueue(port, q);
    else fprintf(stderr, "magled: cannot register for wake notifications\n");

    // On Ctrl-C or launchctl stop, put back what macOS last wanted. Writing 0
    // does not make macOS take over again; it only rewrites on its next event.
    int sigs[] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < 2; i++) {
        signal(sigs[i], SIG_IGN);
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sigs[i], 0, q);
        dispatch_source_set_event_handler(src, ^{
            log_time();
            fprintf(stderr, "stopping, restoring " LED_KEY "=0x%02x\n", g_macos_value);
            smc_write_u8(g_conn, LED_KEY, g_macos_value);
            exit(0);
        });
        dispatch_resume(src);
    }

    log_time();
    fprintf(stderr, "magled " MAGLED_VERSION " started, target " LED_KEY "=0x%02x\n", g_target);
    enforce();
    dispatch_main();
}

__attribute__((noreturn)) static void usage(void) {
    fprintf(stderr,
            "usage: sudo magled [off|green|<hex>]\n"
            "       magled --version\n"
            "\n"
            "Keeps the MagSafe 3 connector LED at the given value (default: off) until stopped,\n"
            "re-applying it whenever macOS changes it. Stopping it restores the last\n"
            "colour macOS set. 'make enable' runs it at boot.\n");
    exit(2);
}

static uint8_t parse_value(const char *s) {
    if (!strcmp(s, "off")) return LED_OFF;
    if (!strcmp(s, "green")) return LED_GREEN;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (!*s || *end || v > 0xff) usage();
    return (uint8_t)v;
}

int main(int argc, char **argv) {
    if (argc > 2) usage();
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("magled " MAGLED_VERSION "\n");
        return 0;
    }
    uint8_t target = parse_value(argc == 2 ? argv[1] : "off");
    run_daemon(smc_open(), target);
}
