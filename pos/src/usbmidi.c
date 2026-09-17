#include "types.h"
#include "regs.h"
#include "cstr.h"
#include "piusb.h"
#include "pictl.h"

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