#include "types.h"
#include "regs.h"
#include "cstr.h"
#include "piusb.h"
#include "pictl.h"
#define MIDI_BUF_SIZE 128
static uint8_t midi_buf[MIDI_BUF_SIZE];
static uint8_t midi_buf_write_idx = 0;
static uint8_t midi_buf_read_idx = 0;

static int curr_xfer_retry = 0;

void midi_flush(usb_context_t *ctx)
{
    int r = -3;
    if (ctx->midi_out.endpoint == 0 || midi_buf_write_idx == midi_buf_read_idx)
        return; // No MIDI OUT endpoint
    uint32_t avail = (midi_buf_write_idx - midi_buf_read_idx + MIDI_BUF_SIZE) % MIDI_BUF_SIZE;
    uint32_t xfersize = avail;
    if (xfersize > 64)
        xfersize = 64;
    if (xfersize > MIDI_BUF_SIZE - midi_buf_read_idx)
        xfersize = MIDI_BUF_SIZE - midi_buf_read_idx;
    xfersize &= ~3u;
    int retries = 3;

    usb_transfer_channel_start(ctx, 3, ctx->device.address,
                               ctx->midi_out.endpoint,
                               EP_TYPE_INTERRUPT, false /*OUT*/,
                               ctx->midi_out.ep_mps,
                               ctx->midi_out.interrupt_toggle,
                               xfersize, 1 /*packet*/,
                               BUS_ADDRESS(&midi_buf[midi_buf_read_idx]));
    r = usb_transfer_channel_wait(ctx, 3);
    if (r == 0)
    {
        ctx->midi_out.interrupt_toggle ^= 2;
        midi_buf_read_idx += xfersize;
        midi_buf_read_idx %= MIDI_BUF_SIZE;
        curr_xfer_retry = 0;
    }
    else if (r == -3 || r == -5)
        // printk("OUT NAK / frame overrun, retry\n");
        {}
    else
    {
        curr_xfer_retry += 1;
        if (curr_xfer_retry > 5)
        {
            printk("MIDI OUT transfer failed %d, dropping %d bytes\n", r, xfersize);
            midi_buf_read_idx += xfersize;
            midi_buf_read_idx %= MIDI_BUF_SIZE;
            curr_xfer_retry = 0;
        }
    }
    // else if (r == -3 || r == -5)
    //     printk("OUT NAK / frame overrun, retry\n");
    // else
    //     printk("OUT failed %d\n", r); // STALL / XACTERR
}

int midi_poll(usb_context_t *ctx, uint8_t *out)
{
    if (ctx->midi_in.endpoint == 0)
        return 0; // No MIDI IN endpoint

    uint8_t *buf = ctx->dma.int_buf[ctx->dma.int_buf_idx];
    int sta = usb_interrupt_poll_ch(ctx, 2, ctx->dma.int_buf_idx, ctx->device.address, &ctx->midi_in, buf, 64);
    if (sta >= 0)
    {
        memcpy(out, buf, sta);
        return sta;
    }
    else
    {
        if (sta != -3)
        {
            printk("MIDI IN poll failed: %d\n", sta);
            return sta;
        }
    }
    return 0;
}

void midi_send_event(uint8_t *event)
{
    if ((midi_buf_write_idx + 4) % MIDI_BUF_SIZE == midi_buf_read_idx)
    {
        printk("MIDI buffer full, dropping event\n");
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        midi_buf[midi_buf_write_idx++] = event[i];
        midi_buf_write_idx %= MIDI_BUF_SIZE;
    }
}

usb_context_t *do_usb()
{
    PUT32(ADD_IRQ_DISABLE1, 0xFFFFFFFF);
    PUT32(ADD_IRQ_DISABLE2, 0xFFFFFFFF);
    usb_context_t *usb_ctx = kalloc(sizeof(usb_context_t));
    usb_context_init(usb_ctx);

    usb_midi_full_init(usb_ctx);
    return usb_ctx;
}
