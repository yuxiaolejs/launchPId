#include "types.h"
#include "piusb.h"
#include <stdint.h>
#include "regs.h"
#include "cstr.h"
#include "pictl.h"
#include "pcm.h"
#include "dmx.h"
#include "project.h"
#include "emmc.h"
#include "pifat.h"

void notmain(void)
{
    kalloc_init();
    emmc_init();
    fat_meta_t *fat = fat32_init();
    printk("INIT!\n");
    engine_init();
    project_init(fat, "FADED   LPD");
    printk("USB INIT DONE!\n");
    while (1)
    {
        engine_tick();
    }
    rpi_reboot();
}
