#include "types.h"
#include "piusb.h"
#include <stdint.h>
#include "regs.h"
#include "cstr.h"
#include "pictl.h"
#include "pcm.h"
#include "dmx.h"
#include "engine.h"

void notmain(void)
{
    kalloc_init();
    printk("INIT!\n");
    usb_context_t *usb_ctx = do_usb();
    printk("USB INIT DONE!\n");
    while(1)
    {
        midi_poll(usb_ctx);
        udelay(1000);
    }
    rpi_reboot();
}
