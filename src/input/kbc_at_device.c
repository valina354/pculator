#include <string.h>
#include "kbc_at_device.h"

void kbc_at_dev_queue_reset(kbc_at_device_t* dev, uint8_t reset_main)
{
    if (dev == NULL) {
        return;
    }

    if (reset_main) {
        dev->queue_start = 0;
        dev->queue_end = 0;
        memset(dev->queue, 0, sizeof(dev->queue));
    }

    dev->cmd_queue_start = 0;
    dev->cmd_queue_end = 0;
    memset(dev->cmd_queue, 0, sizeof(dev->cmd_queue));
}

uint8_t kbc_at_dev_queue_pos(kbc_at_device_t* dev, uint8_t main)
{
    if (dev == NULL) {
        return 0;
    }

    if (main) {
        return (uint8_t)((dev->queue_end - dev->queue_start) & dev->fifo_mask);
    }

    return (uint8_t)((dev->cmd_queue_end - dev->cmd_queue_start) & 0x0F);
}

void kbc_at_dev_queue_add(kbc_at_device_t* dev, uint8_t val, uint8_t main)
{
    if (dev == NULL) {
        return;
    }

    if (main) {
        dev->queue[dev->queue_end] = val;
        dev->queue_end = (uint8_t)((dev->queue_end + 1) & dev->fifo_mask);
        return;
    }

    dev->cmd_queue[dev->cmd_queue_end] = val;
    dev->cmd_queue_end = (uint8_t)((dev->cmd_queue_end + 1) & 0x0F);
}

void kbc_at_dev_poll(void* priv)
{
    kbc_at_device_t* dev = (kbc_at_device_t*)priv;

    if ((dev == NULL) || (dev->port == NULL)) {
        return;
    }

    switch (dev->state) {
    case DEV_STATE_MAIN_1:
        if (dev->port->wantcmd) {
            kbc_at_dev_queue_reset(dev, 0);
            dev->process_cmd(dev);
            dev->port->wantcmd = 0;
        } else {
            dev->state = DEV_STATE_MAIN_2;
        }
        break;
    case DEV_STATE_MAIN_2:
        if (!dev->ignore && (*dev->scan != 0) && (dev->port->out_new == -1) &&
            (dev->queue_start != dev->queue_end)) {
            dev->port->out_new = dev->queue[dev->queue_start];
            if (dev->port->out_new != 0xFE) {
                dev->last_scan_code = (uint8_t)dev->port->out_new;
            }
            dev->queue_start = (uint8_t)((dev->queue_start + 1) & dev->fifo_mask);
        }
        if (dev->ignore || (*dev->scan == 0) || dev->port->wantcmd) {
            dev->state = DEV_STATE_MAIN_1;
        }
        break;
    case DEV_STATE_MAIN_OUT:
        if (dev->port->wantcmd) {
            kbc_at_dev_queue_reset(dev, 0);
            dev->process_cmd(dev);
            dev->port->wantcmd = 0;
            break;
        }
        /* fallthrough */
    case DEV_STATE_MAIN_WANT_IN:
        if ((dev->port->out_new == -1) && (dev->cmd_queue_start != dev->cmd_queue_end)) {
            dev->port->out_new = dev->cmd_queue[dev->cmd_queue_start];
            if (dev->port->out_new != 0xFE) {
                dev->last_scan_code = (uint8_t)dev->port->out_new;
            }
            dev->cmd_queue_start = (uint8_t)((dev->cmd_queue_start + 1) & 0x0F);
        }
        if (dev->cmd_queue_start == dev->cmd_queue_end) {
            dev->state = (uint8_t)(dev->state + 1);
        }
        break;
    case DEV_STATE_MAIN_IN:
        if (dev->port->wantcmd) {
            kbc_at_dev_queue_reset(dev, 0);
            dev->process_cmd(dev);
            dev->port->wantcmd = 0;
        }
        break;
    case DEV_STATE_EXECUTE_BAT:
        dev->state = DEV_STATE_MAIN_OUT;
        dev->execute_bat(dev);
        break;
    case DEV_STATE_MAIN_WANT_EXECUTE_BAT:
        if ((dev->port->out_new == -1) && (dev->cmd_queue_start != dev->cmd_queue_end)) {
            dev->port->out_new = dev->cmd_queue[dev->cmd_queue_start];
            if (dev->port->out_new != 0xFE) {
                dev->last_scan_code = (uint8_t)dev->port->out_new;
            }
            dev->cmd_queue_start = (uint8_t)((dev->cmd_queue_start + 1) & 0x0F);
        }
        if (dev->cmd_queue_start == dev->cmd_queue_end) {
            dev->state = DEV_STATE_EXECUTE_BAT;
        }
        break;
    default:
        break;
    }
}

void kbc_at_dev_reset(kbc_at_device_t* dev, int do_fa)
{
    if ((dev == NULL) || (dev->port == NULL) || (dev->scan == NULL)) {
        return;
    }

    dev->port->out_new = -1;
    dev->port->wantcmd = 0;
    kbc_at_dev_queue_reset(dev, 1);
    dev->command = 0x00;
    dev->flags = 0x00;
    dev->ignore = 0;
    dev->last_scan_code = 0x00;
    *dev->scan = 1;

    if (do_fa) {
        kbc_at_dev_queue_add(dev, 0xFA, 0);
        dev->state = DEV_STATE_MAIN_WANT_EXECUTE_BAT;
    } else {
        dev->state = DEV_STATE_EXECUTE_BAT;
    }
}

void kbc_at_dev_init(kbc_at_device_t* dev, kbc_at_port_t* port)
{
    if (dev == NULL) {
        return;
    }

    memset(dev, 0, sizeof(*dev));
    dev->fifo_mask = (int)(sizeof(dev->queue) - 1);
    dev->port = port;
    if (port != NULL) {
        port->priv = dev;
        port->poll = kbc_at_dev_poll;
        port->out_new = -1;
    }
}
