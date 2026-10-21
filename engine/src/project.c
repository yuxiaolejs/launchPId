#include "project.h"
#include "pictl.h"
#include "dma.h"
#include "piusb.h"
#include "pifat.h"
#include "pcm_aud.h"

#define AUDIO_SAMPLE_BUFFER_SIZE 1024
#define AUDIO_FRAMES (AUDIO_SAMPLE_BUFFER_SIZE / 2)
#define AUDIO_MAX_VOICES 16
#define MIDI_MAX_LIGHTS 256
#define AUDIO_PWM_RANGE 1024 // must match PWM0_RNG / PWM1_RNG in pwm_init()
#define AUDIO_BUFS 4

static project_t *project;

__attribute__((aligned(32))) static dma_cb_t audio_cb[AUDIO_BUFS];
__attribute__((aligned(32))) static uint32_t audio_sample_buffer[AUDIO_BUFS][AUDIO_SAMPLE_BUFFER_SIZE];
static int audio_buf_last_playing = -1;

static audio_sample_t *live_audio[AUDIO_MAX_VOICES];

static uint32_t live_audio_offset[AUDIO_MAX_VOICES];

static light_event_t *live_light[MIDI_MAX_LIGHTS];

static uint32_t live_event_trigger_time[MIDI_MAX_LIGHTS];

static usb_context_t *engine_usb_ctx;

uint32_t sample_blob_start_byte_offset = 0;
uint32_t audio_sample_structs_start_byte_offset = 0;
uint32_t light_event_structs_start_byte_offset = 0;

uint32_t launchpad_current_page = 0;

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
            uint8_t *file_data =
                fat32_read_file(fat,
                                ((uint32_t)entry->dirent.fst_clus_hi << 16) | entry->dirent.fst_clus_lo,
                                entry->dirent.file_size);

            project = (project_t *)file_data;
            printk("Loaded project %s, ver %d, audio_channels %d, light_events %d, audio_samples %d\n",
                   name, project->ver, project->audio_channels, project->light_events, project->audio_samples);
        }
    }
    if (!project)
    {
        printk("Failed to find project file %s\n", path);
        return;
    }
    launchpad_current_page = 0;
    // make sure to set sample_blob_start_byte_offset
    audio_sample_structs_start_byte_offset = sizeof(project_t) + sizeof(button_event_t) * 64 * 8; // 64 buttons, 8 pages
    light_event_structs_start_byte_offset = audio_sample_structs_start_byte_offset + sizeof(audio_sample_t) * project->audio_samples;
    sample_blob_start_byte_offset = light_event_structs_start_byte_offset + sizeof(light_event_t) * project->light_events;
}

void audio_dma_init()
{
    gpio_pin_set_func(PCM_DOUT_PIN, GPIO_ALT0);
    gpio_pin_set_func(PCM_CLK_PIN, GPIO_ALT0);
    gpio_pin_set_func(PCM_FS_PIN, GPIO_ALT0);
    MK_REG(I2S_CS) = I2S_CS_EN | I2S_CS_STBY;
    delay_us(100);
    MK_REG(I2S_MODE) = I2S_MODE_FLEN(63) | I2S_MODE_FSLEN(32) | I2S_MODE_CLKI | I2S_MODE_FSI;
    MK_REG(I2S_TXC) = I2S_TXC_CH1EN | I2S_TXC_CH1WEX |
                      I2S_TXC_CH1WID(8) | I2S_TXC_CH1POS(1) |
                      I2S_TXC_CH2EN | I2S_TXC_CH2WEX |
                      I2S_TXC_CH2WID(8) | I2S_TXC_CH2POS(33);

    MK_REG(I2S_CS) |= I2S_CS_TXCLR;
    delay_us(100); /* datasheet wants 2 PCM clocks; this is ~25 of them */

    MK_REG(I2S_CS) |= I2S_CS_TXERR; /* write 1 to clear a stale underrun */

    MK_REG(I2S_DREQ) = (0x10 << 24) | (0x30 << 8);

    // printk("Priming FIFO with mark before starting DMA\n");
    // while (MK_REG(I2S_CS) & I2S_CS_TXD)
    //     MK_REG(I2S_FIFO) = 0xFFFFFFFFu;

    // DMA_ENABLE
    MK_REG(I2S_CS) |= I2S_CS_DMAEN;
    for (int i = 0; i < AUDIO_BUFS; i++)
    {
        for (int w = 0; w < AUDIO_SAMPLE_BUFFER_SIZE; w++)
            audio_sample_buffer[i][w] = AUDIO_PWM_RANGE / 2;

        audio_cb[i].ti = DMA_TI_PCM_TX;
        audio_cb[i].source_ad = ARM_TO_DMA_BUS((uint32_t)audio_sample_buffer[i]);
        audio_cb[i].dest_ad = 0x7E000000 | (I2S_FIFO & 0xFFFFFF);
        audio_cb[i].txfr_len = sizeof(audio_sample_buffer[i]);
        audio_cb[i].stride = 0;
        audio_cb[i].nextconbk = ARM_TO_DMA_BUS((uint32_t)&audio_cb[(i + 1) % AUDIO_BUFS]);
    }

    MK_REG(I2S_CS) |= I2S_CS_TXON;
    PUT32(DMA_CS_REG(0), 1 << 31); // reset first: it clears CONBLK_AD
    PUT32(DMA_CONBLK_AD_REG(0), ARM_TO_DMA_BUS((uint32_t)&audio_cb[0]));
    PUT32(DMA_CS_REG(0), 1);

    printk("DMA RUNNING, returning control\n");
}

void dispatch_audio_buffer()
{
    // check if DMA's running, if no, return, we are fucked
    if (!(GET32(DMA_CS_REG(0)) & 0x01))
        return; // DMA is not running, return

    // check progress of DMA, and figure out which buffer it is playing, fill till that buf.
    uint32_t cur = GET32(DMA_CONBLK_AD_REG(0));
    int playing = -1;
    for (int i = 0; i < AUDIO_BUFS; i++)
        if (cur == ARM_TO_DMA_BUS((uint32_t)&audio_cb[i]))
            playing = i;
    if (playing < 0 || playing == audio_buf_last_playing)
        return;
    audio_buf_last_playing = playing;
    int i = (playing + AUDIO_BUFS - 1) % AUDIO_BUFS;
    static int32_t mix_l[AUDIO_FRAMES];
    static int32_t mix_r[AUDIO_FRAMES];
    memset(mix_l, 0, sizeof(mix_l));
    memset(mix_r, 0, sizeof(mix_r));

    uint32_t channels = (project && project->audio_channels == 2) ? 2 : 1;
    uint32_t frame_bytes = 2 * channels; // int16 per channel
    for (int i = 0; i < AUDIO_MAX_VOICES; i++)
    {
        if (!live_audio[i])
            continue;
        audio_sample_t *sample = live_audio[i];
        uint32_t offset = live_audio_offset[i]; // bytes already played
        const int16_t *pcm = (const int16_t *)((uint8_t *)project + sample_blob_start_byte_offset + sample->offset);

        uint32_t frames_left = offset < sample->length ? (sample->length - offset) / frame_bytes : 0;
        uint32_t n = frames_left < AUDIO_FRAMES ? frames_left : AUDIO_FRAMES;
        const int16_t *src = pcm + offset / 2;
        for (uint32_t f = 0; f < n; f++)
        {
            int16_t l = src[f * channels];
            int16_t r = channels == 2 ? src[f * channels + 1] : l;
            mix_l[f] += l;
            mix_r[f] += r;
        }

        offset += n * frame_bytes;
        if (offset + frame_bytes > sample->length)
        {
            // sample finished, clear live_audio
            live_audio[i] = NULL;
            live_audio_offset[i] = 0;
        }
        else
        {
            live_audio_offset[i] = offset;
        }
    }

    for (int f = 0; f < AUDIO_FRAMES; f++)
    {
        // int32_t l = AUDIO_PWM_RANGE / 2 + ((mix_l[f] * AUDIO_PWM_RANGE) >> 16);
        // int32_t r = AUDIO_PWM_RANGE / 2 + ((mix_r[f] * AUDIO_PWM_RANGE) >> 16);
        // if (l < 0)
        //     l = 0;
        // if (l > AUDIO_PWM_RANGE - 1)
        //     l = AUDIO_PWM_RANGE - 1;
        // if (r < 0)
        //     r = 0;
        // if (r > AUDIO_PWM_RANGE - 1)
        //     r = AUDIO_PWM_RANGE - 1;
        audio_sample_buffer[i][2 * f] = mix_l[f] << 15;     // PWM0
        audio_sample_buffer[i][2 * f + 1] = mix_r[f] << 15; // PWM1
    }
}

void play_audio_sample(uint32_t sample_idx)
{
    audio_sample_t *sample = (audio_sample_t *)((uint8_t *)project + audio_sample_structs_start_byte_offset + sample_idx * sizeof(audio_sample_t));
    int ok = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++)
    {
        if (!live_audio[i])
        {
            live_audio[i] = sample;
            live_audio_offset[i] = 0;
            ok = 1;
            break;
        }
    }
    if (!ok)
        printk("No free audio sample slots, dropping sample %d\n", sample_idx);
}

void dispatch_light_events()
{
    uint32_t current_time = timer_get_msec();
    for (int i = 0; i < MIDI_MAX_LIGHTS; i++)
    {
        if (!live_light[i])
            continue;
        light_event_t *event = live_light[i];
        while (current_time >= live_event_trigger_time[i] && event)
        {
            // trigger light event
            // send MIDI message to light controller, must be 4 bytes
            if (event->data[0] != 0 || event->data[1] != 0 || event->data[2] != 0 || event->data[3] != 0)
                midi_send_event(event->data);
            // schedule next event
            if (event->next != 0xffffffff)
            {
                live_event_trigger_time[i] = live_event_trigger_time[i] + live_light[i]->delay_after;
                live_light[i] = (light_event_t *)((uint8_t *)project + light_event_structs_start_byte_offset + event->next * sizeof(light_event_t));
            }
            else
            {
                // no more events, clear live_light
                live_light[i] = NULL;
                live_event_trigger_time[i] = 0;
            }
            event = live_light[i];
        }
    }
}

void play_light_event(uint32_t event_idx)
{
    light_event_t *event = (light_event_t *)((uint8_t *)project + light_event_structs_start_byte_offset + event_idx * sizeof(light_event_t));
    int ok = 0;
    for (int i = 0; i < MIDI_MAX_LIGHTS; i++)
    {
        if (!live_light[i])
        {
            live_light[i] = event;
            live_event_trigger_time[i] = timer_get_msec();
            ok = 1;
            break;
        }
    }
    if (!ok)
        printk("No free light event slots, dropping event %d\n", event_idx);
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
    PUT32(PWM_DMAC, (1 << 31) | (7 << 8) | 7);     // enable DMA, threshold 7, panic threshold 7
}

void engine_init()
{
    memset(audio_sample_buffer, 0, sizeof(audio_sample_buffer));
    memset(live_audio, 0, sizeof(live_audio));
    memset(live_audio_offset, 0, sizeof(live_audio_offset));
    memset(live_light, 0, sizeof(live_light));
    memset(live_event_trigger_time, 0, sizeof(live_event_trigger_time));
    project = NULL;
    engine_usb_ctx = do_usb();
    // pwm_init();
    gpio_set_output(0);
    gpio_set_output(1);
    gpio_set_on(0);
    gpio_set_off(1);
    pcm_dmx_init();
    // pcm_dma_start(audio_sample_buffer[0], sizeof(audio_sample_buffer[0]));
    audio_dma_init();
    printk("DMA INIT DONE\n");
    // while(1){
    //     printk("DMACS %b   PCM %b\n", MK_REG(DMA_CS_REG(0)), MK_REG(I2S_CS));
    // }
}

// ============================================================================
// MIDI input: USB-MIDI packets -> button events
// ============================================================================

#define LP_BUTTONS_PER_PAGE 64
#define LP_PAGES 8

// Button index (row from top * 8 + col, see project.h) of a grid pad note, or
// -1 for anything that is not one of the 64 grid pads. Numbering depends on the
// layout, which the MIDI channel tells apart (manual p.6-8):
//   Session (channel 1) and User 2 (channels 14-16): note = 10*row + col from
//   the bottom-left, 11..88; column 9 (x9) is the right-hand column.
//   User 1 / drum (channels 6-8): notes 36..99 in 4x4 quadrants, bottom rows
//   first; 100..107 is the right-hand column.
static int lp_note_to_button(uint8_t status, uint8_t note)
{
    uint8_t channel = status & 0x0F; // 0 = MIDI channel 1
    int row_from_bottom, col;

    if (channel == 0 || channel >= 13)
    {
        row_from_bottom = note / 10 - 1;
        col = note % 10 - 1;
        if (row_from_bottom < 0 || row_from_bottom > 7 || col < 0 || col > 7)
            return -1;
    }
    else
    {
        if (note < 36 || note > 99)
            return -1;
        int idx = note - 36;
        int quadrant = idx / 16; // 0 bottom-left, 1 top-left, 2 bottom-right, 3 top-right
        row_from_bottom = (idx % 16) / 4 + ((quadrant == 1 || quadrant == 3) ? 4 : 0);
        col = (idx % 16) % 4 + (quadrant >= 2 ? 4 : 0);
    }

    int row = 7 - row_from_bottom; // project.h counts rows from the top
    return row * 8 + col;
}

static void button_pressed(int button)
{
    if (!project || launchpad_current_page >= LP_PAGES)
        return;

    button_event_t *buttons = (button_event_t *)((uint8_t *)project + sizeof(project_t));
    button_event_t *ev = &buttons[launchpad_current_page * LP_BUTTONS_PER_PAGE + button];

    if (ev->audio_sample_idx != 0xffffffff && ev->audio_sample_idx < project->audio_samples)
        play_audio_sample(ev->audio_sample_idx);
    if (ev->light_event_idx != 0xffffffff && ev->light_event_idx < project->light_events)
        play_light_event(ev->light_event_idx);
}

static void handle_midi_packet(const uint8_t *p)
{
    uint8_t cin = p[0] & 0x0F;
    uint8_t status = p[1];
    uint8_t data1 = p[2];
    uint8_t data2 = p[3];

    switch (cin)
    {
    case 0x9: // Note On (velocity 0 = release)
    case 0x8: // Note Off
    {
        int pressed = (cin == 0x9 && data2 > 0);
        int button = lp_note_to_button(status, data1);
        if (button >= 0)
        {
            if (pressed)
                button_pressed(button);
        }
        else if (pressed)
        {
            // Right-hand column (Session x9 / User 1 100..107): paging goes here
            printk("side button: raw %d\n", data1);
            launchpad_current_page = (8 - data1 / 10);
            printk("switching to page %d\n", launchpad_current_page);
        }
        break;
    }
    case 0xB:
        if (data2 > 0)
            printk("top button: CC %d\n", data1);
        break;
    default:
        break;
    }
}

void engine_tick()
{
    uint8_t midi_in_buf[64];
    int midi_in_len = midi_poll(engine_usb_ctx, midi_in_buf);
    if (midi_in_len > 0)
    {
        for (int i = 0; i + 4 <= midi_in_len; i += 4)
            handle_midi_packet(&midi_in_buf[i]);
    }
    dispatch_audio_buffer();
    dispatch_light_events();
    midi_flush(engine_usb_ctx);
}