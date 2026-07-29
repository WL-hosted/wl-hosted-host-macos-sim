#include "repl.h"

#include "repl_json.h"

#include "linenoise.h"

#include "wlh/log.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPL_MODE_IPC 0x01u
#define REPL_MODE_USB 0x02u
#define REPL_MODE_ANY (REPL_MODE_IPC | REPL_MODE_USB)

#define REPL_MAX_LINE 4096u
#define REPL_MAX_TOKENS 8u
#define REPL_PING_ID_BASE 0x60000000u
#define REPL_PING_ID_MASK 0xf0000000u

typedef struct repl_command {
    const char *name;
    int min_args;
    int max_args; /* -1: pass the raw remainder as a single argument */
    unsigned modes;
    int (*handler)(app_t *app, int argc, char **argv, bool *quit);
    const char *help;
} repl_command_t;

/* ---- predicates (run under state_mutex inside wlh_app_wait_until) ------- */

static bool line_or_eof(app_t *app) {
    return app->repl.line_ready || app->repl.stdin_eof;
}
static bool op_done(app_t *app) {
    return app->repl.op_done;
}
static bool scan_completed(app_t *app) {
    return app->scan_complete;
}
static bool connect_settled(app_t *app) {
    return app->connected || app->disconnected;
}
static bool wifi_disconnected(app_t *app) {
    return app->disconnected;
}
static bool ethernet_echoed(app_t *app) {
    return app->ethernet_rx;
}
static bool user_result_seen(app_t *app) {
    return app->user_result_received;
}
static bool ping_finished(app_t *app) {
    return app->repl.ping_done;
}
static bool host_ready(app_t *app) {
    wlh_host_diagnostics_t diagnostics;
    wlh_host_get_diagnostics(&app->host, &diagnostics);
    return diagnostics.state == WLH_HOST_STATE_READY;
}

/* ---- shared completion plumbing ----------------------------------------- */

static void repl_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    app_t *app = context;
    (void)payload;
    (void)payload_size;
    pthread_mutex_lock(&app->state_mutex);
    app->repl.op_done = true;
    app->repl.op_result = result;
    app->repl.op_domain = domain;
    app->repl.op_status = status;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

static void repl_device_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_device_info_t *info
) {
    app_t *app = context;
    pthread_mutex_lock(&app->state_mutex);
    app->repl.op_done = true;
    app->repl.op_result = result;
    app->repl.op_domain = domain;
    app->repl.op_status = status;
    app->repl.device_info_ok = result == WLH_HOST_OK && info != NULL;
    if (app->repl.device_info_ok)
        app->repl.device_info = *info;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

static void begin_op(app_t *app) {
    pthread_mutex_lock(&app->state_mutex);
    app->repl.op_done = false;
    pthread_mutex_unlock(&app->state_mutex);
}

/* Waits for the pending RPC completion; on submit/timeout/RPC failure emits
 * an error line and returns the failing code. Returns WLH_HOST_OK only when
 * the completion fired with a successful result. */
static wlh_host_result_t finish_op(
    app_t *app,
    const char *command,
    wlh_host_result_t submit_result,
    uint32_t timeout_ms
) {
    wlh_host_result_t result;
    uint16_t domain = 0u;
    int16_t status = 0;
    /* host-core completes every accepted RPC within rpc_timeout_ms (response,
     * timeout, or session change). Waiting past that bound guarantees op_done
     * cannot fire late and leak into the next command's wait. */
    uint32_t completion_bound = app->host.config.rpc_timeout_ms + 2000u;
    if (timeout_ms < completion_bound)
        timeout_ms = completion_bound;
    if (submit_result != WLH_HOST_OK) {
        result = submit_result;
    } else if (!wlh_app_wait_until(app, op_done, timeout_ms)) {
        result = WLH_HOST_TIMEOUT;
    } else {
        pthread_mutex_lock(&app->state_mutex);
        result = app->repl.op_result;
        domain = app->repl.op_domain;
        status = app->repl.op_status;
        pthread_mutex_unlock(&app->state_mutex);
    }
    if (result != WLH_HOST_OK) {
        cJSON *doc = wlh_repl_json_begin("error");
        if (doc != NULL) {
            cJSON_AddStringToObject(doc, "command", command);
            cJSON_AddNumberToObject(doc, "result", result);
            cJSON_AddNumberToObject(doc, "status_domain", domain);
            cJSON_AddNumberToObject(doc, "status_code", status);
        }
        wlh_repl_json_emit(doc);
    }
    return result;
}

static void emit_error(
    app_t *app, const char *command, int result, const char *detail
) {
    cJSON *doc = wlh_repl_json_begin("error");
    (void)app;
    if (doc != NULL) {
        cJSON_AddStringToObject(doc, "command", command);
        cJSON_AddNumberToObject(doc, "result", result);
        cJSON_AddStringToObject(doc, "detail", detail);
    }
    wlh_repl_json_emit(doc);
}

static wlh_host_result_t require_ready(app_t *app, const char *command) {
    if (wlh_app_wait_until(app, host_ready, 10000u))
        return WLH_HOST_OK;
    emit_error(app, command, WLH_HOST_INVALID_STATE, "host session not READY");
    return WLH_HOST_INVALID_STATE;
}

static wlh_host_result_t ensure_wifi_initialized(
    app_t *app, const char *command
) {
    bool initialized;
    wlh_host_result_t result = require_ready(app, command);
    if (result != WLH_HOST_OK)
        return result;
    pthread_mutex_lock(&app->state_mutex);
    initialized = app->repl.wifi_initialized;
    pthread_mutex_unlock(&app->state_mutex);
    if (initialized)
        return WLH_HOST_OK;
    begin_op(app);
    result = finish_op(
        app,
        command,
        wlh_host_wifi_initialize(&app->host, repl_completion, app),
        3000u
    );
    if (result == WLH_HOST_OK) {
        pthread_mutex_lock(&app->state_mutex);
        app->repl.wifi_initialized = true;
        pthread_mutex_unlock(&app->state_mutex);
    }
    return result;
}

/* ---- command handlers ---------------------------------------------------- */

static int cmd_scan(app_t *app, int argc, char **argv, bool *quit) {
    wlh_wifi_scan_params_t scan = {0u, NULL, 0u, true, 8u};
    static uint32_t scan_id = 1u;
    wlh_host_result_t result = ensure_wifi_initialized(app, argv[0]);
    (void)quit;
    if (result != WLH_HOST_OK)
        return result;
    scan.scan_id = scan_id++;
    if (argc > 1) {
        scan.ssid = (const uint8_t *)argv[1];
        scan.ssid_size = strlen(argv[1]);
    }
    pthread_mutex_lock(&app->state_mutex);
    app->scan_complete = false;
    pthread_mutex_unlock(&app->state_mutex);
    begin_op(app);
    result = finish_op(
        app,
        argv[0],
        wlh_host_wifi_scan(&app->host, &scan, repl_completion, app),
        3000u
    );
    if (result != WLH_HOST_OK)
        return result;
    if (!wlh_app_wait_until(app, scan_completed, 5000u)) {
        emit_error(app, argv[0], WLH_HOST_TIMEOUT, "scan did not complete");
        return WLH_HOST_TIMEOUT;
    }
    return WLH_HOST_OK;
}

static int cmd_connect(app_t *app, int argc, char **argv, bool *quit) {
    wlh_wifi_connect_params_t params;
    wlh_host_result_t result = ensure_wifi_initialized(app, argv[0]);
    bool now_connected;
    (void)quit;
    if (result != WLH_HOST_OK)
        return result;
    memset(&params, 0, sizeof(params));
    params.ssid = (const uint8_t *)argv[1];
    params.ssid_size = strlen(argv[1]);
    if (argc > 2) {
        params.credential = (const uint8_t *)argv[2];
        params.credential_size = strlen(argv[2]);
        params.security = 4u; /* WPA2-PSK */
    } else {
        params.security = 1u; /* Open */
    }
    params.timeout_ms = 10000u;
    pthread_mutex_lock(&app->state_mutex);
    app->connected = false;
    app->disconnected = false;
    pthread_mutex_unlock(&app->state_mutex);
    begin_op(app);
    result = finish_op(
        app,
        argv[0],
        wlh_host_wifi_connect(&app->host, &params, repl_completion, app),
        12000u
    );
    if (result != WLH_HOST_OK)
        return result;
    if (!wlh_app_wait_until(app, connect_settled, 15000u)) {
        emit_error(
            app, argv[0], WLH_HOST_TIMEOUT, "no connect/disconnect event"
        );
        return WLH_HOST_TIMEOUT;
    }
    pthread_mutex_lock(&app->state_mutex);
    now_connected = app->connected;
    pthread_mutex_unlock(&app->state_mutex);
    if (!now_connected) {
        emit_error(app, argv[0], WLH_HOST_INVALID_STATE, "association failed");
        return WLH_HOST_INVALID_STATE;
    }
    return WLH_HOST_OK;
}

static int cmd_disconnect(app_t *app, int argc, char **argv, bool *quit) {
    wlh_host_result_t result = require_ready(app, argv[0]);
    (void)argc;
    (void)quit;
    if (result != WLH_HOST_OK)
        return result;
    pthread_mutex_lock(&app->state_mutex);
    app->disconnected = false;
    pthread_mutex_unlock(&app->state_mutex);
    begin_op(app);
    result = finish_op(
        app,
        argv[0],
        wlh_host_wifi_disconnect(&app->host, repl_completion, app),
        3000u
    );
    if (result != WLH_HOST_OK)
        return result;
    (void)wlh_app_wait_until(app, wifi_disconnected, 3000u);
    return WLH_HOST_OK;
}

static const char *state_name(wlh_host_state_t state) {
    switch (state) {
    case WLH_HOST_STATE_UNINITIALIZED:
        return "uninitialized";
    case WLH_HOST_STATE_TRANSPORT_STARTING:
        return "transport-starting";
    case WLH_HOST_STATE_WAITING_FOR_PEER:
        return "waiting-for-peer";
    case WLH_HOST_STATE_NEGOTIATING:
        return "negotiating";
    case WLH_HOST_STATE_READY:
        return "ready";
    case WLH_HOST_STATE_CONGESTED:
        return "congested";
    case WLH_HOST_STATE_RECOVERING:
        return "recovering";
    case WLH_HOST_STATE_FAILED:
        return "failed";
    case WLH_HOST_STATE_STOPPING:
        return "stopping";
    default:
        return "unknown";
    }
}

static int cmd_status(app_t *app, int argc, char **argv, bool *quit) {
    wlh_host_diagnostics_t diagnostics;
    bool is_connected;
    cJSON *doc;
    (void)argc;
    (void)argv;
    (void)quit;
    wlh_host_get_diagnostics(&app->host, &diagnostics);
    pthread_mutex_lock(&app->state_mutex);
    is_connected = app->connected && !app->disconnected;
    pthread_mutex_unlock(&app->state_mutex);
    doc = wlh_repl_json_begin("status");
    if (doc != NULL) {
        cJSON_AddStringToObject(doc, "state", state_name(diagnostics.state));
        cJSON_AddNumberToObject(doc, "session_id", diagnostics.session_id);
        cJSON_AddNumberToObject(doc, "tx_frames", diagnostics.tx_frames);
        cJSON_AddNumberToObject(doc, "rx_frames", diagnostics.rx_frames);
        cJSON_AddNumberToObject(doc, "pending_rpc", diagnostics.pending_rpc);
        cJSON_AddBoolToObject(doc, "wifi_connected", is_connected);
        cJSON_AddStringToObject(
            doc, "peer_version", wlh_host_get_peer_version(&app->host)
        );
        cJSON_AddNumberToObject(
            doc, "uptime_ms", (double)(wlh_app_monotonic_ms() - app->started_ms)
        );
        cJSON_AddStringToObject(doc, "transport", app->use_usb ? "usb" : "ipc");
    }
    wlh_repl_json_emit(doc);
    return WLH_HOST_OK;
}

static int cmd_device_info(app_t *app, int argc, char **argv, bool *quit) {
    wlh_host_result_t result = require_ready(app, argv[0]);
    wlh_host_device_info_t info;
    bool ok;
    cJSON *doc;
    (void)argc;
    (void)quit;
    if (result != WLH_HOST_OK)
        return result;
    pthread_mutex_lock(&app->state_mutex);
    app->repl.device_info_ok = false;
    pthread_mutex_unlock(&app->state_mutex);
    begin_op(app);
    result = finish_op(
        app,
        argv[0],
        wlh_host_get_device_info(&app->host, repl_device_info_completion, app),
        3000u
    );
    if (result != WLH_HOST_OK)
        return result;
    pthread_mutex_lock(&app->state_mutex);
    ok = app->repl.device_info_ok;
    info = app->repl.device_info;
    pthread_mutex_unlock(&app->state_mutex);
    if (!ok) {
        emit_error(app, argv[0], WLH_HOST_PROTOCOL_ERROR, "no device info");
        return WLH_HOST_PROTOCOL_ERROR;
    }
    doc = wlh_repl_json_begin("device_info");
    if (doc != NULL) {
        cJSON_AddStringToObject(doc, "vendor", info.vendor);
        cJSON_AddStringToObject(doc, "mcu_model", info.mcu_model);
        cJSON_AddStringToObject(doc, "board_profile", info.board_profile);
        wlh_repl_json_add_hex(doc, "uid", info.uid, info.uid_size);
    }
    wlh_repl_json_emit(doc);
    return WLH_HOST_OK;
}

static int cmd_user_message(app_t *app, int argc, char **argv, bool *quit) {
    wlh_host_result_t result = require_ready(app, argv[0]);
    (void)argc;
    (void)quit;
    if (result != WLH_HOST_OK)
        return result;
    pthread_mutex_lock(&app->state_mutex);
    app->user_result_received = false;
    pthread_mutex_unlock(&app->state_mutex);
    begin_op(app);
    result = finish_op(
        app,
        argv[0],
        wlh_host_user_message_send(
            &app->host,
            1u,
            1u,
            1u /* EXPECT_RESULT */,
            (const uint8_t *)argv[1],
            strlen(argv[1]),
            repl_completion,
            app
        ),
        3000u
    );
    if (result != WLH_HOST_OK)
        return result;
    /* A RESULT event is optional; give it a short window. The event line is
     * emitted by the host-event hook when it arrives. */
    (void)wlh_app_wait_until(app, user_result_seen, 1500u);
    return WLH_HOST_OK;
}

static int cmd_eth_echo(app_t *app, int argc, char **argv, bool *quit) {
    uint8_t ethernet[60] = {0x02, 0, 0, 0, 0, 2, 0x02, 0, 0, 0, 0, 1};
    wlh_host_result_t result;
    bool is_connected;
    (void)argc;
    (void)quit;
    pthread_mutex_lock(&app->state_mutex);
    is_connected = app->connected && !app->disconnected;
    app->ethernet_rx = false;
    pthread_mutex_unlock(&app->state_mutex);
    if (!is_connected) {
        emit_error(app, argv[0], WLH_HOST_INVALID_STATE, "not connected");
        return WLH_HOST_INVALID_STATE;
    }
    result = wlh_host_ethernet_sta_send(&app->host, ethernet, sizeof(ethernet));
    if (result != WLH_HOST_OK) {
        emit_error(app, argv[0], result, "ethernet send rejected");
        return result;
    }
    if (!wlh_app_wait_until(app, ethernet_echoed, 3000u)) {
        emit_error(app, argv[0], WLH_HOST_TIMEOUT, "no echo frame");
        return WLH_HOST_TIMEOUT;
    }
    return WLH_HOST_OK;
}

static int cmd_ping(app_t *app, int argc, char **argv, bool *quit) {
    static uint32_t next_ping_id = REPL_PING_ID_BASE;
    uint32_t count = 1u;
    uint32_t timeout_ms = 2000u;
    uint32_t request_id;
    (void)quit;
    if (argc > 2) {
        count = (uint32_t)strtoul(argv[2], NULL, 10);
        if (count < 1u || count > 10u) {
            emit_error(
                app, argv[0], WLH_HOST_INVALID_ARGUMENT, "count must be 1-10"
            );
            return WLH_HOST_INVALID_ARGUMENT;
        }
    }
    if (argc > 3) {
        timeout_ms = (uint32_t)strtoul(argv[3], NULL, 10);
        if (timeout_ms < 1u || timeout_ms > 60000u) {
            emit_error(
                app,
                argv[0],
                WLH_HOST_INVALID_ARGUMENT,
                "timeout_ms must be 1-60000"
            );
            return WLH_HOST_INVALID_ARGUMENT;
        }
    }
    if (app->network == NULL) {
        emit_error(app, argv[0], WLH_HOST_INVALID_STATE, "network unavailable");
        return WLH_HOST_INVALID_STATE;
    }
    request_id = ++next_ping_id;
    pthread_mutex_lock(&app->state_mutex);
    app->repl.ping_request_id = request_id;
    app->repl.ping_done = false;
    pthread_mutex_unlock(&app->state_mutex);
    if (sim_network_ping(
            app->network, request_id, argv[1], count, timeout_ms
        ) != 0) {
        emit_error(app, argv[0], WLH_HOST_INVALID_STATE, "ping not started");
        return WLH_HOST_INVALID_STATE;
    }
    if (!wlh_app_wait_until(app, ping_finished, count * timeout_ms + 3000u)) {
        emit_error(app, argv[0], WLH_HOST_TIMEOUT, "ping result missing");
        return WLH_HOST_TIMEOUT;
    }
    return WLH_HOST_OK;
}

static int cmd_ble(app_t *app, int argc, char **argv, bool *quit) {
    bool peripheral;
    int run_result;
    cJSON *doc;
    (void)argc;
    (void)quit;
    if (strcmp(argv[1], "central") == 0)
        peripheral = false;
    else if (strcmp(argv[1], "peripheral") == 0)
        peripheral = true;
    else {
        emit_error(
            app,
            argv[0],
            WLH_HOST_INVALID_ARGUMENT,
            "expected 'central' or 'peripheral'"
        );
        return WLH_HOST_INVALID_ARGUMENT;
    }
    if (!wlh_app_wait_until(app, host_ready, 5000u)) {
        emit_error(app, argv[0], WLH_HOST_TIMEOUT, "host session not READY");
        return WLH_HOST_TIMEOUT;
    }
    doc = wlh_repl_json_begin("ble_begin");
    if (doc != NULL)
        cJSON_AddStringToObject(
            doc, "role", peripheral ? "peripheral" : "central"
        );
    wlh_repl_json_emit(doc);
    if (wlh_ble_app_start(&app->host, &app->osal_ops, &app->ble) != 0) {
        wlh_ble_app_stop();
        emit_error(app, argv[0], WLH_HOST_INVALID_STATE, "BLE start failed");
        return WLH_HOST_INVALID_STATE;
    }
    run_result = peripheral ? wlh_ble_run_peripheral() : wlh_ble_run_central();
    wlh_ble_app_stop();
    if (run_result != 0) {
        emit_error(app, argv[0], WLH_HOST_PROTOCOL_ERROR, "BLE run failed");
        return WLH_HOST_PROTOCOL_ERROR;
    }
    return WLH_HOST_OK;
}

static int cmd_help(app_t *app, int argc, char **argv, bool *quit);

static int cmd_quit(app_t *app, int argc, char **argv, bool *quit) {
    (void)app;
    (void)argc;
    (void)argv;
    *quit = true;
    return WLH_HOST_OK;
}

static const repl_command_t commands[] = {
    {"scan",
     0,
     1,
     REPL_MODE_ANY,
     cmd_scan,
     "scan [ssid] - Wi-Fi scan (results stream as scan_result events)"},
    {"connect",
     1,
     2,
     REPL_MODE_ANY,
     cmd_connect,
     "connect <ssid> [credential] - join an AP (Open or WPA2-PSK)"},
    {"disconnect",
     0,
     0,
     REPL_MODE_ANY,
     cmd_disconnect,
     "disconnect - leave the current AP"},
    {"status",
     0,
     0,
     REPL_MODE_ANY,
     cmd_status,
     "status - host state, counters and link summary"},
    {"device-info",
     0,
     0,
     REPL_MODE_ANY,
     cmd_device_info,
     "device-info - query the Device Information service"},
    {"user-message",
     1,
     -1,
     REPL_MODE_ANY,
     cmd_user_message,
     "user-message <text> - send a User Passthrough message"},
    {"eth-echo",
     0,
     0,
     REPL_MODE_IPC,
     cmd_eth_echo,
     "eth-echo - send a test Ethernet frame (mock coprocessor echo)"},
    {"ping",
     1,
     3,
     REPL_MODE_ANY,
     cmd_ping,
     "ping <host> [count] [timeout_ms] - ICMP ping over the STA link"},
    {"ble",
     1,
     1,
     REPL_MODE_USB,
     cmd_ble,
     "ble central|peripheral - run the BLE scenario (blocks until done)"},
    {"help", 0, 0, REPL_MODE_ANY, cmd_help, "help - list commands"},
    {"quit", 0, 0, REPL_MODE_ANY, cmd_quit, "quit - exit the REPL"},
    {"exit", 0, 0, REPL_MODE_ANY, cmd_quit, "exit - exit the REPL"},
};

static int cmd_help(app_t *app, int argc, char **argv, bool *quit) {
    cJSON *doc = wlh_repl_json_begin("help");
    (void)app;
    (void)argc;
    (void)argv;
    (void)quit;
    if (doc != NULL) {
        cJSON *list = cJSON_AddArrayToObject(doc, "commands");
        for (size_t index = 0u;
             list != NULL && index < sizeof(commands) / sizeof(commands[0]);
             ++index) {
            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL)
                continue;
            cJSON_AddStringToObject(entry, "name", commands[index].name);
            cJSON_AddStringToObject(entry, "help", commands[index].help);
            if (commands[index].modes != REPL_MODE_ANY)
                cJSON_AddStringToObject(
                    entry,
                    "requires",
                    commands[index].modes == REPL_MODE_USB ? "usb" : "ipc"
                );
            if (!cJSON_AddItemToArray(list, entry))
                cJSON_Delete(entry);
        }
    }
    wlh_repl_json_emit(doc);
    return WLH_HOST_OK;
}

/* ---- line tokenizer ------------------------------------------------------ */

/* Splits in place on whitespace; double quotes group words (no escapes).
 * Returns the token count, or -1 on an unterminated quote / overflow. */
static int tokenize(char *line, char **tokens, size_t max_tokens) {
    size_t count = 0u;
    char *cursor = line;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor == '\0')
            break;
        if (count == max_tokens)
            return -1;
        if (*cursor == '"') {
            cursor++;
            tokens[count++] = cursor;
            while (*cursor != '\0' && *cursor != '"')
                cursor++;
            if (*cursor != '"')
                return -1;
        } else {
            tokens[count++] = cursor;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
                   *cursor != '"')
                cursor++;
            if (*cursor == '"')
                return -1;
        }
        if (*cursor != '\0')
            *cursor++ = '\0';
    }
    return (int)count;
}

static void dispatch_line(app_t *app, char *line, bool *quit) {
    char *tokens[REPL_MAX_TOKENS];
    char raw[REPL_MAX_LINE + 1u];
    int argc;
    const repl_command_t *command = NULL;
    unsigned mode = app->use_usb ? REPL_MODE_USB : REPL_MODE_IPC;
    int result;
    cJSON *doc;

    (void)snprintf(raw, sizeof(raw), "%s", line);
    argc = tokenize(line, tokens, REPL_MAX_TOKENS);
    if (argc < 0) {
        emit_error(
            app,
            "",
            WLH_HOST_INVALID_ARGUMENT,
            "unterminated quote or too many tokens"
        );
        return;
    }
    if (argc == 0)
        return;
    for (size_t index = 0u; index < sizeof(commands) / sizeof(commands[0]);
         ++index) {
        if (strcmp(tokens[0], commands[index].name) == 0) {
            command = &commands[index];
            break;
        }
    }
    if (command == NULL) {
        emit_error(app, tokens[0], WLH_HOST_NOT_FOUND, "unknown command");
        return;
    }
    if ((command->modes & mode) == 0u) {
        emit_error(
            app,
            command->name,
            WLH_HOST_NOT_SUPPORTED,
            command->modes == REPL_MODE_USB ? "requires --usb"
                                            : "requires --ipc"
        );
        return;
    }
    if (command->max_args == -1) {
        /* Raw remainder: everything after the command name, trimmed. */
        char *rest = raw + strlen(command->name);
        while (*rest == ' ' || *rest == '\t')
            rest++;
        if (*rest == '\0') {
            emit_error(
                app, command->name, WLH_HOST_INVALID_ARGUMENT, "missing text"
            );
            return;
        }
        tokens[1] = rest;
        argc = 2;
    } else if (argc - 1 < command->min_args || argc - 1 > command->max_args) {
        emit_error(
            app, command->name, WLH_HOST_INVALID_ARGUMENT, command->help
        );
        return;
    }
    result = command->handler(app, argc, tokens, quit);
    doc = wlh_repl_json_begin("result");
    if (doc != NULL) {
        cJSON_AddStringToObject(doc, "command", command->name);
        cJSON_AddNumberToObject(doc, "result", result);
    }
    wlh_repl_json_emit(doc);
}

/* ---- stdin reader thread ------------------------------------------------- */

static void sigusr1_noop(int signal_number) {
    (void)signal_number;
}

static void *reader_main(void *context) {
    app_t *app = context;
    bool tty = isatty(STDIN_FILENO) != 0;
    for (;;) {
        char *line;
        if (atomic_load(&app->repl.reader_stop))
            break;
        line = linenoise(tty ? "wlh> " : "");
        if (atomic_load(&app->repl.reader_stop)) {
            free(line);
            break;
        }
        if (line == NULL) {
            /* EOF, or Ctrl-C/Ctrl-D captured by linenoise in raw mode. */
            pthread_mutex_lock(&app->state_mutex);
            app->repl.stdin_eof = true;
            pthread_cond_broadcast(&app->state_changed);
            pthread_mutex_unlock(&app->state_mutex);
            break;
        }
        if (strlen(line) > REPL_MAX_LINE) {
            emit_error(
                app, "", WLH_HOST_INVALID_ARGUMENT, "line exceeds 4096 bytes"
            );
            free(line);
            continue;
        }
        if (tty && line[0] != '\0')
            linenoiseHistoryAdd(line);
        pthread_mutex_lock(&app->state_mutex);
        while (app->repl.line_ready && !atomic_load(&app->repl.reader_stop))
            pthread_cond_wait(&app->state_changed, &app->state_mutex);
        if (atomic_load(&app->repl.reader_stop)) {
            pthread_mutex_unlock(&app->state_mutex);
            free(line);
            break;
        }
        app->repl.pending_line = line;
        app->repl.line_ready = true;
        pthread_cond_broadcast(&app->state_changed);
        pthread_mutex_unlock(&app->state_mutex);
    }
    return NULL;
}

/* ---- main loop ------------------------------------------------------------
 */

int wlh_repl_run(app_t *app) {
    struct sigaction action;
    const char *reason = "eof";
    cJSON *doc;

    /* Deliberately without SA_RESTART so pthread_kill(SIGUSR1) forces the
     * blocked stdin read in the reader thread to return EINTR. */
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigusr1_noop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0)
        WLH_LOGW("host-sim", "repl: sigaction(SIGUSR1) failed: %d", errno);

    doc = wlh_repl_json_begin("repl_ready");
    if (doc != NULL)
        cJSON_AddStringToObject(doc, "transport", app->use_usb ? "usb" : "ipc");
    wlh_repl_json_emit(doc);

    atomic_store(&app->repl.reader_stop, false);
    if (pthread_create(&app->repl.reader, NULL, reader_main, app) != 0) {
        WLH_LOGE("host-sim", "repl: failed to start the stdin reader");
        return -1;
    }
    app->repl.reader_started = true;

    for (;;) {
        char *line;
        bool had_line;
        bool at_eof;
        bool quit = false;
        if (!wlh_app_wait_until(app, line_or_eof, UINT32_MAX)) {
            reason = wlh_app_interrupted() ? "signal" : "transport";
            break;
        }
        pthread_mutex_lock(&app->state_mutex);
        line = app->repl.pending_line;
        had_line = app->repl.line_ready;
        app->repl.pending_line = NULL;
        app->repl.line_ready = false;
        at_eof = app->repl.stdin_eof;
        pthread_cond_broadcast(&app->state_changed);
        pthread_mutex_unlock(&app->state_mutex);
        if (!had_line) {
            if (at_eof) {
                reason = "eof";
                break;
            }
            continue;
        }
        dispatch_line(app, line, &quit);
        free(line);
        if (quit) {
            reason = "quit";
            break;
        }
    }

    atomic_store(&app->repl.reader_stop, true);
    pthread_mutex_lock(&app->state_mutex);
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    if (app->repl.reader_started) {
        /* ESRCH just means the reader already exited; ignore it. */
        (void)pthread_kill(app->repl.reader, SIGUSR1);
        pthread_join(app->repl.reader, NULL);
        app->repl.reader_started = false;
    }
    pthread_mutex_lock(&app->state_mutex);
    free(app->repl.pending_line);
    app->repl.pending_line = NULL;
    app->repl.line_ready = false;
    pthread_mutex_unlock(&app->state_mutex);

    doc = wlh_repl_json_begin("repl_exit");
    if (doc != NULL)
        cJSON_AddStringToObject(doc, "reason", reason);
    wlh_repl_json_emit(doc);
    return 0;
}

/* ---- asynchronous host events (Core task thread) --------------------------
 */

static void emit_scan_network(
    void *context, uint32_t scan_id, const wlh_protocol_v1_WifiNetwork *network
) {
    cJSON *doc = wlh_repl_json_begin("scan_result");
    (void)context;
    if (doc != NULL) {
        char bssid[18] = "";
        cJSON_AddNumberToObject(doc, "scan_id", scan_id);
        wlh_repl_json_add_bytes(
            doc, "ssid", network->ssid.bytes, network->ssid.size
        );
        if (network->bssid.size == 6u)
            (void)snprintf(
                bssid,
                sizeof(bssid),
                "%02x:%02x:%02x:%02x:%02x:%02x",
                network->bssid.bytes[0],
                network->bssid.bytes[1],
                network->bssid.bytes[2],
                network->bssid.bytes[3],
                network->bssid.bytes[4],
                network->bssid.bytes[5]
            );
        cJSON_AddStringToObject(doc, "bssid", bssid);
        cJSON_AddNumberToObject(doc, "channel", network->channel);
        cJSON_AddNumberToObject(doc, "rssi_dbm", network->rssi_dbm);
        cJSON_AddStringToObject(
            doc, "security", wlh_app_wifi_security_name(network->security)
        );
    }
    wlh_repl_json_emit(doc);
}

void wlh_repl_on_host_event(app_t *app, const wlh_host_event_t *event) {
    cJSON *doc;
    if (!app->repl.active)
        return;
    switch (event->kind) {
    case WLH_HOST_EVENT_STATE_CHANGED:
        if (event->state != WLH_HOST_STATE_READY) {
            pthread_mutex_lock(&app->state_mutex);
            app->repl.wifi_initialized = false;
            pthread_mutex_unlock(&app->state_mutex);
        }
        doc = wlh_repl_json_begin("state");
        if (doc != NULL)
            cJSON_AddStringToObject(doc, "state", state_name(event->state));
        wlh_repl_json_emit(doc);
        break;
    case WLH_HOST_EVENT_WIFI_SCAN_RESULT:
        if (wlh_app_for_each_scan_network(event, emit_scan_network, app) != 0)
            emit_error(
                app, "", WLH_HOST_PROTOCOL_ERROR, "bad scan result payload"
            );
        break;
    case WLH_HOST_EVENT_WIFI_SCAN_COMPLETED:
        wlh_repl_json_emit(wlh_repl_json_begin("scan_complete"));
        break;
    case WLH_HOST_EVENT_WIFI_CONNECTED:
        wlh_repl_json_emit(wlh_repl_json_begin("wifi_connected"));
        break;
    case WLH_HOST_EVENT_WIFI_DISCONNECTED:
        wlh_repl_json_emit(wlh_repl_json_begin("wifi_disconnected"));
        break;
    case WLH_HOST_EVENT_USER_MESSAGE_RESULT:
        doc = wlh_repl_json_begin("user_message_result");
        if (doc != NULL)
            cJSON_AddNumberToObject(doc, "bytes", (double)event->payload_size);
        wlh_repl_json_emit(doc);
        break;
    case WLH_HOST_EVENT_PROTOCOL_FAULT:
        wlh_repl_json_emit(wlh_repl_json_begin("protocol_fault"));
        break;
    case WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED:
        doc = wlh_repl_json_begin("bluetooth_state");
        if (doc != NULL &&
            event->payload_size >= sizeof(wlh_host_bluetooth_state_event_t)) {
            wlh_host_bluetooth_state_event_t change;
            memcpy(&change, event->payload, sizeof(change));
            cJSON_AddNumberToObject(doc, "state", change.state);
            cJSON_AddNumberToObject(doc, "reason", change.reason);
        }
        wlh_repl_json_emit(doc);
        break;
    default:
        break;
    }
}

bool wlh_repl_on_ping_result(app_t *app, const sim_ping_result_t *result) {
    cJSON *doc;
    if (!app->repl.active)
        return false;
    if ((result->request_id & REPL_PING_ID_MASK) != REPL_PING_ID_BASE)
        return false;
    pthread_mutex_lock(&app->state_mutex);
    app->repl.ping_result = *result;
    app->repl.ping_done = true;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    doc = wlh_repl_json_begin("ping_result");
    if (doc != NULL) {
        cJSON_AddStringToObject(doc, "hostname", result->hostname);
        cJSON_AddStringToObject(doc, "address", result->address);
        cJSON_AddNumberToObject(doc, "transmitted", result->transmitted);
        cJSON_AddNumberToObject(doc, "received", result->received);
        cJSON_AddBoolToObject(doc, "success", result->success);
        cJSON_AddStringToObject(doc, "detail", result->detail);
    }
    wlh_repl_json_emit(doc);
    return true;
}
