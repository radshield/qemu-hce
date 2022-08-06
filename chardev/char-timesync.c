/*
 * QEMU System Emulator
 *
 * Copyright (c) 2021
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/log.h"
#include "chardev/char.h"

#include "chardev-internal.h"

#define TIMESYNC_LEADER_MAGIC 0x71DE7EAD
#define TIMESYNC_FOLLOWER_MAGIC 0x71DEF011

static void timesync_expire_cb(void *opaque);

static void timesync_set_timer(Chardev *chr, int64_t expire_at) {
    TimesyncChardev *ts = TIMESYNC_CHARDEV(chr);

    if (ts->follower_timer == NULL) {
        ts->follower_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timesync_expire_cb, chr);
    }

    if (expire_at >= 0) {
        timer_mod_ns(ts->follower_timer, expire_at);
    } else {
        timer_del(ts->follower_timer);
    }
}

// caller must hold the lock on chr
static int timesync_chr_interact(Chardev *chr, const void *buf, size_t len, const char *reason, Error **errp)
{
    TimesyncChardev *ts = TIMESYNC_CHARDEV(chr);

    uint32_t seq_num = ts->seq_num++;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t now_rt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    // {start,end},now_vt,now_rt,{tx,rx}len,reason
    fprintf(ts->log, "start,%"PRId64",%"PRId64",%zu,%s\n",
            now, now_rt, len, reason);

    // make sure the datatypes fit
    assert(len == (int) (uint32_t) len);

    // let the timesync application know about this write
    uint32_t header_lines[] = {
            g_htonl(TIMESYNC_LEADER_MAGIC),
            g_htonl(seq_num),
            g_htonl(ts->pending_read_data != NULL ? ts->pending_read_len - ts->pending_read_offset : 0),
            g_htonl((uint32_t) now),
            g_htonl((uint32_t) (now >> 32)),
            g_htonl((uint32_t) len),
    };
    assert(sizeof(header_lines) == 6 * 4);

    struct iovec iov[] = {
            { .iov_base = (char *)&header_lines, .iov_len = sizeof(header_lines) },
            { .iov_base = (char *)buf, .iov_len = len },
    };

    if (qio_channel_writev_all(ts->ioc, iov, sizeof(iov) / sizeof(*iov), errp) < 0) {
        return -1;
    }

    // receive the reply
    uint32_t reply_lines[5];
    if (qio_channel_read_all(ts->ioc, (char*) &reply_lines, sizeof(reply_lines), errp) < 0) {
        return -1;
    }

    if (g_ntohl(reply_lines[0]) != TIMESYNC_FOLLOWER_MAGIC) {
        error_setg(errp, "Unexpected reply header magic number");
        return -1;
    }
    if (g_ntohl(reply_lines[1]) != seq_num) {
        error_setg(errp, "Unexpected reply header sequence number");
        return -1;
    }

    int64_t expire_at = ((int64_t) g_ntohl(reply_lines[2])) | (((int64_t) g_ntohl(reply_lines[3])) << 32);
    if (expire_at >= 0 && expire_at < now) {
        error_setg(errp, "Follower attempted to set timer at time before current time");
        return -1;
    }

    timesync_set_timer(chr, expire_at);

    int reply_len = (int) g_ntohl(reply_lines[4]);
    // make sure the datatypes fit
    assert((uint32_t) reply_len == g_ntohl(reply_lines[4]));

    now_rt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    fprintf(ts->log, "end,%"PRId64",%"PRId64",%d,%s\n",
            now, now_rt, reply_len, reason);

    if (reply_len > 0) {
        if (ts->pending_read_data != NULL && ts->pending_read_offset < ts->pending_read_len) {
            error_setg(errp, "Follower attempted to send data when leader could not accept it");
            return -1;
        }

        if (ts->pending_read_data != NULL) {
            g_free(ts->pending_read_data);
            ts->pending_read_data = NULL;
        }

        uint8_t *reply_buf = g_malloc(reply_len);
        if (reply_buf == NULL) {
            error_setg(errp, "Could not allocate memory");
            return -1;
        }
        if (qio_channel_read_all(ts->ioc, (char*) reply_buf, reply_len, errp) < 0) {
            g_free(reply_buf);
            return -1;
        }

        ts->pending_read_data = reply_buf;
        ts->pending_read_offset = 0;
        ts->pending_read_len = reply_len;
    }
    assert(ts->seq_num == seq_num + 1);

    return 0;
}

// caller must hold the lock on chr
static void timesync_pump_input(Chardev *chr)
{
    TimesyncChardev *ts = TIMESYNC_CHARDEV(chr);
    // we keep writing data until we can't anymore
    while (true) {
        int len = qemu_chr_be_can_write(chr);
        int pending_len = ts->pending_read_data == NULL ? 0 : ts->pending_read_len - ts->pending_read_offset;

        int read_len = MIN(len, pending_len);
        assert(read_len >= 0);
        if (read_len == 0) {
            // cannot read data right now
            break;
        }

        // we can read data!
        qemu_chr_be_write(chr, ts->pending_read_data + ts->pending_read_offset, read_len);
        ts->pending_read_offset += read_len;

        assert(ts->pending_read_offset <= ts->pending_read_len);
        if (ts->pending_read_offset == ts->pending_read_len) {
            // that was the last of our data... check immediately whether there's more
            int result = timesync_chr_interact(chr, NULL, 0, "pump_input recheck", &error_fatal);
            // because we fatal on error
            assert(result == 0);
        }
    }
}

static void timesync_expire_cb(void *opaque)
{
    Chardev *chr = CHARDEV(opaque);
    TimesyncChardev *ts = TIMESYNC_CHARDEV(opaque);
    qemu_mutex_lock(&ts->timesync_protocol_lock);
    int result = timesync_chr_interact(chr, NULL, 0, "expire_cb", &error_fatal);
    // because we fatal on error
    assert(result == 0);
    // and make sure any received input is fed to the character device frontend
    timesync_pump_input(chr);
    qemu_mutex_unlock(&ts->timesync_protocol_lock);
}

/* Called with chr_write_lock held.  */
static int timesync_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    TimesyncChardev *ts = TIMESYNC_CHARDEV(chr);
    qemu_mutex_lock(&ts->timesync_protocol_lock);
    int result = timesync_chr_interact(chr, buf, len, "chr_write", &error_fatal);
    // because we fatal on error
    assert(result == 0);
    // and make sure any received input is fed to the character device frontend
    timesync_pump_input(chr);
    qemu_mutex_unlock(&ts->timesync_protocol_lock);
    // because we always write the full buffer
    return len;
}

static void timesync_chr_accept_input(Chardev *chr)
{
    TimesyncChardev *ts = TIMESYNC_CHARDEV(chr);
    qemu_mutex_lock(&ts->timesync_protocol_lock);
    timesync_pump_input(chr);
    qemu_mutex_unlock(&ts->timesync_protocol_lock);
}

static void qemu_chr_parse_timesync(QemuOpts *opts, ChardevBackend *backend, Error **errp)
{
    const char *path = qemu_opt_get(opts, "path");
    ChardevTimesync *tsopts = backend->u.timesync.data;

    if (path == NULL) {
        error_setg(errp, "chardev: timesync: no unix soccket path given");
        return;
    }
    backend->type = CHARDEV_BACKEND_KIND_TIMESYNC;
    tsopts = backend->u.timesync.data = g_new0(ChardevTimesync, 1);
    qemu_chr_parse_common(opts, qapi_ChardevTimesync_base(tsopts));
    tsopts->path = g_strdup(path);
}

static void qemu_chr_open_timesync(Chardev *chr, ChardevBackend *backend, bool *be_opened, Error **errp)
{
    TimesyncChardev *ts = TIMESYNC_CHARDEV(chr);
    ChardevTimesync *opts = backend->u.timesync.data;

    qemu_mutex_init(&ts->timesync_protocol_lock);

    //////////////////
    SocketAddress *addr = g_new0(SocketAddress, 1);

    addr->type = SOCKET_ADDRESS_TYPE_UNIX;
    addr->u.q_unix.path = g_strdup(opts->path);
#ifdef CONFIG_LINUX
    addr->u.q_unix.has_tight = true;
    addr->u.q_unix.tight = true;
    addr->u.q_unix.has_abstract = true;
    addr->u.q_unix.abstract = false;
#endif
    ts->addr = addr;

    /* be isn't opened until we get a connection */
    *be_opened = false;

    QIOChannelSocket *sioc = qio_channel_socket_new();

    char *name = g_strdup_printf("chardev-unix-client-%s", chr->label);
    qio_channel_set_name(QIO_CHANNEL(sioc), name);
    g_free(name);

    if (qio_channel_socket_connect_sync(sioc, addr, errp) < 0) {
        object_unref(OBJECT(sioc));
        return;
    }

    char *log_name = g_strdup_printf("%s.log", opts->path);
    ts->log = fopen(log_name, "w");
    if (ts->log == NULL) {
        error_setg_errno(errp, errno,
                         "Unable to open %s", log_name);
        object_unref(OBJECT(sioc));
        g_free(log_name);
        return;
    }
    g_free(log_name);

    ts->ioc = QIO_CHANNEL(sioc);
    object_ref(OBJECT(sioc));
    ts->sioc = sioc;
    object_ref(OBJECT(sioc));

    // make sure we're in blocking mode
    qio_channel_set_blocking(ts->ioc, true, NULL);

    g_free(chr->filename);

    struct sockaddr_storage *ss = &ts->sioc->localAddr;
    assert(ss->ss_family == AF_UNIX);
    chr->filename = g_strdup_printf("unix:%s", ((struct sockaddr_un *)(ss))->sun_path);

    // very first interaction to set up any initial timers
    timesync_chr_interact(chr, NULL, 0, "initial", errp);

    qemu_chr_be_event(chr, CHR_EVENT_OPENED);

    object_unref(OBJECT(sioc));

    // and make sure any immediately-received data is sent to the frontend
    timesync_pump_input(chr);
}

static void char_timesync_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->parse = qemu_chr_parse_timesync;
    cc->open = qemu_chr_open_timesync;
    cc->chr_write = timesync_chr_write;
    cc->chr_accept_input = timesync_chr_accept_input;
}

static void char_timesync_finalize(Object *obj)
{
    Chardev *chr = CHARDEV(obj);
    TimesyncChardev *ts = TIMESYNC_CHARDEV(obj);

    if (ts->follower_timer != NULL) {
        timer_free(ts->follower_timer);
    }
    fclose(ts->log);
    ts->log = NULL;
    object_unref(OBJECT(ts->sioc));
    ts->sioc = NULL;
    object_unref(OBJECT(ts->ioc));
    ts->ioc = NULL;
    g_free(chr->filename);
    chr->filename = NULL;

    qapi_free_SocketAddress(ts->addr);
    qemu_mutex_destroy(&ts->timesync_protocol_lock);

    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static const TypeInfo char_timesync_type_info = {
    .name = TYPE_CHARDEV_TIMESYNC,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(TimesyncChardev),
    .instance_finalize = char_timesync_finalize,
    .class_init = char_timesync_class_init,
};

static void register_types(void)
{
    type_register_static(&char_timesync_type_info);
}

type_init(register_types);
