/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#include "fuse.h"
#include "fuse_kernel.h"
#include "fuse_device.h"
#include "fuse_ipc.h"
#include "fuse_internal.h"
#include "fuse_kludges.h"
#include "fuse_locking.h"
#include "fuse_nodehash.h"
#include "fuse_sysctl.h"

#include <stdbool.h>
#include <libkern/libkern.h>

#define TAILQ_FOREACH_SAFE(var, head, field, tvar)           \
        for ((var) = TAILQ_FIRST((head));                    \
            (var) && ((tvar) = TAILQ_NEXT((var), field), 1); \
            (var) = (tvar))

static int  fuse_cdev_major          = -1;
static bool fuse_interface_available = false;

static struct fuse_device fuse_device_table[FUSE4X_NDEVICES];

#define FUSE_DEVICE_FROM_UNIT_FAST(u) (fuse_device_t)&(fuse_device_table[(u)])

/* Interface for VFS */

/* Doesn't need lock. */
fuse_device_t
fuse_device_get(dev_t dev)
{
    int unit = minor(dev);

    if ((unit < 0) || (unit >= FUSE4X_NDEVICES)) {
        return NULL;
    }

    return FUSE_DEVICE_FROM_UNIT_FAST(unit);
}

/* Must be called under lock. */
__inline__
void
fuse_device_close_final(fuse_device_t fdev)
{
    fdata_destroy(fdev->data);
    fdev->data   = NULL;
    fdev->pid    = -1;
}


/* Must be called under data->aw_mtx mutex */
static __inline__
void
fuse_reject_answers(struct fuse_data *data)
{
    struct fuse_ticket *ftick;

    fuse_lck_mtx_lock(data->aw_mtx);
    while ((ftick = fuse_aw_pop(data))) {
        fuse_lck_mtx_lock(ftick->tk_aw_mtx);
        fticket_set_answered(ftick);
        ftick->tk_aw_errno = ENOTCONN;
        fuse_wakeup(ftick);
        fuse_lck_mtx_unlock(ftick->tk_aw_mtx);
    }
    fuse_lck_mtx_unlock(data->aw_mtx);
}

/* /dev/fuseN implementation */

d_open_t   fuse_device_open;
d_close_t  fuse_device_close;
d_read_t   fuse_device_read;
d_write_t  fuse_device_write;
d_ioctl_t  fuse_device_ioctl;

#if M_FUSE4X_ENABLE_DSELECT

d_select_t fuse_device_select;

#else
#define fuse_device_select (d_select_t*)enodev
#endif /* M_FUSE4X_ENABLE_DSELECT */

static struct cdevsw fuse_device_cdevsw = {
    /* open     */ fuse_device_open,
    /* close    */ fuse_device_close,
    /* read     */ fuse_device_read,
    /* write    */ fuse_device_write,
    /* ioctl    */ fuse_device_ioctl,
    /* stop     */ eno_stop,
    /* reset    */ eno_reset,
    /* ttys     */ NULL,
    /* select   */ fuse_device_select,
    /* mmap     */ eno_mmap,
    /* strategy */ eno_strat,
    /* getc     */ eno_getc,
    /* putc     */ eno_putc,
    /* flags    */ D_TTY,
};

int
fuse_device_open(dev_t dev, __unused int flags, __unused int devtype,
                 struct proc *p)
{
    int unit;
    struct fuse_device *fdev;
    struct fuse_data   *fdata;

    fuse_trace_printf_func();

    if (!fuse_interface_available) {
        return ENOENT;
    }

    unit = minor(dev);
    if ((unit >= FUSE4X_NDEVICES) || (unit < 0)) {
        fuse_lck_mtx_unlock(fuse_device_mutex);
        return ENOENT;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        fuse_lck_mtx_unlock(fuse_device_mutex);
        log("fuse4x: device found with no softc\n");
        return ENXIO;
    }

    fuse_lck_mtx_lock(fuse_device_mutex);

    if (fdev->usecount != 0) {
        fuse_lck_mtx_unlock(fuse_device_mutex);
        return EBUSY;
    }

    fdev->usecount++;

    fuse_lck_mtx_lock(fdev->mtx);

    fuse_lck_mtx_unlock(fuse_device_mutex);

    /* Could block. */
    fdata = fdata_alloc(p);

    if (fdev->data) {
        /*
         * This slot isn't currently open by a user daemon. However, it was
         * used earlier for a mount that's still lingering, even though the
         * user daemon is dead.
         */

        fuse_lck_mtx_lock(fuse_device_mutex);

        fdev->usecount--;

        fuse_lck_mtx_unlock(fdev->mtx);

        fuse_lck_mtx_unlock(fuse_device_mutex);

        fdata_destroy(fdata);

        return EBUSY;
    } else {
        fdata->dataflags |= FSESS_OPENED;
        fdata->fdev  = fdev;
        fdev->data   = fdata;
        fdev->pid    = proc_pid(p);
    }

    fuse_lck_mtx_unlock(fdev->mtx);

    return KERN_SUCCESS;
}

int
fuse_device_close(dev_t dev, __unused int flags, __unused int devtype,
                  __unused struct proc *p)
{
    int unit;
    struct fuse_device *fdev;
    struct fuse_data   *data;

    fuse_trace_printf_func();

    unit = minor(dev);
    if (unit >= FUSE4X_NDEVICES) {
        return ENOENT;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;
    if (!data) {
        panic("fuse4x: no device private data in device_close");
    }

    fdata_set_dead(data);

    fuse_lck_mtx_lock(fdev->mtx);

    data->dataflags &= ~FSESS_OPENED;

    fuse_reject_answers(data);

#if M_FUSE4X_ENABLE_DSELECT
    selwakeup((struct selinfo*)&data->d_rsel);
#endif /* M_FUSE4X_ENABLE_DSELECT */

    if (!(data->dataflags & FSESS_MOUNTED)) {
        /* We're not mounted. Can destroy mpdata. */
        fuse_device_close_final(fdev);
    }

    fuse_lck_mtx_unlock(fdev->mtx);

    fuse_lck_mtx_lock(fuse_device_mutex);

    /*
     * Even if usecount goes 0 here, at open time, we check if fdev->data
     * is non-NULL (that is, a lingering mount). If so, we return EBUSY.
     * We could make the usecount depend on both device-use and mount-state,
     * but I think this is truer to reality, if a bit more complex to maintain.
     */
    fdev->usecount--;

    fuse_lck_mtx_unlock(fuse_device_mutex);

    return KERN_SUCCESS;
}

int
fuse_device_read(dev_t dev, uio_t uio, int ioflag)
{
    int i, err = 0;
    size_t buflen[3];
    void *buf[] = { NULL, NULL, NULL };

    struct fuse_device *fdev;
    struct fuse_data   *data;
    struct fuse_ticket *ftick;

    fuse_trace_printf_func();

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(minor(dev));
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;

    fuse_lck_mtx_lock(data->ms_mtx);

    /* The read loop (outgoing messages to the user daemon). */

again:
    if (fdata_dead_get(data)) {
        fuse_lck_mtx_unlock(data->ms_mtx);
        return ENODEV;
    }

    if (!(ftick = fuse_ms_pop(data))) {
        if (ioflag & FNONBLOCK) {
            fuse_lck_mtx_unlock(data->ms_mtx);
            return EAGAIN;
        }
        err = fuse_msleep(data, data->ms_mtx, PCATCH, "fu_msg", NULL);
        if (err) {
            fuse_lck_mtx_unlock(data->ms_mtx);
            return (fdata_dead_get(data) ? ENODEV : err);
        }
        goto again;
    }

    fuse_lck_mtx_unlock(data->ms_mtx);

    if (fdata_dead_get(data)) {
         if (ftick) {
             fuse_ticket_drop_invalid(ftick);
         }
         return ENODEV;
    }

    switch (ftick->tk_ms_type) {

    case FT_M_FIOV:
        buf[0]    = ftick->tk_ms_fiov.base;
        buflen[0] = ftick->tk_ms_fiov.len;
        break;

    case FT_M_BUF:
        buf[0]    = ftick->tk_ms_fiov.base;
        buflen[0] = ftick->tk_ms_fiov.len;
        buf[1]    = ftick->tk_ms_bufdata;
        buflen[1] = ftick->tk_ms_bufsize;
        break;

    default:
        panic("fuse4x: unknown message type for ticket %p", ftick);
    }

    for (i = 0; buf[i]; i++) {
        if (uio_resid(uio) < (user_ssize_t)buflen[i]) {
            data->dataflags |= FSESS_DEAD;
            err = ENODEV;
            break;
        }

        err = uiomove(buf[i], (int)buflen[i], uio);

        if (err) {
            break;
        }
    }

    /*
     * XXX: Stop gap! I really need to finish interruption plumbing.
     */
    if (fticket_answered(ftick)) {
        err = EINTR;
    }

    /*
     * The FORGET message is an example of a ticket that has explicitly
     * been invalidated by the sender. The sender is not expecting or wanting
     * a reply, so he sets the FT_INVALID bit in the ticket.
     */

    fuse_ticket_drop_invalid(ftick);

    return err;
}

int
fuse_device_write(dev_t dev, uio_t uio, __unused int ioflag)
{
    int err = 0;
    bool found = false;

    struct fuse_device    *fdev;
    struct fuse_data      *data;
    struct fuse_ticket    *ftick;
    struct fuse_ticket    *x_ftick;
    struct fuse_out_header ohead;

    fuse_trace_printf_func();

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(minor(dev));
    if (!fdev) {
        return ENXIO;
    }

    if (uio_resid(uio) < (user_ssize_t)sizeof(struct fuse_out_header)) {
        log("fuse4x: Incorrect header size. Got %lld, expected at least %lu\n",
              uio_resid(uio), sizeof(struct fuse_out_header));
        return EINVAL;
    }

    if ((err = uiomove((caddr_t)&ohead, (int)sizeof(struct fuse_out_header), uio))) {
        return err;
    }

    /* begin audit */

    if (uio_resid(uio) + sizeof(struct fuse_out_header) != ohead.len) {
        log("fuse4x: message body size does not match that in the header\n");
        return EINVAL;
    }

    if (uio_resid(uio) && ohead.error) {
        log("fuse4x: non-zero error for a message with a body\n");
        return EINVAL;
    }

    ohead.error = -(ohead.error);

    /* end audit */

    data = fdev->data;

    fuse_lck_mtx_lock(data->aw_mtx);

    TAILQ_FOREACH_SAFE(ftick, &data->aw_head, tk_aw_link, x_ftick) {
        if (ftick->tk_unique == ohead.unique) {
            found = true;
            fuse_aw_remove(ftick);
            break;
        }
    }

    fuse_lck_mtx_unlock(data->aw_mtx);

    if (found) {
        if (ftick->tk_aw_handler) {
            memcpy(&ftick->tk_aw_ohead, &ohead, sizeof(ohead));
            err = ftick->tk_aw_handler(ftick, uio);
        } else {
            fuse_ticket_drop(ftick);
            return err;
        }
    } else {
        /* ticket has no response handler */
    }

    return err;
}

int
fuse_devices_start(void)
{
    int i = 0;

    fuse_trace_printf_func();

    bzero((void *)fuse_device_table, sizeof(fuse_device_table));

    if ((fuse_cdev_major = cdevsw_add(-1, &fuse_device_cdevsw)) == -1) {
        goto error;
    }

    for (i = 0; i < FUSE4X_NDEVICES; i++) {

        dev_t dev = makedev(fuse_cdev_major, i);
        fuse_device_table[i].cdev = devfs_make_node(
                                        dev,
                                        DEVFS_CHAR,
                                        UID_ROOT,
                                        GID_OPERATOR,
                                        0666,
                                        FUSE4X_DEVICE_BASENAME "%d",
                                        i);
        if (fuse_device_table[i].cdev == NULL) {
            goto error;
        }

        fuse_device_table[i].data     = NULL;
        fuse_device_table[i].dev      = dev;
        fuse_device_table[i].pid      = -1;
        fuse_device_table[i].usecount = 0;
        fuse_device_table[i].mtx      = lck_mtx_alloc_init(fuse_lock_group,
                                                           fuse_lock_attr);
    }

    fuse_interface_available = true;

    return KERN_SUCCESS;

error:
    for (--i; i >= 0; i--) {
        devfs_remove(fuse_device_table[i].cdev);
        fuse_device_table[i].cdev = NULL;
        fuse_device_table[i].dev  = 0;
        lck_mtx_free(fuse_device_table[i].mtx, fuse_lock_group);
    }

    (void)cdevsw_remove(fuse_cdev_major, &fuse_device_cdevsw);
    fuse_cdev_major = -1;

    return KERN_FAILURE;
}

int
fuse_devices_stop(void)
{
    int i, ret;

    fuse_trace_printf_func();

    fuse_interface_available = false;

    fuse_lck_mtx_lock(fuse_device_mutex);

    if (fuse_cdev_major == -1) {
        fuse_lck_mtx_unlock(fuse_device_mutex);
        return KERN_SUCCESS;
    }

    for (i = 0; i < FUSE4X_NDEVICES; i++) {

        char p_comm[MAXCOMLEN + 1] = { '?', '\0' };

        if (fuse_device_table[i].usecount != 0) {
            fuse_interface_available = true;
            fuse_lck_mtx_unlock(fuse_device_mutex);
            proc_name(fuse_device_table[i].pid, p_comm, MAXCOMLEN + 1);
            log("fuse4x: /dev/fuse%d is still active (pid=%d %s)\n",
                  i, fuse_device_table[i].pid, p_comm);
            return KERN_FAILURE;
        }

        if (fuse_device_table[i].data != NULL) {
            fuse_interface_available = true;
            fuse_lck_mtx_unlock(fuse_device_mutex);
            proc_name(fuse_device_table[i].pid, p_comm, MAXCOMLEN + 1);
            /* The pid can't possibly be active here. */
            log("fuse4x: /dev/fuse%d has a lingering mount (pid=%d, %s)\n",
                  i, fuse_device_table[i].pid, p_comm);
            return KERN_FAILURE;
        }
    }

    /* No device is in use. */

    for (i = 0; i < FUSE4X_NDEVICES; i++) {
        devfs_remove(fuse_device_table[i].cdev);
        lck_mtx_free(fuse_device_table[i].mtx, fuse_lock_group);
        fuse_device_table[i].cdev   = NULL;
        fuse_device_table[i].dev    = 0;
        fuse_device_table[i].pid    = -1;
        fuse_device_table[i].mtx    = NULL;
    }

    ret = cdevsw_remove(fuse_cdev_major, &fuse_device_cdevsw);
    if (ret != fuse_cdev_major) {
        log("fuse4x: fuse_cdev_major != return from cdevsw_remove()\n");
    }

    fuse_cdev_major = -1;

    fuse_lck_mtx_unlock(fuse_device_mutex);

    return KERN_SUCCESS;
}

/* Control/Debug Utilities */


int
fuse_device_ioctl(dev_t dev, u_long cmd, caddr_t udata,
                  __unused int flags, __unused proc_t proc)
{
    int ret = EINVAL;
    struct fuse_device *fdev;
    struct fuse_data   *data;

    fuse_trace_printf_func();

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(minor(dev));
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;
    if (!data) {
        return ENXIO;
    }

    switch (cmd) {
    case FUSEDEVIOCSETDAEMONDEAD:
        fdata_set_dead(data);
        ret = 0;
        break;
    default:
        break;
    }

    return ret;
}

#if M_FUSE4X_ENABLE_DSELECT

int
fuse_device_select(dev_t dev, int events, void *wql, struct proc *p)
{
    int unit, revents = 0;
    struct fuse_device *fdev;
    struct fuse_data  *data;

    fuse_trace_printf_func();

    unit = minor(dev);
    if (unit >= FUSE4X_NDEVICES) {
        return ENOENT;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;
    if (!data) {
        panic("fuse4x: no device private data in device_select");
    }

    if (events & (POLLIN | POLLRDNORM)) {
        fuse_lck_mtx_lock(data->ms_mtx);
        if (fdata_dead_get(data) || STAILQ_FIRST(&data->ms_head)) {
            revents |= (events & (POLLIN | POLLRDNORM));
        } else {
            selrecord((proc_t)p, (struct selinfo*)&data->d_rsel, wql);
        }
        fuse_lck_mtx_unlock(data->ms_mtx);
    }

    if (events & (POLLOUT | POLLWRNORM)) {
        revents |= (events & (POLLOUT | POLLWRNORM));
    }

    return revents;
}

#endif /* M_FUSE4X_ENABLE_DSELECT */

int
fuse_device_kill(int unit, struct proc *p)
{
    int error = ENOENT;
    struct fuse_device *fdev;

    fuse_trace_printf_func();

    if ((unit < 0) || (unit >= FUSE4X_NDEVICES)) {
        return EINVAL;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENOENT;
    }

    fuse_lck_mtx_lock(fdev->mtx);

    struct fuse_data *fdata = fdev->data;
    if (fdata) {
        error = EPERM;
        if (p) {
            kauth_cred_t request_cred = kauth_cred_proc_ref(p);
            if ((kauth_cred_getuid(request_cred) == 0) ||
                (fuse_match_cred(fdata->daemoncred, request_cred) == 0)) {

                /* The following can block. */
                fdata_set_dead(fdata);

                fuse_reject_answers(fdata);

                error = 0;
            }
            kauth_cred_unref(&request_cred);
        }
    }

    fuse_lck_mtx_unlock(fdev->mtx);

    return error;
}

int
fuse_device_print_vnodes(int unit_flags, struct proc *p)
{
    int error = ENOENT;
    struct fuse_device *fdev;

    int unit = unit_flags;

    if ((unit < 0) || (unit >= FUSE4X_NDEVICES)) {
        return EINVAL;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENOENT;
    }

    fuse_lck_mtx_lock(fdev->mtx);

    if (fdev->data) {

        mount_t mp = fdev->data->mp;

        if (vfs_busy(mp, LK_NOWAIT)) {
            fuse_lck_mtx_unlock(fdev->mtx);
            return EBUSY;
        }

        error = EPERM;
        if (p) {
            kauth_cred_t request_cred = kauth_cred_proc_ref(p);
            if ((kauth_cred_getuid(request_cred) == 0) ||
                (fuse_match_cred(fdev->data->daemoncred, request_cred) == 0)) {
                fuse_internal_print_vnodes(fdev->data->mp);
                error = 0;
            }
            kauth_cred_unref(&request_cred);
        }

        vfs_unbusy(mp);
    }

    fuse_lck_mtx_unlock(fdev->mtx);

    return error;
}
