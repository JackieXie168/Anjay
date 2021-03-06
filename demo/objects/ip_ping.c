/*
 * Copyright 2017 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include <poll.h>

#include "../objects.h"
#include "../iosched.h"
#include "../utils.h"

#define IP_PING_HOSTNAME       0
#define IP_PING_REPETITIONS    1
#define IP_PING_TIMEOUT_MS     2
#define IP_PING_BLOCK_SIZE     3
#define IP_PING_DSCP           4
#define IP_PING_RUN            5
#define IP_PING_STATE          6
#define IP_PING_SUCCESS_COUNT  7
#define IP_PING_ERROR_COUNT    8
#define IP_PING_AVG_TIME_MS    9
#define IP_PING_MIN_TIME_MS   10
#define IP_PING_MAX_TIME_MS   11
#define IP_PING_TIME_STDEV_US 12

typedef enum {
    IP_PING_STATE_NONE,
    IP_PING_STATE_IN_PROGRESS,
    IP_PING_STATE_COMPLETE,
    IP_PING_STATE_ERROR_HOST_NAME,
    IP_PING_STATE_ERROR_INTERNAL,
    IP_PING_STATE_ERROR_OTHER
} ip_ping_state_t;

typedef struct {
    char hostname[257];
    uint32_t repetitions;
    uint32_t ms_timeout;
    uint16_t block_size;
    uint8_t dscp;
} ip_ping_conf_t;

typedef struct {
    ip_ping_state_t state;
    unsigned int success_count;
    unsigned int error_count;
    unsigned int avg_response_time;
    unsigned int min_response_time;
    unsigned int max_response_time;
    unsigned int response_time_stdev_us;
} ip_ping_stats_t;

typedef enum {
    IP_PING_HANDLER_HEADER,
    IP_PING_HANDLER_SKIP1,
    IP_PING_HANDLER_SKIP2,
    IP_PING_HANDLER_COUNTS,
    IP_PING_HANDLER_RTT
} ip_ping_handler_state_t;

typedef struct {
    FILE *ping_pipe;
    const iosched_entry_t *iosched_entry;
    ip_ping_handler_state_t state;
    anjay_t *anjay;
} ip_ping_command_state_t;

typedef struct {
    const anjay_dm_object_def_t *def;
    iosched_t *iosched;
    ip_ping_conf_t configuration;
    ip_ping_conf_t saved_configuration;
    ip_ping_stats_t stats;
    ip_ping_command_state_t command_state;
} ip_ping_t;

static inline ip_ping_t *
get_ip_ping(const anjay_dm_object_def_t *const *obj_ptr) {
    if (!obj_ptr) {
        return NULL;
    }
    return container_of(obj_ptr, ip_ping_t, def);
}

static int ip_ping_resource_read(anjay_t *anjay,
                                 const anjay_dm_object_def_t *const *obj_ptr,
                                 anjay_iid_t iid,
                                 anjay_rid_t rid,
                                 anjay_output_ctx_t *ctx) {
    (void) anjay; (void) iid;
    ip_ping_t *ping = get_ip_ping(obj_ptr);

    switch (rid) {
    case IP_PING_HOSTNAME:
        return anjay_ret_string(ctx, ping->configuration.hostname);
    case IP_PING_REPETITIONS:
        return anjay_ret_i64(ctx, ping->configuration.repetitions);
    case IP_PING_TIMEOUT_MS:
        return anjay_ret_i64(ctx, ping->configuration.ms_timeout);
    case IP_PING_BLOCK_SIZE:
        return anjay_ret_i32(ctx, ping->configuration.block_size);
    case IP_PING_DSCP:
        return anjay_ret_i32(ctx, ping->configuration.dscp);
    case IP_PING_STATE:
        return anjay_ret_i32(ctx, ping->stats.state);
    case IP_PING_SUCCESS_COUNT:
        return anjay_ret_i64(ctx, ping->stats.success_count);
    case IP_PING_ERROR_COUNT:
        return anjay_ret_i64(ctx, ping->stats.error_count);
    case IP_PING_AVG_TIME_MS:
        return anjay_ret_i64(ctx, ping->stats.avg_response_time);
    case IP_PING_MIN_TIME_MS:
        return anjay_ret_i64(ctx, ping->stats.min_response_time);
    case IP_PING_MAX_TIME_MS:
        return anjay_ret_i64(ctx, ping->stats.max_response_time);
    case IP_PING_TIME_STDEV_US:
        return anjay_ret_i64(ctx, ping->stats.response_time_stdev_us);
    case IP_PING_RUN:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    default:
        return ANJAY_ERR_NOT_FOUND;
    }
}

static int ip_ping_reset_diagnostic_state(anjay_t *anjay, ip_ping_t *ipping) {
    if (ipping->stats.state == IP_PING_STATE_IN_PROGRESS) {
        demo_log(ERROR, "Canceling a diagnostic in progress is not supported");
        return ANJAY_ERR_INTERNAL;
    } else if (ipping->stats.state != IP_PING_STATE_NONE) {
        ipping->stats.state = IP_PING_STATE_NONE;
        anjay_notify_changed(anjay, ipping->def->oid, 0, IP_PING_STATE);
    }
    return 0;
}

#define DECLARE_GET_NUM(Type, Base) \
static int get_##Type (anjay_input_ctx_t *ctx, Type##_t *out, \
                       Type##_t min, Type##_t max) {\
    int##Base##_t base; \
    int result = anjay_get_i##Base (ctx, &base); \
    if (result) { \
        return result; \
    } \
    if (base < (int##Base##_t) min || base > (int##Base##_t) max) { \
        return ANJAY_ERR_BAD_REQUEST; \
    } \
    *out = (Type##_t) base; \
    return 0; \
}

DECLARE_GET_NUM(uint8, 32)
DECLARE_GET_NUM(uint16, 32)
DECLARE_GET_NUM(uint32, 64)

static int ip_ping_resource_write(anjay_t *anjay,
                                  const anjay_dm_object_def_t *const *obj_ptr,
                                  anjay_iid_t iid,
                                  anjay_rid_t rid,
                                  anjay_input_ctx_t *ctx) {
    (void) anjay; (void) iid;
    ip_ping_t *ping = get_ip_ping(obj_ptr);
    int result;

    switch (rid) {
    case IP_PING_HOSTNAME:
        (void) ((result = ip_ping_reset_diagnostic_state(anjay, ping))
                || (result = anjay_get_string(ctx, ping->configuration.hostname,
                                              sizeof(ping->configuration.hostname))));
        return result;
    case IP_PING_REPETITIONS:
        (void) ((result = ip_ping_reset_diagnostic_state(anjay, ping))
                || (result = get_uint32(ctx, &ping->configuration.repetitions,
                                        1, UINT32_MAX)));
        return result;
    case IP_PING_TIMEOUT_MS:
        (void) ((result = ip_ping_reset_diagnostic_state(anjay, ping))
                || (result = get_uint32(ctx, &ping->configuration.ms_timeout,
                                        1, UINT32_MAX)));
        return result;
    case IP_PING_BLOCK_SIZE:
        (void) ((result = ip_ping_reset_diagnostic_state(anjay, ping))
                || (result = get_uint16(ctx, &ping->configuration.block_size,
                                        1, UINT16_MAX)));
        return result;
    case IP_PING_DSCP:
        (void) ((result = ip_ping_reset_diagnostic_state(anjay, ping))
                || (result = get_uint8(ctx, &ping->configuration.dscp, 0, 63)));
        return result;
    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static void update_response_times(ip_ping_t *ping,
                                  unsigned min, unsigned max, unsigned avg,
                                  unsigned mdev_us) {
    ping->stats.min_response_time = min;
    anjay_notify_changed(ping->command_state.anjay, ping->def->oid, 0,
                         IP_PING_MIN_TIME_MS);
    ping->stats.avg_response_time = max;
    anjay_notify_changed(ping->command_state.anjay, ping->def->oid, 0,
                         IP_PING_AVG_TIME_MS);
    ping->stats.max_response_time = avg;
    anjay_notify_changed(ping->command_state.anjay, ping->def->oid, 0,
                         IP_PING_MAX_TIME_MS);
    ping->stats.response_time_stdev_us = mdev_us;
    anjay_notify_changed(ping->command_state.anjay, ping->def->oid, 0,
                         IP_PING_TIME_STDEV_US);
}

static void ip_ping_command_state_cleanup(ip_ping_t *ping) {
    if (ping->command_state.ping_pipe) {
        pclose(ping->command_state.ping_pipe);
        ping->command_state.ping_pipe = NULL;
    }
    if (ping->command_state.iosched_entry) {
        iosched_entry_remove(ping->iosched, ping->command_state.iosched_entry);
        ping->command_state.iosched_entry = NULL;
    }
}

static void ip_ping_handler(short revents, void *ping_) {
    (void) revents;
    ip_ping_t *ping = (ip_ping_t *) ping_;
    char line[512];

    if (!fgets(line, sizeof(line), ping->command_state.ping_pipe)) {
        goto finish;
    }

    ip_ping_handler_state_t last_state = ping->command_state.state;
    ping->command_state.state =
            (ip_ping_handler_state_t) (ping->command_state.state + 1);
    switch (last_state) {
    case IP_PING_HANDLER_HEADER:
        if (strstr(line, "unknown")) {
            demo_log(ERROR, "Unknown host: %s", ping->configuration.hostname);
            ping->stats.state = IP_PING_STATE_ERROR_HOST_NAME;
            goto finish;
        }
        return;
    case IP_PING_HANDLER_SKIP1:
    case IP_PING_HANDLER_SKIP2:
    default:
        return;
    case IP_PING_HANDLER_COUNTS:
    {
        unsigned total, success;
        if (sscanf(line, "%u %*s %*s %u", &total, &success) != 2) {
            goto finish;
        }
        ping->stats.success_count = success;
        anjay_notify_changed(ping->command_state.anjay, ping->def->oid, 0,
                             IP_PING_SUCCESS_COUNT);
        ping->stats.error_count = total - success;
        anjay_notify_changed(ping->command_state.anjay, ping->def->oid, 0,
                             IP_PING_ERROR_COUNT);
        if (success == 0) {
            ping->stats.state = IP_PING_STATE_COMPLETE;
            update_response_times(ping, 0, 0, 0, 0);
            goto finish;
        }
        return;
    }
    case IP_PING_HANDLER_RTT:
    {
        const char *ptr = strstr(line, "=");
        if (!ptr) {
            demo_log(ERROR, "Invalid output format of ping.");
            goto finish;
        }
        ptr += 2;
        float min, avg, max, mdev;
        if (sscanf(ptr, "%f/%f/%f/%f", &min, &avg, &max, &mdev) != 4) {
            goto finish;
        }
        ping->stats.state = IP_PING_STATE_COMPLETE;
        update_response_times(ping,
                              (unsigned) min,
                              (unsigned) avg,
                              (unsigned) max,
                              (unsigned) (mdev * 1000.0f));
    }
    }

finish:
    ip_ping_command_state_cleanup(ping);
    if (ping->stats.state == IP_PING_STATE_IN_PROGRESS) {
        ping->stats.state = IP_PING_STATE_ERROR_INTERNAL;
    }
    anjay_notify_changed(ping->command_state.anjay,
                         ping->def->oid, 0, IP_PING_STATE);
}

static ip_ping_state_t start_ip_ping(anjay_t *anjay,
                                     ip_ping_t *ping) {
    if (!ping->configuration.repetitions
            || !ping->configuration.ms_timeout
            || !ping->configuration.block_size
            || !ping->configuration.hostname[0]) {
        return IP_PING_STATE_ERROR_OTHER;
    }

    char command[300];
    unsigned timeout_s = ping->configuration.ms_timeout / 1000;
    if (!timeout_s) {
        timeout_s = 1;
    }

    sprintf(command,
            "ping -q -c %u -Q 0x%x -W %u -s %u %s 2>&1",
            ping->configuration.repetitions,
            ping->configuration.dscp << 2,
            timeout_s,
            ping->configuration.block_size,
            ping->configuration.hostname);

    if (!(ping->command_state.ping_pipe = popen(command, "r"))) {
        demo_log(ERROR, "Cannot start child process. Command: %s", command);
        return IP_PING_STATE_ERROR_INTERNAL;
    }

    ping->command_state.iosched_entry =
            iosched_poll_entry_new(ping->iosched,
                                   fileno(ping->command_state.ping_pipe),
                                   POLLIN, ip_ping_handler, ping, NULL);
    if (!ping->command_state.iosched_entry) {
        pclose(ping->command_state.ping_pipe);
        ping->command_state.ping_pipe = NULL;
        return IP_PING_STATE_ERROR_INTERNAL;
    }

    ping->command_state.state = IP_PING_HANDLER_HEADER;
    ping->command_state.anjay = anjay;
    return IP_PING_STATE_IN_PROGRESS;
}

static int ip_ping_resource_execute(anjay_t *anjay,
                                    const anjay_dm_object_def_t *const *obj_ptr,
                                    anjay_iid_t iid,
                                    anjay_rid_t rid,
                                    anjay_execute_ctx_t *arg_ctx) {
    (void) arg_ctx;
    ip_ping_t *ping = get_ip_ping(obj_ptr);
    int result;

    switch (rid) {
    case IP_PING_RUN:
        if ((result = ip_ping_reset_diagnostic_state(anjay, ping))) {
            return result;
        }
        ping->stats.state = start_ip_ping(anjay, ping);
        anjay_notify_changed(anjay, (*obj_ptr)->oid, iid, IP_PING_STATE);
        return 0;
    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static int
ip_ping_transaction_begin(anjay_t *anjay,
                          const anjay_dm_object_def_t *const *obj_ptr) {
    (void) anjay;
    ip_ping_t *repr = get_ip_ping(obj_ptr);
    repr->saved_configuration = repr->configuration;
    return 0;
}

static int
ip_ping_transaction_validate(anjay_t *anjay,
                             const anjay_dm_object_def_t *const *obj_ptr) {
    (void) anjay;
    (void) obj_ptr;
    return 0;
}

static int
ip_ping_transaction_commit(anjay_t *anjay,
                           const anjay_dm_object_def_t *const *obj_ptr) {
    (void) anjay;
    (void) obj_ptr;
    return 0;
}

static int
ip_ping_transaction_rollback(anjay_t *anjay,
                             const anjay_dm_object_def_t *const *obj_ptr) {
    (void) anjay;
    ip_ping_t *repr = get_ip_ping(obj_ptr);
    repr->configuration = repr->saved_configuration;
    return 0;
}

static const anjay_dm_object_def_t IP_PING = {
    .oid = DEMO_OID_IP_PING,
    .supported_rids = ANJAY_DM_SUPPORTED_RIDS(
            IP_PING_HOSTNAME,
            IP_PING_REPETITIONS,
            IP_PING_TIMEOUT_MS,
            IP_PING_BLOCK_SIZE,
            IP_PING_DSCP,
            IP_PING_RUN,
            IP_PING_STATE,
            IP_PING_SUCCESS_COUNT,
            IP_PING_ERROR_COUNT,
            IP_PING_AVG_TIME_MS,
            IP_PING_MIN_TIME_MS,
            IP_PING_MAX_TIME_MS,
            IP_PING_TIME_STDEV_US),
    .handlers = {
        .instance_it = anjay_dm_instance_it_SINGLE,
        .instance_present = anjay_dm_instance_present_SINGLE,
        .resource_present = anjay_dm_resource_present_TRUE,
        .resource_read = ip_ping_resource_read,
        .resource_write = ip_ping_resource_write,
        .resource_execute = ip_ping_resource_execute,
        .transaction_begin = ip_ping_transaction_begin,
        .transaction_validate = ip_ping_transaction_validate,
        .transaction_commit = ip_ping_transaction_commit,
        .transaction_rollback = ip_ping_transaction_rollback
    }
};

const anjay_dm_object_def_t **ip_ping_object_create(iosched_t *iosched) {
    ip_ping_t *repr = (ip_ping_t *) calloc(1, sizeof(ip_ping_t));
    if (!repr) {
        return NULL;
    }

    repr->def = &IP_PING;
    repr->iosched = iosched;

    return &repr->def;
}

void ip_ping_object_release(const anjay_dm_object_def_t **def) {
    ip_ping_t *ping = get_ip_ping(def);
    if (ping) {
        ip_ping_command_state_cleanup(ping);
        free(ping);
    }
}
