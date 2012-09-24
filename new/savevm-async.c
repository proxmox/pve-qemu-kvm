#include "qemu-common.h"
#include "qerror.h"
#include "sysemu.h"
#include "qmp-commands.h"
#include "blockdev.h"
#include "qemu/qom-qobject.h"
#include "buffered_file.h"
#include "migration.h"

//#define DEBUG_SAVEVM_STATE

#ifdef DEBUG_SAVEVM_STATE
#define DPRINTF(fmt, ...) \
    do { printf("savevm-async: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

enum {
    SAVE_STATE_DONE,
    SAVE_STATE_ERROR,
    SAVE_STATE_ACTIVE,
    SAVE_STATE_COMPLETED,
};

static struct SnapshotState {
    BlockDriverState *bs;
    size_t bs_pos;
    int state;
    Error *error;
    int saved_vm_running;
    QEMUFile *file;
    int64_t total_time;
} snap_state;

SaveVMInfo *qmp_query_savevm(Error **errp)
{
    SaveVMInfo *info = g_malloc0(sizeof(*info));
    struct SnapshotState *s = &snap_state;

    if (s->state != SAVE_STATE_DONE) {
        info->has_bytes = true;
        info->bytes = s->bs_pos;
        switch (s->state) {
        case SAVE_STATE_ERROR:
            info->has_status = true;
            info->status = g_strdup("failed");
            info->has_total_time = true;
            info->total_time = s->total_time;
            if (s->error) {
                info->has_error = true;
                info->error = g_strdup(error_get_pretty(s->error));
            }
            break;
        case SAVE_STATE_ACTIVE:
            info->has_status = true;
            info->status = g_strdup("active");
            info->has_total_time = true;
            info->total_time = qemu_get_clock_ms(rt_clock)
                - s->total_time;
            break;
        case SAVE_STATE_COMPLETED:
            info->has_status = true;
            info->status = g_strdup("completed");
            info->has_total_time = true;
            info->total_time = s->total_time;
            break;
        }
    }

    return info;
}

static int save_snapshot_cleanup(void)
{
    int ret = 0;

    DPRINTF("save_snapshot_cleanup\n");

    snap_state.total_time = qemu_get_clock_ms(rt_clock) -
        snap_state.total_time;

    if (snap_state.file) {
        ret = qemu_fclose(snap_state.file);
    }

    if (snap_state.bs) {
        // try to truncate, but ignore errors (will fail on block devices).
        // note: bdrv_read() need whole blocks, so we round up
        size_t size = (snap_state.bs_pos + BDRV_SECTOR_SIZE) & BDRV_SECTOR_MASK;
        bdrv_truncate(snap_state.bs, size);

        bdrv_delete(snap_state.bs);
        snap_state.bs = NULL;
    }

    return ret;
}

static void save_snapshot_error(const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    DPRINTF("save_snapshot_error: %s\n", msg);

    if (!snap_state.error) {
        error_set(&snap_state.error, ERROR_CLASS_GENERIC_ERROR, "%s", msg);
    }

    g_free (msg);

    snap_state.state = SAVE_STATE_ERROR;

    save_snapshot_cleanup();
}

static void save_snapshot_completed(void)
{
    DPRINTF("save_snapshot_completed\n");

    if (save_snapshot_cleanup() < 0) {
        snap_state.state = SAVE_STATE_ERROR;
    } else {
        snap_state.state = SAVE_STATE_COMPLETED;
    }
}

static int block_state_close(void *opaque)
{
    snap_state.file = NULL;
    return bdrv_flush(snap_state.bs);
}

static ssize_t block_state_put_buffer(void *opaque, const void *buf,
                                      size_t size)
{
    int ret;

    if ((ret = bdrv_pwrite(snap_state.bs, snap_state.bs_pos, buf, size)) > 0) {
        snap_state.bs_pos += ret;
    }

    return ret;
}

static void block_state_put_ready(void *opaque)
{
    uint64_t remaining;
    int64_t maxlen;
    int ret;

    if (snap_state.state != SAVE_STATE_ACTIVE) {
        save_snapshot_error("put_ready returning because of non-active state");
        return;
    }

    ret = qemu_savevm_state_iterate(snap_state.file);
    remaining = ram_bytes_remaining();

    // stop if we get to the end of available space,
    // or if remaining is just a few MB
    maxlen = bdrv_getlength(snap_state.bs) - 30*1024*1024;
    if ((remaining < 100000) || ((snap_state.bs_pos + remaining) >= maxlen)) {
        if (runstate_is_running()) {
            vm_stop(RUN_STATE_SAVE_VM);
        }
    }

    if (ret < 0) {
        save_snapshot_error("qemu_savevm_state_iterate error %d", ret);
        return;
    } else if (ret == 1) {
        if (runstate_is_running()) {
            vm_stop(RUN_STATE_SAVE_VM);
        }
        DPRINTF("savevm inerate finished\n");
        if ((ret = qemu_savevm_state_complete(snap_state.file)) < 0) {
            save_snapshot_error("qemu_savevm_state_complete error %d", ret);
            return;
        } else {
            DPRINTF("save complete\n");
            save_snapshot_completed();
            return;
        }
    }
}

static void block_state_wait_for_unfreeze(void *opaque)
{
    /* do nothing here - should not be called */
}

void qmp_savevm_start(bool has_statefile, const char *statefile, Error **errp)
{
    BlockDriver *drv = NULL;
    int bdrv_oflags = BDRV_O_CACHE_WB | BDRV_O_RDWR;
    MigrationParams params = {
        .blk = 0,
        .shared = 0
    };
    int ret;

    if (snap_state.state != SAVE_STATE_DONE) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "VM snapshot already started\n");
        return;
    }

    /* initialize snapshot info */
    snap_state.saved_vm_running = runstate_is_running();
    snap_state.bs_pos = 0;
    snap_state.total_time = qemu_get_clock_ms(rt_clock);

    if (snap_state.error) {
        error_free(snap_state.error);
        snap_state.error = NULL;
    }

    if (!has_statefile) {
        vm_stop(RUN_STATE_SAVE_VM);
        snap_state.state = SAVE_STATE_COMPLETED;
        return;
    }

    if (qemu_savevm_state_blocked(errp)) {
        return;
    }

    /* Open the image */
    snap_state.bs = bdrv_new("vmstate");
    ret = bdrv_open(snap_state.bs, statefile, bdrv_oflags, drv);
    if (ret < 0) {
        error_set(errp, QERR_OPEN_FILE_FAILED, statefile);
        goto restart;
    }

    snap_state.file = qemu_fopen_ops_buffered(&snap_state, 1000000000,
                                              block_state_put_buffer,
                                              block_state_put_ready,
                                              block_state_wait_for_unfreeze,
                                              block_state_close);

    if (!snap_state.file) {
        error_set(errp, QERR_OPEN_FILE_FAILED, statefile);
        goto restart;
    }

    snap_state.state = SAVE_STATE_ACTIVE;

    ret = qemu_savevm_state_begin(snap_state.file, &params);
    if (ret < 0) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "qemu_savevm_state_begin failed\n");
        goto restart;
    }

    block_state_put_ready(&snap_state);

    return;

restart:

    save_snapshot_error("setup failed");

    if (snap_state.saved_vm_running) {
        vm_start();
    }
}

void qmp_savevm_end(Error **errp)
{
    if (snap_state.state == SAVE_STATE_DONE) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "VM snapshot not started\n");
        return;
    }

    if (snap_state.saved_vm_running) {
        vm_start();
    }

    snap_state.state = SAVE_STATE_DONE;
}

void qmp_snapshot_drive(const char *device, const char *name, Error **errp)
{
    BlockDriverState *bs;
    QEMUSnapshotInfo sn1, *sn = &sn1;
    int ret;
#ifdef _WIN32
    struct _timeb tb;
#else
    struct timeval tv;
#endif

    if (snap_state.state != SAVE_STATE_COMPLETED) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "VM snapshot not ready/started\n");
        return;
    }

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (!bdrv_is_inserted(bs)) {
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        return;
    }

    if (bdrv_is_read_only(bs)) {
        error_set(errp, QERR_DEVICE_IS_READ_ONLY, device);
        return;
    }

    if (!bdrv_can_snapshot(bs)) {
        error_set(errp, QERR_NOT_SUPPORTED);
        return;
    }

    if (bdrv_snapshot_find(bs, sn, name) >= 0) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "snapshot '%s' already exists", name);
        return;
    }

    sn = &sn1;
    memset(sn, 0, sizeof(*sn));

#ifdef _WIN32
    _ftime(&tb);
    sn->date_sec = tb.time;
    sn->date_nsec = tb.millitm * 1000000;
#else
    gettimeofday(&tv, NULL);
    sn->date_sec = tv.tv_sec;
    sn->date_nsec = tv.tv_usec * 1000;
#endif
    sn->vm_clock_nsec = qemu_get_clock_ns(vm_clock);

    pstrcpy(sn->name, sizeof(sn->name), name);

    sn->vm_state_size = 0; /* do not save state */

    ret = bdrv_snapshot_create(bs, sn);
    if (ret < 0) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "Error while creating snapshot on '%s'\n", device);
        return;
    }
}

void qmp_delete_drive_snapshot(const char *device, const char *name,
                               Error **errp)
{
    BlockDriverState *bs;
    QEMUSnapshotInfo sn1, *sn = &sn1;
    int ret;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }
    if (bdrv_is_read_only(bs)) {
        error_set(errp, QERR_DEVICE_IS_READ_ONLY, device);
        return;
    }

    if (!bdrv_can_snapshot(bs)) {
        error_set(errp, QERR_NOT_SUPPORTED);
        return;
    }

    if (bdrv_snapshot_find(bs, sn, name) < 0) {
        /* return success if snapshot does not exists */
        return;
    }

    ret = bdrv_snapshot_delete(bs, name);
    if (ret < 0) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "Error while deleting snapshot on '%s'\n", device);
        return;
    }
}

static int loadstate_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    BlockDriverState *bs = (BlockDriverState *)opaque;
    int64_t maxlen = bdrv_getlength(bs);
    if (pos > maxlen) {
        return -EIO;
    }
    if ((pos + size) > maxlen) {
        size = maxlen - pos - 1;
    }
    if (size == 0) {
        return 0;
    }
    return bdrv_pread(bs, pos, buf, size);
}

int load_state_from_blockdev(const char *filename)
{
    BlockDriverState *bs = NULL;
    BlockDriver *drv = NULL;
    QEMUFile *f;
    int ret = -1;

    bs = bdrv_new("vmstate");
    ret = bdrv_open(bs, filename, BDRV_O_CACHE_WB, drv);
    if (ret < 0) {
        error_report("Could not open VM state file");
        goto the_end;
    }

    /* restore the VM state */
    f = qemu_fopen_ops(bs, NULL, loadstate_get_buffer, NULL, NULL, NULL, NULL);
    if (!f) {
        error_report("Could not open VM state file");
        ret = -EINVAL;
        goto the_end;
    }

    qemu_system_reset(VMRESET_SILENT);
    ret = qemu_loadvm_state(f);

    qemu_fclose(f);
    if (ret < 0) {
        error_report("Error %d while loading VM state", ret);
        goto the_end;
    }

    ret = 0;

 the_end:
    if (bs) {
        bdrv_delete(bs);
    }
    return ret;
}
