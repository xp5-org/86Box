/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Remote machine monitor: a line-based TCP console on 127.0.0.1,
 *          one client connection. Each command line gets one reply line
 *          starting "OK" or "ERR". Reads need the machine paused.
 *          Pause-on-connect rules: see releasenotes.txt.
 *
 * Authors: xp5-org
 *
 * Copyright 2026
 *
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    define rmm_closesocket closesocket
typedef SOCKET rmm_socket_t;
#    define RMM_BAD_SOCKET INVALID_SOCKET
#else
#    include <unistd.h>
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/select.h>
#    include <errno.h>
#    define rmm_closesocket close
typedef int rmm_socket_t;
#    define RMM_BAD_SOCKET (-1)
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include <86box/mem.h>
#include <86box/io.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/keyboard.h>
#include <stdbool.h> /* before sound.h */
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/nvr.h>
#include <86box/apm.h>
#include <86box/acpi.h>
#include <86box/sound.h>
#include <86box/fdd.h>
#include <86box/cdrom.h>
#include <86box/machine.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/video.h>
#include <86box/network.h>
#include <86box/hdc.h>
#include <86box/scsi.h>
#include <86box/fdc_ext.h>
#include <86box/cdrom_interface.h>
#include <86box/isamem.h>
#include <86box/mcamem.h>
#include <86box/isarom.h>
#include <86box/isartc.h>
#include <86box/char.h>
#include <86box/mouse.h>
#include <86box/gameport.h>
#include <86box/midi.h>
#include <86box/rmm.h>
#include <86box/rmm_snap.h>
#include <cJSON.h>

#define RMM_VERSION  "0.1"
/* Wire protocol, negotiated by rpc.hello. Bump RMM_PROTOCOL when methods or events are
   added; raise RMM_PROTOCOL_MIN only when an older client can no longer work. */
#define RMM_PROTOCOL     1
#define RMM_PROTOCOL_MIN 1
#define RMM_LINE_MAX 16384
#define RMM_MEM_MAX  4096

/* Application error codes, shared with the JSON-RPC dialect. */
#define RMM_E_NOT_PAUSED 1
#define RMM_E_EMPTY      2 /* slot empty */
#define RMM_E_NO_SLOT    3 /* slot out of range, no drive of that type, or no such input device */
#define RMM_E_BUSY       4 /* device busy, absent or disabled */
#define RMM_E_DROPPED    5
#define RMM_E_NO_SNAP    7 /* no such snapshot */
#define RMM_E_SNAP_FAIL  8 /* cannot snapshot */
#define RMM_E_NOT_MAPPED 9 /* linear address with no present page */
#define RMM_E_BAD_CONFIG 10 /* config.load: data {errors:[{section,key,value,why}]} */
#define RMM_E_PROTOCOL   11 /* rpc.hello: client protocol too old; data {protocol,min_protocol} */

extern void cpu_386_flags_rebuild(void); /* cpu/386_common.c */

int rmm_port             = 0;
int rmm_port_cmdline     = 0;
int rmm_pause_on_connect = 1;
int rmm_sticky_pause     = 0;
int rmm_sticky_cmdline   = 0;
volatile int rmm_hard_resets = 0;

/* Set by the platform UI once a plat_pause_async() request has been applied. */
volatile int rmm_pause_applied = -1;

static rmm_socket_t   listen_socket = RMM_BAD_SOCKET;
static rmm_socket_t   client_socket = RMM_BAD_SOCKET;
static volatile int   rmm_running   = 0;
static int            paused_by_us  = 0;
static int            pause_auto    = 0; /* from pause-on-connect, not the client */
static int            json_seen     = 0; /* this client has sent a JSON-RPC line */
static int            quitting      = 0; /* machine.quit answered; power off after the reply */

#ifdef ENABLE_RMM_LOG
int rmm_do_log = ENABLE_RMM_LOG;

static void
rmm_log(const char *fmt, ...)
{
    va_list ap;

    if (rmm_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define rmm_log(fmt, ...)
#endif

/* ---------------------------------------------------------------- I/O */

/* send() with MSG_NOSIGNAL on Linux; SO_NOSIGPIPE on macOS, set at accept. */
#ifdef MSG_NOSIGNAL
#    define RMM_SEND_FLAGS MSG_NOSIGNAL
#else
#    define RMM_SEND_FLAGS 0
#endif

/* Send timeout set at accept; a failed or timed-out write ends the session. */
#define RMM_SEND_TIMEOUT_MS 10000

static int client_dead = 0;

static void
rmm_write(const char *buf, int len)
{
    if (client_dead)
        return;
    for (int off = 0; off < len;) {
        int sent = send(client_socket, buf + off, len - off, RMM_SEND_FLAGS);
        if (sent < 0) {
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            client_dead = 1;
            return;
        }
        if (sent == 0) {
            client_dead = 1;
            return;
        }
        off += sent;
    }
}

static void
rmm_send(const char *fmt, ...)
{
    char    buf[RMM_LINE_MAX * 3 + 64];
    va_list ap;
    int     len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (len < 0)
        return;
    if (len > (int) sizeof(buf) - 2)
        len = sizeof(buf) - 2;
    buf[len++] = '\n';
    rmm_write(buf, len);
}

/* ---------------------------------------------------------- pause/run */

/* Ask the UI thread for a pause state; wait for it to apply and, on pause,
   for the CPU thread ack. */
static int
rmm_set_pause(int want_paused)
{
    const int was_paused = !!dopause;

    rmm_pause_ack_clear(); /* a stale ack from a timed-out pause */
    for (int tries = 0; tries < 10; tries++) {
        rmm_pause_applied = -1;
        plat_pause_async(want_paused);
        for (int i = 0; i < 50; i++) {
            if ((rmm_pause_applied >= 0) && (!!dopause == !!want_paused))
                goto applied;
            plat_delay_ms(10);
        }
    }
    return !!dopause == !!want_paused;

applied:
    if (want_paused && !was_paused) { /* only a running -> paused change is acked by the CPU thread */
        /* the CPU thread ack is slow under valgrind */
        for (int i = 0; i < 6000 && !rmm_pause_acked(); i++)
            plat_delay_ms(10);
        if (!rmm_pause_acked())
            return 0;
        rmm_pause_ack_clear();
    }
    return 1;
}

static const char *
rmm_state(void)
{
    return dopause ? "paused" : "running";
}

/* --------------------------------------------------------- keyboard */

typedef struct {
    const char *name;
    uint16_t    scan; /* set 1, 0x100 = E0 prefix */
} rmm_key_t;

static const rmm_key_t rmm_keys[] = {
    { "esc", 0x01 }, { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 },
    { "5", 0x06 }, { "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0a },
    { "0", 0x0b }, { "-", 0x0c }, { "=", 0x0d }, { "backspace", 0x0e }, { "tab", 0x0f },
    { "q", 0x10 }, { "w", 0x11 }, { "e", 0x12 }, { "r", 0x13 }, { "t", 0x14 },
    { "y", 0x15 }, { "u", 0x16 }, { "i", 0x17 }, { "o", 0x18 }, { "p", 0x19 },
    { "[", 0x1a }, { "]", 0x1b }, { "enter", 0x1c }, { "ctrl", 0x1d }, { "a", 0x1e },
    { "s", 0x1f }, { "d", 0x20 }, { "f", 0x21 }, { "g", 0x22 }, { "h", 0x23 },
    { "j", 0x24 }, { "k", 0x25 }, { "l", 0x26 }, { ";", 0x27 }, { "'", 0x28 },
    { "`", 0x29 }, { "shift", 0x2a }, { "\\", 0x2b }, { "z", 0x2c }, { "x", 0x2d },
    { "c", 0x2e }, { "v", 0x2f }, { "b", 0x30 }, { "n", 0x31 }, { "m", 0x32 },
    { ",", 0x33 }, { ".", 0x34 }, { "/", 0x35 }, { "rshift", 0x36 }, { "kp_multiply", 0x37 },
    { "alt", 0x38 }, { "space", 0x39 }, { "capslock", 0x3a },
    { "f1", 0x3b }, { "f2", 0x3c }, { "f3", 0x3d }, { "f4", 0x3e }, { "f5", 0x3f },
    { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 }, { "f10", 0x44 },
    { "numlock", 0x45 }, { "scrolllock", 0x46 }, { "f11", 0x57 }, { "f12", 0x58 },
    { "kpenter", 0x11c }, { "rctrl", 0x11d }, { "kp_divide", 0x135 }, { "ralt", 0x138 },
    { "home", 0x147 }, { "up", 0x148 }, { "pgup", 0x149 }, { "left", 0x14b },
    { "right", 0x14d }, { "end", 0x14f }, { "down", 0x150 }, { "pgdn", 0x151 },
    { "ins", 0x152 }, { "del", 0x153 }, { "lwin", 0x15b }, { "rwin", 0x15c },
    { "lctrl", 0x1d }, { "lshift", 0x2a }, { "lalt", 0x38 },
    { NULL, 0 }
};

/* Shifted printable characters, mapped to their unshifted key. */
static const char rmm_shifted[]   = "!@#$%^&*()_+{}:\"~|<>?";
static const char rmm_unshifted[] = "1234567890-=[];'`\\,./";

static int
rmm_key_lookup(const char *name)
{
    for (const rmm_key_t *key = rmm_keys; key->name; key++)
        if (!strcasecmp(key->name, name))
            return key->scan;
    return -1;
}

static void
rmm_key_tap(uint16_t scan, int shift)
{
    if (shift) {
        keyboard_input(1, 0x2a);
        plat_delay_ms(20);
    }
    keyboard_input(1, scan);
    plat_delay_ms(30);
    keyboard_input(0, scan);
    plat_delay_ms(20);
    if (shift) {
        keyboard_input(0, 0x2a);
        plat_delay_ms(20);
    }
}

/* "ctrl+alt+del": press each part in order, release in reverse. */
static int
rmm_sendkey(char *combo)
{
    int   scans[8];
    int   count = 0;
    char *save = NULL;

    for (char *tok = strtok_r(combo, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        if (count >= 8)
            return 0;
        if ((scans[count] = rmm_key_lookup(tok)) < 0)
            return 0;
        count++;
    }
    if (count == 0)
        return 0;

    for (int i = 0; i < count; i++) {
        keyboard_input(1, scans[i]);
        plat_delay_ms(30);
    }
    for (int i = count - 1; i >= 0; i--) {
        keyboard_input(0, scans[i]);
        plat_delay_ms(20);
    }
    return 1;
}

/* With dry set, only checks that every character is typeable. */
static int
rmm_type_ex(const char *text, int dry)
{
    char name[2] = { 0, 0 };

    for (const char *ch = text; *ch; ch++) {
        int         shift = 0;
        const char *shifted_at;
        int         scan;

        if (*ch == ' ') {
            if (!dry)
                rmm_key_tap(0x39, 0);
            continue;
        }
        if (isupper((unsigned char) *ch)) {
            shift   = 1;
            name[0] = (char) tolower((unsigned char) *ch);
        } else if ((shifted_at = strchr(rmm_shifted, *ch)) != NULL) {
            shift   = 1;
            name[0] = rmm_unshifted[shifted_at - rmm_shifted];
        } else
            name[0] = *ch;

        if ((scan = rmm_key_lookup(name)) < 0)
            return 0;
        if (!dry)
            rmm_key_tap(scan, shift);
    }
    return 1;
}

static int
rmm_type(const char *text)
{
    return rmm_type_ex(text, 1) && rmm_type_ex(text, 0);
}

/* --------------------------------------------------------- commands */

/* 286+ cores keep flags lazily in flags_op; fold them into cpu_state.flags first. */
static void
rmm_flags_rebuild(void)
{
    if (is286)
        cpu_386_flags_rebuild();
}

static void
rmm_cmd_status(void)
{
    rmm_send("OK %s tsc=%" PRIu64 " cs:eip=%04X:%08X", rmm_state(), tsc,
             cpu_state.seg_cs.seg, cpu_state.pc);
}

static void
rmm_cmd_regs(void)
{
    if (!dopause) {
        rmm_send("ERR %d not paused", RMM_E_NOT_PAUSED);
        return;
    }
    rmm_flags_rebuild();
    rmm_send("OK eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X "
             "eip=%08X eflags=%08X cs=%04X ss=%04X ds=%04X es=%04X fs=%04X gs=%04X "
             "cs_base=%08X cr0=%08X cr2=%08X cr3=%08X cr4=%08X",
             cpu_state.regs[0].l, cpu_state.regs[1].l, cpu_state.regs[2].l, cpu_state.regs[3].l,
             cpu_state.regs[4].l, cpu_state.regs[5].l, cpu_state.regs[6].l, cpu_state.regs[7].l,
             cpu_state.pc, ((uint32_t) cpu_state.eflags << 16) | cpu_state.flags,
             cpu_state.seg_cs.seg, cpu_state.seg_ss.seg, cpu_state.seg_ds.seg,
             cpu_state.seg_es.seg, cpu_state.seg_fs.seg, cpu_state.seg_gs.seg,
             cpu_state.seg_cs.base, cr0, cr2, cr3, cr4);
}

/* mem <phys addr hex> <len>: physical memory, hex dump on one line. */
static void
rmm_cmd_mem(char *args)
{
    char    *end;
    uint32_t addr;
    long     len;
    char     hex[RMM_MEM_MAX * 2 + 1];

    if (!dopause) {
        rmm_send("ERR %d not paused", RMM_E_NOT_PAUSED);
        return;
    }
    if (!args || !*args) {
        rmm_send("ERR usage: mem <addr> <len>");
        return;
    }
    addr = (uint32_t) strtoul(args, &end, 16);
    len  = strtol(end, NULL, 0);
    if ((len <= 0) || (len > RMM_MEM_MAX)) {
        rmm_send("ERR len must be 1..%d", RMM_MEM_MAX);
        return;
    }
    for (long i = 0; i < len; i++)
        sprintf(&hex[i * 2], "%02X", mem_rmm_readb_phys(addr + (uint32_t) i));
    rmm_send("OK %08X %s", addr, hex);
}

static int
rmm_hexval(int ch)
{
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    if ((ch >= 'a') && (ch <= 'f'))
        return ch - 'a' + 10;
    if ((ch >= 'A') && (ch <= 'F'))
        return ch - 'A' + 10;
    return -1;
}

/* memw <hexaddr> <hexdata> [raw]: write physical memory while paused.
 * Each write is read back; a mismatch is ERR 5. Translated code is invalidated
 * unless raw. See releasenotes.txt. */
/* Write, invalidate translated code unless raw, read back. Returns the offset of
   the first byte that did not land, or -1; *took counts the rest. */
static int
rmm_phys_write(uint32_t addr, const uint8_t *data, int len, int raw, int *took)
{
    int first_bad = -1;

    for (int i = 0; i < len; i++)
        mem_rmm_writeb_phys(addr + (uint32_t) i, data[i]);
    if (!raw)
        mem_invalidate_range(addr, addr + (uint32_t) len - 1);

    *took = 0;
    for (int i = 0; i < len; i++) {
        if (mem_rmm_readb_phys(addr + (uint32_t) i) == data[i])
            (*took)++;
        else if (first_bad < 0)
            first_bad = i;
    }
    return first_bad;
}

static void
rmm_cmd_memw(char *args)
{
    char    *end;
    char    *hexp;
    uint32_t addr;
    int      raw = 0;
    int      len;
    uint8_t  data[RMM_MEM_MAX];
    int      took = 0;
    int      first_bad = -1;

    if (!dopause) {
        rmm_send("ERR %d not paused", RMM_E_NOT_PAUSED);
        return;
    }
    if (!args || !*args) {
        rmm_send("ERR usage: memw <addr> <hexdata> [raw]");
        return;
    }

    addr = (uint32_t) strtoul(args, &end, 16);
    if ((end == args) || (*end != ' ')) {
        rmm_send("ERR usage: memw <addr> <hexdata> [raw]");
        return;
    }
    while (*end == ' ')
        end++;
    hexp = end;

    for (len = 0; rmm_hexval(hexp[len * 2]) >= 0; len++) {
        int hi = rmm_hexval(hexp[len * 2]);
        int lo = rmm_hexval(hexp[len * 2 + 1]);

        if (lo < 0) {
            rmm_send("ERR hex data must be whole bytes");
            return;
        }
        if (len >= RMM_MEM_MAX) {
            rmm_send("ERR at most %d bytes per call", RMM_MEM_MAX);
            return;
        }
        data[len] = (uint8_t) ((hi << 4) | lo);
    }
    end = hexp + len * 2;
    while (*end == ' ')
        end++;
    if (!strcasecmp(end, "raw"))
        raw = 1;
    else if (*end) {
        rmm_send("ERR unexpected '%s'", end);
        return;
    }
    if (len == 0) {
        rmm_send("ERR no data");
        return;
    }
    if ((uint64_t) addr + (uint64_t) len > 0x100000000ULL) {
        rmm_send("ERR range passes the end of the 32-bit address space");
        return;
    }

    first_bad = rmm_phys_write(addr, data, len, raw, &took);
    if (first_bad >= 0)
        rmm_send("ERR %d write dropped: %d of %d bytes read back, first mismatch at %08X",
                 RMM_E_DROPPED, took, len, addr + (uint32_t) first_bad);
    else
        rmm_send("OK %08X %d%s", addr, len, raw ? " raw" : "");
}

/* ----------------------------------------------------- JSON-RPC 2.0 */

/* One JSON object per line; a line starting with '{' is JSON-RPC, anything else
   is the legacy dialect. Methods must match rmm_client/rmm_registry.py. */

#define RPC_PARSE_ERROR    -32700
#define RPC_INVALID_REQ    -32600
#define RPC_NO_METHOD      -32601
#define RPC_INVALID_PARAMS -32602
#define RPC_INTERNAL_ERROR -32603

typedef struct {
    int    code;
    char   msg[1024];
    cJSON *data;
} rmm_rpc_err_t;

typedef cJSON *(*rmm_rpc_fn)(cJSON *params, rmm_rpc_err_t *err);

static cJSON *
rpc_fail(rmm_rpc_err_t *err, int code, const char *fmt, ...)
{
    va_list ap;

    err->code = code;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
    return NULL;
}

static void
rmm_send_json(cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);

    if (text) {
        int len = (int) strlen(text);
        text[len] = '\n'; /* overwrite the terminator; length is known */
        rmm_write(text, len + 1);
        cJSON_free(text);
    }
}

/* params may be NULL. */
static void
rmm_event(const char *method, cJSON *params)
{
    if (json_seen) {
        cJSON *notification = cJSON_CreateObject();

        cJSON_AddStringToObject(notification, "jsonrpc", "2.0");
        cJSON_AddStringToObject(notification, "method", method);
        cJSON_AddItemToObject(notification, "params", params ? params : cJSON_CreateObject());
        rmm_send_json(notification);
        cJSON_Delete(notification);
    } else if (params)
        cJSON_Delete(params);
}

/* An integer parameter in [0, max]. */
static int
rpc_uint(cJSON *params, const char *name, double max, uint32_t *out, int required, rmm_rpc_err_t *err)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, name);

    if (!item) {
        if (required)
            rpc_fail(err, RPC_INVALID_PARAMS, "missing parameter '%s'", name);
        return !required;
    }
    if (!cJSON_IsNumber(item) || (item->valuedouble < 0) || (item->valuedouble > max) ||
        (item->valuedouble != (double) (uint32_t) item->valuedouble)) {
        rpc_fail(err, RPC_INVALID_PARAMS, "'%s' must be an integer 0..%.0f", name, max);
        return 0;
    }
    *out = (uint32_t) item->valuedouble;
    return 1;
}

static const char *
rpc_string(cJSON *params, const char *name, rmm_rpc_err_t *err)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, name);

    if (!cJSON_IsString(item)) {
        rpc_fail(err, RPC_INVALID_PARAMS, "'%s' must be a string", name);
        return NULL;
    }
    return item->valuestring;
}

/* An optional string parameter: *out is NULL when absent; 0 if present but not a string. */
static int
rpc_opt_string(cJSON *params, const char *name, const char **out, rmm_rpc_err_t *err)
{
    *out = NULL;
    if (!cJSON_GetObjectItemCaseSensitive(params, name))
        return 1;
    return (*out = rpc_string(params, name, err)) != NULL;
}

static int
rpc_need_paused(rmm_rpc_err_t *err)
{
    if (!dopause) {
        rpc_fail(err, RMM_E_NOT_PAUSED, "not paused");
        return 0;
    }
    return 1;
}

/* -- debug executor --------------------------------------------------------------
   Armed breakpoints switch cpu_exec to exec386_rmm. A hit sets rmm_cpu_halted and
   rmm_debug_poll pauses from the monitor thread. See releasenotes.txt. */

#define RMM_BP_MAX     64
#define RMM_STEP_MAX   1000000
#define RMM_STOP_BREAK 1
#define RMM_STOP_STEP  2
#define RMM_STOP_WATCH 3

/* Function contract for debug.break.set pre/post: clauses "lhs op rhs" joined by &&.
   A side is a constant, a register, argN or dargN. A violated clause is a hit. */
#define RMM_CL_MAX     4
#define RMM_FRAME_MAX  8
#define RMM_SIDE_CONST 0
#define RMM_SIDE_REG   1
#define RMM_SIDE_ARG   2
#define RMM_SIDE_DARG  3

typedef struct {
    uint8_t kind;
    uint8_t reg;  /* 0..7 in eax ecx edx ebx esp ebp esi edi order */
    uint8_t size; /* register width: 1, 2 or 4 */
    uint8_t high; /* ah ch dh bh */
    int32_t argnum;   /* argument number */
    int64_t constant; /* constant */
} rmm_side_t;

typedef struct {
    rmm_side_t left, right;
    uint8_t    op;  /* 0 == 1 != 2 < 3 <= 4 > 5 >= */
    uint8_t    uns; /* unsigned compare */
} rmm_clause_t;

typedef struct {
    uint32_t lin; /* where the call returns to */
    uint32_t sp;  /* sp at entry, before the return address was pushed on */
    uint32_t after; /* sp the return leaves behind */
} rmm_frame_t;

typedef struct {
    int          id;
    uint32_t     addr; /* linear: cs base + eip */
    int          npre, npost;
    rmm_clause_t pre[RMM_CL_MAX], post[RMM_CL_MAX];
    int          ret_far;  /* the return address on the stack is off16:seg16 */
    int          ret_size; /* near return address: 2 or 4 bytes */
    int          stop;     /* a violation stops the guest, else it is only counted */
    uint32_t     calls, viols;
    int          last_phase; /* 1 pre, 2 post */
    int          last_clause;
    int64_t      last_l, last_r;
    rmm_frame_t  fr[RMM_FRAME_MAX];
    int          nfr;
} rmm_bp_t;

static rmm_bp_t     rmm_bps[RMM_BP_MAX];
static int          rmm_bp_count    = 0;
static int          rmm_bp_next_id  = 1;
static volatile int rmm_bp_skip     = 0;  /* a resume steps over the breakpoint it sits on */
static volatile int rmm_step_left   = -1; /* instructions still to step; -1 = not stepping */
static volatile int rmm_stop_reason = 0;  /* set by the CPU thread, taken by rmm_debug_poll */
static uint32_t     rmm_stop_addr;
static int          rmm_stop_id;
static int          rmm_stop_vals; /* watchpoints: old/new valid */
static int          rmm_stop_owner; /* watchpoints: rule that armed this one */
static uint32_t     rmm_stop_src;
static uint32_t     rmm_stop_old;
static uint32_t     rmm_stop_new;
static int          rmm_stop_viol; /* breakpoints: 1 = precondition, 2 = postcondition broken */
static int          rmm_stop_clause;
static int64_t      rmm_stop_lhs, rmm_stop_rhs;
static int          rmm_stop_kind; /* watchpoints: RMM_WATCH_R or RMM_WATCH_W, the access that hit */
static void       (*rmm_exec_stock)(int32_t cycs) = NULL;
volatile int        rmm_cpu_halted = 0;

/* -- watchpoints ----------------------------------------------------------------
   Linear addresses. Writes use a page_lookup override, reads a replaced read_mapping.
   See releasenotes.txt. */

static int64_t rmm_lin_to_phys(uint32_t lin);

#define RMM_WATCH_MAX   64
#define RMM_WATCH_PAGES (RMM_WATCH_MAX * 2) /* len <= 4096: at most two pages each */
#define RMM_ACT_STOP    0
#define RMM_ACT_ARM     1
#define RMM_ACT_DISARM  2
#define RMM_ACT_REWRITE 3
#define RMM_WATCH_R     1
#define RMM_WATCH_W     2

typedef struct {
    int      id;
    uint32_t addr; /* linear */
    uint32_t len;
    int      kind; /* RMM_WATCH_R | RMM_WATCH_W */
    /* write watches of 1, 2 or 4 bytes: the hit counts only if the old value was from
       and/or the new value is to */
    int      has_from;
    int      has_to;
    uint32_t from;
    uint32_t to;
    /*   ARM     put a child watch on a range read from guest memory
         DISARM  remove the children made from this watch's address
         REWRITE replace the written bytes with rw_val; hits counts firings */
    int      action;
    int32_t  a_base_off; /* signed byte offset from `addr` to the base field */
    int      a_base_size; /* 2 or 4 */
    int      a_far;       /* base is off16:seg16 in real mode: seg*16+off */
    uint32_t a_len;       /* constant child length, or 0 to read it */
    int32_t  a_len_off;
    int      a_len_size;  /* 1, 2 or 4 */
    int      a_kind;      /* child kind */
    int      owner;       /* child: id of the rule that armed it, else 0 */
    uint32_t src;         /* child: the rule's address */
    uint32_t rw_val;      /* REWRITE: replacement value */
    uint32_t hits;        /* REWRITE: times it fired */
} rmm_watch_t;

/* A physical page whose read mapping is replaced. */
typedef struct {
    uint32_t       page; /* physical page number */
    mem_mapping_t *orig;
    mem_mapping_t  ours;
} rmm_rpage_t;

static rmm_watch_t rmm_watches[RMM_WATCH_MAX];
static int         rmm_watch_count = 0;
/* linear pages with a page_lookup override, for write watches */
static uint32_t    rmm_wpages[RMM_WATCH_PAGES];
static int         rmm_wpage_count = 0;
/* linear pages under read watch, their physical pages, and the deduplicated
   physical pages whose read mapping is replaced */
static uint32_t    rmm_rlin[RMM_WATCH_PAGES];
static int64_t     rmm_rlin_phys[RMM_WATCH_PAGES];
static int         rmm_rlin_count = 0;
static rmm_rpage_t rmm_rpages[RMM_WATCH_PAGES];
static int         rmm_rpage_count = 0;
/* the hit of the instruction in progress, CPU thread only */
static int         rmm_watch_hit_id = 0;
static uint32_t    rmm_watch_hit_addr;
static int         rmm_watch_hit_kind;
static int         rmm_watch_hit_vals; /* old/new below are valid for write watches of 1, 2 or 4 bytes */
static uint32_t    rmm_watch_hit_old;
static uint32_t    rmm_watch_hit_new;

/* Little-endian read of `size` bytes at a linear address; 0 if unmapped. */
static int
rmm_read_lin(uint32_t lin, int size, uint32_t *out)
{
    uint32_t value = 0;

    for (int j = 0; j < size; j++) {
        const int64_t pa = rmm_lin_to_phys(lin + j);

        if (pa < 0)
            return 0;
        value |= (uint32_t) mem_rmm_readb_phys((uint32_t) pa) << (8 * j);
    }
    *out = value;
    return 1;
}

static void
rmm_watch_remove_at(int i)
{
    rmm_watches[i] = rmm_watches[--rmm_watch_count];
}

/* Remove the children armed from address `src` or by rule `owner`; owner 0 matches any rule. */
static void
rmm_children_remove(uint32_t src, int owner)
{
    for (int i = 0; i < rmm_watch_count;) {
        if (rmm_watches[i].owner && (owner ? (rmm_watches[i].owner == owner) : (rmm_watches[i].src == src)))
            rmm_watch_remove_at(i);
        else
            i++;
    }
}

/* The watched range's value now, and with a write of len bytes of val at lin laid over it. */
static int
rmm_watch_values(const rmm_watch_t *watch, uint32_t lin, uint32_t len, uint32_t val, uint32_t *old, uint32_t *new)
{
    uint32_t old_value = 0;
    uint32_t new_value = 0;

    for (uint32_t j = 0; j < watch->len; j++) {
        const int64_t pa = rmm_lin_to_phys(watch->addr + j);
        uint32_t      byte;
        uint64_t      at = (uint64_t) watch->addr + j;

        if (pa < 0)
            return 0;
        byte = mem_rmm_readb_phys((uint32_t) pa);
        old_value |= byte << (8 * j);
        if ((at >= lin) && (at < (uint64_t) lin + len))
            byte = (val >> (8 * (at - lin))) & 0xff;
        new_value |= byte << (8 * j);
    }
    *old = old_value;
    *new = new_value;
    return 1;
}

/* val is the value being written, NULL for reads. A matching REWRITE watch changes it in place. */
static void
rmm_watch_access(uint32_t lin, uint32_t len, int kind, uint32_t *val)
{
    for (int i = 0; i < rmm_watch_count; i++) {
        rmm_watch_t *watch = &rmm_watches[i];

        if ((watch->kind & kind) && ((uint64_t) lin < (uint64_t) watch->addr + watch->len) &&
            ((uint64_t) watch->addr < (uint64_t) lin + len)) {
            uint32_t old = 0;
            uint32_t new = 0;
            int      vals = 0;

            if ((watch->action != RMM_ACT_REWRITE) && rmm_watch_hit_id)
                continue; /* first stop of the instruction wins */
            if ((kind == RMM_WATCH_W) && (watch->len <= 4) && ((watch->len == 1) || (watch->len == 2) || (watch->len == 4)))
                vals = rmm_watch_values(watch, lin, len, val ? *val : 0, &old, &new);
            if ((watch->has_from || watch->has_to) && (!vals || (watch->has_from && (old != watch->from)) || (watch->has_to && (new != watch->to))))
                continue; /* the write is not the transition asked for */
            if (watch->action == RMM_ACT_REWRITE) {
                if (!val)
                    continue;
                for (uint32_t j = 0; j < watch->len; j++) {
                    const uint64_t at = (uint64_t) watch->addr + j;

                    if ((at >= lin) && (at < (uint64_t) lin + len)) {
                        const unsigned sh = 8 * (unsigned) (at - lin);

                        *val = (*val & ~(0xffu << sh)) | (((watch->rw_val >> (8 * j)) & 0xff) << sh);
                    }
                }
                watch->hits++;
                continue;
            }
            rmm_watch_hit_vals = vals;
            rmm_watch_hit_old  = old;
            rmm_watch_hit_new  = new;
            rmm_watch_hit_id   = watch->id;
            rmm_watch_hit_addr = watch->addr;
            rmm_watch_hit_kind = kind;
            /* report it before any hardware interrupt gets in: the MOV SS/STI shadow */
            cpu_end_block_after_ins = 2;
        }
    }
}

/* What writemembl does after its page_lookup check: the physical address, or -1 on a page fault. */
static int64_t
rmm_watch_wphys(uint32_t lin)
{
    uint64_t address = lin;

    if (cr0 >> 31) {
        address = mmutranslatereal(lin, 1);
        if (address > 0xffffffffULL)
            return -1;
    }
    return (uint32_t) address & rammask;
}

static void
rmm_watch_wb(uint32_t addr, uint8_t val, UNUSED(page_t *page))
{
    const int64_t  pa  = rmm_watch_wphys(addr);
    mem_mapping_t *map;

    if (pa < 0)
        return;
    uint32_t       value   = val;
    rmm_watch_access(addr, 1, RMM_WATCH_W, &value);
    val = (uint8_t) value;
    map = write_mapping[pa >> MEM_GRANULARITY_BITS];
    if (map && map->write_b)
        map->write_b((uint32_t) pa, val, map->priv);
}

static void
rmm_watch_ww(uint32_t addr, uint16_t val, UNUSED(page_t *page))
{
    const int64_t  pa  = rmm_watch_wphys(addr);
    mem_mapping_t *map;

    if (pa < 0)
        return;
    uint32_t       value   = val;
    rmm_watch_access(addr, 2, RMM_WATCH_W, &value);
    val = (uint16_t) value;
    map = write_mapping[pa >> MEM_GRANULARITY_BITS];
    if (map && map->write_w)
        map->write_w((uint32_t) pa, val, map->priv);
    else if (map && map->write_b) {
        map->write_b((uint32_t) pa, val, map->priv);
        map->write_b((uint32_t) pa + 1, val >> 8, map->priv);
    }
}

static void
rmm_watch_wl(uint32_t addr, uint32_t val, UNUSED(page_t *page))
{
    const int64_t  pa  = rmm_watch_wphys(addr);
    mem_mapping_t *map;

    if (pa < 0)
        return;
    uint32_t       value   = val;
    rmm_watch_access(addr, 4, RMM_WATCH_W, &value);
    val = value;
    map = write_mapping[pa >> MEM_GRANULARITY_BITS];
    if (map && map->write_l)
        map->write_l((uint32_t) pa, val, map->priv);
    else if (map && map->write_w) {
        map->write_w((uint32_t) pa, val, map->priv);
        map->write_w((uint32_t) pa + 2, val >> 16, map->priv);
    } else if (map && map->write_b) {
        for (int i = 0; i < 4; i++)
            map->write_b((uint32_t) pa + i, val >> (i * 8), map->priv);
    }
}

/* Outside pages[]: page-crossing writes pass a computed address to the _no_mmut write,
   which translates again. */
static page_t rmm_watch_page = { .write_b = rmm_watch_wb, .write_w = rmm_watch_ww, .write_l = rmm_watch_wl };

static void
rmm_watch_read_done(uint32_t len)
{
    const uint32_t lin = mem_logical_addr;

    if (lin == 0xffffffff) /* mem_readb_phys and friends: not a CPU access */
        return;
    rmm_watch_access(lin, len, RMM_WATCH_R, NULL);
    /* the RAM read handler has just re-added a fast lookup for this page */
    readlookup2[lin >> 12]             = LOOKUP_INV;
    readlookup2[(lin >> 12) | 1048576] = LOOKUP_INV;
}

static uint8_t
rmm_watch_rb(uint32_t addr, void *priv)
{
    const mem_mapping_t *orig  = ((rmm_rpage_t *) priv)->orig;
    uint8_t              value = 0xff;

    if (orig && orig->read_b)
        value = orig->read_b(addr, orig->priv);
    rmm_watch_read_done(1);
    return value;
}

static uint16_t
rmm_watch_rw(uint32_t addr, void *priv)
{
    const mem_mapping_t *orig  = ((rmm_rpage_t *) priv)->orig;
    uint16_t             value = 0xffff;

    if (orig && orig->read_w)
        value = orig->read_w(addr, orig->priv);
    else if (orig && orig->read_b)
        value = orig->read_b(addr, orig->priv) | (orig->read_b(addr + 1, orig->priv) << 8);
    rmm_watch_read_done(2);
    return value;
}

static uint32_t
rmm_watch_rl(uint32_t addr, void *priv)
{
    const mem_mapping_t *orig  = ((rmm_rpage_t *) priv)->orig;
    uint32_t             value = 0xffffffff;

    if (orig && orig->read_l)
        value = orig->read_l(addr, orig->priv);
    else if (orig && orig->read_w)
        value = orig->read_w(addr, orig->priv) | ((uint32_t) orig->read_w(addr + 2, orig->priv) << 16);
    else if (orig && orig->read_b) {
        value = 0;
        for (int i = 0; i < 4; i++)
            value |= (uint32_t) orig->read_b(addr + i, orig->priv) << (i * 8);
    }
    rmm_watch_read_done(4);
    return value;
}

static void
rmm_rpage_install(rmm_rpage_t *rp)
{
    rp->orig = read_mapping[rp->page];
    if (rp->orig)
        rp->ours = *rp->orig; /* flags, exec, base/mask as before */
    else
        memset(&rp->ours, 0, sizeof(rp->ours));
    rp->ours.read_b       = rmm_watch_rb;
    rp->ours.read_w       = rmm_watch_rw;
    rp->ours.read_l       = rmm_watch_rl;
    rp->ours.priv         = rp;
    read_mapping[rp->page] = &rp->ours;
}

/* Take every override out. CPU parked, or on the CPU thread. */
static void
rmm_watch_uninstall(void)
{
    for (int i = 0; i < rmm_wpage_count; i++)
        if (page_lookup[rmm_wpages[i]] == &rmm_watch_page)
            page_lookup[rmm_wpages[i]] = NULL;
    for (int i = 0; i < rmm_rpage_count; i++)
        if (read_mapping[rmm_rpages[i].page] == &rmm_rpages[i].ours)
            read_mapping[rmm_rpages[i].page] = rmm_rpages[i].orig;
    rmm_wpage_count = rmm_rlin_count = rmm_rpage_count = 0;
}

static void
rmm_page_add(uint32_t *list, int *count, uint32_t page)
{
    for (int i = 0; i < *count; i++)
        if (list[i] == page)
            return;
    list[(*count)++] = page;
}

/* Put the overrides in for the current watch list. */
static void
rmm_watch_install(void)
{
    rmm_watch_uninstall();
    for (int i = 0; i < rmm_watch_count; i++) {
        const rmm_watch_t *watch = &rmm_watches[i];

        for (uint32_t page = watch->addr >> 12; page <= (watch->addr + watch->len - 1) >> 12; page++) {
            if (watch->kind & RMM_WATCH_W)
                rmm_page_add(rmm_wpages, &rmm_wpage_count, page);
            if (watch->kind & RMM_WATCH_R)
                rmm_page_add(rmm_rlin, &rmm_rlin_count, page);
        }
    }
    if (rmm_wpage_count) {
        flushmmucache_write(); /* no watched page may stay in the fast-write ring */
        for (int i = 0; i < rmm_wpage_count; i++)
            page_lookup[rmm_wpages[i]] = &rmm_watch_page;
    }
    for (int i = 0; i < rmm_rlin_count; i++) {
        const int64_t pa = rmm_lin_to_phys(rmm_rlin[i] << 12);
        int           j;

        rmm_rlin_phys[i] = pa;
        readlookup2[rmm_rlin[i]]           = LOOKUP_INV;
        readlookup2[rmm_rlin[i] | 1048576] = LOOKUP_INV;
        if (pa < 0)
            continue; /* not present now: re-checked before each instruction */
        for (j = 0; (j < rmm_rpage_count) && (rmm_rpages[j].page != ((uint32_t) pa & rammask) >> 12); j++)
            ;
        if (j == rmm_rpage_count) {
            rmm_rpages[j].page = ((uint32_t) pa & rammask) >> 12;
            rmm_rpage_install(&rmm_rpages[rmm_rpage_count++]);
        }
    }
}

/* CPU thread, before each instruction while a watch is armed: repair what
   something else has replaced, follow page-table changes. */
static void
rmm_watch_sync(void)
{
    int rebuild = 0;

    for (int i = 0; i < rmm_wpage_count; i++)
        if (page_lookup[rmm_wpages[i]] != &rmm_watch_page)
            rebuild = 1;
    for (int i = 0; i < rmm_rpage_count; i++)
        if (read_mapping[rmm_rpages[i].page] != &rmm_rpages[i].ours)
            rebuild = 1;
    if (cr0 >> 31)
        for (int i = 0; i < rmm_rlin_count; i++)
            if (rmm_lin_to_phys(rmm_rlin[i] << 12) != rmm_rlin_phys[i])
                rebuild = 1;
    if (rebuild)
        rmm_watch_install();
}

/* CPU thread, between instructions: a rule watch has matched. */
static void
rmm_rule_run(const rmm_watch_t *rule)
{
    const rmm_watch_t rule_copy = *rule; /* the list is edited below */
    uint32_t          base;
    uint32_t          len = rule_copy.a_len;
    uint32_t          value;

    if (rule_copy.action == RMM_ACT_DISARM) {
        rmm_children_remove(rule_copy.addr, 0);
        rmm_watch_install();
        return;
    }
    if (rule_copy.a_far) {
        uint32_t off;
        uint32_t seg;

        if (!rmm_read_lin(rule_copy.addr + rule_copy.a_base_off, 2, &off) || !rmm_read_lin(rule_copy.addr + rule_copy.a_base_off + 2, 2, &seg))
            return;
        base = (seg << 4) + off;
    } else if (!rmm_read_lin(rule_copy.addr + rule_copy.a_base_off, rule_copy.a_base_size, &base))
        return;
    if (!len) {
        if (!rmm_read_lin(rule_copy.addr + rule_copy.a_len_off, rule_copy.a_len_size, &value))
            return;
        len = value;
    }
    rmm_children_remove(rule_copy.addr, 0);
    if ((len < 1) || (len > 4096) || ((uint64_t) base + len > 0x100000000ULL) || (rmm_watch_count >= RMM_WATCH_MAX)) {
        rmm_watch_install();
        return;
    }
    memset(&rmm_watches[rmm_watch_count], 0, sizeof(rmm_watch_t));
    rmm_watches[rmm_watch_count].id    = rmm_bp_next_id++;
    rmm_watches[rmm_watch_count].addr  = base;
    rmm_watches[rmm_watch_count].len   = len;
    rmm_watches[rmm_watch_count].kind  = rule_copy.a_kind;
    rmm_watches[rmm_watch_count].owner = rule_copy.id;
    rmm_watches[rmm_watch_count].src   = rule_copy.addr;
    rmm_watch_count++;
    rmm_watch_install();
}

/* -- function contracts ------------------------------------------------------- */

static const char *const rmm_reg32[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
static const char *const rmm_reg16[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
static const char *const rmm_reg8l[4] = { "al", "cl", "dl", "bl" };
static const char *const rmm_reg8h[4] = { "ah", "ch", "dh", "bh" };

static int
rmm_parse_side(const char *text, size_t len, rmm_side_t *side)
{
    char  buf[32];
    char *end;

    while (len && isspace((unsigned char) *text)) {
        text++;
        len--;
    }
    while (len && isspace((unsigned char) text[len - 1]))
        len--;
    if (!len || (len >= sizeof(buf)))
        return 0;
    memcpy(buf, text, len);
    buf[len] = 0;
    memset(side, 0, sizeof(*side));
    for (int i = 0; i < 8; i++) {
        if (!strcmp(buf, rmm_reg32[i])) {
            side->kind = RMM_SIDE_REG;
            side->reg  = i;
            side->size = 4;
            return 1;
        }
        if (!strcmp(buf, rmm_reg16[i])) {
            side->kind = RMM_SIDE_REG;
            side->reg  = i;
            side->size = 2;
            return 1;
        }
        if ((i < 4) && (!strcmp(buf, rmm_reg8l[i]) || !strcmp(buf, rmm_reg8h[i]))) {
            side->kind = RMM_SIDE_REG;
            side->reg  = i;
            side->size = 1;
            side->high = !strcmp(buf, rmm_reg8h[i]);
            return 1;
        }
    }
    if (!strncmp(buf, "darg", 4) || !strncmp(buf, "arg", 3)) {
        const int is_darg = (buf[0] == 'd');
        const char *digits = buf + (is_darg ? 4 : 3);
        long  argnum;

        argnum = strtol(digits, &end, 10);
        if ((end == digits) || *end || (argnum < 0) || (argnum > 64))
            return 0;
        side->kind = is_darg ? RMM_SIDE_DARG : RMM_SIDE_ARG;
        side->argnum    = (int32_t) argnum;
        return 1;
    }
    side->constant = strtoll(buf, &end, 0);
    if (*end)
        return 0;
    side->kind = RMM_SIDE_CONST;
    return 1;
}

/* "ax >= 0 && ax <= arg1" -> clauses; returns 0 and an error text on a bad spec. */
static int
rmm_parse_spec(const char *spec, rmm_clause_t *out, int *count, char *emsg, size_t esz)
{
    int nclauses = 0;

    while (*spec) {
        const char *and_at  = strstr(spec, "&&");
        const char *cl = spec;
        size_t      len = and_at ? (size_t) (and_at - spec) : strlen(spec);
        const char *op_at   = spec;
        rmm_clause_t clause;
        size_t      oplen;

        (void) cl;
        while ((size_t) (op_at - spec) < len && !strchr("=!<>", *op_at))
            op_at++;
        if ((size_t) (op_at - spec) >= len) {
            snprintf(emsg, esz, "no comparison in '%.*s'", (int) len, spec);
            return 0;
        }
        memset(&clause, 0, sizeof(clause));
        if (!strncmp(op_at, "==", 2)) {
            clause.op  = 0;
            oplen = 2;
        } else if (!strncmp(op_at, "!=", 2)) {
            clause.op  = 1;
            oplen = 2;
        } else if (!strncmp(op_at, "<=", 2)) {
            clause.op  = 3;
            oplen = 2;
        } else if (!strncmp(op_at, ">=", 2)) {
            clause.op  = 5;
            oplen = 2;
        } else if (*op_at == '<') {
            clause.op  = 2;
            oplen = 1;
        } else if (*op_at == '>') {
            clause.op  = 4;
            oplen = 1;
        } else {
            snprintf(emsg, esz, "bad operator in '%.*s'", (int) len, spec);
            return 0;
        }
        if (op_at[oplen] == 'u') {
            clause.uns = 1;
            oplen++;
        }
        if (!rmm_parse_side(spec, (size_t) (op_at - spec), &clause.left) || ((size_t) (op_at - spec) + oplen > len) ||
            !rmm_parse_side(op_at + oplen, len - (size_t) (op_at - spec) - oplen, &clause.right)) {
            snprintf(emsg, esz, "bad operand in '%.*s'", (int) len, spec);
            return 0;
        }
        if (nclauses >= RMM_CL_MAX) {
            snprintf(emsg, esz, "at most %d clauses", RMM_CL_MAX);
            return 0;
        }
        out[nclauses++] = clause;
        spec += len;
        if (and_at)
            spec += 2;
    }
    *count = nclauses;
    return nclauses > 0;
}

/* One side's value; 0 if the stack slot cannot be read. */
static int
rmm_side_val(const rmm_side_t *side, uint32_t sp, int uns, int64_t *out)
{
    uint32_t raw;
    uint32_t mask = stack32 ? 0xffffffffu : 0xffffu;

    switch (side->kind) {
        case RMM_SIDE_CONST:
            *out = side->constant;
            return 1;
        case RMM_SIDE_REG:
            raw = cpu_state.regs[side->reg].l;
            if (side->size == 1)
                raw = side->high ? (raw >> 8) & 0xff : raw & 0xff;
            else if (side->size == 2)
                raw &= 0xffff;
            *out = uns ? (int64_t) raw : (side->size == 1) ? (int64_t) (int8_t) raw : (side->size == 2) ? (int64_t) (int16_t) raw : (int64_t) (int32_t) raw;
            return 1;
        default: {
            const int wide = (side->kind == RMM_SIDE_DARG);

            if (!rmm_read_lin(ss + ((sp + (wide ? 4 + 4 * side->argnum : 2 + 2 * side->argnum)) & mask), wide ? 4 : 2, &raw))
                return 0;
            *out = uns ? (int64_t) raw : wide ? (int64_t) (int32_t) raw : (int64_t) (int16_t) raw;
            return 1;
        }
    }
}

/* 0 = clause k of the list is broken, recorded in the breakpoint. */
static int
rmm_contract_check(rmm_bp_t *bp, const rmm_clause_t *cl, int nclauses, uint32_t sp, int phase)
{
    for (int i = 0; i < nclauses; i++) {
        int64_t left, right;
        int     ok;

        if (!rmm_side_val(&cl[i].left, sp, cl[i].uns, &left) || !rmm_side_val(&cl[i].right, sp, cl[i].uns, &right))
            continue; /* cannot judge */
        switch (cl[i].op) {
            case 0: ok = (left == right); break;
            case 1: ok = (left != right); break;
            case 2: ok = (left < right); break;
            case 3: ok = (left <= right); break;
            case 4: ok = (left > right); break;
            default: ok = (left >= right); break;
        }
        if (!ok) {
            bp->viols++;
            bp->last_phase  = phase;
            bp->last_clause = i;
            bp->last_l      = left;
            bp->last_r      = right;
            return 0;
        }
    }
    return 1;
}

static int rmm_cpu_stop(int reason, uint32_t addr, int id, int kind);

/* CPU thread, before the instruction at lin. ret 0: a contract breakpoint was reached,
   ret 1: a watched call is returning. 1 = stop. */
static int
rmm_contract_enter(rmm_bp_t *bp, uint32_t lin)
{
    const uint32_t sp = stack32 ? ESP : SP;
    uint32_t       off, seg = 0;
    rmm_frame_t    frame;

    bp->calls++;
    if (bp->npost) {
        const int sz = bp->ret_far ? 2 : bp->ret_size;

        if (rmm_read_lin(ss + sp, sz, &off) && (!bp->ret_far || rmm_read_lin(ss + sp + 2, 2, &seg))) {
            frame.lin   = bp->ret_far ? (seg << 4) + off : cs + off;
            frame.sp    = sp;
            frame.after = sp + (bp->ret_far ? 4 : sz);
            while (bp->nfr && (bp->fr[bp->nfr - 1].sp <= sp)) /* calls that never returned */
                bp->nfr--;
            if (bp->nfr == RMM_FRAME_MAX) {
                memmove(&bp->fr[0], &bp->fr[1], sizeof(bp->fr[0]) * (RMM_FRAME_MAX - 1));
                bp->nfr--;
            }
            bp->fr[bp->nfr++] = frame;
        }
    }
    if (bp->npre && !rmm_contract_check(bp, bp->pre, bp->npre, sp, 1) && bp->stop) {
        rmm_stop_viol   = 1;
        rmm_stop_clause = bp->last_clause;
        rmm_stop_lhs    = bp->last_l;
        rmm_stop_rhs    = bp->last_r;
        return rmm_cpu_stop(RMM_STOP_BREAK, lin, bp->id, 0);
    }
    return 0;
}

static int
rmm_contract_return(rmm_bp_t *bp, uint32_t lin)
{
    const uint32_t sp = stack32 ? ESP : SP;

    if (!bp->nfr || (bp->fr[bp->nfr - 1].lin != lin) || (bp->fr[bp->nfr - 1].after != sp))
        return 0;
    bp->nfr--;
    if (rmm_contract_check(bp, bp->post, bp->npost, bp->fr[bp->nfr].sp, 2) || !bp->stop)
        return 0; /* holds, or only counted */
    rmm_stop_viol   = 2;
    rmm_stop_clause = bp->last_clause;
    rmm_stop_lhs    = bp->last_l;
    rmm_stop_rhs    = bp->last_r;
    return rmm_cpu_stop(RMM_STOP_BREAK, lin, bp->id, 0);
}

static int
rmm_cpu_stop(int reason, uint32_t addr, int id, int kind)
{
    rmm_stop_addr   = addr;
    rmm_stop_id     = id;
    rmm_stop_kind   = kind;
    rmm_step_left   = -1;
    rmm_cpu_halted  = 1;
    rmm_stop_reason = reason; /* last: this is what the monitor thread polls */
    return 1;
}

/* CPU thread, debug executor only, before each instruction. 1 = stop here. */
int
rmm_cpu_instruction(void)
{
    const uint32_t lin = cs + cpu_state.pc;

    rmm_stop_viol = 0;
    if (rmm_watch_count) {
        if (rmm_watch_hit_id) { /* the previous instruction touched a watched range */
            const int id       = rmm_watch_hit_id;
            int       act      = RMM_ACT_STOP;
            int       owner    = 0;
            uint32_t  src      = 0;
            rmm_watch_hit_id   = 0;
            for (int i = 0; i < rmm_watch_count; i++)
                if (rmm_watches[i].id == id) {
                    act   = rmm_watches[i].action;
                    owner = rmm_watches[i].owner;
                    src   = rmm_watches[i].src;
                    if (act != RMM_ACT_STOP)
                        rmm_rule_run(&rmm_watches[i]);
                    break;
                }
            if (act == RMM_ACT_STOP) {
                rmm_stop_vals  = rmm_watch_hit_vals;
                rmm_stop_old   = rmm_watch_hit_old;
                rmm_stop_new   = rmm_watch_hit_new;
                rmm_stop_owner = owner;
                rmm_stop_src   = src;
                return rmm_cpu_stop(RMM_STOP_WATCH, rmm_watch_hit_addr, id, rmm_watch_hit_kind);
            }
        }
        rmm_watch_sync();
    }
    if (rmm_bp_skip)
        rmm_bp_skip = 0;
    else {
        for (int i = 0; i < rmm_bp_count; i++) {
            rmm_bp_t *bp = &rmm_bps[i];

            if (bp->nfr && rmm_contract_return(bp, lin))
                return 1;
            if (bp->addr != lin)
                continue;
            if (!bp->npre && !bp->npost)
                return rmm_cpu_stop(RMM_STOP_BREAK, lin, bp->id, 0);
            if (rmm_contract_enter(bp, lin))
                return 1;
        }
    }
    if (rmm_step_left >= 0) {
        if (rmm_step_left == 0)
            return rmm_cpu_stop(RMM_STOP_STEP, lin, 0, 0);
        rmm_step_left--;
        /* No hardware interrupt between stepped instructions: the MOV SS / STI
           interrupt-shadow counter. */
        cpu_end_block_after_ins = 2;
    }
    return 0;
}

static int
rmm_debug_armed(void)
{
    return (rmm_bp_count > 0) || (rmm_step_left >= 0) || (rmm_watch_count > 0);
}

/* Machine paused: put the debug executor in front, or the stock one back. */
static void
rmm_exec_update(void)
{
    if (rmm_debug_armed() && (cpu_exec != exec386_rmm)) {
        rmm_exec_stock = cpu_exec;
        cpu_exec       = exec386_rmm;
    } else if (!rmm_debug_armed() && (cpu_exec == exec386_rmm))
        cpu_exec = rmm_exec_stock;
}

/* cpu_set() on a hard reset has just assigned a fresh stock executor. */
void
rmm_cpu_init(void)
{
    rmm_exec_update();
}

/* The debug executor needs a 386-class or later CPU; the 286 and 808x cores have no hook. */
static int
rpc_need_debug_cpu(rmm_rpc_err_t *err)
{
    if (!is386) {
        rpc_fail(err, RMM_E_BUSY, "breakpoints and stepping need a 386-class or later CPU");
        return 0;
    }
    return 1;
}

/* Pause without telling the client, for changes that need a parked CPU. */
static int
rmm_quiesce(void)
{
    const int was = dopause;

    if (!was)
        rmm_set_pause(1);
    return was;
}

static void
rmm_unquiesce(int was)
{
    if (!was)
        rmm_set_pause(0);
}

/* Disconnect: no breakpoint may outlive the client that set it. */
static void
rmm_debug_reset(void)
{
    int was;

    if (!rmm_debug_armed() && !rmm_cpu_halted && (cpu_exec != exec386_rmm) && !rmm_wpage_count && !rmm_rpage_count)
        return;
    was              = rmm_quiesce();
    rmm_bp_count     = 0;
    rmm_watch_count  = 0;
    rmm_watch_hit_id = 0;
    rmm_watch_uninstall();
    rmm_step_left    = -1;
    rmm_stop_reason  = 0;
    rmm_cpu_halted   = 0;
    rmm_exec_update();
    rmm_unquiesce(was);
}

/* Monitor thread, every few ms while a client is connected. */
static void
rmm_debug_poll(void)
{
    const int reason = rmm_stop_reason;
    cJSON    *ev;

    if (!reason)
        return;
    if (!dopause) {
        if (!rmm_set_pause(1))
            return; /* try again on the next poll */
        paused_by_us = 1;
    }
    pause_auto      = 0;
    rmm_stop_reason = 0;
    rmm_cpu_halted  = 0;
    rmm_exec_update(); /* a finished step with no breakpoint left: stock executor back */

    ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "reason", (reason == RMM_STOP_BREAK) ? "breakpoint" : (reason == RMM_STOP_WATCH) ? "watchpoint" : "step");
    cJSON_AddNumberToObject(ev, "addr", rmm_stop_addr);
    if (reason != RMM_STOP_STEP)
        cJSON_AddNumberToObject(ev, "id", rmm_stop_id);
    if (reason == RMM_STOP_WATCH) {
        cJSON_AddStringToObject(ev, "kind", (rmm_stop_kind == RMM_WATCH_R) ? "r" : "w");
        cJSON_AddNumberToObject(ev, "eip", cpu_state.pc); /* past the accessing instruction */
        if (rmm_stop_vals) {
            cJSON_AddNumberToObject(ev, "old", rmm_stop_old);
            cJSON_AddNumberToObject(ev, "new", rmm_stop_new);
        }
        if (rmm_stop_owner) {
            cJSON_AddNumberToObject(ev, "rule", rmm_stop_owner);
            cJSON_AddNumberToObject(ev, "src", rmm_stop_src);
        }
    }
    if ((reason == RMM_STOP_BREAK) && rmm_stop_viol) {
        cJSON_AddStringToObject(ev, "violation", (rmm_stop_viol == 1) ? "pre" : "post");
        cJSON_AddNumberToObject(ev, "clause", rmm_stop_clause);
        cJSON_AddNumberToObject(ev, "lhs", (double) rmm_stop_lhs);
        cJSON_AddNumberToObject(ev, "rhs", (double) rmm_stop_rhs);
    }
    rmm_event("machine.stopped", ev);
}

/* Pause or resume for the client; emits the state-change event on a real change. */
static int
rmm_machine_run(int run)
{
    int changed = (!!dopause == !!run);

    if (!run && !dopause)
        paused_by_us = 1;
    if (!run)
        pause_auto = 0; /* the client asked for this one */
    if (run) {
        paused_by_us = 0;
        if (cpu_exec == exec386_rmm)
            rmm_bp_skip = 1;
    }
    if (!rmm_set_pause(!run))
        return 0;
    if (!run && (rmm_step_left >= 0)) { /* a pause ends a step early */
        rmm_step_left = -1;
        rmm_exec_update();
    }
    if (changed) {
        if (run)
            rmm_event("machine.resumed", NULL);
        else {
            cJSON *event_params = cJSON_CreateObject();
            cJSON_AddStringToObject(event_params, "reason", "pause");
            rmm_event("machine.stopped", event_params);
        }
    }
    return 1;
}

static cJSON *
rpc_state(void)
{
    cJSON *reply = cJSON_CreateObject();

    cJSON_AddStringToObject(reply, "state", rmm_state());
    return reply;
}

static cJSON *
rpc_machine_status(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = rpc_state();

    cJSON_AddBoolToObject(reply, "sticky_pause", !!rmm_sticky_pause);
    cJSON_AddNumberToObject(reply, "tsc", (double) tsc);
    cJSON_AddNumberToObject(reply, "cs", cpu_state.seg_cs.seg);
    cJSON_AddNumberToObject(reply, "eip", cpu_state.pc);
    return reply;
}

static cJSON *
rpc_machine_pause(UNUSED(cJSON *params), rmm_rpc_err_t *err)
{
    if (!rmm_machine_run(0))
        return rpc_fail(err, -32603, "pause did not take effect");
    return rpc_state();
}

static cJSON *
rpc_sticky_get(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = cJSON_CreateObject();

    cJSON_AddBoolToObject(reply, "value", !!rmm_sticky_pause);
    return reply;
}

/* Runtime switch only; [General] rmm_sticky_pause is the saved default. */
static cJSON *
rpc_sticky_set(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON *value_item = cJSON_GetObjectItemCaseSensitive(params, "value");

    if (!cJSON_IsBool(value_item))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'value' must be a boolean");
    rmm_sticky_pause = !!cJSON_IsTrue(value_item);
    return rpc_sticky_get(params, err);
}

static cJSON *
rpc_machine_resume(UNUSED(cJSON *params), rmm_rpc_err_t *err)
{
    if (!rmm_machine_run(1))
        return rpc_fail(err, -32603, "resume did not take effect");
    return rpc_state();
}

static const char *rmm_gpr[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

/* Index into rmm_gpr, 8 = eip, 9 = eflags, -1 = not settable. Segment registers are excluded. */
static int
rmm_settable_reg(const char *name)
{
    for (int i = 0; i < 8; i++)
        if (!strcmp(name, rmm_gpr[i]))
            return i;
    if (!strcmp(name, "eip"))
        return 8;
    if (!strcmp(name, "eflags"))
        return 9;
    return -1;
}

static cJSON *
rpc_cpu_regs_get(UNUSED(cJSON *params), rmm_rpc_err_t *err)
{
    cJSON *reply;

    if (!rpc_need_paused(err))
        return NULL;
    rmm_flags_rebuild();
    reply = cJSON_CreateObject();
    for (int i = 0; i < 8; i++)
        cJSON_AddNumberToObject(reply, rmm_gpr[i], cpu_state.regs[i].l);
    cJSON_AddNumberToObject(reply, "eip", cpu_state.pc);
    cJSON_AddNumberToObject(reply, "eflags", ((uint32_t) cpu_state.eflags << 16) | cpu_state.flags);
    cJSON_AddNumberToObject(reply, "cs", cpu_state.seg_cs.seg);
    cJSON_AddNumberToObject(reply, "ss", cpu_state.seg_ss.seg);
    cJSON_AddNumberToObject(reply, "ds", cpu_state.seg_ds.seg);
    cJSON_AddNumberToObject(reply, "es", cpu_state.seg_es.seg);
    cJSON_AddNumberToObject(reply, "fs", cpu_state.seg_fs.seg);
    cJSON_AddNumberToObject(reply, "gs", cpu_state.seg_gs.seg);
    cJSON_AddNumberToObject(reply, "cs_base", cpu_state.seg_cs.base);
    cJSON_AddNumberToObject(reply, "cr0", cr0);
    cJSON_AddNumberToObject(reply, "cr2", cr2);
    cJSON_AddNumberToObject(reply, "cr3", cr3);
    cJSON_AddNumberToObject(reply, "cr4", cr4);
    return reply;
}

/* cpu.regs.set {eax.., eip, eflags}: all values are checked before any is written. */
static cJSON *
rpc_cpu_regs_set(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON   *it;
    uint32_t value;

    if (!cJSON_IsObject(params) || !params->child)
        return rpc_fail(err, RPC_INVALID_PARAMS, "give at least one register (eax..edi, eip, eflags)");
    cJSON_ArrayForEach(it, params) {
        if (rmm_settable_reg(it->string) < 0)
            return rpc_fail(err, RPC_INVALID_PARAMS, "'%s' is not a settable register (eax..edi, eip, eflags)",
                            it->string);
        if (!rpc_uint(params, it->string, 4294967295.0, &value, 1, err))
            return NULL;
    }
    if (!rpc_need_paused(err))
        return NULL;
    rmm_flags_rebuild();
    it = cJSON_GetObjectItemCaseSensitive(params, "eflags");
    if (it && ((((uint32_t) it->valuedouble >> 16) ^ cpu_state.eflags) & VM_FLAG))
        return rpc_fail(err, RPC_INVALID_PARAMS, "eflags.VM cannot be changed: it switches CPU mode");

    cJSON_ArrayForEach(it, params) {
        int idx = rmm_settable_reg(it->string);

        value = (uint32_t) it->valuedouble;
        if (idx < 8)
            cpu_state.regs[idx].l = value;
        else if (idx == 8)
            cpu_state.pc = value;
        else {
            cpu_state.flags  = (uint16_t) (value | 2); /* bit 1 always reads 1 */
            cpu_state.eflags = (uint16_t) (value >> 16);
        }
    }
    return rpc_cpu_regs_get(NULL, err);
}

/* Linear to physical through the guest page tables at CPL 0, read only. Paging off:
   linear is physical. -1 when not present or above 4 GiB. */
static int64_t
rmm_lin_to_phys(uint32_t lin)
{
    uint64_t  pa;
    uint8_t   abrt   = cpu_state.abrt;
    const int cpl_ov = cpl_override;

    if (!(cr0 & 0x80000000))
        return lin;
    cpu_state.abrt = 0;
    cpl_override   = 1;
    pa             = mmutranslate_noabrt(lin, 0);
    cpu_state.abrt = abrt;
    cpl_override   = cpl_ov;
    return (pa > 0xffffffffULL) ? -1 : (int64_t) pa;
}

/* addr/len as for mem.phys.read; shared by both read methods. */
static int
rpc_mem_span(cJSON *params, uint32_t *addr, uint32_t *len, rmm_rpc_err_t *err)
{
    if (!rpc_uint(params, "addr", 4294967295.0, addr, 1, err) ||
        !rpc_uint(params, "len", RMM_MEM_MAX, len, 1, err))
        return 0;
    if (*len == 0) {
        rpc_fail(err, RPC_INVALID_PARAMS, "'len' must be 1..%d", RMM_MEM_MAX);
        return 0;
    }
    if ((uint64_t) *addr + *len > 0x100000000ULL) {
        rpc_fail(err, RPC_INVALID_PARAMS, "range runs past the end of the 32-bit address space");
        return 0;
    }
    return rpc_need_paused(err);
}

static cJSON *
rpc_mem_read_reply(uint32_t addr, uint32_t len, const char *hex)
{
    cJSON *reply = cJSON_CreateObject();

    cJSON_AddNumberToObject(reply, "addr", addr);
    cJSON_AddNumberToObject(reply, "len", len);
    cJSON_AddStringToObject(reply, "data", hex);
    return reply;
}

/* mem.lin.read: one page-table walk per 4K page. A page not present fails the read with error 9. */
static cJSON *
rpc_mem_lin_read(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t addr;
    uint32_t len;
    int64_t  pa = 0;
    char     hex[RMM_MEM_MAX * 2 + 1];

    if (!rpc_mem_span(params, &addr, &len, err))
        return NULL;
    for (uint32_t i = 0; i < len; i++) {
        const uint32_t lin = addr + i;

        if ((i == 0) || !(lin & 0xfff)) {
            if ((pa = rmm_lin_to_phys(lin & ~0xfffU)) < 0) {
                err->data = cJSON_CreateObject();
                cJSON_AddNumberToObject(err->data, "first_bad", lin);
                return rpc_fail(err, RMM_E_NOT_MAPPED, "linear %08X is not mapped", lin);
            }
        }
        sprintf(&hex[i * 2], "%02x", mem_rmm_readb_phys((uint32_t) pa + (lin & 0xfff)));
    }
    return rpc_mem_read_reply(addr, len, hex);
}

static cJSON *
rpc_mem_phys_read(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t addr;
    uint32_t len;
    char     hex[RMM_MEM_MAX * 2 + 1];

    if (!rpc_mem_span(params, &addr, &len, err))
        return NULL;
    for (uint32_t i = 0; i < len; i++)
        sprintf(&hex[i * 2], "%02x", mem_rmm_readb_phys(addr + i));
    return rpc_mem_read_reply(addr, len, hex);
}

static cJSON *
rpc_mem_phys_write(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t    addr;
    const char *hexp;
    size_t      hlen;
    int         len;
    int         took;
    int         first_bad;
    uint8_t     data[RMM_MEM_MAX];
    cJSON      *raw;
    cJSON      *reply;

    if (!rpc_uint(params, "addr", 4294967295.0, &addr, 1, err) ||
        !(hexp = rpc_string(params, "data", err)))
        return NULL;
    hlen = strlen(hexp);
    if ((hlen == 0) || (hlen & 1))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'data' must be a non-empty even number of hex digits");
    if (hlen > RMM_MEM_MAX * 2)
        return rpc_fail(err, RPC_INVALID_PARAMS, "at most %d bytes per call", RMM_MEM_MAX);
    len = (int) (hlen / 2);
    for (int i = 0; i < len; i++) {
        int hi = rmm_hexval(hexp[i * 2]);
        int lo = rmm_hexval(hexp[i * 2 + 1]);
        if ((hi < 0) || (lo < 0))
            return rpc_fail(err, RPC_INVALID_PARAMS, "'data' must be hex digits");
        data[i] = (uint8_t) ((hi << 4) | lo);
    }
    if ((uint64_t) addr + (uint64_t) len > 0x100000000ULL)
        return rpc_fail(err, RPC_INVALID_PARAMS, "range runs past the end of the 32-bit address space");
    raw = cJSON_GetObjectItemCaseSensitive(params, "raw");
    if (raw && !cJSON_IsBool(raw))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'raw' must be a boolean");
    if (!rpc_need_paused(err))
        return NULL;

    first_bad = rmm_phys_write(addr, data, len, cJSON_IsTrue(raw), &took);
    if (first_bad >= 0) {
        err->data = cJSON_CreateObject();
        cJSON_AddNumberToObject(err->data, "took", took);
        cJSON_AddNumberToObject(err->data, "len", len);
        cJSON_AddNumberToObject(err->data, "first_bad", addr + (uint32_t) first_bad);
        return rpc_fail(err, RMM_E_DROPPED, "write dropped: %d of %d bytes read back", took, len);
    }
    reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "addr", addr);
    cJSON_AddNumberToObject(reply, "len", len);
    return reply;
}

static cJSON *
rpc_input_key_send(cJSON *params, rmm_rpc_err_t *err)
{
    const char *combo = rpc_string(params, "combo", err);
    char        buf[128];

    if (!combo)
        return NULL;
    snprintf(buf, sizeof(buf), "%s", combo); /* rmm_sendkey tokenises in place */
    if (!rmm_sendkey(buf))
        return rpc_fail(err, RPC_INVALID_PARAMS, "unknown key in '%s'", combo);
    return cJSON_CreateObject();
}

static cJSON *
rpc_input_text_type(cJSON *params, rmm_rpc_err_t *err)
{
    const char *text = rpc_string(params, "text", err);

    if (!text)
        return NULL;
    if (!rmm_type(text))
        return rpc_fail(err, RPC_INVALID_PARAMS, "untypeable character in text");
    return cJSON_CreateObject();
}

/* -- input.events / input.mouse.move / input.release_all / input.devices --------
   One ordered queue of keys, mouse motion and buttons, drained on the monitor thread
   (rmm_input_poll, every few ms). Motion goes at once. A key waits RMM_KEY_GAP_MS of guest
   time after the previous key (the 8042's FIFO holds 16 bytes), a button change
   RMM_BTN_GAP_MS after the previous one (a serial mouse reports one state per packet), and
   everything behind it waits too. Paused, guest time stands still, so they wait for a resume. */

#define RMM_INPUT_MAX  256
#define RMM_KEY_GAP_MS 2
#define RMM_BTN_GAP_MS 30   /* a 1200-baud serial mouse sends a packet every 25 ms */
#define RMM_BTN_MASK   0x1f /* left, right, middle, buttons 4 and 5 */
#define RMM_DELTA_MAX  32767
#ifdef __APPLE__
#    define RMM_WHEEL_DELTA 1 /* device/mouse.c's WHEEL_DELTA: mouse_z units per notch */
#else
#    define RMM_WHEEL_DELTA 120
#endif

enum {
    RMM_IN_KEY,
    RMM_IN_REL,
    RMM_IN_ABS,
    RMM_IN_BTN
};

typedef struct {
    int      type;
    int      down;       /* key */
    uint16_t scan;       /* key: set 1, 0x1xx = E0 prefix */
    int32_t  dx, dy, dz; /* rel; btn: dx is the mask */
    double   abs_x, abs_y; /* abs: 0..1 across the screen */
} rmm_input_t;

static rmm_input_t rmm_inq[RMM_INPUT_MAX];
static int         rmm_inq_head;
static int         rmm_inq_count;
static uint64_t    rmm_key_tsc = UINT64_MAX; /* tsc at the last key; UINT64_MAX = none yet */
static uint64_t    rmm_btn_tsc = UINT64_MAX; /* tsc at the last button change */
static uint8_t     rmm_key_held[0x200];      /* keys this client holds down */
static int         rmm_btn_held;             /* the button mask this client set */
static int         rmm_prox_ours;            /* this client put the tablet pen in proximity */

static int rpc_int(cJSON *params, const char *name, double lo, double hi, int32_t *out, int *present, rmm_rpc_err_t *err);

/* Has the guest run ms since last? tsc counts at cpuclock (not the CPU's rated speed on an XT)
   and restarts at a hard reset. */
static int
rmm_input_due(uint64_t last, int ms)
{
    const uint64_t now = tsc;

    return (last == UINT64_MAX) || (now < last) || ((now - last) >= (uint64_t) (cpuclock / 1000.0 * ms));
}

/* Monitor thread: deliver the queue in order, as far as the gaps allow. */
static void
rmm_input_poll(void)
{
    while (rmm_inq_count) {
        const rmm_input_t *event = &rmm_inq[rmm_inq_head];

        switch (event->type) {
            case RMM_IN_KEY:
                if (!rmm_input_due(rmm_key_tsc, RMM_KEY_GAP_MS))
                    return;
                keyboard_input(event->down, event->scan);
                rmm_key_held[event->scan & 0x1ff] = event->down;
                rmm_key_tsc                   = tsc;
                break;
            case RMM_IN_BTN:
                if (!rmm_input_due(rmm_btn_tsc, RMM_BTN_GAP_MS))
                    return;
                /* mouse devices drop input unless the UI has captured the pointer */
                mouse_automation = 1;
                mouse_set_buttons_ex(event->dx);
                rmm_btn_held = event->dx;
                rmm_btn_tsc  = tsc;
                break;
            case RMM_IN_REL:
                mouse_automation = 1;
                mouse_scale(event->dx, event->dy);
                if (event->dz)
                    mouse_set_z(event->dz * RMM_WHEEL_DELTA);
                break;
            default: /* RMM_IN_ABS */
                mouse_x_abs = event->abs_x;
                mouse_y_abs = event->abs_y;
                if (!mouse_tablet_in_proximity) {
                    mouse_tablet_in_proximity = 1; /* as the UI does while the pointer is over monitor 0 */
                    rmm_prox_ours             = 1;
                }
                break;
        }
        rmm_inq_head = (rmm_inq_head + 1) % RMM_INPUT_MAX;
        rmm_inq_count--;
    }
}

static void
rmm_input_push(const rmm_input_t *event)
{
    if (rmm_inq_count < RMM_INPUT_MAX) {
        rmm_inq[(rmm_inq_head + rmm_inq_count) % RMM_INPUT_MAX] = *event;
        rmm_inq_count++;
    }
}

/* Drop the queue and release every key and button this client holds: queued, paced like
   any other input, or with now set at once (on disconnect nothing drains the queue).
   Returns {keys, buttons}: what was released. */
static cJSON *
rmm_input_release(int now)
{
    cJSON      *reply = cJSON_CreateObject();
    cJSON      *keys  = cJSON_CreateArray();
    rmm_input_t event = { 0 };

    rmm_inq_count = 0;
    for (int scan = 0; scan < 0x200; scan++) {
        if (!rmm_key_held[scan])
            continue;
        cJSON_AddItemToArray(keys, cJSON_CreateNumber(scan));
        if (now) {
            keyboard_input(0, (uint16_t) scan);
            rmm_key_held[scan] = 0;
            plat_delay_ms(10);
        } else {
            event.type = RMM_IN_KEY;
            event.scan = (uint16_t) scan;
            rmm_input_push(&event);
        }
    }
    cJSON_AddItemToObject(reply, "keys", keys);
    cJSON_AddNumberToObject(reply, "buttons", rmm_btn_held);
    if (rmm_btn_held) {
        if (now) {
            mouse_set_buttons_ex(0);
            rmm_btn_held = 0;
        } else {
            event.type = RMM_IN_BTN;
            event.dx   = 0;
            rmm_input_push(&event);
        }
    }
    if (rmm_prox_ours) {
        mouse_tablet_in_proximity = 0;
        rmm_prox_ours             = 0;
    }
    return reply;
}

/* Prefixes err->msg with the event's index; returns 0. */
static int
rmm_input_where(rmm_rpc_err_t *err, int i)
{
    char original[sizeof(err->msg)];

    snprintf(original, sizeof(original), "%s", err->msg);
    snprintf(err->msg, sizeof(err->msg), "events[%d]: %.1000s", i, original);
    return 0;
}

/* rpc_fail for event i; returns 0. */
static int
rmm_input_bad(rmm_rpc_err_t *err, int i, int code, const char *fmt, ...)
{
    va_list ap;

    err->code = code;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
    return rmm_input_where(err, i);
}

static int
rmm_input_need_mouse(rmm_rpc_err_t *err)
{
    if (!mouse_type && !tablet_type) {
        rpc_fail(err, RMM_E_NO_SLOT, "no mouse (mouse_type and tablet_type are none)");
        return 0;
    }
    return 1;
}

static int
rmm_input_parse_key(cJSON *ev, int i, rmm_input_t *event, rmm_rpc_err_t *err)
{
    const cJSON *down = cJSON_GetObjectItemCaseSensitive(ev, "down");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(ev, "key");
    int32_t      value;
    int          present;

    if (!cJSON_IsBool(down))
        return rmm_input_bad(err, i, RPC_INVALID_PARAMS, "'down' must be true or false");
    if (cJSON_IsString(name)) {
        if ((value = rmm_key_lookup(name->valuestring)) < 0)
            return rmm_input_bad(err, i, RPC_INVALID_PARAMS, "unknown key '%.64s'", name->valuestring);
    } else if (!rpc_int(ev, "scan", 1, 0x1ff, &value, &present, err))
        return rmm_input_where(err, i);
    else if (!present)
        return rmm_input_bad(err, i, RPC_INVALID_PARAMS, "a key needs 'scan' or 'key'");
    event->type = RMM_IN_KEY;
    event->down = cJSON_IsTrue(down);
    event->scan = (uint16_t) value;
    return 1;
}

static int
rmm_input_parse_rel(cJSON *ev, int i, rmm_input_t *event, rmm_rpc_err_t *err)
{
    int present;

    if (!rpc_int(ev, "dx", -RMM_DELTA_MAX - 1, RMM_DELTA_MAX, &event->dx, &present, err) || !present ||
        !rpc_int(ev, "dy", -RMM_DELTA_MAX - 1, RMM_DELTA_MAX, &event->dy, &present, err) || !present ||
        !rpc_int(ev, "dz", -127, 127, &event->dz, &present, err)) {
        if (!err->code)
            rpc_fail(err, RPC_INVALID_PARAMS, "rel needs dx and dy");
        return rmm_input_where(err, i);
    }
    if (!rmm_input_need_mouse(err))
        return rmm_input_where(err, i);
    event->type = RMM_IN_REL;
    return 1;
}

static int
rmm_input_parse_btn(cJSON *ev, int i, rmm_input_t *event, rmm_rpc_err_t *err)
{
    int present;

    if (!rpc_int(ev, "buttons", 0, RMM_BTN_MASK, &event->dx, &present, err) || !present) {
        if (!err->code)
            rpc_fail(err, RPC_INVALID_PARAMS, "missing parameter 'buttons'");
        return rmm_input_where(err, i);
    }
    if (!rmm_input_need_mouse(err))
        return rmm_input_where(err, i);
    event->type = RMM_IN_BTN;
    return 1;
}

static int
rmm_input_parse_abs(cJSON *ev, int i, rmm_input_t *event, rmm_rpc_err_t *err)
{
    const cJSON *x_item = cJSON_GetObjectItemCaseSensitive(ev, "x");
    const cJSON *y_item = cJSON_GetObjectItemCaseSensitive(ev, "y");

    if (!cJSON_IsNumber(x_item) || !cJSON_IsNumber(y_item) || (x_item->valuedouble < 0) || (x_item->valuedouble > 1) ||
        (y_item->valuedouble < 0) || (y_item->valuedouble > 1))
        return rmm_input_bad(err, i, RPC_INVALID_PARAMS, "'x' and 'y' must be numbers 0..1");
    if (!tablet_type)
        return rmm_input_bad(err, i, RMM_E_NO_SLOT, "no tablet (tablet_type is none)");
    if (mouse_input_mode < 1)
        return rmm_input_bad(err, i, RMM_E_BUSY, "the pointer is in relative (mouse) mode");
    event->type = RMM_IN_ABS;
    event->abs_x    = x_item->valuedouble;
    event->abs_y    = y_item->valuedouble;
    return 1;
}

/* One element of input.events into *e. */
static int
rmm_input_parse(cJSON *ev, int i, rmm_input_t *event, rmm_rpc_err_t *err)
{
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(ev, "type");
    const char  *type      = cJSON_IsString(type_item) ? type_item->valuestring : "";

    memset(event, 0, sizeof(*event));
    if (!strcmp(type, "key"))
        return rmm_input_parse_key(ev, i, event, err);
    if (!strcmp(type, "rel"))
        return rmm_input_parse_rel(ev, i, event, err);
    if (!strcmp(type, "btn"))
        return rmm_input_parse_btn(ev, i, event, err);
    if (!strcmp(type, "abs"))
        return rmm_input_parse_abs(ev, i, event, err);
    return rmm_input_bad(err, i, RPC_INVALID_PARAMS, "'type' must be key, rel, abs or btn");
}

static cJSON *
rmm_input_queued(int count)
{
    cJSON *reply = cJSON_CreateObject();

    rmm_input_poll();
    cJSON_AddNumberToObject(reply, "queued", count);
    cJSON_AddNumberToObject(reply, "pending", rmm_inq_count);
    return reply;
}

/* All or nothing: a bad event or a full queue queues none of the batch. */
static cJSON *
rpc_input_events(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON      *evs = cJSON_GetObjectItemCaseSensitive(params, "events");
    rmm_input_t batch[RMM_INPUT_MAX];
    int         count;

    if (!cJSON_IsArray(evs))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'events' must be a list");
    if ((count = cJSON_GetArraySize(evs)) > RMM_INPUT_MAX)
        return rpc_fail(err, RPC_INVALID_PARAMS, "at most %d events per call", RMM_INPUT_MAX);
    for (int i = 0; i < count; i++)
        if (!rmm_input_parse(cJSON_GetArrayItem(evs, i), i, &batch[i], err))
            return NULL;
    if (count > (RMM_INPUT_MAX - rmm_inq_count))
        return rpc_fail(err, RMM_E_BUSY, "input queue full: %d pending, room for %d", rmm_inq_count,
                        RMM_INPUT_MAX - rmm_inq_count);
    for (int i = 0; i < count; i++)
        rmm_input_push(&batch[i]);
    return rmm_input_queued(count);
}

/* {dx, dy, buttons?}: the button change, then the motion, through the queue. */
static cJSON *
rpc_input_mouse_move(cJSON *params, rmm_rpc_err_t *err)
{
    rmm_input_t rel = { .type = RMM_IN_REL };
    rmm_input_t btn = { .type = RMM_IN_BTN };
    int         present;
    int         has_btn;

    if (!rpc_int(params, "dx", -RMM_DELTA_MAX - 1, RMM_DELTA_MAX, &rel.dx, &present, err))
        return NULL;
    if (!present)
        return rpc_fail(err, RPC_INVALID_PARAMS, "missing parameter 'dx'");
    if (!rpc_int(params, "dy", -RMM_DELTA_MAX - 1, RMM_DELTA_MAX, &rel.dy, &present, err))
        return NULL;
    if (!present)
        return rpc_fail(err, RPC_INVALID_PARAMS, "missing parameter 'dy'");
    if (!rpc_int(params, "buttons", 0, RMM_BTN_MASK, &btn.dx, &has_btn, err))
        return NULL;
    if (!mouse_type)
        return rpc_fail(err, RMM_E_NO_SLOT, "no mouse (mouse_type is none)");
    if ((rmm_inq_count + 2) > RMM_INPUT_MAX)
        return rpc_fail(err, RMM_E_BUSY, "input queue full: %d pending", rmm_inq_count);
    if (has_btn)
        rmm_input_push(&btn);
    rmm_input_push(&rel);
    return rmm_input_queued(has_btn + 1);
}

static cJSON *
rpc_input_release_all(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = rmm_input_release(0);

    rmm_input_poll();
    cJSON_AddNumberToObject(reply, "pending", rmm_inq_count);
    return reply;
}

/* A device's int config value as the cfg holds it, def when it has no such option. */
static int
rmm_device_cfg_int(const device_t *dev, const char *name, int def)
{
    for (const device_config_t *cfg = dev ? dev->config : NULL; cfg && (cfg->type != CONFIG_END); cfg++)
        if (!strcmp(cfg->name, name))
            return config_get_int((char *) dev->name, (char *) name, cfg->default_int);
    return def;
}

/* What the emulator instantiated (mouse_type and the rest change only at a hard reset). */
static cJSON *
rpc_input_devices(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = cJSON_CreateObject();
    cJSON *info  = cJSON_CreateObject();

    cJSON_AddStringToObject(info, "type", keyboard_get_internal_name(keyboard_type));
    cJSON_AddItemToObject(reply, "keyboard", info);

    if (mouse_type) {
        const device_t *dev  = mouse_get_device(mouse_type);
        const int       port = rmm_device_cfg_int(dev, "port", -1);

        info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "type", mouse_get_internal_name(mouse_type));
        cJSON_AddNumberToObject(info, "buttons", mouse_get_buttons());
        if (port >= 0) /* a serial mouse: the COM port, 0 = COM1 */
            cJSON_AddNumberToObject(info, "port", port);
        cJSON_AddItemToObject(reply, "mouse", info);
    } else
        cJSON_AddNullToObject(reply, "mouse");

    if (joystick_type[0]) {
        const uint16_t gp = gameport_get_active_addr();

        info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "type", joystick_get_internal_name(joystick_type[0]));
        cJSON_AddNumberToObject(info, "axes", joystick_get_axis_count(joystick_type[0]));
        cJSON_AddNumberToObject(info, "buttons", joystick_get_button_count(joystick_type[0]));
        if (gp)
            cJSON_AddNumberToObject(info, "gameport", gp);
        else
            cJSON_AddNullToObject(info, "gameport");
        cJSON_AddItemToObject(reply, "joystick", info);
    } else
        cJSON_AddNullToObject(reply, "joystick");

    if (tablet_type) {
        info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "type", tablet_get_internal_name(tablet_type));
        cJSON_AddItemToObject(reply, "tablet", info);
    } else
        cJSON_AddNullToObject(reply, "tablet");

    /* absolute: input.events abs moves the pointer; relative: send rel, i.e. grab the host pointer */
    cJSON_AddStringToObject(reply, "pointer", (mouse_input_mode >= 1) ? "absolute" : "relative");
    cJSON_AddNumberToObject(reply, "pending", rmm_inq_count);
    return reply;
}

/* -- config.get/set/describe/pending/discard ------------------------------------
   Every settable key needs a schema row here. Keys applying now are saved at once;
   reset keys are staged and applied by rmm_pending_reset_apply(). */

static int cfg_ram_min(void) { return machine_get_min_ram(machine); }
static int cfg_ram_max(void) { return machine_get_max_ram(machine); }
static int cfg_ram_step(void) { return machine_get_ram_granularity(machine); }
static int cfg_gain_min(void) { return 0; }
static int cfg_gain_max(void) { return 18; }
static int cfg_one(void) { return 1; }
static int cfg_ram_get(void) { return (int) mem_size; }
static void cfg_ram_set(int v) { mem_size = (uint32_t) v; }
static int cfg_gain_get(void) { return sound_gain; }
static void cfg_gain_set(int v) { sound_gain = v; }

static const struct {
    const char *section;
    const char *key;
    int         on_reset; /* 0 applies now, 1 staged for the next hard reset */
    int         def;
    int       (*min)(void);
    int       (*max)(void);
    int       (*step)(void);
    int       (*get)(void);
    void      (*set)(int v);
} rmm_cfg[] = {
    { "Machine", "mem_size",   1, 64, cfg_ram_min,  cfg_ram_max,  cfg_ram_step, cfg_ram_get,  cfg_ram_set  },
    { "General", "sound_gain", 0, 0,  cfg_gain_min, cfg_gain_max, cfg_one,      cfg_gain_get, cfg_gain_set },
    { NULL,      NULL,         0, 0,  NULL,         NULL,         NULL,         NULL,         NULL         }
};

#define RMM_PENDING_MAX 32

typedef struct {
    int cfg; /* row in rmm_cfg[] */
    int value;
} rmm_pending_t;

/* The queue rmm_pending_reset_apply() owns; RMM thread adds, CPU thread drains. */
static rmm_pending_t rmm_pending_reset_apply_list[RMM_PENDING_MAX];
static int           rmm_pending_reset_apply_count = 0;
/* What the last drain applied, for machine.reset to report. */
static rmm_pending_t rmm_pending_reset_apply_done[RMM_PENDING_MAX];
static int           rmm_pending_reset_apply_done_count = 0;
static mutex_t      *rmm_pending_reset_apply_lock = NULL;

/* config.load's staged cfg, {section: {key: value | null}}, and whether each section
   replaces or merges. Same lock and drain as the queue above, applied first. */
static cJSON *rmm_load_cfg     = NULL;
static int    rmm_load_replace = 1;
static int    rmm_load_done    = -1; /* keys the last drain loaded, -1 none; for machine.reset */

typedef struct {
    const char *section;
    cJSON      *keys;
} rmm_keys_t;

static void
rmm_load_keys(const char *section, const char *key, UNUSED(const char *value), void *priv)
{
    rmm_keys_t *keys = (rmm_keys_t *) priv;

    if (!strcmp(section, keys->section))
        cJSON_AddItemToArray(keys->keys, cJSON_CreateString(key));
}

/* CPU thread, machine closed: merge running settings and staged sections into the cfg,
   write it, read it back into the globals. */
static void
rmm_load_apply(void)
{
    ini_t ini;
    int   applied = 0;

    config_save();
    ini = config_get_ini();
    for (cJSON *section = rmm_load_cfg->child; section; section = section->next) {
        ini_section_t sec = ini_find_or_create_section(ini, section->string);

        if (rmm_load_replace && strcmp(section->string, "General")) {
            rmm_keys_t section_keys = { section->string, cJSON_CreateArray() };
            ini_foreach(ini, rmm_load_keys, &section_keys);
            for (cJSON *old_key = section_keys.keys->child; old_key; old_key = old_key->next)
                ini_section_delete_var(sec, old_key->valuestring);
            cJSON_Delete(section_keys.keys);
        }
        for (cJSON *kv = section->child; kv; kv = kv->next, applied++) {
            char num[64];

            if (cJSON_IsNull(kv))
                ini_section_delete_var(sec, kv->string);
            else if (cJSON_IsString(kv))
                ini_section_set_string(sec, kv->string, kv->valuestring);
            else if (cJSON_IsBool(kv))
                ini_section_set_string(sec, kv->string, cJSON_IsTrue(kv) ? "1" : "0");
            else {
                snprintf(num, sizeof(num), "%.17g", kv->valuedouble);
                ini_section_set_string(sec, kv->string, num);
            }
        }
    }
    ini_write(ini, cfg_path);
    config_rmm_reload();
    pclog("RMM: config.load applied, %d keys\n", applied);
    cJSON_Delete(rmm_load_cfg);
    rmm_load_cfg  = NULL;
    rmm_load_done = applied;
}

/* Called at the start of pc_reset_hard_init(), on the CPU thread. */
void
rmm_pending_reset_apply(void)
{
    if (!rmm_pending_reset_apply_lock)
        return;
    thread_wait_mutex(rmm_pending_reset_apply_lock);
    rmm_load_done = -1;
    if (rmm_load_cfg)
        rmm_load_apply();
    for (int i = 0; i < rmm_pending_reset_apply_count; i++) {
        const rmm_pending_t *pending = &rmm_pending_reset_apply_list[i];
        rmm_cfg[pending->cfg].set(pending->value);
        pclog("RMM: applying staged %s/%s = %d\n", rmm_cfg[pending->cfg].section, rmm_cfg[pending->cfg].key, pending->value);
    }
    memcpy(rmm_pending_reset_apply_done, rmm_pending_reset_apply_list, sizeof(rmm_pending_reset_apply_list));
    rmm_pending_reset_apply_done_count = rmm_pending_reset_apply_count;
    if (rmm_pending_reset_apply_count)
        config_changed = 1; /* pc_reset_hard_init saves the cfg at its end */
    rmm_pending_reset_apply_count = 0;
    thread_release_mutex(rmm_pending_reset_apply_lock);
}

static int
rmm_pending_find(int cfg)
{
    for (int i = 0; i < rmm_pending_reset_apply_count; i++)
        if (rmm_pending_reset_apply_list[i].cfg == cfg)
            return i;
    return -1;
}

static cJSON *
rmm_pending_json(const rmm_pending_t *list, int count)
{
    cJSON *array = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "section", rmm_cfg[list[i].cfg].section);
        cJSON_AddStringToObject(entry, "key", rmm_cfg[list[i].cfg].key);
        cJSON_AddNumberToObject(entry, "value", list[i].value);
        cJSON_AddItemToArray(array, entry);
    }
    return array;
}

/* Schema row for section/key, -1 if none. */
static int
rmm_cfg_find(const char *section, const char *key)
{
    for (int i = 0; rmm_cfg[i].key; i++)
        if (!strcmp(rmm_cfg[i].section, section) && !strcmp(rmm_cfg[i].key, key))
            return i;
    return -1;
}

static int
rmm_cfg_section_known(const char *section)
{
    for (int i = 0; rmm_cfg[i].key; i++)
        if (!strcmp(rmm_cfg[i].section, section))
            return 1;
    return ini_find_section(config_get_ini(), section) != NULL;
}

static cJSON *
rpc_config_get(cJSON *params, rmm_rpc_err_t *err)
{
    const char *section = rpc_string(params, "section", err);
    cJSON      *keyj    = cJSON_GetObjectItemCaseSensitive(params, "key");
    cJSON      *reply;
    int         row;

    if (!section)
        return NULL;
    if (!rmm_cfg_section_known(section))
        return rpc_fail(err, RPC_INVALID_PARAMS, "no section [%s]", section);
    reply = cJSON_CreateObject();

    if (!keyj) { /* the keys the monitor has a schema for, live values */
        cJSON *vals = cJSON_CreateObject();
        for (int i = 0; rmm_cfg[i].key; i++)
            if (!strcmp(rmm_cfg[i].section, section))
                cJSON_AddNumberToObject(vals, rmm_cfg[i].key, rmm_cfg[i].get());
        cJSON_AddItemToObject(reply, "values", vals);
        return reply;
    }
    if (!cJSON_IsString(keyj)) {
        cJSON_Delete(reply);
        return rpc_fail(err, RPC_INVALID_PARAMS, "'key' must be a string");
    }

    if ((row = rmm_cfg_find(section, keyj->valuestring)) >= 0) {
        int pending_idx;
        cJSON_AddNumberToObject(reply, "value", rmm_cfg[row].get());
        thread_wait_mutex(rmm_pending_reset_apply_lock);
        if ((pending_idx = rmm_pending_find(row)) >= 0)
            cJSON_AddNumberToObject(reply, "pending", rmm_pending_reset_apply_list[pending_idx].value);
        thread_release_mutex(rmm_pending_reset_apply_lock);
        return reply;
    }

    /* No schema: the raw string from the in-memory cfg, read only. */
    ini_section_t cat = ini_find_section(config_get_ini(), section);
    if (!cat || !ini_has_entry(cat, keyj->valuestring)) {
        cJSON_Delete(reply);
        return rpc_fail(err, RPC_INVALID_PARAMS, "no key %s/%s", section, keyj->valuestring);
    }
    cJSON_AddStringToObject(reply, "value", ini_section_get_string(cat, keyj->valuestring, ""));
    return reply;
}

static int
rpc_cfg_row(cJSON *params, rmm_rpc_err_t *err)
{
    const char *section = rpc_string(params, "section", err);
    const char *key     = section ? rpc_string(params, "key", err) : NULL;
    int         row;

    if (!key)
        return -1;
    if ((row = rmm_cfg_find(section, key)) < 0)
        rpc_fail(err, RPC_INVALID_PARAMS, "%s/%s has no schema in the monitor yet", section, key);
    return row;
}

/* -- hardware-name keys: config.enums, config.describe, config.load --------------
   Each row is a key, or a numbered run of keys, naming entries of an 86Box table
   filtered as the Settings page filters its combo box. */

enum {
    RMM_ENUM_MACHINE,
    RMM_ENUM_CPU_FAMILY,
    RMM_ENUM_CPU_SPEED,
    RMM_ENUM_FPU,
    RMM_ENUM_WAITSTATES,
    RMM_ENUM_GFXCARD,
    RMM_ENUM_GFXCARD_2,
    RMM_ENUM_SNDCARD,
    RMM_ENUM_MIDI_OUT,
    RMM_ENUM_MIDI_IN,
    RMM_ENUM_FM_DRIVER,
    RMM_ENUM_NETCARD,
    RMM_ENUM_FDC,
    RMM_ENUM_CDROM_IF,
    RMM_ENUM_HDC,
    RMM_ENUM_SCSI,
    RMM_ENUM_ISAMEM,
    RMM_ENUM_MCAMEM,
    RMM_ENUM_ISAROM,
    RMM_ENUM_ISARTC,
    RMM_ENUM_LPT,
    RMM_ENUM_COM,
    RMM_ENUM_KEYBOARD,
    RMM_ENUM_MOUSE,
    RMM_ENUM_TABLET,
    RMM_ENUM_JOYSTICK
};

typedef struct {
    const char *section;
    const char *key;      /* with one %d when the run numbers its keys */
    int         first;    /* the number of the first key */
    int         count;    /* keys in the run */
    int         kind;
    int         internal; /* this key may name the machine's own internal device */
    const char *def;      /* the value when the cfg leaves the key out; NULL: hdc_1's */
} rmm_enum_row_t;

static const rmm_enum_row_t rmm_enum_rows[] = {
  // clang-format off
    { "Machine",             "machine",         0, 1,              RMM_ENUM_MACHINE,    0, ""         },
    { "Machine",             "cpu_family",      0, 1,              RMM_ENUM_CPU_FAMILY, 0, ""         },
    { "Machine",             "cpu_speed",       0, 1,              RMM_ENUM_CPU_SPEED,  0, ""         },
    { "Machine",             "fpu_type",        0, 1,              RMM_ENUM_FPU,        0, "none"     },
    { "Machine",             "cpu_waitstates",  0, 1,              RMM_ENUM_WAITSTATES, 0, "0"        },
    { "Video",               "gfxcard",         0, 1,              RMM_ENUM_GFXCARD,    1, ""         },
    { "Video",               "gfxcard_2",       0, 1,              RMM_ENUM_GFXCARD_2,  0, "none"     },
    { "Sound",               "sndcard",         0, 1,              RMM_ENUM_SNDCARD,    1, "none"     },
    { "Sound",               "sndcard%d",       2, SOUND_CARD_MAX - 1, RMM_ENUM_SNDCARD, 0, "none"   },
    { "Sound",               "midi_device",     0, 1,              RMM_ENUM_MIDI_OUT,   0, "none"     },
    { "Sound",               "midi_in_device",  0, 1,              RMM_ENUM_MIDI_IN,    0, "none"     },
    { "Sound",               "fm_driver",       0, 1,              RMM_ENUM_FM_DRIVER,  0, "nuked"    },
    { "Network",             "net_%02d_card",   1, NET_CARD_MAX,   RMM_ENUM_NETCARD,    0, "none"     },
    { "Storage controllers", "fdc",             0, 1,              RMM_ENUM_FDC,        0, "internal" },
    { "Storage controllers", "cdrom_interface", 0, 1,              RMM_ENUM_CDROM_IF,   0, "none"     },
    { "Storage controllers", "hdc_%d",          1, 1,              RMM_ENUM_HDC,        1, NULL       },
    { "Storage controllers", "hdc_%d",          2, HDC_MAX - 1,    RMM_ENUM_HDC,        0, "none"     },
    { "Storage controllers", "scsicard_%d",     1, SCSI_CARD_MAX,  RMM_ENUM_SCSI,       0, "none"     },
    { "Other peripherals",   "isartc_type",     0, 1,              RMM_ENUM_ISARTC,     0, "none"     },
    { "Other peripherals",   "isamem%d_type",   0, ISAMEM_MAX,     RMM_ENUM_ISAMEM,     0, "none"     },
    { "Other peripherals",   "mcamem%d_type",   0, MCAMEM_MAX,     RMM_ENUM_MCAMEM,     0, "none"     },
    { "Other peripherals",   "isarom%d_type",   0, ISAROM_MAX,     RMM_ENUM_ISAROM,     0, "none"     },
    { "Ports (COM & LPT)",   "lpt%d_device",    1, PARALLEL_MAX,   RMM_ENUM_LPT,        0, "none"     },
    { "Ports (COM & LPT)",   "serial%d_device", 1, SERIAL_MAX - 1, RMM_ENUM_COM,        0, "none"     },
    { "Input devices",       "keyboard_type",   0, 1,              RMM_ENUM_KEYBOARD,   0, ""         },
    { "Input devices",       "mouse_type",      0, 1,              RMM_ENUM_MOUSE,      0, ""         },
    { "Input devices",       "tablet_type",     0, 1,              RMM_ENUM_TABLET,     0, ""         },
    { "Input devices",       "joystick_type",   0, 1,              RMM_ENUM_JOYSTICK,   0, "none"     },
    { NULL }
  // clang-format on
};

/* What the lists depend on: machine, CPU family, CPU, and the two video cards. -1 = unknown or none. */
typedef struct {
    int                 machine_idx;
    const cpu_family_t *family;
    int                 cpu;
    int                 gfx[2];
    int                 speed_bad; /* a load's cpu_speed and cpu_multi match no CPU of the family */
} rmm_enum_ctx_t;

static void
rmm_enum_running(rmm_enum_ctx_t *ctx)
{
    ctx->machine_idx      = machine;
    ctx->family      = cpu_f;
    ctx->cpu    = cpu;
    ctx->gfx[0] = gfxcard[0];
    ctx->gfx[1] = gfxcard[1];
    ctx->speed_bad = 0;
}

static const char *
rmm_enum_keyname(const rmm_enum_row_t *row, int i, char *buf, size_t len)
{
    if (!strchr(row->key, '%'))
        return row->key;
    snprintf(buf, len, row->key, row->first + i);
    return buf;
}

/* The row for section/key, NULL if it is not a hardware-name key. */
static const rmm_enum_row_t *
rmm_enum_find(const char *section, const char *key)
{
    char buf[64];

    for (const rmm_enum_row_t *row = rmm_enum_rows; row->section; row++) {
        if (strcmp(row->section, section))
            continue;
        for (int i = 0; i < row->count; i++)
            if (!strcmp(rmm_enum_keyname(row, i, buf, sizeof(buf)), key))
                return row;
    }
    return NULL;
}

/* What one kind of key knows about its 86Box table. A kind fills only the hooks it needs:
   - name:  table_name(i) for a table that ends at its first unnamed entry, else name().
   - label: label() for a kind with its own labels; the rest label by device name.
   - fits:  fits() for a kind with its own rule; the rest use avail(), the machine's own
            device at entry 1 (internal_flag) and device_is_valid(), per `flags`. */
typedef struct {
    const device_t *(*dev)(int i);
    const char     *(*table_name)(int i);
    const char     *(*name)(const rmm_enum_ctx_t *x, int i, char *buf, size_t len);
    const char     *(*label)(const rmm_enum_ctx_t *x, int i, char *buf, size_t len);
    int             (*avail)(int i);
    int             (*fits)(const rmm_enum_row_t *row, const rmm_enum_ctx_t *x, int i, const char *name, const device_t *d);
    int             (*listed)(int m);
    int             (*precheck)(const rmm_enum_ctx_t *x, const char **why); /* 1: `why` settles config.load's check */
    uintptr_t       internal_flag; /* nonzero: entry 1 is the machine's own device, offered when the machine has this flag */
    unsigned        flags;
    const char     *unknown_msg;   /* NULL: the generic one */
    const char     *nofit_msg;     /* NULL: the generic one */
} rmm_enum_ops_t;

#define RMM_EF_VALID   0x01 /* the device must pass device_is_valid() */
#define RMM_EF_NUMERIC 0x02 /* config.load may give a number */

static int
rmm_enum_waitstates_apply(const rmm_enum_ctx_t *ctx)
{
    uint64_t cpu_type;

    if (!ctx->family || (ctx->cpu < 0))
        return 0;
    cpu_type = ctx->family->cpus[ctx->cpu].cpu_type;
    return (cpu_type >= CPU_286) && (cpu_type < CPU_386DX) && ((ctx->machine_idx < 0) || (machines[ctx->machine_idx].init != machine_at_tandy_vis_init));
}

/* -- names: tables with their own rules ------------------------------------------- */

static const char *
rmm_name_machine(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return machines[i].init ? machine_get_internal_name_ex(i) : NULL;
}

static const char *
rmm_name_cpu_family(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return cpu_families[i].package ? cpu_families[i].internal_name : NULL;
}

static const char *
rmm_name_cpu_speed(const rmm_enum_ctx_t *ctx, int i, char *buf, size_t len)
{
    if (!ctx->family || !ctx->family->cpus[i].cpu_type)
        return NULL;
    snprintf(buf, len, "%" PRIu32, ctx->family->cpus[i].rspeed);
    return buf;
}

static const char *
rmm_name_fpu(const rmm_enum_ctx_t *ctx, int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return (ctx->family && (ctx->cpu >= 0)) ? ctx->family->cpus[ctx->cpu].fpus[i].internal_name : NULL;
}

static const char *
rmm_name_waitstates(const rmm_enum_ctx_t *ctx, int i, char *buf, size_t len)
{
    if (i >= (rmm_enum_waitstates_apply(ctx) ? 9 : 1))
        return NULL;
    snprintf(buf, len, "%d", i);
    return buf;
}

static const char *
rmm_name_fm_driver(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return (i == 0) ? "nuked" : ((i == 1) ? "ymfm" : NULL);
}

static const char *
rmm_name_joystick(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return joystick_get_name(i) ? joystick_get_internal_name(i) : NULL;
}

static const char *
rmm_name_char(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return char_get_device(i) ? char_get_device(i)->internal_name : NULL;
}

/* tables with a size: an unnamed entry inside is skipped, not the end */
static const char *
rmm_name_keyboard(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return (i < keyboard_get_ndev()) ? keyboard_get_internal_name(i) : NULL;
}

static const char *
rmm_name_mouse(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return (i < mouse_get_ndev()) ? mouse_get_internal_name(i) : NULL;
}

static const char *
rmm_name_tablet(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return (i < tablet_get_ndev()) ? tablet_get_internal_name(i) : NULL;
}

/* -- labels: kinds that do not label by device name ------------------------------- */

static const char *
rmm_label_machine(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return machines[i].name;
}

static const char *
rmm_label_cpu_family(UNUSED(const rmm_enum_ctx_t *ctx), int i, char *buf, size_t len)
{
    snprintf(buf, len, "%s %s", cpu_families[i].manufacturer, cpu_families[i].name);
    return buf;
}

static const char *
rmm_label_cpu_speed(const rmm_enum_ctx_t *ctx, int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return ctx->family->cpus[i].name;
}

static const char *
rmm_label_fpu(const rmm_enum_ctx_t *ctx, int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return ctx->family->cpus[ctx->cpu].fpus[i].name;
}

static const char *
rmm_label_waitstates(UNUSED(const rmm_enum_ctx_t *ctx), int i, char *buf, size_t len)
{
    if (i == 0)
        return "Default";
    snprintf(buf, len, "%d wait state%s", i - 1, (i == 2) ? "" : "s");
    return buf;
}

static const char *
rmm_label_fm_driver(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return i ? "YMFM (faster)" : "Nuked (more accurate)";
}

static const char *
rmm_label_joystick(UNUSED(const rmm_enum_ctx_t *ctx), int i, UNUSED(char *buf), UNUSED(size_t len))
{
    return joystick_get_name(i);
}

/* -- fits: kinds with their own rule, as the Settings page decides what its combo offers -- */

static int
rmm_fits_cpu_family(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, UNUSED(const char *name), UNUSED(const device_t *dev))
{
    return cpu_family_is_eligible(&cpu_families[i], ctx->machine_idx);
}

static int
rmm_fits_cpu_speed(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, UNUSED(const char *name), UNUSED(const device_t *dev))
{
    return (ctx->machine_idx < 0) || cpu_is_eligible(ctx->family, i, ctx->machine_idx);
}

static int
rmm_fits_gfxcard(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, UNUSED(const char *name), const device_t *dev)
{
    if ((i == VID_EXTERNAL) || ((i == VID_INTERNAL) && !machine_has_flags(ctx->machine_idx, MACHINE_VIDEO)))
        return 0;
    return video_card_available(i) && device_is_valid(dev, ctx->machine_idx);
}

static int
rmm_fits_gfxcard_2(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, UNUSED(const char *name), const device_t *dev)
{
    const device_t *card = video_card_getdevice(ctx->gfx[0]);

    if (i == VID_NONE)
        return 1;
    if ((video_card_get_flags(ctx->gfx[0]) == VIDEO_FLAG_TYPE_MDA) || (i == VID_INTERNAL))
        return 0;
    if (i == VID_EXTERNAL)
        return machine_has_flags(ctx->machine_idx, MACHINE_VIDEO_EXT) > 0;
    return video_card_available(i) && device_is_valid(dev, ctx->machine_idx) && (video_card_get_flags(i) == VIDEO_FLAG_TYPE_SECONDARY) &&
           !(card && (card->flags & DEVICE_AGP) && dev && (dev->flags & DEVICE_AGP));
}

/* memory expansion: ISA and MCA cards share the code; a NULL device is the "none" entry */
static int
rmm_fits_mem(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, UNUSED(int i), UNUSED(const char *name), const device_t *dev)
{
    return (!dev || device_available(dev)) && device_is_valid(dev, ctx->machine_idx);
}

static int
rmm_fits_char(const rmm_enum_row_t *row, const rmm_enum_ctx_t *ctx, UNUSED(int i), UNUSED(const char *name), const device_t *dev)
{
    return dev && (dev->flags & ((row->kind == RMM_ENUM_LPT) ? DEVICE_LPT : DEVICE_COM)) && device_available(dev) &&
           device_is_valid(dev, ctx->machine_idx);
}

static int
rmm_fits_keyboard(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, const char *name, const device_t *dev)
{
    if ((i == KEYBOARD_TYPE_INTERNAL) != !!machine_has_flags(ctx->machine_idx, MACHINE_KEYBOARD))
        return 0;
    return device_is_valid(dev, ctx->machine_idx) && !(strstr(name, "ps") && (machines[ctx->machine_idx].init == machine_xt_pc5086_init));
}

static int
rmm_fits_mouse(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, UNUSED(const char *name), const device_t *dev)
{
    if ((i == MOUSE_TYPE_INTERNAL) && !machine_has_flags(ctx->machine_idx, MACHINE_MOUSE))
        return 0;
    return device_is_valid(dev, ctx->machine_idx);
}

static int
rmm_fits_tablet(UNUSED(const rmm_enum_row_t *row), const rmm_enum_ctx_t *ctx, int i, const char *name, const device_t *dev)
{
    if ((i == TABLET_TYPE_INTERNAL) && !machine_get_tablet_device(ctx->machine_idx))
        return 0;
    if (!strcmp(name, "cga_lightpen")) { /* a light pen needs a CGA to plug into */
        int pen = 0;
        for (int gfx_slot = 0; gfx_slot < 2; gfx_slot++)
            pen |= !strcmp(video_get_internal_name(ctx->gfx[gfx_slot]), "cga") || !strcmp(video_get_internal_name(ctx->gfx[gfx_slot]), "plantronics");
        if (!pen)
            return 0;
    }
    return device_is_valid(dev, ctx->machine_idx);
}

/* -- listed and precheck ----------------------------------------------------------- */

/* Memory expansion slots take ISA or MCA cards, by the machine's bus. */
static int
rmm_listed_isamem(int machine_idx)
{
    return !machine_has_bus(machine_idx, MACHINE_BUS_MCA);
}

static int
rmm_listed_mcamem(int machine_idx)
{
    return machine_has_bus(machine_idx, MACHINE_BUS_MCA) > 0;
}

static int
rmm_precheck_cpu_speed(const rmm_enum_ctx_t *ctx, const char **why)
{
    *why = (ctx->family && ctx->speed_bad) ? "no CPU of this family runs at this speed and multiplier (see config.enums)" : NULL;
    return 1;
}

static int
rmm_precheck_needs_cpu(const rmm_enum_ctx_t *ctx, const char **why)
{
    *why = NULL; /* the CPU family is refused already */
    return !ctx->family || (ctx->cpu < 0);
}

/* -- the table, indexed by kind ---------------------------------------------------- */

static const rmm_enum_ops_t rmm_enum_ops[] = {
  // clang-format off
    [RMM_ENUM_MACHINE]    = { .name = rmm_name_machine, .label = rmm_label_machine, .avail = machine_available },
    [RMM_ENUM_CPU_FAMILY] = { .name = rmm_name_cpu_family, .label = rmm_label_cpu_family, .fits = rmm_fits_cpu_family,
                              .nofit_msg = "this CPU family does not fit the machine" },
    [RMM_ENUM_CPU_SPEED]  = { .name = rmm_name_cpu_speed, .label = rmm_label_cpu_speed, .fits = rmm_fits_cpu_speed,
                              .precheck = rmm_precheck_cpu_speed, .flags = RMM_EF_NUMERIC },
    [RMM_ENUM_FPU]        = { .name = rmm_name_fpu, .label = rmm_label_fpu, .precheck = rmm_precheck_needs_cpu,
                              .unknown_msg = "this CPU has no such FPU (see config.enums)" },
    [RMM_ENUM_WAITSTATES] = { .name = rmm_name_waitstates, .label = rmm_label_waitstates, .precheck = rmm_precheck_needs_cpu,
                              .flags = RMM_EF_NUMERIC,
                              .unknown_msg = "not a wait-state setting this CPU takes (see config.enums)" },
    [RMM_ENUM_GFXCARD]    = { .dev = video_card_getdevice, .table_name = video_get_internal_name, .fits = rmm_fits_gfxcard },
    [RMM_ENUM_GFXCARD_2]  = { .dev = video_card_getdevice, .table_name = video_get_internal_name, .fits = rmm_fits_gfxcard_2 },
    [RMM_ENUM_SNDCARD]    = { .dev = sound_card_getdevice, .table_name = sound_card_get_internal_name, .avail = sound_card_available,
                              .internal_flag = MACHINE_SOUND, .flags = RMM_EF_VALID },
    [RMM_ENUM_MIDI_OUT]   = { .dev = midi_out_device_getdevice, .table_name = midi_out_device_get_internal_name,
                              .avail = midi_out_device_available },
    [RMM_ENUM_MIDI_IN]    = { .dev = midi_in_device_getdevice, .table_name = midi_in_device_get_internal_name,
                              .avail = midi_in_device_available },
    [RMM_ENUM_FM_DRIVER]  = { .name = rmm_name_fm_driver, .label = rmm_label_fm_driver },
    [RMM_ENUM_NETCARD]    = { .dev = network_card_getdevice, .table_name = network_card_get_internal_name,
                              .avail = network_card_available, .flags = RMM_EF_VALID },
    [RMM_ENUM_FDC]        = { .dev = fdc_card_getdevice, .table_name = fdc_card_get_internal_name,
                              .avail = fdc_card_available, .flags = RMM_EF_VALID },
    [RMM_ENUM_CDROM_IF]   = { .dev = cdrom_interface_get_device, .table_name = cdrom_interface_get_internal_name,
                              .avail = cdrom_interface_available, .flags = RMM_EF_VALID },
    [RMM_ENUM_HDC]        = { .dev = hdc_get_device, .table_name = hdc_get_internal_name, .avail = hdc_available,
                              .internal_flag = MACHINE_HDC, .flags = RMM_EF_VALID },
    [RMM_ENUM_SCSI]       = { .dev = scsi_card_getdevice, .table_name = scsi_card_get_internal_name,
                              .avail = scsi_card_available, .flags = RMM_EF_VALID },
    [RMM_ENUM_ISAMEM]     = { .dev = isamem_get_device, .table_name = isamem_get_internal_name, .fits = rmm_fits_mem,
                              .listed = rmm_listed_isamem },
    [RMM_ENUM_MCAMEM]     = { .dev = mcamem_get_device, .table_name = mcamem_get_internal_name, .fits = rmm_fits_mem,
                              .listed = rmm_listed_mcamem },
    [RMM_ENUM_ISAROM]     = { .dev = isarom_get_device, .table_name = isarom_get_internal_name, .flags = RMM_EF_VALID },
    [RMM_ENUM_ISARTC]     = { .dev = isartc_get_device, .table_name = isartc_get_internal_name, .flags = RMM_EF_VALID },
    [RMM_ENUM_LPT]        = { .dev = char_get_device, .name = rmm_name_char, .fits = rmm_fits_char },
    [RMM_ENUM_COM]        = { .dev = char_get_device, .name = rmm_name_char, .fits = rmm_fits_char },
    [RMM_ENUM_KEYBOARD]   = { .dev = keyboard_get_device, .name = rmm_name_keyboard, .fits = rmm_fits_keyboard },
    [RMM_ENUM_MOUSE]      = { .dev = mouse_get_device, .name = rmm_name_mouse, .fits = rmm_fits_mouse },
    [RMM_ENUM_TABLET]     = { .dev = tablet_get_device, .name = rmm_name_tablet, .fits = rmm_fits_tablet },
    [RMM_ENUM_JOYSTICK]   = { .name = rmm_name_joystick, .label = rmm_label_joystick }
  // clang-format on
};

static inline const rmm_enum_ops_t *
rmm_enum_ops_of(int kind)
{
    return &rmm_enum_ops[kind];
}

static const device_t *
rmm_enum_dev(int kind, int i)
{
    const rmm_enum_ops_t *ops = rmm_enum_ops_of(kind);

    return ops->dev ? ops->dev(i) : NULL;
}

/* Internal name of entry i of the kind's table: NULL past the end, "" to skip.
   buf holds names built on the fly. */
static const char *
rmm_enum_name(int kind, const rmm_enum_ctx_t *ctx, int i, char *buf, size_t len)
{
    const rmm_enum_ops_t *ops = rmm_enum_ops_of(kind);
    const char           *name;

    if (ops->name)
        return ops->name(ctx, i, buf, len);
    /* a table that ends at the first unnamed entry */
    name = ops->table_name(i);
    return (name && name[0]) ? name : NULL;
}

static const char *
rmm_enum_label(int kind, const rmm_enum_ctx_t *ctx, int i, const char *name, char *buf, size_t len)
{
    const rmm_enum_ops_t *ops = rmm_enum_ops_of(kind);
    const device_t       *dev;

    if (ops->label)
        return ops->label(ctx, i, buf, len);
    if (!strcmp(name, "none"))
        return "None";
    if (!strcmp(name, "internal"))
        return "Internal";
    dev = rmm_enum_dev(kind, i);
    return (dev && dev->name) ? dev->name : name;
}

/* Does entry i, named `name`, fit? As the Settings page decides what its combo offers. */
static int
rmm_enum_fits(const rmm_enum_row_t *row, const rmm_enum_ctx_t *ctx, int i, const char *name)
{
    const rmm_enum_ops_t *ops = rmm_enum_ops_of(row->kind);
    const device_t       *dev = rmm_enum_dev(row->kind, i);

    if ((ctx->machine_idx < 0) && (row->kind != RMM_ENUM_CPU_SPEED))
        return 1;
    if (ops->fits)
        return ops->fits(row, ctx, i, name, dev);
    if (ops->internal_flag && (i == 1) && !(row->internal && machine_has_flags(ctx->machine_idx, ops->internal_flag)))
        return 0;
    if (ops->avail && !ops->avail(i))
        return 0;
    return !(ops->flags & RMM_EF_VALID) || device_is_valid(dev, ctx->machine_idx);
}

static const char *
rmm_enum_default(const rmm_enum_row_t *row, const rmm_enum_ctx_t *ctx)
{
    if (row->def)
        return row->def;
    return ((ctx->machine_idx >= 0) && machine_has_flags(ctx->machine_idx, MACHINE_HDC)) ? "internal" : "none"; /* hdc_1 */
}

/* One key as config.enums lists it; the caller adds section and key. */
static cJSON *
rmm_enum_json(const rmm_enum_row_t *row, const char *key, const rmm_enum_ctx_t *ctx)
{
    cJSON        *desc   = cJSON_CreateObject();
    cJSON        *vals   = cJSON_CreateArray();
    cJSON        *labels = cJSON_CreateArray();
    cJSON        *multi  = (row->kind == RMM_ENUM_CPU_SPEED) ? cJSON_CreateArray() : NULL;
    ini_section_t sec    = ini_find_section(config_get_ini(), row->section);
    const char   *cur    = sec ? ini_section_get_string(sec, key, NULL) : NULL;
    char          nb[64];
    char          lb[160];
    const char   *name;

    for (int i = 0; (name = rmm_enum_name(row->kind, ctx, i, nb, sizeof(nb))); i++) {
        if (!name[0] || !rmm_enum_fits(row, ctx, i, name))
            continue;
        cJSON_AddItemToArray(vals, cJSON_CreateString(name));
        cJSON_AddItemToArray(labels, cJSON_CreateString(rmm_enum_label(row->kind, ctx, i, name, lb, sizeof(lb))));
        if (multi)
            cJSON_AddItemToArray(multi, cJSON_CreateNumber(ctx->family->cpus[i].multi));
    }
    cJSON_AddStringToObject(desc, "type", "enum");
    cJSON_AddStringToObject(desc, "applies", "reset");
    cJSON_AddStringToObject(desc, "value", cur ? cur : rmm_enum_default(row, ctx));
    cJSON_AddItemToObject(desc, "values", vals);
    cJSON_AddItemToObject(desc, "labels", labels);
    if (multi)
        cJSON_AddItemToObject(desc, "multi", multi);
    return desc;
}

/* Is the row listed for this machine? */
static int
rmm_enum_listed(const rmm_enum_row_t *row, int machine_idx)
{
    const rmm_enum_ops_t *ops = rmm_enum_ops_of(row->kind);

    return !ops->listed || ops->listed(machine_idx);
}

static cJSON *
rpc_config_enums(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON         *sj   = cJSON_GetObjectItemCaseSensitive(params, "section");
    cJSON         *keys = cJSON_CreateArray();
    cJSON         *reply;
    rmm_enum_ctx_t ctx;
    char           buf[64];

    if (sj && !cJSON_IsString(sj)) {
        cJSON_Delete(keys);
        return rpc_fail(err, RPC_INVALID_PARAMS, "'section' must be a string");
    }
    if (sj && !rmm_cfg_section_known(sj->valuestring)) {
        int known = 0;
        for (const rmm_enum_row_t *row = rmm_enum_rows; row->section; row++)
            known |= !strcmp(row->section, sj->valuestring);
        if (!known) {
            cJSON_Delete(keys);
            return rpc_fail(err, RPC_INVALID_PARAMS, "no section [%s]", sj->valuestring);
        }
    }
    config_save(); /* the cfg as the running machine has it, keys at their default left out */
    rmm_enum_running(&ctx);
    for (const rmm_enum_row_t *row = rmm_enum_rows; row->section; row++) {
        if ((sj && strcmp(sj->valuestring, row->section)) || !rmm_enum_listed(row, ctx.machine_idx))
            continue;
        for (int i = 0; i < row->count; i++) {
            const char *key   = rmm_enum_keyname(row, i, buf, sizeof(buf));
            cJSON      *entry = cJSON_CreateObject();

            cJSON_AddStringToObject(entry, "section", row->section);
            cJSON_AddStringToObject(entry, "key", key);
            for (cJSON *enum_json = rmm_enum_json(row, key, &ctx), *item = enum_json->child; item; item = enum_json->child)
                cJSON_AddItemToObject(entry, item->string, cJSON_DetachItemViaPointer(enum_json, item));
            cJSON_AddItemToArray(keys, entry);
        }
    }
    reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "keys", keys);
    return reply;
}

/* Does v fit the row? NULL = yes, else why not. */
static const char *
rmm_enum_check(const rmm_enum_row_t *row, const rmm_enum_ctx_t *ctx, const cJSON *value)
{
    const rmm_enum_ops_t *ops = rmm_enum_ops_of(row->kind);
    char                  vb[64];
    char                  nb[64];
    const char           *val;
    const char           *name;
    const char           *why;
    int                   i;
    int                   found;

    if (cJSON_IsString(value))
        val = value->valuestring;
    else if (cJSON_IsNumber(value) && (ops->flags & RMM_EF_NUMERIC)) {
        snprintf(vb, sizeof(vb), "%.0f", value->valuedouble);
        val = vb;
    } else
        return "must be a string (an internal name)";

    if (ops->precheck && ops->precheck(ctx, &why))
        return why;
    /* a name can appear twice in a table: any fitting entry will do */
    for (i = 0, found = 0; (name = rmm_enum_name(row->kind, ctx, i, nb, sizeof(nb))); i++)
        if (name[0] && !strcmp(name, val)) {
            if (rmm_enum_fits(row, ctx, i, name))
                return NULL;
            found = 1;
        }
    if (!found)
        return ops->unknown_msg ? ops->unknown_msg : "no such name in 86Box's table (see config.enums)";
    return ops->nofit_msg ? ops->nofit_msg : "does not fit this machine (see config.enums)";
}

/* Index of `name` in the kind's table, -1 if none. */
static int
rmm_enum_index(int kind, const rmm_enum_ctx_t *ctx, const char *name)
{
    char        nb[64];
    const char *entry_name;

    for (int i = 0; (entry_name = rmm_enum_name(kind, ctx, i, nb, sizeof(nb))); i++)
        if (entry_name[0] && !strcmp(entry_name, name))
            return i;
    return -1;
}

/* The load's [Machine] and [Video] decide what the other keys may name. */
static void
rmm_enum_load_ctx(rmm_enum_ctx_t *ctx, cJSON *cfg, int machine_idx)
{
    cJSON *mc  = cJSON_GetObjectItemCaseSensitive(cfg, "Machine");
    cJSON *vc  = cJSON_GetObjectItemCaseSensitive(cfg, "Video");
    cJSON *fj  = cJSON_GetObjectItemCaseSensitive(mc, "cpu_family");
    cJSON *sj  = cJSON_GetObjectItemCaseSensitive(mc, "cpu_speed");
    cJSON *mj  = cJSON_GetObjectItemCaseSensitive(mc, "cpu_multi");
    double sp  = cJSON_IsNumber(sj) ? sj->valuedouble : (cJSON_IsString(sj) ? atof(sj->valuestring) : -1);
    double mul = cJSON_IsNumber(mj) ? mj->valuedouble : (cJSON_IsString(mj) ? atof(mj->valuestring) : -1);

    rmm_enum_running(ctx);
    ctx->machine_idx = machine_idx;
    if (cJSON_IsString(fj)) {
        int family_idx = rmm_enum_index(RMM_ENUM_CPU_FAMILY, ctx, fj->valuestring);
        ctx->family  = ((family_idx >= 0) && ((machine_idx < 0) || cpu_family_is_eligible(&cpu_families[family_idx], machine_idx))) ? &cpu_families[family_idx] : NULL;
    } else if ((machine_idx >= 0) && (machine_idx != machine) && !cpu_family_is_eligible(cpu_f, machine_idx))
        ctx->family = NULL;
    if (ctx->family && ((sp >= 0) || (ctx->family != cpu_f))) {
        /* the CPU config.c will pick: exact speed and multiplier, else the closest faster,
           else the fastest eligible */
        int exact = -1, closest = -1, fastest = -1;

        for (int i = 0; ctx->family->cpus[i].cpu_type; i++) {
            if ((machine_idx >= 0) && !cpu_is_eligible(ctx->family, i, machine_idx))
                continue;
            if ((sp < 0) || ((ctx->family->cpus[i].rspeed == (uint32_t) sp) && ((mul < 0) || (ctx->family->cpus[i].multi == mul)))) {
                exact = i;
                break;
            }
            if ((closest < 0) && (ctx->family->cpus[i].rspeed >= (uint32_t) sp))
                closest = i;
            fastest = i;
        }
        ctx->speed_bad = (sp >= 0) && (exact < 0);
        ctx->cpu       = (exact >= 0) ? exact : ((closest >= 0) ? closest : fastest);
    }
    for (int gfx_slot = 0; gfx_slot < 2; gfx_slot++) {
        cJSON *gj = cJSON_GetObjectItemCaseSensitive(vc, gfx_slot ? "gfxcard_2" : "gfxcard");
        if (cJSON_IsString(gj) && (rmm_enum_index(RMM_ENUM_GFXCARD, ctx, gj->valuestring) >= 0))
            ctx->gfx[gfx_slot] = rmm_enum_index(RMM_ENUM_GFXCARD, ctx, gj->valuestring);
    }
}

static cJSON *
rpc_config_describe(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON                *sj = cJSON_GetObjectItemCaseSensitive(params, "section");
    cJSON                *kj = cJSON_GetObjectItemCaseSensitive(params, "key");
    const rmm_enum_row_t *row;
    int                   rw;
    cJSON                *reply;

    if (cJSON_IsString(sj) && cJSON_IsString(kj) && (row = rmm_enum_find(sj->valuestring, kj->valuestring))) {
        rmm_enum_ctx_t ctx;

        config_save();
        rmm_enum_running(&ctx);
        return rmm_enum_json(row, kj->valuestring, &ctx);
    }
    if ((rw = rpc_cfg_row(params, err)) < 0)
        return NULL;
    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "int");
    cJSON_AddStringToObject(reply, "applies", rmm_cfg[rw].on_reset ? "reset" : "now");
    cJSON_AddNumberToObject(reply, "min", rmm_cfg[rw].min());
    cJSON_AddNumberToObject(reply, "max", rmm_cfg[rw].max());
    cJSON_AddNumberToObject(reply, "step", rmm_cfg[rw].step());
    cJSON_AddNumberToObject(reply, "default", rmm_cfg[rw].def);
    return reply;
}

static cJSON *
rpc_config_set(cJSON *params, rmm_rpc_err_t *err)
{
    int    row = rpc_cfg_row(params, err);
    cJSON *value_item;
    cJSON *reply;
    int    val;
    int    staged = 0;

    if (row < 0)
        return NULL;
    value_item = cJSON_GetObjectItemCaseSensitive(params, "value");
    if (!cJSON_IsNumber(value_item) || (value_item->valuedouble != (double) (int) value_item->valuedouble))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'value' must be an integer");
    val = (int) value_item->valuedouble;
    if ((val < rmm_cfg[row].min()) || (val > rmm_cfg[row].max()) ||
        ((val - rmm_cfg[row].min()) % rmm_cfg[row].step()))
        return rpc_fail(err, RPC_INVALID_PARAMS, "%s must be %d..%d in steps of %d", rmm_cfg[row].key,
                        rmm_cfg[row].min(), rmm_cfg[row].max(), rmm_cfg[row].step());

    if (!rmm_cfg[row].on_reset) {
        rmm_cfg[row].set(val);
        config_save();
    } else {
        int pending_idx;

        thread_wait_mutex(rmm_pending_reset_apply_lock);
        pending_idx = rmm_pending_find(row);
        if (val == rmm_cfg[row].get()) { /* the running value: unstage */
            if (pending_idx >= 0)
                rmm_pending_reset_apply_list[pending_idx] = rmm_pending_reset_apply_list[--rmm_pending_reset_apply_count];
        } else if (pending_idx >= 0) {
            rmm_pending_reset_apply_list[pending_idx].value = val;
            staged = 1;
        } else if (rmm_pending_reset_apply_count < RMM_PENDING_MAX) {
            rmm_pending_reset_apply_list[rmm_pending_reset_apply_count].cfg   = row;
            rmm_pending_reset_apply_list[rmm_pending_reset_apply_count].value = val;
            rmm_pending_reset_apply_count++;
            staged = 1;
        }
        thread_release_mutex(rmm_pending_reset_apply_lock);
    }
    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "applies", rmm_cfg[row].on_reset ? "reset" : "now");
    cJSON_AddBoolToObject(reply, "pending", staged);
    return reply;
}

static cJSON *
rpc_config_pending(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = cJSON_CreateObject();

    thread_wait_mutex(rmm_pending_reset_apply_lock);
    cJSON_AddItemToObject(reply, "pending", rmm_pending_json(rmm_pending_reset_apply_list, rmm_pending_reset_apply_count));
    /* a staged config.load, as the cfg it will write: null when none */
    cJSON_AddItemToObject(reply, "load", rmm_load_cfg ? cJSON_Duplicate(rmm_load_cfg, 1) : cJSON_CreateNull());
    thread_release_mutex(rmm_pending_reset_apply_lock);
    return reply;
}

static cJSON *
rpc_config_discard(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = cJSON_CreateObject();

    thread_wait_mutex(rmm_pending_reset_apply_lock);
    cJSON_AddNumberToObject(reply, "discarded", rmm_pending_reset_apply_count + (rmm_load_cfg ? 1 : 0));
    rmm_pending_reset_apply_count = 0;
    cJSON_Delete(rmm_load_cfg);
    rmm_load_cfg = NULL;
    thread_release_mutex(rmm_pending_reset_apply_lock);
    return reply;
}

/* -- config.dump / config.load: the whole VM cfg --------------------------------
   config.dump returns the running cfg. config.load stages one for the next hard reset;
   failed checks refuse the whole load with error 10. See releasenotes.txt. */

typedef struct {
    cJSON      *root;
    const char *only;
} rmm_dump_t;

static void
rmm_dump_entry(const char *section, const char *key, const char *value, void *priv)
{
    rmm_dump_t *dump = (rmm_dump_t *) priv;
    cJSON      *section_json;

    if (dump->only && strcmp(dump->only, section))
        return;
    if (!(section_json = cJSON_GetObjectItemCaseSensitive(dump->root, section)))
        section_json = cJSON_AddObjectToObject(dump->root, section);
    cJSON_AddStringToObject(section_json, key, value);
}

static cJSON *
rpc_config_dump(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON     *sj = cJSON_GetObjectItemCaseSensitive(params, "section");
    cJSON     *reply;
    rmm_dump_t dump;

    if (sj && !cJSON_IsString(sj))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'section' must be a string");
    config_save();
    dump.root = cJSON_CreateObject();
    dump.only = sj ? sj->valuestring : NULL;
    config_rmm_foreach(rmm_dump_entry, &dump);
    if (dump.only && !dump.root->child) {
        cJSON_Delete(dump.root);
        return rpc_fail(err, RPC_INVALID_PARAMS, "no section [%s]", dump.only);
    }
    reply = cJSON_CreateObject();
    cJSON_AddItemToObject(reply, "cfg", dump.root);
    cJSON_AddStringToObject(reply, "path", cfg_path);
    return reply;
}

static void
rmm_load_error(cJSON *errors, const char *section, const char *key, const cJSON *value, const char *why)
{
    cJSON *error = cJSON_CreateObject();

    cJSON_AddStringToObject(error, "section", section);
    cJSON_AddStringToObject(error, "key", key);
    cJSON_AddItemToObject(error, "value", cJSON_Duplicate(value, 1));
    cJSON_AddStringToObject(error, "why", why);
    cJSON_AddItemToArray(errors, error);
}

static int
rmm_is_image_key(const char *section, const char *key)
{
    size_t len = strlen(key);

    if (strcmp(section, "Hard disks") && strcmp(section, "Floppy and CD-ROM drives") &&
        strcmp(section, "Other removable devices"))
        return 0;
    return ((len > 3) && !strcmp(key + len - 3, "_fn")) || ((len > 11) && !strcmp(key + len - 11, "_image_path"));
}

/* Why one cfg value is refused, or NULL. Keys nothing can check are noted in 'unchecked'. */
static const char *
rmm_load_check_value(const char *sec, const char *key, const cJSON *kv, int machine_idx, rmm_enum_ctx_t *ctx, cJSON *unchecked)
{
    const rmm_enum_row_t *row = rmm_enum_find(sec, key);

    if (!cJSON_IsString(kv) && !cJSON_IsNumber(kv) && !cJSON_IsBool(kv) && !cJSON_IsNull(kv))
        return "a value must be a string, number, bool or null";
    if (!strcmp(sec, "General") && (!strncmp(key, "rmm_", 4) || !strcmp(key, "uuid")))
        return "the monitor's own settings and the VM uuid cannot be loaded";
    if (cJSON_IsNull(kv))
        return NULL; /* a delete */
    if (row && (row->kind == RMM_ENUM_MACHINE)) {
        if (!cJSON_IsString(kv))
            return "must be a string (an internal name)";
        if (machine_idx < 0)
            return "no such machine";
        if (!machine_available(machine_idx))
            return "86Box has no ROM for this machine";
        return NULL;
    }
    if (row)
        return rmm_enum_check(row, ctx, kv);
    if (!strcmp(sec, "Machine") && !strcmp(key, "mem_size")) {
        int ram_mb = cJSON_IsNumber(kv) ? (int) kv->valuedouble : (cJSON_IsString(kv) ? atoi(kv->valuestring) : -1);

        if ((machine_idx >= 0) && ((ram_mb < machine_get_min_ram(machine_idx)) || (ram_mb > machine_get_max_ram(machine_idx)) ||
                         ((ram_mb - machine_get_min_ram(machine_idx)) % machine_get_ram_granularity(machine_idx))))
            return "outside this machine's RAM range or step (config.describe Machine mem_size)";
        return NULL;
    }
    if (rmm_is_image_key(sec, key) && cJSON_IsString(kv) && kv->valuestring[0]) {
        FILE *file = plat_fopen(kv->valuestring, "rb");

        if (!file)
            return "image file not found";
        fclose(file);
        return NULL;
    }
    cJSON *unchecked_entry = cJSON_CreateObject();
    cJSON_AddStringToObject(unchecked_entry, "section", sec);
    cJSON_AddStringToObject(unchecked_entry, "key", key);
    cJSON_AddItemToArray(unchecked, unchecked_entry);
    return NULL;
}

/* Checks every key of cfg; returns how many there were. */
static int
rmm_load_check_cfg(const cJSON *cfg, int machine_idx, rmm_enum_ctx_t *ctx, cJSON *errors, cJSON *unchecked)
{
    int keys = 0;

    for (cJSON *section = cfg->child; section; section = section->next) {
        if (!cJSON_IsObject(section)) {
            rmm_load_error(errors, section->string, "", section, "a section must be an object of key: value");
            continue;
        }
        for (cJSON *kv = section->child; kv; kv = kv->next, keys++) {
            const char *why = rmm_load_check_value(section->string, kv->string, kv, machine_idx, ctx, unchecked);

            if (why)
                rmm_load_error(errors, section->string, kv->string, kv, why);
        }
    }
    return keys;
}

/* Hands cfg to the reset path, which applies it. */
static void
rmm_load_stage(const cJSON *cfg, int replace)
{
    thread_wait_mutex(rmm_pending_reset_apply_lock);
    cJSON_Delete(rmm_load_cfg);
    rmm_load_cfg     = cJSON_Duplicate(cfg, 1);
    rmm_load_replace = replace;
    thread_release_mutex(rmm_pending_reset_apply_lock);
}

static cJSON *
rpc_config_load(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON         *cfg         = cJSON_GetObjectItemCaseSensitive(params, "cfg");
    cJSON         *repj        = cJSON_GetObjectItemCaseSensitive(params, "replace");
    cJSON         *valj        = cJSON_GetObjectItemCaseSensitive(params, "validate_only");
    cJSON         *errors      = cJSON_CreateArray();
    cJSON         *unchecked   = cJSON_CreateArray();
    cJSON         *mj          = NULL;
    cJSON         *reply;
    int            machine_idx = machine;
    int            keys;
    int            stage       = !valj || !cJSON_IsTrue(valj);
    rmm_enum_ctx_t ctx;

    if (!cJSON_IsObject(cfg) || (repj && !cJSON_IsBool(repj)) || (valj && !cJSON_IsBool(valj))) {
        cJSON_Delete(errors);
        cJSON_Delete(unchecked);
        return rpc_fail(err, RPC_INVALID_PARAMS, "want {cfg: {section: {key: value}}, replace?: bool, validate_only?: bool}");
    }

    /* the machine being loaded decides what else fits, and its CPU and video cards some of it */
    if (cJSON_GetObjectItemCaseSensitive(cfg, "Machine"))
        mj = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cfg, "Machine"), "machine");
    rmm_enum_running(&ctx);
    if (cJSON_IsString(mj))
        machine_idx = rmm_enum_index(RMM_ENUM_MACHINE, &ctx, mj->valuestring);
    rmm_enum_load_ctx(&ctx, cfg, machine_idx);

    keys = rmm_load_check_cfg(cfg, machine_idx, &ctx, errors, unchecked);
    /* a new machine keeps the running CPU only if it fits */
    if ((machine_idx >= 0) && (machine_idx != machine) && !cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cfg, "Machine"), "cpu_family") &&
        !cpu_family_is_eligible(cpu_f, machine_idx))
        rmm_load_error(errors, "Machine", "cpu_family", mj, "the running CPU family does not fit the new machine; give cpu_family");

    if (cJSON_GetArraySize(errors)) {
        cJSON_Delete(unchecked);
        err->data = cJSON_CreateObject();
        cJSON_AddItemToObject(err->data, "errors", errors);
        return rpc_fail(err, RMM_E_BAD_CONFIG, "config refused: %d bad key(s)", cJSON_GetArraySize(err->data->child));
    }
    cJSON_Delete(errors);

    if (stage)
        rmm_load_stage(cfg, !repj || cJSON_IsTrue(repj));
    reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "staged", stage);
    cJSON_AddStringToObject(reply, "applies", "reset");
    cJSON_AddNumberToObject(reply, "keys", keys);
    cJSON_AddItemToObject(reply, "unchecked", unchecked);
    return reply;
}

/* -- capture.screenshot {step?, wait_ms?} ---------------------------------------
   Monitor 0's frame as base64 RGB24, copied on the blitter thread. fresh=false: no
   new frame within wait_ms, the last one is returned. */

static const char rmm_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
rmm_base64(const uint8_t *in, size_t len)
{
    char  *out  = malloc(((len + 2) / 3) * 4 + 1);
    char  *outp = out;
    size_t i;

    if (!out)
        return NULL;
    for (i = 0; (i + 2) < len; i += 3) {
        uint32_t group = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8) | in[i + 2];
        *outp++       = rmm_b64[group >> 18];
        *outp++       = rmm_b64[(group >> 12) & 63];
        *outp++       = rmm_b64[(group >> 6) & 63];
        *outp++       = rmm_b64[group & 63];
    }
    if (i < len) {
        uint32_t group = (uint32_t) in[i] << 16;
        if ((i + 1) < len)
            group |= (uint32_t) in[i + 1] << 8;
        *outp++ = rmm_b64[group >> 18];
        *outp++ = rmm_b64[(group >> 12) & 63];
        *outp++ = ((i + 1) < len) ? rmm_b64[(group >> 6) & 63] : '=';
        *outp++ = '=';
    }
    *outp = '\0';
    return out;
}

/* Every step-th pixel of the frame as packed RGB24; returns the byte count. */
static size_t
rmm_frame_rgb24(const uint32_t *px, int width, int height, int step, uint8_t *out)
{
    const uint8_t *start = out;

    for (int row = 0; row < height; row += step)
        for (int col = 0; col < width; col += step) {
            const uint32_t pixel = px[(row * width) + col];

            *out++ = (pixel >> 16) & 0xff;
            *out++ = (pixel >> 8) & 0xff;
            *out++ = pixel & 0xff;
        }
    return (size_t) (out - start);
}

static cJSON *
rpc_capture_screenshot(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t        step = 1;
    uint32_t        wait = 200;
    uint32_t        seq;
    const uint32_t *px;
    int             width;
    int             height;
    int             fresh;
    int             ow;
    int             oh;
    uint8_t        *rgb;
    char           *b64;
    cJSON          *reply;

    if (!rpc_uint(params, "step", 8.0, &step, 0, err) || !rpc_uint(params, "wait_ms", 2000.0, &wait, 0, err))
        return NULL;
    if (step < 1)
        return rpc_fail(err, RPC_INVALID_PARAMS, "'step' must be 1..8");
    if (!video_rmm_capture(&px, &width, &height, &seq, &fresh, (int) wait))
        return rpc_fail(err, RMM_E_BUSY, "no frame has been drawn yet");
    ow  = (width + (int) step - 1) / (int) step;
    oh  = (height + (int) step - 1) / (int) step;
    rgb = malloc((size_t) ow * oh * 3);
    if (!rgb) {
        video_rmm_capture_release();
        return rpc_fail(err, -32603, "out of memory");
    }
    rmm_frame_rgb24(px, width, height, (int) step, rgb);
    video_rmm_capture_release();
    b64 = rmm_base64(rgb, (size_t) ow * oh * 3);
    free(rgb);
    if (!b64)
        return rpc_fail(err, -32603, "out of memory");
    reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "w", ow);
    cJSON_AddNumberToObject(reply, "h", oh);
    cJSON_AddNumberToObject(reply, "src_w", width);
    cJSON_AddNumberToObject(reply, "src_h", height);
    cJSON_AddNumberToObject(reply, "step", step);
    cJSON_AddStringToObject(reply, "format", "rgb24");
    cJSON_AddNumberToObject(reply, "seq", seq);
    cJSON_AddBoolToObject(reply, "fresh", fresh);
    cJSON_AddStringToObject(reply, "data", b64);
    free(b64);
    return reply;
}

/* -- io.read / io.write ---------------------------------------------------------- */

/* {port, size?, value for writes}. Paused only. */
static int
rpc_io_args(cJSON *params, int write, uint32_t *port, uint32_t *size, uint32_t *value, rmm_rpc_err_t *err)
{
    *size = 1;
    if (!rpc_need_paused(err) || !rpc_uint(params, "port", 65535.0, port, 1, err) ||
        !rpc_uint(params, "size", 4.0, size, 0, err))
        return 0;
    if ((*size != 1) && (*size != 2) && (*size != 4)) {
        rpc_fail(err, RPC_INVALID_PARAMS, "'size' must be 1, 2 or 4");
        return 0;
    }
    if ((*port + *size - 1) > 0xffff) {
        rpc_fail(err, RPC_INVALID_PARAMS, "port + size runs past 0xFFFF");
        return 0;
    }
    if (write && !rpc_uint(params, "value", (*size == 4) ? 4294967295.0 : (double) ((1u << (8 * *size)) - 1), value, 1, err))
        return 0;
    return 1;
}

static cJSON *
rpc_io_reply(uint32_t port, uint32_t size, uint32_t value)
{
    cJSON *reply = cJSON_CreateObject();

    cJSON_AddNumberToObject(reply, "port", port);
    cJSON_AddNumberToObject(reply, "size", size);
    cJSON_AddNumberToObject(reply, "value", value);
    return reply;
}

/* inb/outb charge unclaimed-port delay and I/O trap notes to the guest; both are undone. */
static cJSON *
rpc_io_access(cJSON *params, int write, rmm_rpc_err_t *err)
{
    uint32_t port;
    uint32_t size;
    uint32_t value = 0;
    int      old_cycles;
    uint16_t old_port;

    if (!rpc_io_args(params, write, &port, &size, &value, err))
        return NULL;
    old_cycles = cycles;
    old_port   = io_port;
    if (write) {
        if (size == 1)
            outb(port, value);
        else if (size == 2)
            outw(port, value);
        else
            outl(port, value);
    } else
        value = (size == 1) ? inb(port) : (size == 2) ? inw(port) : inl(port);
    cycles  = old_cycles;
    io_port = old_port;
    return rpc_io_reply(port, size, value);
}

static cJSON *
rpc_io_read(cJSON *params, rmm_rpc_err_t *err)
{
    return rpc_io_access(params, 0, err);
}

static cJSON *
rpc_io_write(cJSON *params, rmm_rpc_err_t *err)
{
    return rpc_io_access(params, 1, err);
}

/* mem.info: the RAM size the machine was last reset with. */
static cJSON *
rpc_mem_info(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *reply = cJSON_CreateObject();

    cJSON_AddNumberToObject(reply, "ram_kb", mem_size);
    return reply;
}

/* machine.reset {hard=true}: the Hard Reset menu without its dialog. Waits for the
   CPU thread to apply the reset. */
static cJSON *
rpc_machine_reset(cJSON *params, rmm_rpc_err_t *err)
{
    cJSON    *hard   = cJSON_GetObjectItemCaseSensitive(params, "hard");
    const int before = rmm_hard_resets;
    cJSON    *ev;

    if (hard && !cJSON_IsBool(hard))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'hard' must be a boolean");
    if (hard && !cJSON_IsTrue(hard))
        return rpc_fail(err, RPC_INVALID_PARAMS, "only a hard reset is implemented");
    pc_reset_hard();
    /* hard_reset_pending is cleared before the reset runs; wait for the end of it */
    for (int i = 0; (rmm_hard_resets == before) && (i < 1000); i++)
        plat_delay_ms(10);
    if (rmm_hard_resets == before)
        return rpc_fail(err, -32603, "reset was not applied within 10 s");
    ev = cJSON_CreateObject();
    cJSON_AddBoolToObject(ev, "hard", 1);
    rmm_event("machine.reset", ev);

    cJSON *reply = rpc_state();
    thread_wait_mutex(rmm_pending_reset_apply_lock);
    cJSON_AddItemToObject(reply, "config_applied",
                          rmm_pending_json(rmm_pending_reset_apply_done, rmm_pending_reset_apply_done_count));
    rmm_pending_reset_apply_done_count = 0;
    /* keys a staged config.load wrote, null when there was none */
    if (rmm_load_done >= 0)
        cJSON_AddNumberToObject(reply, "config_loaded", rmm_load_done);
    else
        cJSON_AddNullToObject(reply, "config_loaded");
    rmm_load_done = -1;
    thread_release_mutex(rmm_pending_reset_apply_lock);
    return reply;
}

static cJSON *
rpc_machine_cad(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    pc_send_cad();
    return cJSON_CreateObject();
}

static cJSON *
rpc_machine_cae(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    pc_send_cae();
    return cJSON_CreateObject();
}

static cJSON *
rpc_machine_nmi(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    nmi_raise();
    return cJSON_CreateObject();
}

/* The soft power button. Never exits 86Box; that is machine.quit. */
static cJSON *
rpc_machine_acpi_shutdown(UNUSED(cJSON *params), rmm_rpc_err_t *err)
{
    if (acpi_enabled) {
        acpi_pwrbut_pressed = 1;
        return cJSON_CreateObject();
    }
    if (device_has_power_button()) {
        const int was_paused = dopause;

        rmm_set_pause(1); /* device state changes need a quiescent CPU, as in the menu */
        device_power_button();
        rmm_set_pause(was_paused);
        return cJSON_CreateObject();
    }
    return rpc_fail(err, RMM_E_BUSY, "this machine has no ACPI or soft power button");
}

static cJSON *
rpc_machine_quit(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    quitting = 1; /* rmm_json powers off once the reply is out */
    return cJSON_CreateObject();
}

/* -- get/set properties: <name>.get, <name>.set {value}. Set triggers the menu QAction
   when the value differs. */

static int prop_fast_forward(void) { return !!fast_forward; }
static int prop_force_interp(void) { return !!cpu_force_interpreter; }
static int prop_sound_muted(void) { return !!sound_muted; }

static const struct {
    const char *name;
    int       (*get)(void);
    const char *action;
    const char *doc;
} rmm_props[] = {
    { "machine.fast_forward",  prop_fast_forward, "actionFast_forward",         "fast-forward on/off" },
    { "cpu.force_interpreter", prop_force_interp, "actionForce_interpretation", "force the interpreter (no dynarec) on/off" },
    { "sound.mute",            prop_sound_muted,  "actionMute_Unmute",          "mute on/off" },
    { NULL,                    NULL,              NULL,                         NULL }
};

static cJSON *
rpc_prop(int idx, int set, cJSON *params, rmm_rpc_err_t *err)
{
    cJSON *reply;

    if (set) {
        cJSON *value_item = cJSON_GetObjectItemCaseSensitive(params, "value");

        if (!cJSON_IsBool(value_item))
            return rpc_fail(err, RPC_INVALID_PARAMS, "'value' must be a boolean");
        if (rmm_props[idx].get() != cJSON_IsTrue(value_item)) {
            int trigger_result = plat_rmm_trigger_action(rmm_props[idx].action);
            if (trigger_result < 0)
                return rpc_fail(err, RMM_E_BUSY, "%s is disabled on this machine", rmm_props[idx].name);
            if (trigger_result == 0)
                return rpc_fail(err, -32603, "UI action %s not found", rmm_props[idx].action);
        }
    }
    reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "value", rmm_props[idx].get());
    return reply;
}

/* -- removable media: media.list and <dev>.<slot>.insert|eject --------------------
   Runs the media menu code on the UI thread via plat_rmm_media. */

#define RMM_DEV_FDD   0
#define RMM_DEV_CDROM 1

static const char *rmm_media_devs[] = { "fdd", "cdrom", NULL };

static int
rmm_media_dev(const char *name)
{
    for (int i = 0; rmm_media_devs[i]; i++)
        if (!strcmp(rmm_media_devs[i], name))
            return i;
    return -1;
}

static int
rmm_media_has_slot(int dev, int slot)
{
    int slots[16];
    int nslots = plat_rmm_media_slots(dev, slots, 16);

    for (int i = 0; i < nslots; i++)
        if (slots[i] == slot)
            return 1;
    return 0;
}

/* Current image without the wp:// prefix; "" if empty. */
static const char *
rmm_media_path(int dev, int slot, int *wp)
{
    const char *path;

    if (dev == RMM_DEV_FDD) {
        path   = drives[slot].image_path;
        *wp = !!drives[slot].read_only;
        if (!strncmp(path, "wp://", 5))
            path += 5;
        return drives[slot].empty ? "" : path;
    }
    *wp = 1;
    return cdrom_is_empty((uint8_t) slot) ? "" : cdrom[slot].image_path;
}

static cJSON *
rpc_media_list(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *list  = cJSON_CreateArray();
    cJSON *reply = cJSON_CreateObject();

    for (int dev = 0; rmm_media_devs[dev]; dev++) {
        int slots[16];
        int nslots = plat_rmm_media_slots(dev, slots, 16);

        for (int i = 0; i < nslots; i++) {
            cJSON      *entry = cJSON_CreateObject();
            int         wp;
            const char *path = rmm_media_path(dev, slots[i], &wp);

            cJSON_AddStringToObject(entry, "dev", rmm_media_devs[dev]);
            cJSON_AddNumberToObject(entry, "slot", slots[i]);
            cJSON_AddBoolToObject(entry, "empty", !*path);
            cJSON_AddStringToObject(entry, "path", path);
            cJSON_AddBoolToObject(entry, "wp", wp);
            cJSON_AddItemToArray(list, entry);
        }
    }
    cJSON_AddItemToObject(reply, "slots", list);
    return reply;
}

static void
rmm_media_event(int dev, int slot, const char *path)
{
    cJSON *ev = cJSON_CreateObject();

    cJSON_AddStringToObject(ev, "dev", rmm_media_devs[dev]);
    cJSON_AddNumberToObject(ev, "slot", slot);
    cJSON_AddStringToObject(ev, "path", path);
    rmm_event("media.changed", ev);
}

static cJSON *
rpc_media_call(int dev, int slot, const char *verb, cJSON *params, rmm_rpc_err_t *err)
{
    int wp;

    if (!rmm_media_has_slot(dev, slot))
        return rpc_fail(err, RMM_E_NO_SLOT, "no %s drive %d on this machine", rmm_media_devs[dev], slot);

    if (!strcmp(verb, "eject")) {
        if (!*rmm_media_path(dev, slot, &wp))
            return rpc_fail(err, RMM_E_EMPTY, "%s %d is empty", rmm_media_devs[dev], slot);
        plat_rmm_media(dev, slot, NULL, 0);
        rmm_media_event(dev, slot, "");
        return cJSON_CreateObject();
    }

    /* insert {path, wp=false} */
    const char *path = rpc_string(params, "path", err);
    cJSON      *wpj  = cJSON_GetObjectItemCaseSensitive(params, "wp");
    FILE       *file;

    if (!path)
        return NULL;
    if (wpj && !cJSON_IsBool(wpj))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'wp' must be a boolean");
    if (!(file = fopen(path, "rb")))
        return rpc_fail(err, RPC_INVALID_PARAMS, "cannot open '%s'", path);
    fclose(file);
    plat_rmm_media(dev, slot, path, cJSON_IsTrue(wpj));
    rmm_media_event(dev, slot, path);
    return cJSON_CreateObject();
}

/* "fdd.1.eject" -> dev, slot, verb; 0 if the name is not a media call. */
static int
rmm_media_parse(const char *method, int *dev, int *slot, const char **verb)
{
    char        name[16];
    const char *dot = strchr(method, '.');
    char       *end;
    long        slot_num;

    if (!dot || (dot - method) >= (long) sizeof(name))
        return 0;
    memcpy(name, method, dot - method);
    name[dot - method] = '\0';
    if ((*dev = rmm_media_dev(name)) < 0)
        return 0;
    slot_num = strtol(dot + 1, &end, 10);
    if ((end == dot + 1) || (*end != '.') || (slot_num < 0) || (slot_num > 255))
        return 0;
    *slot = (int) slot_num;
    *verb = end + 1;
    return !strcmp(*verb, "insert") || !strcmp(*verb, "eject");
}

/* -- debug.break.set/clear/list, debug.step ----------------------------------- */

/* An optional breakpoint clause list ('pre' or 'post'). */
static int
rpc_break_spec(cJSON *params, const char *name, rmm_clause_t *out, int *count, rmm_rpc_err_t *err)
{
    const char *text;
    char        emsg[96];

    if (!rpc_opt_string(params, name, &text, err))
        return 0;
    if (text && !rmm_parse_spec(text, out, count, emsg, sizeof(emsg)))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "%s: %s", name, emsg);
    return 1;
}

/* 'ret': how the function returns, for the post clause. */
static int
rpc_break_ret(cJSON *params, rmm_bp_t *nb, rmm_rpc_err_t *err)
{
    const char *text;

    if (!rpc_opt_string(params, "ret", &text, err))
        return 0;
    if (!text || !strcmp(text, "near"))
        return 1;
    if (!strcmp(text, "far"))
        nb->ret_far = 1;
    else if (!strcmp(text, "near32"))
        nb->ret_size = 4;
    else
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'ret' must be near, far or near32");
    return 1;
}

/* 'on_violation': stop the machine, or just count. */
static int
rpc_break_violation(cJSON *params, rmm_bp_t *nb, rmm_rpc_err_t *err)
{
    const char *text;

    if (!rpc_opt_string(params, "on_violation", &text, err))
        return 0;
    if (!text || !strcmp(text, "stop"))
        return 1;
    if (strcmp(text, "count"))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'on_violation' must be stop or count");
    nb->stop = 0;
    return 1;
}

static cJSON *
rpc_debug_break_set(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t addr;
    int      was;
    cJSON   *reply;
    rmm_bp_t nb;

    if (!rpc_uint(params, "addr", 4294967295.0, &addr, 1, err) || !rpc_need_debug_cpu(err))
        return NULL;
    if (rmm_bp_count >= RMM_BP_MAX)
        return rpc_fail(err, RMM_E_BUSY, "at most %d breakpoints", RMM_BP_MAX);
    memset(&nb, 0, sizeof(nb));
    nb.addr     = addr;
    nb.ret_size = 2;
    nb.stop     = 1;
    if (!rpc_break_spec(params, "pre", nb.pre, &nb.npre, err) || !rpc_break_spec(params, "post", nb.post, &nb.npost, err) ||
        !rpc_break_ret(params, &nb, err) || !rpc_break_violation(params, &nb, err))
        return NULL;

    was                     = rmm_quiesce();
    nb.id                   = rmm_bp_next_id++;
    rmm_bps[rmm_bp_count++] = nb;
    rmm_exec_update();
    rmm_unquiesce(was);
    reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "id", nb.id);
    return reply;
}

static cJSON *
rpc_debug_break_clear(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t id;
    int      was;

    if (!rpc_uint(params, "id", 2147483647.0, &id, 1, err))
        return NULL;
    for (int i = 0; i < rmm_bp_count; i++) {
        if (rmm_bps[i].id == (int) id) {
            was        = rmm_quiesce();
            rmm_bps[i] = rmm_bps[--rmm_bp_count];
            rmm_exec_update();
            rmm_unquiesce(was);
            return cJSON_CreateObject();
        }
    }
    return rpc_fail(err, RPC_INVALID_PARAMS, "no breakpoint %u", id);
}

static cJSON *
rpc_debug_break_list(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *list  = cJSON_CreateArray();
    cJSON *reply = cJSON_CreateObject();

    for (int i = 0; i < rmm_bp_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", rmm_bps[i].id);
        cJSON_AddNumberToObject(entry, "addr", rmm_bps[i].addr);
        if (rmm_bps[i].npre || rmm_bps[i].npost) {
            cJSON_AddNumberToObject(entry, "calls", rmm_bps[i].calls);
            cJSON_AddNumberToObject(entry, "violations", rmm_bps[i].viols);
            cJSON_AddBoolToObject(entry, "stop", rmm_bps[i].stop);
            if (rmm_bps[i].viols) {
                cJSON_AddStringToObject(entry, "last", (rmm_bps[i].last_phase == 1) ? "pre" : "post");
                cJSON_AddNumberToObject(entry, "last_clause", rmm_bps[i].last_clause);
                cJSON_AddNumberToObject(entry, "last_lhs", (double) rmm_bps[i].last_l);
                cJSON_AddNumberToObject(entry, "last_rhs", (double) rmm_bps[i].last_r);
            }
        }
        cJSON_AddItemToArray(list, entry);
    }
    cJSON_AddItemToObject(reply, "breakpoints", list);
    return reply;
}

/* debug.step {count=1}: answers at once; machine.stopped {step} or {breakpoint} follows. */
static cJSON *
rpc_debug_step(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t count = 1;

    if (!rpc_uint(params, "count", RMM_STEP_MAX, &count, 0, err)) {
        return NULL;
    }

    if (count == 0) {
        return rpc_fail(err, RPC_INVALID_PARAMS, "'count' must be 1..%d", RMM_STEP_MAX);
    }

    if (!rpc_need_paused(err) || !rpc_need_debug_cpu(err)) {
        return NULL;
    }

    rmm_step_left = (int)count;
    rmm_bp_skip = 1;
    rmm_exec_update();

    if (!rmm_set_pause(0)) {
        rmm_step_left = -1;
        rmm_exec_update();
        return rpc_fail(err, RPC_INTERNAL_ERROR, "resume did not take effect");
    }

    return cJSON_CreateObject();
}

/* -- debug.watch.set/clear/list ------------------------------------------------ */

/* An optional signed integer parameter. */
static int
rpc_int(cJSON *params, const char *name, double lo, double hi, int32_t *out, int *present, rmm_rpc_err_t *err)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, name);

    *present = item != NULL;
    if (!item)
        return 1;
    if (!cJSON_IsNumber(item) || (item->valuedouble < lo) || (item->valuedouble > hi) || (item->valuedouble != (double) (int32_t) item->valuedouble)) {
        rpc_fail(err, RPC_INVALID_PARAMS, "'%s' must be an integer %.0f..%.0f", name, lo, hi);
        return 0;
    }
    *out = (int32_t) item->valuedouble;
    return 1;
}

static const char *
rmm_watch_kind_name(int kind)
{
    return (kind == (RMM_WATCH_R | RMM_WATCH_W)) ? "rw" : (kind == RMM_WATCH_R) ? "r" : "w";
}

/* 'r', 'w' or 'rw' as RMM_WATCH_* flags; 0 if s is none of those. */
static int
rmm_watch_kind_parse(const char *text)
{
    return !strcmp(text, "r") ? RMM_WATCH_R : !strcmp(text, "w") ? RMM_WATCH_W : !strcmp(text, "rw") ? (RMM_WATCH_R | RMM_WATCH_W) : 0;
}

static int
rmm_width_ok(uint32_t len)
{
    return (len == 1) || (len == 2) || (len == 4);
}

/* The optional from/to value filter. */
static int
rpc_watch_filter(cJSON *params, rmm_watch_t *nw, rmm_rpc_err_t *err)
{
    uint32_t from = 0;
    uint32_t to   = 0;

    if (!rpc_uint(params, "from", 4294967295.0, &from, 0, err) || !rpc_uint(params, "to", 4294967295.0, &to, 0, err))
        return 0;
    nw->has_from = cJSON_GetObjectItemCaseSensitive(params, "from") != NULL;
    nw->has_to   = cJSON_GetObjectItemCaseSensitive(params, "to") != NULL;
    nw->from     = from;
    nw->to       = to;
    return 1;
}

/* Checks the filter against kind and len; call once both are known. */
static int
rpc_watch_filter_check(int kind, uint32_t len, const rmm_watch_t *nw, rmm_rpc_err_t *err)
{
    if (!nw->has_from && !nw->has_to)
        return 1;
    if ((kind != RMM_WATCH_W) || !rmm_width_ok(len))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'from'/'to' need kind w and len 1, 2 or 4");
    if ((len < 4) && ((nw->has_from && (nw->from >> (8 * len))) || (nw->has_to && (nw->to >> (8 * len)))))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'from'/'to' do not fit in %u byte(s)", len);
    return 1;
}

/* action "rewrite": store 'value' into the watched location on a hit. */
static int
rpc_watch_rewrite(cJSON *params, int kind, uint32_t len, rmm_watch_t *nw, rmm_rpc_err_t *err)
{
    if ((kind != RMM_WATCH_W) || !rmm_width_ok(len))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "rewrite needs kind w and len 1, 2 or 4");
    if (!rpc_uint(params, "value", 4294967295.0, &nw->rw_val, 1, err))
        return 0;
    if ((len < 4) && (nw->rw_val >> (8 * len)))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'value' does not fit in %u byte(s)", len);
    return 1;
}

/* action "arm": on a hit, watch the child range found through a pointer in the written data. */
static int
rpc_watch_arm(cJSON *params, rmm_watch_t *nw, rmm_rpc_err_t *err)
{
    int32_t     base_off   = 0;
    int32_t     len_off    = 0;
    int         has_base   = 0;
    int         has_lenoff = 0;
    uint32_t    base_size  = 4;
    uint32_t    far_f      = 0;
    uint32_t    clen       = 0;
    uint32_t    len_size   = 0;
    const char *ck         = "w";

    if (!rpc_int(params, "base_off", -2147483647.0, 2147483647.0, &base_off, &has_base, err) ||
        !rpc_int(params, "len_off", -2147483647.0, 2147483647.0, &len_off, &has_lenoff, err) ||
        !rpc_uint(params, "base_size", 4, &base_size, 0, err) || !rpc_uint(params, "far", 1, &far_f, 0, err) ||
        !rpc_uint(params, "child_len", 4096, &clen, 0, err) || !rpc_uint(params, "len_size", 4, &len_size, 0, err))
        return 0;
    if (cJSON_GetObjectItemCaseSensitive(params, "child_kind") && !(ck = rpc_string(params, "child_kind", err)))
        return 0;
    if (!has_base)
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "action arm needs 'base_off'");
    if (!far_f && (base_size != 2) && (base_size != 4))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'base_size' must be 2 or 4");
    if (clen ? has_lenoff : (!has_lenoff || !rmm_width_ok(len_size)))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "arm needs either 'child_len' or 'len_off' with 'len_size' 1, 2 or 4");
    if (!(nw->a_kind = rmm_watch_kind_parse(ck)))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "'child_kind' must be r, w or rw");
    nw->a_base_off  = base_off;
    nw->a_base_size = (int) base_size;
    nw->a_far       = (int) far_f;
    nw->a_len       = clen;
    nw->a_len_off   = len_off;
    nw->a_len_size  = (int) len_size;
    return 1;
}

/* The optional 'action' and its parameters. */
static int
rpc_watch_action(cJSON *params, int kind, uint32_t len, rmm_watch_t *nw, rmm_rpc_err_t *err)
{
    const char *text;

    if (cJSON_GetObjectItemCaseSensitive(params, "action")) {
        if (!(text = rpc_string(params, "action", err)))
            return 0;
        if (!strcmp(text, "arm"))
            nw->action = RMM_ACT_ARM;
        else if (!strcmp(text, "disarm"))
            nw->action = RMM_ACT_DISARM;
        else if (!strcmp(text, "rewrite"))
            nw->action = RMM_ACT_REWRITE;
        else if (strcmp(text, "stop"))
            return !!rpc_fail(err, RPC_INVALID_PARAMS, "'action' must be stop, arm, disarm or rewrite");
    }
    if ((nw->action == RMM_ACT_REWRITE) && !rpc_watch_rewrite(params, kind, len, nw, err))
        return 0;
    if ((nw->action == RMM_ACT_ARM) && !rpc_watch_arm(params, nw, err))
        return 0;
    if (nw->action && (kind != RMM_WATCH_W))
        return !!rpc_fail(err, RPC_INVALID_PARAMS, "arm/disarm/rewrite rules need kind w");
    return 1;
}

static cJSON *
rpc_debug_watch_set(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t    addr;
    uint32_t    len;
    const char *kind_text;
    int         kind;
    int         was;
    cJSON      *reply;
    rmm_watch_t nw;

    memset(&nw, 0, sizeof(nw));
    if (!rpc_watch_filter(params, &nw, err))
        return NULL;
    if (!rpc_uint(params, "addr", 4294967295.0, &addr, 1, err) || !rpc_uint(params, "len", 4096, &len, 1, err) ||
        !(kind_text = rpc_string(params, "kind", err)))
        return NULL;
    if (!(kind = rmm_watch_kind_parse(kind_text)))
        return rpc_fail(err, RPC_INVALID_PARAMS, "'kind' must be r, w or rw");
    if (len == 0)
        return rpc_fail(err, RPC_INVALID_PARAMS, "'len' must be 1..4096");
    if (!rpc_watch_filter_check(kind, len, &nw, err))
        return NULL;
    if ((uint64_t) addr + len > 0x100000000ULL)
        return rpc_fail(err, RPC_INVALID_PARAMS, "range runs past the end of the 32-bit address space");
    if (!rpc_watch_action(params, kind, len, &nw, err))
        return NULL;
    if (!rpc_need_debug_cpu(err))
        return NULL;
    if (rmm_watch_count >= RMM_WATCH_MAX)
        return rpc_fail(err, RMM_E_BUSY, "at most %d watchpoints", RMM_WATCH_MAX);

    was     = rmm_quiesce();
    nw.id   = rmm_bp_next_id++;
    nw.addr = addr;
    nw.len  = len;
    nw.kind = kind;
    rmm_watches[rmm_watch_count++] = nw;
    rmm_watch_install();
    rmm_exec_update();
    rmm_unquiesce(was);
    reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "id", rmm_watches[rmm_watch_count - 1].id);
    return reply;
}

static cJSON *
rpc_debug_watch_clear(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t id;
    int      was;

    if (!rpc_uint(params, "id", 2147483647.0, &id, 1, err))
        return NULL;
    for (int i = 0; i < rmm_watch_count; i++) {
        if (rmm_watches[i].id == (int) id) {
            was = rmm_quiesce();
            rmm_children_remove(0, (int) id);
            for (i = 0; rmm_watches[i].id != (int) id; i++)
                ;
            rmm_watch_remove_at(i);
            if (rmm_watch_count)
                rmm_watch_install();
            else
                rmm_watch_uninstall();
            rmm_watch_hit_id = 0;
            rmm_exec_update();
            rmm_unquiesce(was);
            return cJSON_CreateObject();
        }
    }
    return rpc_fail(err, RPC_INVALID_PARAMS, "no watchpoint %u", id);
}

static cJSON *
rpc_debug_watch_list(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *list  = cJSON_CreateArray();
    cJSON *reply = cJSON_CreateObject();

    for (int i = 0; i < rmm_watch_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", rmm_watches[i].id);
        cJSON_AddNumberToObject(entry, "addr", rmm_watches[i].addr);
        cJSON_AddNumberToObject(entry, "len", rmm_watches[i].len);
        cJSON_AddStringToObject(entry, "kind", rmm_watch_kind_name(rmm_watches[i].kind));
        if (rmm_watches[i].has_from)
            cJSON_AddNumberToObject(entry, "from", rmm_watches[i].from);
        if (rmm_watches[i].has_to)
            cJSON_AddNumberToObject(entry, "to", rmm_watches[i].to);
        if (rmm_watches[i].action)
            cJSON_AddStringToObject(entry, "action", (rmm_watches[i].action == RMM_ACT_ARM) ? "arm" : (rmm_watches[i].action == RMM_ACT_REWRITE) ? "rewrite" : "disarm");
        if (rmm_watches[i].action == RMM_ACT_REWRITE) {
            cJSON_AddNumberToObject(entry, "value", rmm_watches[i].rw_val);
            cJSON_AddNumberToObject(entry, "hits", rmm_watches[i].hits);
        }
        if (rmm_watches[i].owner) {
            cJSON_AddNumberToObject(entry, "rule", rmm_watches[i].owner);
            cJSON_AddNumberToObject(entry, "src", rmm_watches[i].src);
        }
        cJSON_AddItemToArray(list, entry);
    }
    cJSON_AddItemToObject(reply, "watchpoints", list);
    return reply;
}

/* -- snapshots: rmm_snap.c ----------------------------------------------------- */

static const char *
rpc_tag(cJSON *params, rmm_rpc_err_t *err)
{
    const char *tag = rpc_string(params, "tag", err);

    if (tag && (!tag[0] || (strlen(tag) > 63))) {
        rpc_fail(err, RPC_INVALID_PARAMS, "'tag' must be 1..63 characters");
        return NULL;
    }
    return tag;
}

static cJSON *
rpc_snapshot_save(cJSON *params, rmm_rpc_err_t *err)
{
    const char *tag = rpc_tag(params, err);
    int         code;

    if (!tag || !rpc_need_paused(err))
        return NULL;
    if ((code = rmm_snap_save(tag, err->msg, sizeof(err->msg))) != 0) {
        err->code = code;
        return NULL;
    }
    return cJSON_CreateObject();
}

/* Pauses first if running; leaves the machine paused. */
static cJSON *
rpc_snapshot_restore(cJSON *params, rmm_rpc_err_t *err)
{
    const char *tag = rpc_tag(params, err);
    int         code;

    if (!tag)
        return NULL;
    if (!dopause && !rmm_machine_run(0))
        return rpc_fail(err, -32603, "pause did not take effect");
    /* Breakpoints and watches stay armed; watch overrides come out before the restore
       and go back after. */
    rmm_watch_uninstall();
    code = rmm_snap_restore(tag, err->msg, sizeof(err->msg));
    if (rmm_watch_count)
        rmm_watch_install();
    if (code != 0) {
        err->code = code;
        return NULL;
    }
    return cJSON_CreateObject();
}

static cJSON *
rpc_snapshot_delete(cJSON *params, rmm_rpc_err_t *err)
{
    const char *tag = rpc_tag(params, err);

    if (!tag)
        return NULL;
    if (rmm_snap_delete(tag) != 0)
        return rpc_fail(err, 7, "no snapshot '%s'", tag);
    return cJSON_CreateObject();
}

static cJSON *
rpc_snapshot_list(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON   *reply = cJSON_CreateObject();
    cJSON   *list  = cJSON_CreateArray();
    uint64_t snap_tsc;
    uint64_t bytes;

    for (int i = 0; i < rmm_snap_slots(); i++) {
        const char *tag = rmm_snap_tag(i, &snap_tsc);

        if (tag) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "tag", tag);
            cJSON_AddNumberToObject(entry, "tsc", (double) snap_tsc);
            cJSON_AddStringToObject(entry, "state", rmm_snap_state_name(i, &bytes));
            cJSON_AddNumberToObject(entry, "bytes", (double) bytes);
            cJSON_AddItemToArray(list, entry);
        }
    }
    cJSON_AddItemToObject(reply, "snapshots", list);
    return reply;
}

/* Writes a snapshot to a file another 86Box of the same build and config can load. */
static cJSON *
rpc_snapshot_export(cJSON *params, rmm_rpc_err_t *err)
{
    const char *tag  = rpc_tag(params, err);
    const char *path = tag ? rpc_string(params, "path", err) : NULL;
    int         code;

    if (!path || !rpc_need_paused(err))
        return NULL;
    if (!path[0])
        return rpc_fail(err, RPC_INVALID_PARAMS, "'path' must not be empty");
    if ((code = rmm_snap_export(tag, path, err->msg, sizeof(err->msg))) != 0) {
        err->code = code;
        return NULL;
    }
    return cJSON_CreateObject();
}

/* Reads a snapshot file into a slot, its own tag or 'tag'; restore=true restores it
   too. Pauses first if running; leaves the machine paused. */
static cJSON *
rpc_snapshot_load(cJSON *params, rmm_rpc_err_t *err)
{
    const char *path = rpc_string(params, "path", err);
    cJSON      *tj   = cJSON_GetObjectItemCaseSensitive(params, "tag");
    cJSON      *rj   = cJSON_GetObjectItemCaseSensitive(params, "restore");
    const char *tag  = NULL;
    char        got[64];
    int         code;
    cJSON      *reply;

    if (!path)
        return NULL;
    if (tj && !(tag = rpc_tag(params, err)))
        return NULL;
    if (!dopause && !rmm_machine_run(0))
        return rpc_fail(err, -32603, "pause did not take effect");
    if ((code = rmm_snap_load(path, tag, got, sizeof(got), err->msg, sizeof(err->msg))) != 0) {
        err->code = code;
        return NULL;
    }
    if (!rj || cJSON_IsTrue(rj)) {
        rmm_watch_uninstall();
        code = rmm_snap_restore(got, err->msg, sizeof(err->msg));
        if (rmm_watch_count)
            rmm_watch_install();
        if (code != 0) {
            err->code = code;
            return NULL;
        }
    }
    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "tag", got);
    return reply;
}

/* Test hook: restore through the pointer table, as a fresh process would. */
static cJSON *
rpc_snapshot_reloc(cJSON *params, UNUSED(rmm_rpc_err_t *err))
{
    cJSON *force = cJSON_GetObjectItemCaseSensitive(params, "force");

    rmm_snap_reloc_force(cJSON_IsTrue(force));
    return cJSON_CreateObject();
}

/* Which words of the dumped structs are addresses of this process. Development aid. */
static cJSON *
rpc_snapshot_ptrscan(UNUSED(cJSON *params), rmm_rpc_err_t *err)
{
    int         total;
    cJSON      *reply, *array;
    const char *label;
    unsigned    region, rlen, off;
    int         code;
    uint64_t    val, base;
    int         target;
    unsigned    toff;
    char        hex[24];

    if (!rpc_need_paused(err))
        return NULL;
    if ((total = rmm_snap_ptrscan(err->msg, sizeof(err->msg))) < 0) {
        err->code = 8;
        return NULL;
    }
    reply = cJSON_CreateObject();
    array = cJSON_AddArrayToObject(reply, "hits");
    for (int i = 0; rmm_snap_ptrscan_hit(i, &label, &region, &rlen, &off, &code, &val, &base, &target, &toff); i++) {
        cJSON *hit = cJSON_CreateObject();

        cJSON_AddStringToObject(hit, "dev", label);
        cJSON_AddNumberToObject(hit, "region", region);
        cJSON_AddNumberToObject(hit, "len", rlen);
        cJSON_AddNumberToObject(hit, "off", off);
        cJSON_AddBoolToObject(hit, "code", code);
        snprintf(hex, sizeof(hex), "%" PRIx64, val);
        cJSON_AddStringToObject(hit, "val", hex);
        snprintf(hex, sizeof(hex), "%" PRIx64, base);
        cJSON_AddStringToObject(hit, "base", hex);
        cJSON_AddNumberToObject(hit, "target", target);
        cJSON_AddNumberToObject(hit, "toff", toff);
        cJSON_AddItemToArray(array, hit);
    }
    cJSON_AddNumberToObject(reply, "total", total);
    array = cJSON_AddArrayToObject(reply, "regions");
    for (int i = 0; rmm_snap_ptrscan_region(i, &label, &val, &base, &code); i++) {
        cJSON *region_entry = cJSON_CreateArray();

        cJSON_AddItemToArray(region_entry, cJSON_CreateString(label));
        cJSON_AddItemToArray(region_entry, cJSON_CreateNumber((double) base));
        cJSON_AddItemToArray(region_entry, cJSON_CreateNumber(code));
        cJSON_AddItemToArray(array, region_entry);
    }
    return reply;
}

/* Waits for the async writer to put every queued snapshot on disk. Needs RMM_SNAP_DIR.
   Runs while the guest runs. */
static cJSON *
rpc_snapshot_flush(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t timeout = 60000;
    int      errors, pending;
    cJSON   *reply;

    if (!rpc_uint(params, "timeout_ms", 3600000, &timeout, 0, err))
        return NULL;
    pending = rmm_snap_flush((int) timeout, &errors);
    reply       = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "pending", pending);
    cJSON_AddNumberToObject(reply, "errors", errors);
    return reply;
}

/* Starts the async snapshot writer: snapshots are copied to dir by a thread and
   dropped from memory. Not needed when RMM_SNAP_DIR is set. */
static cJSON *
rpc_snapshot_writer(cJSON *params, rmm_rpc_err_t *err)
{
    const char *dir = rpc_string(params, "dir", err);
    uint32_t    budget = 0;
    cJSON      *keep   = cJSON_GetObjectItemCaseSensitive(params, "keep");
    cJSON      *hold   = cJSON_GetObjectItemCaseSensitive(params, "hold");

    if (!dir || !rpc_uint(params, "budget_mb", 1 << 20, &budget, 0, err))
        return NULL;
    if (rmm_snap_writer_config(dir, (int) budget, cJSON_IsTrue(keep), err->msg, sizeof(err->msg)) != 0) {
        err->code = 8;
        return NULL;
    }
    if (cJSON_IsBool(hold))
        rmm_snap_writer_hold(cJSON_IsTrue(hold));
    return cJSON_CreateObject();
}

static cJSON *rpc_discover(cJSON *params, rmm_rpc_err_t *err);
static cJSON *rpc_hello(cJSON *params, rmm_rpc_err_t *err);

/* The method schema: rpc.discover publishes every column, so this table is the one place a
   method is described (the Python client's registry is a fallback for older servers, and
   its tests check it against this table). Rows use designated initializers, so a row names
   what it sets and leaves the rest empty. rmm_methods_check() runs at startup and logs any
   row that is malformed (unknown type, missing space between words, unlisted error code).
     .params   space-separated name:type; '?' marks optional; '=a|b|c' lists the accepted values
     .returns  the result's top-level fields, space-separated
     .flags    RMM_MF_PAUSED: error 1 unless the machine is paused
     .errors   the app error codes it can raise, besides 1: RMM_ERR(BUSY) " " RMM_ERR(NO_SLOT)
     .doc      one line of help
   Adjacent string pieces are joined as written, so each piece but the last ends in a space.
   Types: int, bool, string, hex (a string of hex byte pairs), object, array. */
#define RMM_MF_PAUSED 0x01

#define RMM_STR_(x) #x
#define RMM_STR(x)  RMM_STR_(x)
#define RMM_ERR(x)  RMM_STR(RMM_E_##x) /* RMM_ERR(BUSY) is "4" */

typedef struct {
    const char *name;
    rmm_rpc_fn  fn;
    const char *params;
    const char *returns;
    int         flags;
    const char *errors;
    const char *doc;
} rmm_method_t;

static const rmm_method_t rmm_methods[] = {
    { .name    = "rpc.hello",
      .fn      = rpc_hello,
      .params  = "protocol:int? client:string?",
      .returns = "server rmm_version protocol min_protocol negotiated capabilities limits",
      .errors  = RMM_ERR(PROTOCOL),
      .doc     = "version handshake (optional)" },
    { .name    = "rpc.discover",
      .fn      = rpc_discover,
      .returns = "methods events errors",
      .doc     = "this schema: methods, events and error codes" },
    { .name    = "machine.status",
      .fn      = rpc_machine_status,
      .returns = "state sticky_pause tsc cs eip",
      .doc     = "run state and time-stamp counter" },
    { .name    = "machine.pause",
      .fn      = rpc_machine_pause,
      .returns = "state",
      .doc     = "pause the guest (waits for the CPU thread)" },
    { .name    = "machine.resume",
      .fn      = rpc_machine_resume,
      .returns = "state",
      .doc     = "resume the guest" },
    { .name    = "rmm.sticky_pause.get",
      .fn      = rpc_sticky_get,
      .returns = "value",
      .doc     = "does a client pause outlive its connection?" },
    { .name    = "rmm.sticky_pause.set",
      .fn      = rpc_sticky_set,
      .params  = "value:bool",
      .returns = "value",
      .doc     = "make a client pause outlive its connection" },
    { .name    = "cpu.regs.get",
      .fn      = rpc_cpu_regs_get,
      .returns = "eax ecx edx ebx esp ebp esi edi eip eflags cs ss ds es fs gs cs_base cr0 cr2 cr3 cr4",
      .flags   = RMM_MF_PAUSED,
      .doc     = "registers" },
    { .name    = "cpu.regs.set",
      .fn      = rpc_cpu_regs_set,
      .params  = "eax:int? ecx:int? edx:int? ebx:int? esp:int? ebp:int? esi:int? edi:int? eip:int? "
                 "eflags:int?",
      .returns = "eax ecx edx ebx esp ebp esi edi eip eflags",
      .flags   = RMM_MF_PAUSED,
      .doc     = "set general registers, all or nothing" },
    { .name    = "mem.phys.read",
      .fn      = rpc_mem_phys_read,
      .params  = "addr:int len:int",
      .returns = "addr len data",
      .flags   = RMM_MF_PAUSED,
      .doc     = "read physical memory as hex (len up to limits.mem_max)" },
    { .name    = "mem.lin.read",
      .fn      = rpc_mem_lin_read,
      .params  = "addr:int len:int",
      .returns = "addr len data",
      .flags   = RMM_MF_PAUSED,
      .errors  = RMM_ERR(NOT_MAPPED),
      .doc     = "read linear memory through the page tables" },
    { .name    = "mem.phys.write",
      .fn      = rpc_mem_phys_write,
      .params  = "addr:int data:hex raw:bool?",
      .returns = "addr len",
      .flags   = RMM_MF_PAUSED,
      .errors  = RMM_ERR(DROPPED),
      .doc     = "write physical RAM from hex bytes, read back to verify" },
    { .name    = "input.key.send",
      .fn      = rpc_input_key_send,
      .params  = "combo:string",
      .doc     = "press a key combo, e.g. ctrl+alt+del, enter, f1" },
    { .name    = "input.text.type",
      .fn      = rpc_input_text_type,
      .params  = "text:string",
      .doc     = "type text" },
    { .name    = "input.events",
      .fn      = rpc_input_events,
      .params  = "events:array",
      .returns = "queued pending",
      .errors  = RMM_ERR(NO_SLOT) " " RMM_ERR(BUSY),
      .doc     = "ordered input, all or nothing: {type:key, scan|key, down}, {type:rel, dx, dy, dz?} "
                 "(dz in wheel notches, + = away from the user), {type:abs, x, y} (0..1 across the "
                 "screen; a tablet in absolute mode), {type:btn, buttons} (bit 0 left, 1 right, 2 "
                 "middle, 3-4 extra); a key waits 2 ms of guest time after the last key, a button "
                 "change 30 ms after the last change, and whatever follows waits with it; error 4 "
                 "when the queue (256) has no room" },
    { .name    = "input.mouse.move",
      .fn      = rpc_input_mouse_move,
      .params  = "dx:int dy:int buttons:int?",
      .returns = "queued pending",
      .errors  = RMM_ERR(NO_SLOT) " " RMM_ERR(BUSY),
      .doc     = "relative motion, after the button mask (bit 0 left, 1 right, 2 middle) if given; "
                 "it stays held" },
    { .name    = "input.release_all",
      .fn      = rpc_input_release_all,
      .returns = "keys buttons pending",
      .doc     = "drop queued input and release every key and button this client holds (done on "
                 "disconnect too)" },
    { .name    = "input.devices",
      .fn      = rpc_input_devices,
      .returns = "keyboard mouse joystick tablet pointer pending",
      .doc     = "the input devices the emulator instantiated; pointer: absolute (send abs) or "
                 "relative (send rel: grab)" },
    { .name    = "machine.reset",
      .fn      = rpc_machine_reset,
      .params  = "hard:bool?",
      .returns = "state config_applied config_loaded",
      .doc     = "hard reset; applies staged config" },
    { .name = "machine.cad", .fn = rpc_machine_cad, .doc = "send Ctrl+Alt+Del" },
    { .name = "machine.cae", .fn = rpc_machine_cae, .doc = "send Ctrl+Alt+Esc" },
    { .name = "machine.nmi", .fn = rpc_machine_nmi, .doc = "raise an NMI" },
    { .name    = "machine.acpi_shutdown",
      .fn      = rpc_machine_acpi_shutdown,
      .errors  = RMM_ERR(BUSY),
      .doc     = "press the soft power button" },
    { .name    = "machine.quit",
      .fn      = rpc_machine_quit,
      .doc     = "power 86Box off, no confirmation (machine.exit, then the socket closes)" },
    { .name    = "media.list",
      .fn      = rpc_media_list,
      .returns = "slots",
      .doc     = "removable media slots and their images" },
    { .name = "mem.info", .fn = rpc_mem_info, .returns = "ram_kb", .doc = "RAM size" },
    { .name    = "io.read",
      .fn      = rpc_io_read,
      .params  = "port:int size:int?=1|2|4",
      .returns = "port size value",
      .flags   = RMM_MF_PAUSED,
      .doc     = "read an I/O port (the device sees a real read)" },
    { .name    = "io.write",
      .fn      = rpc_io_write,
      .params  = "port:int value:int size:int?=1|2|4",
      .returns = "port size value",
      .flags   = RMM_MF_PAUSED,
      .doc     = "write an I/O port" },
    { .name    = "config.get",
      .fn      = rpc_config_get,
      .params  = "section:string key:string?",
      .returns = "value pending values",
      .doc     = "read a cfg key or a section" },
    { .name    = "config.set",
      .fn      = rpc_config_set,
      .params  = "section:string key:string value:int",
      .returns = "applies pending",
      .doc     = "set a cfg key (now or at the next hard reset)" },
    { .name    = "config.describe",
      .fn      = rpc_config_describe,
      .params  = "section:string key:string",
      .returns = "type applies min max step default value values labels multi",
      .doc     = "what a cfg key takes" },
    { .name    = "config.enums",
      .fn      = rpc_config_enums,
      .params  = "section:string?",
      .returns = "keys",
      .doc     = "every hardware-choice key: value, values, labels for this machine" },
    { .name    = "config.pending",
      .fn      = rpc_config_pending,
      .returns = "pending load",
      .doc     = "what the next hard reset will apply" },
    { .name    = "config.discard",
      .fn      = rpc_config_discard,
      .returns = "discarded",
      .doc     = "drop everything staged" },
    { .name    = "config.dump",
      .fn      = rpc_config_dump,
      .params  = "section:string?",
      .returns = "cfg path",
      .doc     = "the whole VM cfg" },
    { .name    = "config.load",
      .fn      = rpc_config_load,
      .params  = "cfg:object replace:bool? validate_only:bool?",
      .returns = "staged applies keys unchecked",
      .errors  = RMM_ERR(BAD_CONFIG),
      .doc     = "stage a whole cfg for the next hard reset" },
    { .name    = "capture.screenshot",
      .fn      = rpc_capture_screenshot,
      .params  = "step:int? wait_ms:int?",
      .returns = "w h src_w src_h step format seq fresh data",
      .errors  = RMM_ERR(BUSY),
      .doc     = "the guest screen, RGB24 base64; never pauses the guest" },
    { .name    = "debug.break.set",
      .fn      = rpc_debug_break_set,
      .params  = "addr:int pre:string? post:string? ret:string?=near|far|near32 "
                 "on_violation:string?=stop|count",
      .returns = "id",
      .errors  = RMM_ERR(BUSY),
      .doc     = "execute breakpoint at a linear address; pre/post are function contracts, e.g. pre "
                 "'arg0 > 0 && arg1 <= 100', post 'ax >= 0' (stop on broken clause, or count it)" },
    { .name = "debug.break.clear", .fn = rpc_debug_break_clear, .params = "id:int",
      .doc = "clear a breakpoint" },
    { .name = "debug.break.list", .fn = rpc_debug_break_list, .returns = "breakpoints",
      .doc = "breakpoints" },
    { .name    = "debug.step",
      .fn      = rpc_debug_step,
      .params  = "count:int?",
      .flags   = RMM_MF_PAUSED,
      .errors  = RMM_ERR(BUSY),
      .doc     = "single-step; emits machine.stopped" },
    { .name    = "debug.watch.set",
      .fn      = rpc_debug_watch_set,
      .params  = "addr:int len:int kind:string=r|w|rw from:int? to:int? "
                 "action:string?=stop|arm|disarm|rewrite value:int? base_off:int? base_size:int? "
                 "far:int? child_len:int? len_off:int? len_size:int? child_kind:string?=r|w|rw",
      .returns = "id",
      .errors  = RMM_ERR(BUSY),
      .doc     = "watchpoint r|w|rw on a linear range; w of 1/2/4 bytes may need from/to (value "
                 "before/after); action arm|disarm makes it an in-emulator rule that arms/removes a "
                 "child watch on a (base,len) read from guest memory next to the watched address; "
                 "rewrite (kind w, len 1/2/4) replaces the written value with value" },
    { .name = "debug.watch.clear", .fn = rpc_debug_watch_clear, .params = "id:int",
      .doc = "clear a watchpoint" },
    { .name = "debug.watch.list", .fn = rpc_debug_watch_list, .returns = "watchpoints",
      .doc = "watchpoints" },
    { .name    = "snapshot.save",
      .fn      = rpc_snapshot_save,
      .params  = "tag:string",
      .flags   = RMM_MF_PAUSED,
      .errors  = RMM_ERR(SNAP_FAIL),
      .doc     = "save the machine in memory" },
    { .name    = "snapshot.restore",
      .fn      = rpc_snapshot_restore,
      .params  = "tag:string",
      .errors  = RMM_ERR(NO_SNAP) " " RMM_ERR(SNAP_FAIL),
      .doc     = "roll the machine back; pauses first and leaves it paused" },
    { .name    = "snapshot.list",
      .fn      = rpc_snapshot_list,
      .returns = "snapshots",
      .doc     = "snapshots, oldest first" },
    { .name    = "snapshot.delete",
      .fn      = rpc_snapshot_delete,
      .params  = "tag:string",
      .errors  = RMM_ERR(NO_SNAP),
      .doc     = "drop a snapshot" },
    { .name    = "snapshot.flush",
      .fn      = rpc_snapshot_flush,
      .params  = "timeout_ms:int?",
      .returns = "pending errors",
      .doc     = "wait for the writer to put every snapshot on disk" },
    { .name    = "snapshot.export",
      .fn      = rpc_snapshot_export,
      .params  = "tag:string path:string",
      .flags   = RMM_MF_PAUSED,
      .errors  = RMM_ERR(NO_SNAP) " " RMM_ERR(SNAP_FAIL),
      .doc     = "write a snapshot to one file on the 86Box host" },
    { .name    = "snapshot.load",
      .fn      = rpc_snapshot_load,
      .params  = "path:string tag:string? restore:bool?",
      .returns = "tag",
      .errors  = RMM_ERR(SNAP_FAIL),
      .doc     = "read a snapshot file into a slot and restore it (pauses first)" },
    { .name    = "snapshot.ptrscan",
      .fn      = rpc_snapshot_ptrscan,
      .returns = "hits total",
      .flags   = RMM_MF_PAUSED,
      .errors  = RMM_ERR(SNAP_FAIL),
      .doc     = "development aid: pointer words in the snapshot dumps" },
    { .name    = "snapshot.reloc",
      .fn      = rpc_snapshot_reloc,
      .params  = "force:bool",
      .doc     = "test hook: restore through the pointer table" },
    { .name    = "snapshot.writer",
      .fn      = rpc_snapshot_writer,
      .params  = "dir:string budget_mb:int? keep:bool? hold:bool?",
      .errors  = RMM_ERR(SNAP_FAIL),
      .doc     = "start the async writer: snapshots move from memory to dir" },
    { .name = NULL }
};

/* <dev>.<slot>.<verb> for every rmm_media_devs[] entry; discover lists them as <dev>.N.<verb>. */
static const struct {
    const char *verb;
    const char *params;
    const char *errors;
    const char *doc;
} rmm_media_verbs[] = {
    { "insert", "path:string wp:bool?", "3", "insert an image (-32602 when the file cannot be opened)" },
    { "eject",  "",                     "2 3", "eject the image" },
    { NULL,     NULL,                   NULL, NULL }
};

/* Notifications (JSON-RPC requests without an id); fields use the params syntax. */
static const struct {
    const char *name;
    const char *fields;
    const char *doc;
} rmm_events[] = {
    { "machine.stopped",
      "reason:string=pause|breakpoint|watchpoint|step addr:int? id:int? kind:string?=r|w eip:int? old:int? "
      "new:int? rule:int? src:int? violation:string?=pre|post clause:int? lhs:int? rhs:int?",
      "the guest stopped: a pause, a breakpoint, a watchpoint or the end of a step" },
    { "machine.resumed", "", "the guest is running again" },
    { "machine.reset", "hard:bool", "a hard reset finished" },
    { "machine.exit", "", "86Box is powering off; the socket closes next" },
    { "media.changed", "dev:string slot:int path:string", "an image was inserted or ejected (path \"\")" },
    { NULL, NULL, NULL }
};

/* App error codes (JSON-RPC error.code); -32xxx are the JSON-RPC standard ones. */
static const struct {
    int         code;
    const char *name;
} rmm_errors[] = {
    { RMM_E_NOT_PAUSED, "not paused" },
    { RMM_E_EMPTY,      "slot empty" },
    { RMM_E_NO_SLOT,    "no such slot, drive or device" },
    { RMM_E_BUSY,       "busy, absent or disabled" },
    { RMM_E_DROPPED,    "write dropped" },
    { RMM_E_NO_SNAP,    "no such snapshot" },
    { RMM_E_SNAP_FAIL,  "cannot snapshot" },
    { RMM_E_NOT_MAPPED, "linear address not mapped" },
    { RMM_E_BAD_CONFIG, "config refused" },
    { RMM_E_PROTOCOL,   "client protocol too old" },
    { 0,                NULL }
};

/* Startup self-check of the method table: logs each row a human could have got wrong, so a
   typo shows in the log instead of as a client that quietly sees a bad schema. Returns the
   number of problems. */
static int
rmm_methods_check(void)
{
    static const char *types[] = { "int", "bool", "string", "hex", "object", "array", NULL };
    int                bad     = 0;

    for (int i = 0; rmm_methods[i].name; i++) {
        const rmm_method_t *method = &rmm_methods[i];
        const char         *params = method->params ? method->params : "";
        const char         *errors = method->errors ? method->errors : "";

        if (!method->fn || !method->doc || !*method->doc) {
            pclog("RMM: method table: %s has no handler or no doc\n", method->name);
            bad++;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(rmm_methods[j].name, method->name)) {
                pclog("RMM: method table: %s listed twice\n", method->name);
                bad++;
            }
        while (*params) {
            const char *end = params + strcspn(params, " ");
            const char *col = memchr(params, ':', end - params);
            int         ok  = 0;

            if (col) {
                const char *eq = memchr(col, '=', end - col);
                size_t      tl = (eq ? eq : end) - col - 1;

                if (tl && col[tl] == '?')
                    tl--;
                for (int type_idx = 0; types[type_idx]; type_idx++)
                    if (strlen(types[type_idx]) == tl && !strncmp(col + 1, types[type_idx], tl))
                        ok = 1;
            }
            if (!ok) {
                pclog("RMM: method table: %s: bad param '%.*s' (name:type[?][=a|b], words need a space between)\n",
                        method->name, (int) (end - params), params);
                bad++;
            }
            params = end + strspn(end, " ");
        }
        while (*errors) {
            char *next;
            long  code = strtol(errors, &next, 10);
            int   known = 0;

            if (next == errors) {
                pclog("RMM: method table: %s: bad errors list '%s'\n", method->name, errors);
                bad++;
                break;
            }
            for (int code_idx = 0; rmm_errors[code_idx].name; code_idx++)
                known |= (rmm_errors[code_idx].code == code);
            if (!known) {
                pclog("RMM: method table: %s raises %ld, which rmm_errors[] does not list\n", method->name, code);
                bad++;
            }
            errors = next + strspn(next, " ");
        }
    }
    return bad;
}

/* Version handshake. The client names the newest protocol it speaks; it is refused only
   when that is older than RMM_PROTOCOL_MIN. The reply carries what the server speaks so a
   client can refuse a server that is too old for it. Calling it is optional. */
static cJSON *
rpc_hello(cJSON *params, rmm_rpc_err_t *err)
{
    uint32_t cproto = RMM_PROTOCOL;
    cJSON   *reply;
    cJSON   *caps;
    cJSON   *lim;

    if (!rpc_uint(params, "protocol", 1000000, &cproto, 0, err))
        return NULL;
    if (cproto < RMM_PROTOCOL_MIN) {
        err->data = cJSON_CreateObject();
        cJSON_AddNumberToObject(err->data, "protocol", RMM_PROTOCOL);
        cJSON_AddNumberToObject(err->data, "min_protocol", RMM_PROTOCOL_MIN);
        return rpc_fail(err, RMM_E_PROTOCOL, "client protocol %u is older than the minimum %d",
                        (unsigned) cproto, RMM_PROTOCOL_MIN);
    }

    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "server", "86Box");
    cJSON_AddStringToObject(reply, "rmm_version", RMM_VERSION);
    cJSON_AddNumberToObject(reply, "protocol", RMM_PROTOCOL);
    cJSON_AddNumberToObject(reply, "min_protocol", RMM_PROTOCOL_MIN);
    cJSON_AddNumberToObject(reply, "negotiated", (cproto < RMM_PROTOCOL) ? cproto : RMM_PROTOCOL);

    /* Capabilities: the method namespaces this build implements ("debug", "config", ...). */
    caps = cJSON_CreateArray();
    for (int i = 0; rmm_methods[i].name; i++) {
        const char *dot = strchr(rmm_methods[i].name, '.');
        char        ns[32];
        int         seen = 0;

        if (!dot || ((size_t) (dot - rmm_methods[i].name) >= sizeof(ns)))
            continue;
        snprintf(ns, sizeof(ns), "%.*s", (int) (dot - rmm_methods[i].name), rmm_methods[i].name);
        for (cJSON *cap = caps->child; cap; cap = cap->next)
            if (!strcmp(cap->valuestring, ns))
                seen = 1;
        if (!seen)
            cJSON_AddItemToArray(caps, cJSON_CreateString(ns));
    }
    cJSON_AddItemToObject(reply, "capabilities", caps);

    lim = cJSON_CreateObject();
    cJSON_AddNumberToObject(lim, "mem_max", RMM_MEM_MAX);
    cJSON_AddNumberToObject(lim, "line_max", RMM_LINE_MAX);
    cJSON_AddItemToObject(reply, "limits", lim);
    return reply;
}

/* Adds each "name:type[?][=a|b|c]" of spec to obj as name: "type[?]", and each '=' list to
   enums as name: [values] (numbers when the type is int). Works in place: no length limit. */
static void
rmm_spec_to_json(const char *spec, cJSON *obj, cJSON *enums)
{
    while (*spec) {
        const char *end;
        const char *colon;
        const char *eq;
        char        name[64];
        char        type[16];

        spec += strspn(spec, " ");
        end   = spec + strcspn(spec, " ");
        colon = memchr(spec, ':', end - spec);
        if (colon) {
            eq = memchr(colon, '=', end - colon);
            snprintf(name, sizeof(name), "%.*s", (int) (colon - spec), spec);
            snprintf(type, sizeof(type), "%.*s", (int) ((eq ? eq : end) - colon - 1), colon + 1);
            cJSON_AddStringToObject(obj, name, type);
            if (eq) {
                cJSON *vals = cJSON_CreateArray();

                for (const char *value_start = eq + 1; value_start < end;) {
                    const char *ve = value_start + strcspn(value_start, "| ");
                    char        val[32];

                    snprintf(val, sizeof(val), "%.*s", (int) (ve - value_start), value_start);
                    cJSON_AddItemToArray(vals, strncmp(type, "int", 3) ? cJSON_CreateString(val)
                                                                       : cJSON_CreateNumber(atoi(val)));
                    value_start = (*ve == '|') ? (ve + 1) : ve;
                }
                cJSON_AddItemToObject(enums, name, vals);
            }
        }
        spec = end;
    }
}

/* Appends "a b c" to array a as "a","b","c" (numbers when as_int); returns a. */
static cJSON *
rmm_words_to_json(cJSON *array, const char *words, int as_int)
{
    while (*words) {
        size_t len;
        char   word[64];

        words += strspn(words, " ");
        if (!(len = strcspn(words, " ")))
            break;
        snprintf(word, sizeof(word), "%.*s", (int) len, words);
        cJSON_AddItemToArray(array, as_int ? cJSON_CreateNumber(atoi(word)) : cJSON_CreateString(word));
        words += len;
    }
    return array;
}

/* One discover entry. enums and errors are left out when empty, paused when 0. */
static cJSON *
rmm_schema_entry(const char *name, const char *params, const char *returns, int paused,
                 const char *errors, const char *doc)
{
    cJSON *method = cJSON_CreateObject();
    cJSON *ps     = cJSON_CreateObject();
    cJSON *enums  = cJSON_CreateObject();
    cJSON *errs   = cJSON_CreateArray();

    /* table rows leave empty columns NULL */
    params  = params ? params : "";
    returns = returns ? returns : "";
    errors  = errors ? errors : "";
    if (paused)
        cJSON_AddItemToArray(errs, cJSON_CreateNumber(RMM_E_NOT_PAUSED));
    rmm_words_to_json(errs, errors, 1);
    rmm_spec_to_json(params, ps, enums);
    cJSON_AddStringToObject(method, "name", name);
    cJSON_AddItemToObject(method, "params", ps);
    if (enums->child)
        cJSON_AddItemToObject(method, "enums", enums);
    else
        cJSON_Delete(enums);
    cJSON_AddItemToObject(method, "returns", rmm_words_to_json(cJSON_CreateArray(), returns, 0));
    if (paused)
        cJSON_AddTrueToObject(method, "paused");
    if (errs->child)
        cJSON_AddItemToObject(method, "errors", errs);
    else
        cJSON_Delete(errs);
    cJSON_AddStringToObject(method, "doc", doc);
    return method;
}

/* The whole schema: {methods, events, errors}. A pre-schema client reads only name, params,
   indices and verbs, which keep their old shapes. */
static cJSON *
rpc_discover(UNUSED(cJSON *params), UNUSED(rmm_rpc_err_t *err))
{
    cJSON *list  = cJSON_CreateArray();
    cJSON *evs   = cJSON_CreateArray();
    cJSON *errs  = cJSON_CreateArray();
    cJSON *reply = cJSON_CreateObject();

    for (int i = 0; rmm_methods[i].name; i++)
        cJSON_AddItemToArray(list, rmm_schema_entry(rmm_methods[i].name, rmm_methods[i].params,
                                                    rmm_methods[i].returns, rmm_methods[i].flags & RMM_MF_PAUSED,
                                                    rmm_methods[i].errors, rmm_methods[i].doc));
    for (int dev = 0; rmm_media_devs[dev]; dev++) {
        int slots[16];
        int nslots = plat_rmm_media_slots(dev, slots, 16);

        for (int verb_idx = 0; rmm_media_verbs[verb_idx].verb; verb_idx++) {
            char   name[32];
            cJSON *method;

            snprintf(name, sizeof(name), "%s.N.%s", rmm_media_devs[dev], rmm_media_verbs[verb_idx].verb);
            method = rmm_schema_entry(name, rmm_media_verbs[verb_idx].params, "", 0, rmm_media_verbs[verb_idx].errors,
                                 rmm_media_verbs[verb_idx].doc);
            cJSON_AddItemToObject(method, "indices", cJSON_CreateIntArray(slots, nslots));
            cJSON_AddItemToArray(list, method);
        }
    }
    for (int i = 0; rmm_props[i].name; i++) {
        /* params are set's; get takes none */
        cJSON *method = rmm_schema_entry(rmm_props[i].name, "value:bool", "value", 0, "4", rmm_props[i].doc);

        cJSON_AddItemToObject(method, "verbs", cJSON_CreateStringArray((const char *[]) { "get", "set" }, 2));
        cJSON_AddItemToArray(list, method);
    }
    cJSON_AddItemToObject(reply, "methods", list);

    for (int i = 0; rmm_events[i].name; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON *fs    = cJSON_CreateObject();
        cJSON *enums = cJSON_CreateObject();

        rmm_spec_to_json(rmm_events[i].fields, fs, enums);
        cJSON_AddStringToObject(entry, "name", rmm_events[i].name);
        cJSON_AddItemToObject(entry, "fields", fs);
        if (enums->child)
            cJSON_AddItemToObject(entry, "enums", enums);
        else
            cJSON_Delete(enums);
        cJSON_AddStringToObject(entry, "doc", rmm_events[i].doc);
        cJSON_AddItemToArray(evs, entry);
    }
    cJSON_AddItemToObject(reply, "events", evs);

    for (int i = 0; rmm_errors[i].name; i++) {
        cJSON *entry = cJSON_CreateObject();

        cJSON_AddNumberToObject(entry, "code", rmm_errors[i].code);
        cJSON_AddStringToObject(entry, "name", rmm_errors[i].name);
        cJSON_AddItemToArray(errs, entry);
    }
    cJSON_AddItemToObject(reply, "errors", errs);
    return reply;
}

static void
rmm_rpc_reply_error(cJSON *id, int code, const char *msg, cJSON *data)
{
    cJSON *resp      = cJSON_CreateObject();
    cJSON *error_obj = cJSON_CreateObject();

    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddNumberToObject(error_obj, "code", code);
    cJSON_AddStringToObject(error_obj, "message", msg);
    if (data)
        cJSON_AddItemToObject(error_obj, "data", data);
    cJSON_AddItemToObject(resp, "error", error_obj);
    rmm_send_json(resp);
    cJSON_Delete(resp);
}

/* "<prop>.get" / "<prop>.set" -> index into rmm_props, or -1. */
static int
rmm_prop_lookup(const char *method, int *set)
{
    size_t len = strlen(method);
    int    prop = -1;

    for (int i = 0; rmm_props[i].name; i++) {
        size_t name_len = strlen(rmm_props[i].name);

        if ((len == name_len + 4) && !strncmp(method, rmm_props[i].name, name_len) && (!strcmp(method + name_len, ".get") || !strcmp(method + name_len, ".set"))) {
            prop = i;
            *set = (method[name_len + 1] == 's');
        }
    }
    return prop;
}

/* Runs one method: a table entry, a property accessor or a media verb. NULL result: err is set. */
static cJSON *
rmm_rpc_invoke(const char *method, cJSON *params, rmm_rpc_err_t *err)
{
    rmm_rpc_fn  fn       = NULL;
    int         prop     = -1;
    int         prop_set = 0;
    int         media    = 0;
    int         mdev     = 0;
    int         mslot    = 0;
    const char *mverb    = NULL;
    cJSON      *empty;
    cJSON      *result;

    for (int i = 0; rmm_methods[i].name; i++)
        if (!strcmp(rmm_methods[i].name, method))
            fn = rmm_methods[i].fn;
    if (!fn)
        prop = rmm_prop_lookup(method, &prop_set);
    if (!fn && (prop < 0))
        media = rmm_media_parse(method, &mdev, &mslot, &mverb);
    if (!fn && (prop < 0) && !media)
        return rpc_fail(err, RPC_NO_METHOD, "method not found: %s", method);
    if (params && !cJSON_IsObject(params))
        return rpc_fail(err, RPC_INVALID_PARAMS, "params must be an object");

    empty = params ? NULL : cJSON_CreateObject();
    if (!params)
        params = empty;
    if (fn)
        result = fn(params, err);
    else if (media)
        result = rpc_media_call(mdev, mslot, mverb, params, err);
    else
        result = rpc_prop(prop, prop_set, params, err);
    cJSON_Delete(empty);
    return result;
}

/* Takes result and err.data. A request without an id is a notification: executed, never answered. */
static void
rmm_rpc_reply(cJSON *id, cJSON *result, rmm_rpc_err_t *err)
{
    if (id) {
        if (result) {
            cJSON *resp = cJSON_CreateObject();

            cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
            cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
            cJSON_AddItemToObject(resp, "result", result);
            rmm_send_json(resp);
            cJSON_Delete(resp);
            result = NULL;
        } else {
            rmm_rpc_reply_error(id, err->code, err->msg, err->data);
            err->data = NULL;
        }
    }
    cJSON_Delete(result);
    cJSON_Delete(err->data);
}

static void
rmm_json(const char *line)
{
    cJSON        *req = cJSON_Parse(line);
    cJSON        *id;
    cJSON        *method;
    cJSON        *ver;
    cJSON        *result;
    rmm_rpc_err_t err = { 0, "", NULL };

    json_seen = 1;
    if (!req) {
        rmm_rpc_reply_error(NULL, RPC_PARSE_ERROR, "parse error", NULL);
        return;
    }
    id     = cJSON_IsObject(req) ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
    method = cJSON_GetObjectItemCaseSensitive(req, "method");
    ver    = cJSON_GetObjectItemCaseSensitive(req, "jsonrpc");
    if (!cJSON_IsObject(req) || !cJSON_IsString(method) || !cJSON_IsString(ver) || strcmp(ver->valuestring, "2.0")) {
        rmm_rpc_reply_error(id, RPC_INVALID_REQ, "invalid request", NULL);
        cJSON_Delete(req);
        return;
    }

    rmm_log("RMM: rpc '%s'\n", method->valuestring);
    result = rmm_rpc_invoke(method->valuestring, cJSON_GetObjectItemCaseSensitive(req, "params"), &err);
    rmm_rpc_reply(id, result, &err);
    cJSON_Delete(req);

    if (quitting) {
        rmm_event("machine.exit", NULL);
        plat_rmm_quit();
    }
}

static const char rmm_help[] =
    "OK commands: status pause continue regs mem<addr,len> memw<addr,hex[,raw]> sendkey<k[+k]> type<text> "
    "savevm<tag> loadvm<tag> delvm<tag> snapshots devices quit help";

/* savevm/loadvm/delvm <tag>, as QEMU's HMP. The run state is kept; snapshot/restore
   are the old spellings. */
static void
rmm_cmd_snap(const char *verb, char *args)
{
    char  why[2048] = "";
    char *tag      = args;
    char *end;
    int   was_running;
    int   code;

    while (tag && ((*tag == ' ') || (*tag == '\t')))
        tag++;
    if (tag) {
        for (end = tag + strlen(tag); (end > tag) && ((end[-1] == ' ') || (end[-1] == '\t')); end--)
            ;
        *end = '\0';
    }
    if (!tag || !tag[0] || (strlen(tag) > 63) || strpbrk(tag, " \t")) {
        rmm_send("ERR tag must be 1..63 characters, no spaces");
        return;
    }
    if (!strcmp(verb, "delvm")) {
        if (rmm_snap_delete(tag) != 0)
            rmm_send("ERR 7 no snapshot '%s'", tag);
        else
            rmm_send("OK");
        return;
    }

    was_running = !dopause;
    if (was_running && !rmm_machine_run(0)) {
        rmm_send("ERR pause did not take effect");
        return;
    }
    if (!strcmp(verb, "savevm"))
        code = rmm_snap_save(tag, why, sizeof(why));
    else {
        rmm_watch_uninstall();
        code = rmm_snap_restore(tag, why, sizeof(why));
        if (rmm_watch_count)
            rmm_watch_install();
    }
    if (was_running && !rmm_machine_run(1)) {
        rmm_send("ERR resume did not take effect");
        return;
    }
    if (code)
        rmm_send("ERR %d %s", code, why);
    else
        rmm_send("OK %s", rmm_state());
}

/* exportvm TAG PATH / loadfile PATH: snapshot files. The rest of the line is the path.
   Pauses for the call; loadfile also restores. */
static void
rmm_cmd_snapfile(const char *verb, char *args)
{
    char  why[2048] = "";
    char  got[64]  = "";
    char *tag      = NULL;
    char *path     = args;
    char *end;
    int   was_running;
    int   code;

    while (path && ((*path == ' ') || (*path == '\t')))
        path++;
    if (path && !strcmp(verb, "exportvm")) {
        tag = path;
        path += strcspn(path, " \t");
        if (*path)
            *path++ = '\0';
        while ((*path == ' ') || (*path == '\t'))
            path++;
    }
    if (path) {
        for (end = path + strlen(path); (end > path) && ((end[-1] == ' ') || (end[-1] == '\t')); end--)
            ;
        *end = '\0';
    }
    if (!path || !path[0] || (tag && (!tag[0] || (strlen(tag) > 63)))) {
        rmm_send(strcmp(verb, "exportvm") ? "ERR usage: loadfile PATH" : "ERR usage: exportvm TAG PATH");
        return;
    }
    was_running = !dopause;
    if (was_running && !rmm_machine_run(0)) {
        rmm_send("ERR pause did not take effect");
        return;
    }
    if (tag)
        code = rmm_snap_export(tag, path, why, sizeof(why));
    else if (!(code = rmm_snap_load(path, NULL, got, sizeof(got), why, sizeof(why)))) {
        rmm_watch_uninstall();
        code = rmm_snap_restore(got, why, sizeof(why));
        if (rmm_watch_count)
            rmm_watch_install();
    }
    if (was_running && !rmm_machine_run(1)) {
        rmm_send("ERR resume did not take effect");
        return;
    }
    if (code)
        rmm_send("ERR %d %s", code, why);
    else if (tag)
        rmm_send("OK %s", rmm_state());
    else
        rmm_send("OK %s %s", got, rmm_state());
}

/* The machine's devices by internal name, in device-table order (the snapshot's). */
extern int device_rmm_get(int c, const device_t **dev, void **priv);

static void
rmm_cmd_devices(void)
{
    char           line[RMM_LINE_MAX];
    size_t         used = (size_t) snprintf(line, sizeof(line), "OK");
    const device_t *dev;
    void           *priv;

    for (int dev_idx = 0; device_rmm_get(dev_idx, &dev, &priv); dev_idx++) {
        if (dev && dev->internal_name && (used + strlen(dev->internal_name) + 2 < sizeof(line)))
            used += snprintf(line + used, sizeof(line) - used, " %s", dev->internal_name);
    }
    rmm_send("%s", line);
}

static void
rmm_cmd_snapshots(void)
{
    char     line[RMM_LINE_MAX];
    size_t   used = (size_t) snprintf(line, sizeof(line), "OK");
    uint64_t snap_tsc;

    for (int i = 0; i < rmm_snap_slots(); i++) {
        const char *tag = rmm_snap_tag(i, &snap_tsc);

        if (tag && (used < sizeof(line)))
            used += snprintf(line + used, sizeof(line) - used, " %s", tag);
    }
    rmm_send("%s", line);
}

static void
rmm_cmd_run(int run)
{
    if (rmm_machine_run(run))
        rmm_cmd_status();
    else
        rmm_send(run ? "ERR resume did not take effect" : "ERR pause did not take effect");
}

static void
rmm_cmd_sendkey(char *args)
{
    if (args && rmm_sendkey(args))
        rmm_send("OK");
    else
        rmm_send("ERR unknown key");
}

static void
rmm_cmd_type(char *args)
{
    if (args && rmm_type(args))
        rmm_send("OK");
    else
        rmm_send("ERR untypeable character");
}

/* The snapshot verbs, under their aliases; NULL if cmd is not one. */
static const char *
rmm_snap_verb(const char *cmd)
{
    if (!strcasecmp(cmd, "savevm") || !strcasecmp(cmd, "snapshot"))
        return "savevm";
    if (!strcasecmp(cmd, "loadvm") || !strcasecmp(cmd, "restore"))
        return "loadvm";
    return !strcasecmp(cmd, "delvm") ? "delvm" : NULL;
}

/* Returns 0 when the client asked to leave. */
static int
rmm_dispatch(char *line)
{
    const char *snap;
    char       *args = NULL;
    char       *cmd  = line;

    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (*cmd == '{') {
        rmm_json(cmd);
        return !quitting;
    }
    if (!*cmd) {
        rmm_send("ERR empty command");
        return 1;
    }
    if ((args = strchr(cmd, ' ')) != NULL)
        *args++ = '\0';

    rmm_log("RMM: command '%s' args '%s'\n", cmd, args ? args : "");

    if ((snap = rmm_snap_verb(cmd)) != NULL)
        rmm_cmd_snap(snap, args);
    else if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?"))
        rmm_send("%s", rmm_help);
    else if (!strcasecmp(cmd, "status") || !strcasecmp(cmd, "info"))
        rmm_cmd_status();
    else if (!strcasecmp(cmd, "pause") || !strcasecmp(cmd, "stop"))
        rmm_cmd_run(0);
    else if (!strcasecmp(cmd, "continue") || !strcasecmp(cmd, "cont") || !strcasecmp(cmd, "c"))
        rmm_cmd_run(1);
    else if (!strcasecmp(cmd, "regs"))
        rmm_cmd_regs();
    else if (!strcasecmp(cmd, "mem") || !strcasecmp(cmd, "x") || !strcasecmp(cmd, "mem.phys.read"))
        rmm_cmd_mem(args);
    else if (!strcasecmp(cmd, "memw") || !strcasecmp(cmd, "mem.phys.write"))
        rmm_cmd_memw(args);
    else if (!strcasecmp(cmd, "sendkey"))
        rmm_cmd_sendkey(args);
    else if (!strcasecmp(cmd, "type"))
        rmm_cmd_type(args);
    else if (!strcasecmp(cmd, "snapshots"))
        rmm_cmd_snapshots();
    else if (!strcasecmp(cmd, "devices"))
        rmm_cmd_devices();
    else if (!strcasecmp(cmd, "exportvm") || !strcasecmp(cmd, "loadfile"))
        rmm_cmd_snapfile(strcasecmp(cmd, "exportvm") ? "loadfile" : "exportvm", args);
    else if (!strcasecmp(cmd, "quit") || !strcasecmp(cmd, "exit")) {
        rmm_send("OK bye");
        return 0;
    } else
        rmm_send("ERR unknown command '%s'", cmd);

    return 1;
}

/* ----------------------------------------------------------- server */

static void
rmm_session_begin(void)
{
    paused_by_us = 0;
    pause_auto   = 0;
    json_seen    = 0;
    client_dead  = 0;
    if (rmm_pause_on_connect && !rmm_port_cmdline && !dopause) {
        paused_by_us = 1;
        pause_auto   = 1;
        rmm_set_pause(1);
    }
    rmm_send("OK 86Box RMM " RMM_VERSION " %s", rmm_state());
}

/* Hand the machine back as found: no key or button held, no breakpoint, resumed if we
   paused it. */
static void
rmm_session_end(void)
{
    if (rmm_running && !quitting) {
        cJSON_Delete(rmm_input_release(1));
        rmm_debug_reset();
    }
    if (paused_by_us && rmm_running && !quitting && !(rmm_sticky_pause && !pause_auto))
        rmm_set_pause(0);
    paused_by_us = 0;
    pause_auto   = 0;
}

/* Waits up to 5 ms for the client and turns a breakpoint hit into a pause and event.
   1: data to read, 0: nothing yet, -1: the connection failed. */
static int
rmm_session_wait(void)
{
    struct timeval tv = { 0, 5000 };
    fd_set         rd;
    int            ready;

    FD_ZERO(&rd);
    FD_SET(client_socket, &rd);
    ready = select((int) client_socket + 1, &rd, NULL, NULL, &tv);
    rmm_debug_poll();
    rmm_input_poll();
    if (ready > 0)
        return 1;
#ifndef _WIN32
    if ((ready < 0) && (errno != EINTR))
        return -1;
#else
    if (ready < 0)
        return -1;
#endif
    return 0;
}

/* Dispatches the complete lines in buf[0..*pos) and keeps the partial one.
   Returns 0 when the client asked to end the session. */
static int
rmm_session_lines(char *buf, int *pos, int *discarding)
{
    char *start = buf;
    char *nl;

    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (*discarding) { /* the end of the over-long line: already answered */
            *discarding = 0;
            start       = nl + 1;
            continue;
        }
        if ((nl > start) && (nl[-1] == '\r'))
            nl[-1] = '\0';
        if (!rmm_dispatch(start))
            return 0;
        start = nl + 1;
    }
    *pos -= (int) (start - buf);
    memmove(buf, start, *pos);
    if (*discarding)
        *pos = 0;
    else if (*pos >= RMM_LINE_MAX - 1) {
        /* One error for the line; the rest is dropped up to its newline. */
        rmm_send("ERR line too long");
        *pos        = 0;
        *discarding = 1;
    }
    return 1;
}

static void
rmm_client_session(void)
{
    char buf[RMM_LINE_MAX];
    int  pos        = 0;
    int  discarding = 0; /* inside an over-long line: drop bytes up to its newline */
    int  got;
    int  ready;

    rmm_session_begin();
    while (rmm_running && !client_dead) {
        if ((ready = rmm_session_wait()) < 0)
            break;
        if (!ready)
            continue;
        if ((got = recv(client_socket, buf + pos, sizeof(buf) - 1 - pos, 0)) <= 0)
            break;
        pos += got;
        buf[pos] = '\0';
        if (!rmm_session_lines(buf, &pos, &discarding))
            break;
    }
    rmm_session_end();
}

static void
rmm_server_thread(UNUSED(void *priv))
{
    struct sockaddr_in addr;
    socklen_t          sl = sizeof(addr);

    listen(listen_socket, 1);

    while (rmm_running) {
        rmm_socket_t sock;

        sl = sizeof(addr);
        sock  = accept(listen_socket, (struct sockaddr *) &addr, &sl);
        if (sock == RMM_BAD_SOCKET) {
            /* Only shutdown ends the listener. */
            if (!rmm_running || (listen_socket == RMM_BAD_SOCKET))
                break;
            plat_delay_ms(50);
            continue;
        }

#ifdef _WIN32
        {
            DWORD ms = RMM_SEND_TIMEOUT_MS;
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *) &ms, sizeof(ms));
        }
#else
        {
            struct timeval tv = { RMM_SEND_TIMEOUT_MS / 1000, 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const void *) &tv, sizeof(tv));
        }
#endif

#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (const void *) &one, sizeof(one));
        }
#endif
        client_socket = sock;
        pclog("RMM: client connected\n");
        rmm_client_session();
        pclog("RMM: client disconnected\n");
        client_socket = RMM_BAD_SOCKET;
        rmm_closesocket(sock);
    }
}

void
rmm_init(void)
{
    int port = rmm_port_cmdline ? rmm_port_cmdline : rmm_port;
    int yes  = 1;

    if (rmm_sticky_cmdline)
        rmm_sticky_pause = 1; /* the config is loaded after the command line, so apply it here */
    if (port <= 0)
        return;
    if (rmm_methods_check())
        pclog("RMM: the method table above has errors; rpc.discover will publish them\n");

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    if ((listen_socket = socket(AF_INET, SOCK_STREAM, 0)) == RMM_BAD_SOCKET) {
        pclog("RMM: failed to create socket\n");
        return;
    }
#ifdef _WIN32
    /* Windows SO_REUSEADDR lets a second process share the port; bind exclusively. */
    setsockopt(listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &yes, sizeof(yes));
#else
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &yes, sizeof(yes));
#endif

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_addr   = { .s_addr = htonl(INADDR_LOOPBACK) },
        .sin_port   = htons(port)
    };
    if (bind(listen_socket, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) != 0) {
        pclog("RMM: failed to bind 127.0.0.1:%d\n", port);
        /* pclog is silent without a log file; launchers also see stderr */
        fprintf(stderr, "RMM: failed to bind 127.0.0.1:%d\n", port);
        rmm_closesocket(listen_socket);
        listen_socket = RMM_BAD_SOCKET;
        return;
    }

    rmm_pending_reset_apply_lock = thread_create_mutex();
    video_rmm_capture_init();
    rmm_running = 1;
    pclog("RMM: listening on 127.0.0.1:%d (pause on connect: %s, sticky pause: %s)\n", port,
          (rmm_pause_on_connect && !rmm_port_cmdline) ? "yes" : "no", rmm_sticky_pause ? "yes" : "no");
    thread_create(rmm_server_thread, NULL);
}

void
rmm_close(void)
{
    rmm_snap_shutdown();
    if (listen_socket == RMM_BAD_SOCKET)
        return;

    rmm_running = 0;
    if (client_socket != RMM_BAD_SOCKET)
        shutdown(client_socket, 2);
#ifndef _WIN32
    shutdown(listen_socket, SHUT_RDWR);
#endif
    rmm_closesocket(listen_socket);
    listen_socket = RMM_BAD_SOCKET;
}
