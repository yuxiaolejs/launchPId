#ifndef PROJECT_H
#define PROJECT_H
#include "types.h"

#define PWM_BASE 0x2020C000
#define PWM_CTL (PWM_BASE + 0x00)
#define PWM_STA (PWM_BASE + 0x04)
#define PWM_DMAC (PWM_BASE + 0x08)
#define PWM0_RNG (PWM_BASE + 0x10)
#define PWM0_DAT (PWM_BASE + 0x14)
#define PWM0_FIF (PWM_BASE + 0x18)
#define PWM1_RNG (PWM_BASE + 0x20)
#define PWM1_DAT (PWM_BASE + 0x24)
#define USEF1 (1 << 5)
#define USEF2 (1 << 13)
#define PWEN1 (1 << 0)
#define PWEN2 (1 << 8)

#define CM_PWMCTL 0x201010A0
#define CM_PWMDIV 0x201010A4
#define CM_PASSWD 0x5A000000

typedef struct project
{
    uint32_t ver;
    uint32_t audio_channels;
    uint32_t light_events;
    uint32_t audio_samples;
    // button_event_structs[64 * 8], 64 per page, 8 pages, goes from pg0 btn0 to pg0 btn63, then pg1 btn0 to pg1 btn63, etc, LS to MS = col, row, page, top-left to bottom-right
    // audio_sample_structs[audio_samples]
    // light_event_structs[light_events]
    // audio sample blob
} project_t;

typedef struct light_event // 4 byte midi message
{
    uint8_t data[4]; // 00 00 00 00 for no-op
    uint32_t delay_after; // ms should wait before next gets triggered
    uint32_t next; // struct idx of next light_event, relative to light_event_structs[0], set 0xffffffff for no-op / eoc
} light_event_t;

typedef struct audio_sample
{
    uint32_t offset; // byte offset from beginning of sample blob region
    uint32_t length; // length of audio sample data in bytes
} audio_sample_t;

typedef struct button_event
{
    uint32_t audio_sample_idx; // struct idx of audio_sample, relative to audio_sample_structs[0], set 0xffffffff for no-op
    uint32_t light_event_idx;  // struct idx of light_event, relative to light_event_structs[0], set 0xffffffff for no-op
} button_event_t;

#endif