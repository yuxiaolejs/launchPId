#include "types.h"
#include "regs.h"
#include "cstr.h"
#include "piusb.h"
#include "pictl.h"

static uint8_t midi_buf[64];
static uint8_t midi_buf_write_idx = 0;
static uint8_t midi_buf_read_idx = 0;

void midi_flush(usb_context_t *ctx, uint8_t *buf, int len)
{
    if (ctx->midi_out.endpoint == 0)
        return; // No MIDI OUT endpoint

    usb_transfer_channel_start(ctx, 3, ctx->device.address,
                               ctx->midi_out.endpoint, // 2
                               EP_TYPE_INTERRUPT, false /*OUT*/,
                               ctx->midi_out.ep_mps,           // 64
                               ctx->midi_out.interrupt_toggle, // DATA0 for the very first one
                               len /*bytes, multiple of 4*/, 1 /*packet*/,
                               BUS_ADDRESS(&buf[0]));
    int r = usb_transfer_channel_wait(ctx, 3);
    if (r == 0)
        ctx->midi_out.interrupt_toggle ^= 2; // ACK → next is DATA1
    else if (r == -3 || r == -5)             /* NAK / frame overrun: resend, SAME toggle */
        printk("OUT NAK / frame overrun, retry\n");
    else
        printk("OUT failed %d\n", r); // STALL / XACTERR
}

void midi_poll(usb_context_t *ctx)
{
    if (ctx->midi_in.endpoint == 0)
        return; // No MIDI IN endpoint

    uint8_t *buf = ctx->dma.int_buf[ctx->dma.int_buf_idx];
    int sta = usb_interrupt_poll_ch(ctx, 2, ctx->dma.int_buf_idx, ctx->device.address, &ctx->midi_in, buf, 64);
    if (sta >= 0)
    {
        printk("MIDI IN: ");
        for (int i = 0; i < 8; i++)
            printk("%x ", buf[i]);
        printk("\n");
        if ((buf[0] & 0x0F) == 0x9)
        {
            buf[0] = 0x09;            // Note On
            buf[1] = 0x90;            // Channel 1
            buf[3] = buf[3] ? 21 : 0; // Velocity: 21 if pressed, 0 if released
            midi_flush(ctx, buf, 4);
        }
    }
    else
    {
        if (sta != -3)
            printk("MIDI IN poll failed: %d\n", sta);
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