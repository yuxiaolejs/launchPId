#include "project.h"
#include "pictl.h"
#include "dma.h"
#include "piusb.h"
#include "pifat.h"

#define AUDIO_SAMPLE_BUFFER_SIZE 1024

static project_t *project;

__attribute__((aligned(32))) static uint16_t audio_sample_buffer[AUDIO_SAMPLE_BUFFER_SIZE];

static audio_sample_t *live_audio[16];

static uint32_t live_audio_offset[16];

static light_event_t *live_light[16];

static uint32_t live_event_trigger_time[16];

static usb_context_t *engine_usb_ctx;

uint32_t sample_blob_start_byte_offset = 0;
uint32_t audio_sample_structs_start_byte_offset = 0;
uint32_t light_event_structs_start_byte_offset = 0;

__attribute__((aligned(32))) static dma_cb_t audio_dma_cb;

void project_init(fat_meta_t *fat, const char *path)
{
    // load file and allocate for project;

    fat32_file_list_entry_t *file_list = fat32_list_fat(fat, fat->bpb.root_clus);
    if (!file_list)
    {
        printk("Failed to list FAT32 root directory\n");
        return;
    }
    struct list_head *pos;
    for (pos = file_list->list.next; pos != &file_list->list; pos = pos->next)
    {
        fat32_file_list_entry_t *entry = container_of(pos, fat32_file_list_entry_t, list);
        char name[12];
        memcpy(name, entry->dirent.name, 11);
        name[11] = '\0';
        if (strstr(name, path))
        {
            uint8_t *file_data = fat32_read_file(fat,
                                                 ((uint32_t)entry->dirent.fst_clus_hi << 16) | entry->dirent.fst_clus_lo,
                                                 entry->dirent.file_size);

            project = (project_t *)file_data;
            printk("Loaded project %s, ver %d, audio_channels %d, light_events %d, audio_samples %d\n",
                   name, project->ver, project->audio_channels, project->light_events, project->audio_samples);
        }
    }
    // make sure to set sample_blob_start_byte_offset
}

void dispatch_audio_buffer()
{
    // check if DMA's running, if so, return
    if (GET32(DMA_CS_REG(0)) & 0x01)
        return; // DMA is running, return
    // not running = render audio buffer, then start DMA
    // render audio buffer
    memset(audio_sample_buffer, 0, sizeof(audio_sample_buffer));
    for (int i = 0; i < 16; i++)
    {
        if (!live_audio[i])
            continue;
        audio_sample_t *sample = live_audio[i];
        uint32_t offset = live_audio_offset[i];
        if (offset < sample->length)
            for (int j = 0; j < AUDIO_SAMPLE_BUFFER_SIZE && offset + j < sample->length; j++)
                audio_sample_buffer[j] += *((uint16_t *)((uint8_t *)project + sample->offset + offset + j + sample_blob_start_byte_offset));
        else
        {
            // sample finished, clear live_audio
            live_audio[i] = NULL;
            live_audio_offset[i] = 0;
        }
    }
    // submit to DMA
    audio_dma_cb.ti = DMA_TI_PCM_TX;
    audio_dma_cb.source_ad = ARM_TO_DMA_BUS((uint32_t)audio_sample_buffer);
    audio_dma_cb.dest_ad = 0x7E20C018; // PCM FIFO address
    audio_dma_cb.txfr_len = AUDIO_SAMPLE_BUFFER_SIZE * sizeof(uint16_t);
    audio_dma_cb.stride = 0;
    audio_dma_cb.nextconbk = 0;
    audio_dma_cb.reserved1 = 0;
    audio_dma_cb.reserved2 = 0;
    PUT32(DMA_CS_REG(0), (1 << 31)); // reset channel
    PUT32(DMA_CONBLK_AD_REG(0), ARM_TO_DMA_BUS((uint32_t)&audio_dma_cb));
    PUT32(DMA_CS_REG(0), 1); // go
}

void play_audio_sample(uint32_t sample_idx)
{
    audio_sample_t *sample = (audio_sample_t *)((uint8_t *)project + audio_sample_structs_start_byte_offset + sample_idx * sizeof(audio_sample_t));
    for (int i = 0; i < 16; i++)
    {
        if (!live_audio[i])
        {
            live_audio[i] = sample;
            live_audio_offset[i] = 0;
            break;
        }
    }
}

void dispatch_light_events()
{
    uint32_t current_time = timer_get_msec();
    for (int i = 0; i < 16; i++)
    {
        if (!live_light[i])
            continue;
        light_event_t *event = live_light[i];
        if (current_time >= live_event_trigger_time[i])
        {
            // trigger light event
            // send MIDI message to light controller, must be 4 bytes
            midi_send_event(event->data);
            // schedule next event
            if (event->next != 0xffffffff)
            {
                live_light[i] = (light_event_t *)((uint8_t *)project + light_event_structs_start_byte_offset + event->next * sizeof(light_event_t));
                live_event_trigger_time[i] = live_event_trigger_time[i] + live_light[i]->delay_after;
            }
            else
            {
                // no more events, clear live_light
                live_light[i] = NULL;
                live_event_trigger_time[i] = 0;
            }
        }
    }
}

void play_light_event(uint32_t event_idx)
{
    light_event_t *event = (light_event_t *)((uint8_t *)project + light_event_structs_start_byte_offset + event_idx * sizeof(light_event_t));
    for (int i = 0; i < 16; i++)
    {
        if (!live_light[i])
        {
            live_light[i] = event;
            live_event_trigger_time[i] = timer_get_msec();
            break;
        }
    }
}

void pwm_init()
{
    printk("PWM CLOCK DANCE\n");
    PUT32(CM_PWMCTL, CM_PASSWD | (1 << 5)); // KILL
    while (GET32(CM_PWMCTL) & (1 << 7))
        ; // wait for BUSY to clear

    PUT32(CM_PWMDIV, CM_PASSWD | (11 << 12));

    PUT32(CM_PWMCTL, CM_PASSWD | (1 << 4) | 6);
    while (!(GET32(CM_PWMCTL) & (1 << 7)))
        ; // wait for BUSY
    printk("starting pwm...\n");
    gpio_pin_set_func(12, 0b100);                  // set to alt0 (PWM0)
    gpio_pin_set_func(13, 0b100);                  // set to alt0 (PWM1)
    PUT32(PWM0_RNG, 1024);                         // set range
    PUT32(PWM1_RNG, 1024);                         // set range
    PUT32(PWM_CTL, PWEN1 | PWEN2 | USEF1 | USEF2); // enable pwm
}

void engine_init()
{
    memset(audio_sample_buffer, 0, sizeof(audio_sample_buffer));
    memset(live_audio, 0, sizeof(live_audio));
    memset(live_audio_offset, 0, sizeof(live_audio_offset));
    memset(live_light, 0, sizeof(live_light));
    memset(live_event_trigger_time, 0, sizeof(live_event_trigger_time));
    memset(project, 0, sizeof(project_t));
    engine_usb_ctx = do_usb();
    pwm_init();
}

void engine_tick()
{
    uint8_t midi_in_buf[64];
    int midi_in_len = midi_poll(engine_usb_ctx, midi_in_buf);
    if (midi_in_len > 0)
    {
        // handle midi events
        for (int i = 0; i < midi_in_len; i += 4)
        {
            uint8_t *event = &midi_in_buf[i];
        }
    }
    dispatch_audio_buffer();
    dispatch_light_events();
    midi_flush(engine_usb_ctx);
}