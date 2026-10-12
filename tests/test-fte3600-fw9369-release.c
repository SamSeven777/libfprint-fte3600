/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independent synthetic physical bus and expected values from release-v1.
 * No implementation source is included and no high-level stage is replaced. */
#include "fte3600_fw9369_release.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
struct event { size_t length; uint8_t data[22]; uint32_t delay; };
struct bus {
    uint16_t words[32768], adc[8][8], frame[8], irq[8];
    unsigned adc_count, adc_reads, frames, calls, n_events;
    unsigned fail_adc, fail_fifo, fail_word, not_ready, last_row_spike;
    unsigned irq_count, irq_reads, manual_ready, id_reads, id_failures, negotiation_mismatch;
    uint8_t mode;
    struct event events[4096];
};
static void scalar(uint16_t samples[8], uint16_t value)
{ for (unsigned i = 0; i < 8; ++i) samples[i] = value; }
static void init_bus(struct bus *b)
{
    memset(b, 0, sizeof(*b)); b->mode = 0x50;
    for (unsigned i = 0; i < 8; ++i) { scalar(b->adc[i], 500); b->frame[i] = 2040; }
    b->adc_count = 1; b->words[0x1881] = 2; b->words[0x1a8b] = 0x9362;
}
static uint32_t spi(void *context, uint8_t *data, size_t length)
{
    struct bus *b = context;
    CHECK(b->n_events < 4096); struct event *e = &b->events[b->n_events++];
    e->length = length; memcpy(e->data, data, length < sizeof(e->data) ? length : sizeof(e->data));
    ++b->calls;
    if (length == 3) {
        if (data[0] == 0x5a || data[0] == 0xc0) b->mode = 0x50;
        if (data[0] == 0xc4) b->mode = 0x54;
        return 0;
    }
    if (data[0] == 8 && length == 5) { data[4] = data[2] == 0xc6 ? !b->negotiation_mismatch : b->mode; return 0; }
    if (data[0] == 9 && length == 4) return 0;
    if (data[0] == 5 && length >= 8) {
        size_t at = ((size_t)(data[2] & 127) << 8) | data[3];
        if (at == 0x1885) b->manual_ready = 1;
        for (size_t i = 6; i + 1 < length; i += 2)
            b->words[at++] = (uint16_t)((uint16_t)data[i] << 8 | data[i+1]);
        return 0;
    }
    if (data[0] == 4 && length >= 10) {
        size_t at = ((size_t)(data[2] & 127) << 8) | data[3];
        if (at == 0xb8 || at == 0xe8) {
            if (b->fail_adc) return UINT32_C(0xc0000185);
            CHECK(length == 14 || length == 22);
            unsigned n = b->adc_reads < b->adc_count ? b->adc_reads : b->adc_count - 1;
            for (size_t i = 0; i < (length - 6) / 2; ++i) {
                data[6+i*2] = (uint8_t)(b->adc[n][i] >> 8); data[7+i*2] = (uint8_t)b->adc[n][i];
            }
            ++b->adc_reads; b->manual_ready = 0; return 0;
        }
        if (b->fail_word) return UINT32_C(0xc0000185);
        CHECK(length == 12);
        uint16_t value = b->words[at];
        if (at == 0x1a82) {
            value = b->not_ready ? 0 : 0x28;
            if (b->irq_count && !b->manual_ready) {
                unsigned n = b->irq_reads < b->irq_count ? b->irq_reads : b->irq_count - 1;
                value = b->irq[n]; ++b->irq_reads;
            }
        }
        if (at == 0x1a8b) { ++b->id_reads; if (b->id_failures) { --b->id_failures; value = 0; } }
        data[6] = (uint8_t)(value >> 8); data[7] = (uint8_t)value; return 0;
    }
    if (data[0] == 6 && length == 10246) {
        if (b->fail_fifo) return UINT32_C(0xc0000185);
        unsigned n = b->frames < 8 ? b->frames : 7;
        for (size_t i = 0; i < 5120; ++i) {
            uint16_t value = b->last_row_spike && i == 5119 ? 2301 : b->frame[n];
            data[6+i*2] = (uint8_t)(value >> 8); data[7+i*2] = (uint8_t)value;
        }
        ++b->frames; return 0;
    }
    CHECK(0); return 0;
}
static void wait_ms(void *context, uint32_t delay)
{
    struct bus *b = context; CHECK(b->n_events < 4096);
    b->events[b->n_events++].delay = delay;
}
static struct fte3600_fw9369_startup_state state(void)
{
    struct fte3600_fw9369_startup_state s = FTE3600_FW9369_STARTUP_STATE_INITIALIZER;
    s.fdt.configured = s.fdt.fdt_configured = s.fdt.valid = s.image_valid = 1;
    s.fdt.mode = 0xa1; s.image_median = 500;
    s.baseline_flag_1 = s.baseline_flag_2 = 1;
    scalar(s.fdt_reference, 500); scalar(s.fdt_down, 500); scalar(s.fdt_up, 500);
    s.fdt_down_crc = s.fdt_up_crc = 0x5b5b;
    for (unsigned i = 0; i < 5120; ++i) s.baseline[i] = s.work_image[i] = 2000;
    return s;
}
static void classifications(void)
{
    const struct { unsigned sample; enum fte3600_fw9369_detection choice; enum fte3600_fw9369_contact expected; } cases[] = {
        {449, FTE3600_FW9369_DOWN, FTE3600_FW9369_CONTACT_DOWN},
        {450, FTE3600_FW9369_DOWN, FTE3600_FW9369_CONTACT_NONE},
        {550, FTE3600_FW9369_DOWN, FTE3600_FW9369_CONTACT_NONE},
        {551, FTE3600_FW9369_DOWN, FTE3600_FW9369_CONTACT_INVALID},
        {455, FTE3600_FW9369_UP, FTE3600_FW9369_CONTACT_NONE},
        {456, FTE3600_FW9369_UP, FTE3600_FW9369_CONTACT_UP},
    };
    struct bus *b = calloc(1, sizeof(*b)); CHECK(b);
    const struct fte3600_capture_io io = {b, spi, wait_ms};
    struct fte3600_fw9369_release_diagnostics d;
    struct fte3600_fw9369_observation o;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b); scalar(b->adc[0], cases[i].sample);
        CHECK(!fte3600_fw9369_observe(&s, cases[i].choice, &o, &io, &d));
        CHECK(o.contact == cases[i].expected && d.observations == 1 && b->adc_reads == 1);
    }
    struct fte3600_fw9369_startup_state s = state(); init_bus(b);
    s.config.repeat = 2; b->adc[0][0] = b->adc[0][1] = 449;
    CHECK(!fte3600_fw9369_observe(&s, FTE3600_FW9369_DOWN, &o, &io, &d));
    CHECK(o.contact == FTE3600_FW9369_CONTACT_DOWN);
    b->adc[0][3] = 551;
    CHECK(!fte3600_fw9369_observe(&s, FTE3600_FW9369_DOWN, &o, &io, &d));
    CHECK(o.contact == FTE3600_FW9369_CONTACT_INVALID);
    scalar(s.fdt_down, 65535); scalar(b->adc[0], 0);
    CHECK(!fte3600_fw9369_observe(&s, FTE3600_FW9369_DOWN, &o, &io, &d));
    CHECK(o.contact == FTE3600_FW9369_CONTACT_DOWN && o.differences[0] == -1);
    scalar(s.fdt_up, 65535); scalar(b->adc[0], 65520);
    CHECK(!fte3600_fw9369_observe(&s, FTE3600_FW9369_UP, &o, &io, &d));
    CHECK(o.contact == FTE3600_FW9369_CONTACT_UP);
    s.config.channel_mode = 0; s.config.average = 3; scalar(s.fdt_up, 500); scalar(b->adc[0], 450);
    b->adc[0][5] = b->adc[0][6] = b->adc[0][7] = 500;
    CHECK(!fte3600_fw9369_observe(&s, FTE3600_FW9369_UP, &o, &io, &d));
    CHECK(o.contact == FTE3600_FW9369_CONTACT_UP && o.samples[7] == 500);
    free(b);
}
static void confirmation(void)
{
    struct bus *b = calloc(1, sizeof(*b)); CHECK(b);
    const struct fte3600_capture_io io = {b, spi, wait_ms};
    struct fte3600_fw9369_release_diagnostics d;
    uint16_t samples[3][8]; int confirmed;
    for (unsigned quiet = 2; quiet <= 4; ++quiet) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b); scalar(s.fdt_up, 300);
        for (unsigned c = 0; c < 4; ++c) b->adc[0][c] = c < quiet ? (c & 1 ? 451 : 549) : (c & 1 ? 450 : 550);
        memset(samples, 0x5a, sizeof(samples)); confirmed = -1;
        CHECK(!fte3600_fw9369_confirm_release(&s, samples, &confirmed, &io, &d));
        CHECK(confirmed == (quiet >= 3)); CHECK(b->adc_reads == (quiet >= 3 ? 3u : 1u));
        CHECK(samples[0][0] == (quiet >= 3 ? 549 : 0x5a5a));
    }
    struct fte3600_fw9369_startup_state s = state(); init_bus(b); b->adc_count = 3;
    scalar(b->adc[1], 450); confirmed = -1; memset(samples, 0x5a, sizeof(samples));
    CHECK(!fte3600_fw9369_confirm_release(&s, samples, &confirmed, &io, &d));
    CHECK(!confirmed && b->adc_reads == 2 && samples[0][0] == 0x5a5a);
    s = state(); init_bus(b); s.config.channel_mode = 0;
    for (unsigned c = 4; c < 8; ++c) b->adc[0][c] = 900;
    CHECK(!fte3600_fw9369_confirm_release(&s, samples, &confirmed, &io, &d));
    CHECK(confirmed && samples[2][7] == 900); /* quietness is first-four only */
    s = state(); init_bus(b); scalar(s.fdt_down, 0); scalar(s.fdt_up, 0); scalar(b->adc[0], 65535);
    CHECK(!fte3600_fw9369_confirm_release(&s, samples, &confirmed, &io, &d)); CHECK(confirmed);
    free(b);
}
static void maintenance(void)
{
    struct fte3600_fw9369_startup_state s = state();
    uint16_t samples[3][8];
    for (unsigned n = 0; n < 3; ++n) {
        samples[n][0] = (uint16_t)(519+n); samples[n][1] = (uint16_t)(549+n);
        samples[n][2] = 65535; samples[n][3] = 30;
        for (unsigned c = 4; c < 8; ++c) samples[n][c] = (uint16_t)(727+c);
    }
    s.fdt_down[2] = 65510; s.fdt_down[3] = 0;
    CHECK(!fte3600_fw9369_update_fdt(&s, (const uint16_t (*)[8])samples));
    const uint16_t expected[] = {500,520,0,0,500,500,500,500};
    CHECK(!memcmp(s.fdt_down, expected, sizeof(expected)));
    CHECK(!memcmp(s.fdt_up, expected, sizeof(expected)) && !memcmp(s.fdt_reference, expected, sizeof(expected)));
    CHECK(s.fdt_up_crc == 0x827e && s.fdt_down_crc == 0x827e);
    s.config.channel_mode = 0;
    CHECK(!fte3600_fw9369_update_fdt(&s, (const uint16_t (*)[8])samples));
    const uint16_t expected8[] = {500,520,65505,0,701,702,703,704};
    CHECK(!memcmp(s.fdt_down, expected8, sizeof(expected8)));
    /* Active/inactive distinction: switching to eight channels admits all words. */
    s = state(); s.config.channel_mode = 0;
    for (unsigned n = 0; n < 3; ++n) scalar(samples[n], 500);
    CHECK(!fte3600_fw9369_update_fdt(&s, (const uint16_t (*)[8])samples));
    CHECK(s.fdt_up_crc == 0xd616 && s.fdt_down_crc == 0xd616);
}
static int reset_needed(struct fte3600_fw9369_startup_state *s)
{ int needed = -1; CHECK(!fte3600_fw9369_baseline_needs_reset(s, &needed)); return needed; }
static void fill_image(uint16_t *image, uint16_t value)
{ for (unsigned i = 0; i < 5120; ++i) image[i] = value; }
static void baseline_checks(void)
{
    struct fte3600_fw9369_startup_state s = state(); CHECK(!reset_needed(&s));
    s.baseline_crc ^= 1; CHECK(!reset_needed(&s)); /* image CRC alone is not a trigger */
    s.fdt_down_crc ^= 1; CHECK(reset_needed(&s)); s = state(); s.fdt_up_crc ^= 1; CHECK(reset_needed(&s));
    s = state(); for (unsigned i = 0; i < 4; ++i) s.fdt_down[i] = 512; CHECK(reset_needed(&s));
    s = state(); for (unsigned i = 0; i < 4; ++i) s.fdt_up[i] = 512; CHECK(reset_needed(&s));
    s = state(); fill_image(s.baseline, 2120); CHECK(!reset_needed(&s));
    fill_image(s.baseline, 2124); CHECK(reset_needed(&s));
    s = state(); fill_image(s.work_image, 1880); CHECK(!reset_needed(&s));
    fill_image(s.work_image, 1876); CHECK(reset_needed(&s));
    for (unsigned count = 1024; count <= 1230; count += 206) {
        s = state(); s.image_median = 510;
        for (unsigned i = 0; i < 5120; ++i) {
            int deviation = i < count ? 13 : 12;
            s.work_image[i] = (uint16_t)(1876 + (i & 1 ? deviation : -deviation));
        }
        CHECK(reset_needed(&s) == (count == 1024)); /* exact spreads149 and150 */
    }
    s = state(); s.image_median = 600; fill_image(s.baseline, 1000);
    CHECK(!reset_needed(&s)); /* negative sum wraps: spread167772, not zero */
    s = state(); s.image_median = 20; fill_image(s.baseline, 0); fill_image(s.work_image, 0);
    CHECK(!reset_needed(&s)); /* signed learned median minus30 */
    s = state(); fill_image(s.baseline, 65535); fill_image(s.work_image, 65535); s.image_median = 1000;
    CHECK(!reset_needed(&s)); /* raw values are divided by4 and capped1023 */
    s = state(); fill_image(s.baseline, 65535);
    for (unsigned y = 1; y <= 78; ++y) for (unsigned x = 2; x <= 61; ++x) s.baseline[y*64+x] = 2000;
    CHECK(!reset_needed(&s)); /* excluded borders do not alter the median */
    unsigned n = 0;
    for (unsigned y = 1; y <= 78; ++y) for (unsigned x = 2; x <= 61; ++x)
        s.baseline[y*64+x] = n++ < 2340 ? 2120 : 2124;
    CHECK(reset_needed(&s)); /* upper median, not mean of the central pair */
}
static void arm_and_errors(void)
{
    struct bus *b = calloc(1, sizeof(*b)); CHECK(b);
    const struct fte3600_capture_io io = {b, spi, wait_ms};
    struct fte3600_fw9369_release_diagnostics d;
    for (unsigned choice = 1; choice <= 2; ++choice) for (unsigned foundry = 0; foundry < 2; ++foundry) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b); s.config.foundry = foundry ? 0xaa : 0;
        for (unsigned i = 0; i < 8; ++i) { s.fdt_up[i] = (uint16_t)(600+i); s.fdt_down[i] = (uint16_t)(500+i); }
        b->words[0x1881] = 0x12fe;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            CHECK(!fte3600_fw9369_arm_detection(&s, (enum fte3600_fw9369_detection)choice, &io, &d));
            CHECK(!b->adc_reads && !b->frames && s.arm_mode == (choice == FTE3600_FW9369_UP ? 0u : 1u));
            CHECK(s.fdt.valid && s.image_valid && s.baseline_flag_1 && s.baseline_flag_2);
            CHECK(b->n_events >= 8); struct event *tail = b->events+b->n_events-8;
            CHECK(tail[0].length == 4 && tail[0].data[0] == 9 && tail[0].data[2] == 0x9a && tail[0].data[3] == 0x5a);
            CHECK(tail[1].length == 22 && tail[1].data[0] == 5 && tail[1].data[3] == (foundry ? 0xe0 : 0xb0));
            CHECK(tail[1].data[5] == 8);
            for (unsigned c = 0; c < 8; ++c) CHECK(((unsigned)tail[1].data[6+c*2]*256+tail[1].data[7+c*2]) == (choice == 2 ? 600u+c : 500u+c));
            CHECK(tail[2].length == 4 && tail[2].data[3] == 0);
            CHECK(tail[3].length == 12 && tail[3].data[3] == 0x81);
            CHECK(tail[4].length == 8 && tail[4].data[6] == 0x12 && tail[4].data[7] == (choice == 2 ? 0x7c : 0xfd));
            CHECK(tail[5].length == 8 && tail[5].data[3] == 0x84 && tail[5].data[7] == 0x2f);
            CHECK(tail[6].length == 3 && tail[6].data[0] == 0xc2 && tail[7].delay == 1);
        }
    }
    init_bus(b); uint16_t irq = 0xffff;
    CHECK(!fte3600_fw9369_read_irq(&irq, &io, &d)); CHECK(irq == 0x28 && b->words[0x1a84] == 0x28 && b->calls == 2);
    init_bus(b); b->fail_word = 1; irq = 0xffff;
    CHECK(fte3600_fw9369_read_irq(&irq, &io, &d) == FTE3600_FW9369_RELEASE_WORD_READ_ERROR);
    CHECK(irq == 0xffff && d.transport.exhausted_transactions == 1 && d.transport.failed_attempts == 3 && b->calls == 3);
    struct fte3600_fw9369_startup_state s = state(); struct fte3600_fw9369_observation o, before;
    memset(&o, 0x5a, sizeof(o)); before = o; init_bus(b); b->fail_adc = 1;
    CHECK(fte3600_fw9369_observe(&s, FTE3600_FW9369_UP, &o, &io, &d) == FTE3600_FW9369_RELEASE_ADC_READ_ERROR);
    CHECK(!memcmp(&o, &before, sizeof(o)) && !d.observations);
    s = state(); init_bus(b); b->not_ready = 1;
    CHECK(fte3600_fw9369_observe(&s, FTE3600_FW9369_UP, &o, &io, &d) == FTE3600_FW9369_RELEASE_NOT_READY);
    CHECK(!memcmp(&o, &before, sizeof(o)) && d.transport.ready_timeout);
    init_bus(b); memset(&d, 0x5a, sizeof(d)); struct fte3600_fw9369_release_diagnostics saved = d;
    CHECK(fte3600_fw9369_observe(&s, 0, &o, &io, &d) == FTE3600_FW9369_RELEASE_INVALID_ARGUMENT);
    CHECK(!b->calls && !memcmp(&saved, &d, sizeof(d)));
    free(b);
}
static void composed(void)
{
    struct bus *b = calloc(1, sizeof(*b)); CHECK(b);
    struct fte3600_fw9369_startup_workspace *w = calloc(1, sizeof(*w)); CHECK(w);
    const struct fte3600_capture_io io = {b, spi, wait_ms};
    struct fte3600_fw9369_release_diagnostics d;
    enum fte3600_fw9369_release_outcome outcome;
    for (unsigned branch = 0; branch < 6; ++branch) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b); b->adc_count = 5;
        if (branch == 1) scalar(b->adc[0], 450);
        if (branch == 2) scalar(b->adc[4], 400);
        if (branch == 3) { scalar(b->adc[3], 400); s.image_median = 0; }
        if (branch == 4) { s.image_median = 0; b->frame[1] = 2048; b->frame[2] = b->frame[3] = 2052; }
        outcome = (enum fte3600_fw9369_release_outcome)99;
        CHECK(!fte3600_fw9369_release_run(&s, branch != 5, w, sizeof(*w), &outcome, &io, &d));
        CHECK(outcome == (branch == 1 ? FTE3600_FW9369_RELEASE_PENDING : branch == 2 ? FTE3600_FW9369_RELEASE_RENEWED_TOUCH : FTE3600_FW9369_RELEASE_CONFIRMED));
        CHECK(b->adc_reads == (branch == 1 ? 1u : branch == 5 ? 3u : 5u));
        CHECK(b->frames == (branch == 1 || branch == 5 ? 0u : branch == 4 ? 4u : 1u));
        CHECK(d.recalibrated == (branch == 4));
        CHECK(s.baseline[0] == (branch == 1 || branch == 5 ? 2000 : branch == 4 ? 2052 : 2040));
    }
    for (unsigned failure = 0; failure < 2; ++failure) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b);
        b->fail_fifo = failure == 0; b->last_row_spike = failure == 1;
        outcome = (enum fte3600_fw9369_release_outcome)99;
        enum fte3600_fw9369_release_status rc = fte3600_fw9369_release_run(&s, 1, w, sizeof(*w), &outcome, &io, &d);
        CHECK(rc == (failure == 0 ? FTE3600_FW9369_RELEASE_IMAGE_ERROR : FTE3600_FW9369_RELEASE_UNDEFINED_NEIGHBORHOOD));
        CHECK(outcome == (enum fte3600_fw9369_release_outcome)99 && s.baseline[0] == 2000 && s.baseline[5119] == 2000 && !s.baseline_crc);
    }
    free(w); free(b);
}
static unsigned acknowledgments(const struct bus *b, uint16_t value)
{
    unsigned count = 0;
    for (unsigned i = 0; i < b->n_events; ++i) {
        const struct event *e = &b->events[i];
        if (e->length == 8 && e->data[0] == 5 && e->data[2] == 0x9a && e->data[3] == 0x84 &&
            e->data[6] == value/256 && e->data[7] == value%256) ++count;
    }
    return count;
}
static void event_queries(void)
{
    struct bus *b = calloc(1, sizeof(*b)); CHECK(b);
    struct fte3600_fw9369_startup_workspace *w = calloc(1, sizeof(*w)); CHECK(w);
    const struct fte3600_capture_io io = {b, spi, wait_ms};
    struct fte3600_fw9369_release_diagnostics d;
    enum fte3600_fw9369_event outcome;
    const uint16_t initial[] = {0, 0x20, 6, 0x12, 0x14, 0x11, 0x200, 0x400, 5};
    for (unsigned i = 0; i < sizeof(initial)/sizeof(initial[0]); ++i) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b);
        b->irq_count = 2; b->irq[0] = initial[i]; unsigned rejected = 3;
        CHECK(!fte3600_fw9369_event_run(&s, 1, &rejected, w, sizeof(*w), &outcome, &io, &d));
        CHECK(outcome == (initial[i] & 0x610 ? FTE3600_FW9369_EVENT_RECOVERY : initial[i] & 2 ? FTE3600_FW9369_EVENT_TOUCH : initial[i] == 5 ? FTE3600_FW9369_EVENT_RELEASED : FTE3600_FW9369_EVENT_PENDING));
        CHECK(b->id_reads == 3 && b->irq_reads == 2 && rejected == 3);
        CHECK(b->adc_reads == (initial[i] == 5 ? 5u : 0u));
        CHECK(b->frames == (initial[i] == 5 ? 1u : 0u));
        CHECK(acknowledgments(b, initial[i]) >= 1);
        /* Complete query starts with three negotiation/ID pairs in this order. */
        for (unsigned pair = 0; pair < 3; ++pair) {
            const struct event *e = b->events+4*pair;
            CHECK(e[0].length == 4 && e[0].data[2] == 0xc6 && e[0].data[3] == 1);
            CHECK(e[1].delay == 4 && e[2].length == 5 && e[2].data[2] == 0xc6);
            CHECK(e[3].length == 12 && e[3].data[2] == 0x9a && e[3].data[3] == 0x8b);
        }
    }
    /* Late UP supersedes both touch and recovery and forces maintenance even
     * when the caller did not have any pending maintenance. */
    for (unsigned i = 0; i < 3; ++i) {
        struct fte3600_fw9369_startup_state s = state(); init_bus(b); b->irq_count = 2;
        b->irq[0] = i == 0 ? 2 : i == 1 ? 0x10 : 0x200; b->irq[1] = 0xc;
        unsigned rejected = 7;
        CHECK(!fte3600_fw9369_event_run(&s, 0, &rejected, w, sizeof(*w), &outcome, &io, &d));
        CHECK(outcome == FTE3600_FW9369_EVENT_RELEASED && b->adc_reads == 5 && b->frames == 1 && rejected == 7);
        CHECK(acknowledgments(b, 0xc) == 3);
    }
    struct fte3600_fw9369_startup_state s = state(); init_bus(b); b->irq_count = 2; b->irq[0] = 2; b->irq[1] = 4;
    b->adc_count = 5; scalar(b->adc[4], 400); unsigned rejected = 7;
    CHECK(!fte3600_fw9369_event_run(&s, 0, &rejected, w, sizeof(*w), &outcome, &io, &d));
    CHECK(outcome == FTE3600_FW9369_EVENT_TOUCH && b->frames == 1); /* local late-DOWN protection */
    rejected = 0;
    for (unsigned i = 1; i <= 11; ++i) {
        s = state(); init_bus(b); b->irq_count = 2; b->irq[0] = 4; scalar(b->adc[0], 450);
        CHECK(!fte3600_fw9369_event_run(&s, 1, &rejected, w, sizeof(*w), &outcome, &io, &d));
        CHECK(rejected == (i == 11 ? 0 : i));
        CHECK(outcome == (i == 11 ? FTE3600_FW9369_EVENT_RECOVERY : FTE3600_FW9369_EVENT_PENDING));
        CHECK(b->adc_reads == 1 && !b->frames);
    }
    s = state(); init_bus(b); b->irq_count = 2; b->irq[0] = 0x10; b->irq[1] = 4;
    scalar(b->adc[0], 450); rejected = 7;
    CHECK(!fte3600_fw9369_event_run(&s, 1, &rejected, w, sizeof(*w), &outcome, &io, &d));
    CHECK(outcome == FTE3600_FW9369_EVENT_PENDING && rejected == 7 && s.arm_mode == 0);
    for (unsigned fail = 1; fail <= 11; fail += 10) {
        s = state(); init_bus(b); b->irq_count = 2; b->irq[0] = 2; b->id_failures = fail;
        CHECK(!fte3600_fw9369_event_run(&s, 1, &rejected, w, sizeof(*w), &outcome, &io, &d));
        CHECK(outcome == (fail == 1 ? FTE3600_FW9369_EVENT_TOUCH : FTE3600_FW9369_EVENT_RECOVERY));
        CHECK(b->id_reads == (fail == 1 ? 4u : 11u));
        CHECK(b->irq_reads == (fail == 1 ? 2u : 0u));
    }
    s = state(); init_bus(b); b->negotiation_mismatch = 1; b->irq_count = 2; b->irq[0] = 2;
    CHECK(!fte3600_fw9369_event_run(&s, 1, &rejected, w, sizeof(*w), &outcome, &io, &d));
    CHECK(outcome == FTE3600_FW9369_EVENT_TOUCH && b->id_reads == 3); /* ID decides after31 mismatches */
    free(w); free(b);
}
int main(void)
{
    classifications(); confirmation(); maintenance(); baseline_checks(); arm_and_errors(); composed(); event_queries();
    puts("FW9369 release: independent physical transactions, strict boundaries, finite-width statistics, confirmed maintenance and failure guards passed");
    return 0;
}
