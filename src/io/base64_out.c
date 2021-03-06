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

#include <config.h>
#include <avsystem/commons/stream.h>
#include <avsystem/commons/base64.h>

#include <anjay/anjay.h>

#include "../utils.h"
#include "base64_out.h"
#include "vtable.h"

VISIBILITY_SOURCE_BEGIN

typedef struct base64_ret_bytes_ctx {
    const anjay_ret_bytes_ctx_vtable_t *vtable;
    avs_stream_abstract_t *stream;
    uint8_t bytes_cached[2];
    size_t num_bytes_cached;
    size_t num_bytes_left;
} base64_ret_bytes_ctx_t;

#define TEXT_CHUNK_SIZE 3 * 64u
AVS_STATIC_ASSERT(TEXT_CHUNK_SIZE % 3 == 0, chunk_must_be_a_multiple_of_3);

static int base64_ret_encode_and_write(base64_ret_bytes_ctx_t *ctx,
                                       const uint8_t *buffer,
                                       const size_t buffer_size) {
    if (!buffer_size) {
        return 0;
    }
    size_t encoded_size = avs_base64_encoded_size(buffer_size);
    assert(encoded_size <= 4 * (TEXT_CHUNK_SIZE / 3) + 1);
    char encoded[encoded_size];
    int retval = avs_base64_encode(encoded, sizeof(encoded), buffer, buffer_size);
    if (retval) {
        return retval;
    }
    return avs_stream_write(ctx->stream, encoded, sizeof(encoded) - 1);
}

static int base64_ret_bytes_flush(base64_ret_bytes_ctx_t *ctx,
                                  const uint8_t **dataptr,
                                  size_t bytes_to_write) {
    uint8_t chunk[TEXT_CHUNK_SIZE];
    while (bytes_to_write > 0) {
        memcpy(chunk, ctx->bytes_cached, ctx->num_bytes_cached);
        size_t new_bytes_written =
                ANJAY_MIN(TEXT_CHUNK_SIZE - ctx->num_bytes_cached,
                          bytes_to_write);
        assert(new_bytes_written <= TEXT_CHUNK_SIZE);
        memcpy(&chunk[ctx->num_bytes_cached], *dataptr, new_bytes_written);
        *dataptr += new_bytes_written;

        int retval;
        if ((retval = base64_ret_encode_and_write(
                     ctx, chunk, new_bytes_written + ctx->num_bytes_cached))) {
            return retval;
        }
        bytes_to_write -= new_bytes_written;
        ctx->num_bytes_left -= new_bytes_written;
        ctx->num_bytes_cached = 0;
    }
    return 0;
}

static int base64_ret_bytes_append(anjay_ret_bytes_ctx_t *ctx_,
                                   const void *data,
                                   size_t size) {
    base64_ret_bytes_ctx_t *ctx = (base64_ret_bytes_ctx_t *) ctx_;
    if (size > ctx->num_bytes_left) {
        return -1;
    }
    const uint8_t *dataptr = (const uint8_t *) data;
    size_t bytes_to_store;
    if (size + ctx->num_bytes_cached < 3) {
        bytes_to_store = size;
    } else {
        bytes_to_store = (ctx->num_bytes_cached + size) % 3;
    }
    assert(bytes_to_store <= 2);

    int retval = base64_ret_bytes_flush(ctx, &dataptr, size - bytes_to_store);
    if (retval) {
        return retval;
    }
    assert(ctx->num_bytes_cached + bytes_to_store
               <= sizeof(ctx->bytes_cached));
    memcpy(&ctx->bytes_cached[ctx->num_bytes_cached], dataptr, bytes_to_store);
    ctx->num_bytes_cached += bytes_to_store;
    ctx->num_bytes_left -= bytes_to_store;
    return 0;
}

static const anjay_ret_bytes_ctx_vtable_t BASE64_OUT_BYTES_VTABLE = {
    .append = base64_ret_bytes_append
};

anjay_ret_bytes_ctx_t *
_anjay_base64_ret_bytes_ctx_new(avs_stream_abstract_t *stream,
                                size_t length) {
    base64_ret_bytes_ctx_t *ctx =
            (base64_ret_bytes_ctx_t *) calloc(1,
                                              sizeof(base64_ret_bytes_ctx_t));
    if (ctx) {
        ctx->vtable = &BASE64_OUT_BYTES_VTABLE;
        ctx->stream = stream;
        ctx->num_bytes_left = length;
    }
    return (anjay_ret_bytes_ctx_t *) ctx;
}

int _anjay_base64_ret_bytes_ctx_close(anjay_ret_bytes_ctx_t *ctx_) {
    base64_ret_bytes_ctx_t *ctx = (base64_ret_bytes_ctx_t *) ctx_;
    if (ctx->num_bytes_left != 0) {
        /* Some bytes were not written as we have expected */
        return -1;
    }
    return base64_ret_encode_and_write(ctx, ctx->bytes_cached,
                                       ctx->num_bytes_cached);
}

void
_anjay_base64_ret_bytes_ctx_delete(anjay_ret_bytes_ctx_t **ctx_) {
    if (!ctx_ || !*ctx_) {
        return;
    }
    base64_ret_bytes_ctx_t *ctx = (base64_ret_bytes_ctx_t *) *ctx_;
    assert(ctx->vtable == &BASE64_OUT_BYTES_VTABLE);
    free(ctx);
    *ctx_ = NULL;
}
