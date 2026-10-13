/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Public-entrypoint test: production runtime/session objects, synthetic
 * character devices and the independently frozen startup transcript. */
#define _XOPEN_SOURCE 700
#include "fte3600_linux_runtime.h"
#define main board_suite_main
#define config board_test_config
#define calls board_metadata_calls
#include "test-fte3600-linux-board.c"
#undef calls
#undef config
#undef main
#undef CHECK
#define main session_suite_main
#include "test-fte3600-device-session.c"
#undef main

static struct bus *fw_bus;
static uint64_t fw_wait_remaining;
static struct {
    uint16_t status, adc_value, warm_value, up_status, down_status;
    unsigned arm, bank_pending, up_arms, down_arms, adc_reads, user_frames, warm_frames;
    unsigned reset_levels[16], resets, recovering, recovery_frames, recovery_adc;
    uint64_t reset_times[16], reset_spi_times[5];
    unsigned fail_adc, fail_word, fail_warm, spike, not_ready, no_irq;
    unsigned cancel_at, deadline_at, fail_reset, recovery_seen;
    unsigned status_reads, late_at, adc_since_frame;
    uint16_t late_irq;
    /* Synthetic discrimination interval, not an electrical timing claim. */
    unsigned quiet_ms, quiet_waiting, quiet_mutations, quiet_reads;
    unsigned latched_events, irq_pending, irq_delivered, auto_arms, bad_policy;
    unsigned policy_read_failure, policy_write_failure, startup_arms;
    uint64_t armed_at, cancel_time;
    GCancellable *cancel;
} hw;
gint64 __wrap_g_get_monotonic_time(void)
{ return (gint64)fake.clock_ms * 1000; }
static void fw_advance(void)
{
    if (hw.cancel_time && fake.clock_ms>=hw.cancel_time && hw.cancel)
        g_cancellable_cancel(hw.cancel);
    if (!hw.quiet_ms || !hw.quiet_waiting || fake.clock_ms-hw.armed_at<hw.quiet_ms ||
        (fw_bus->words[0x1a83]&0x7f)!=0x16) return;
    uint16_t event=hw.arm==1?hw.down_status:hw.up_status;
    if (event) {
        hw.status|=event;hw.quiet_waiting=0;
        ++hw.latched_events;++hw.irq_pending;
    }
}
static void fw_automatic_arm(int startup)
{
    hw.arm=(fw_bus->words[0x1881]&0x81)==0x81?1:2;
    ++hw.auto_arms;if (startup) ++hw.startup_arms;
    if ((fw_bus->words[0x1a83]&0x7f)!=0x16) ++hw.bad_policy;
    if (hw.quiet_ms) { hw.quiet_waiting=1;hw.armed_at=fake.clock_ms; }
}
static uint32_t fw_spi(uint8_t *data, size_t length)
{
    uint16_t at=length>=4?(uint16_t)(((data[2]&127u)<<8)|data[3]):0;
    if (fw_bus->next < fw_bus->fixture->count) {
        const struct event *next=&fw_bus->fixture->events[fw_bus->next];
        int policy=next->kind==0 && next->length==8 && next->tx[0]==5 &&
            next->tx[2]==0x9a && next->tx[3]==0x84 && next->tx[7]==0x2f;
        if (policy && at==0x1a83 && ((data[0]==4 && hw.policy_read_failure) ||
            (data[0]==5 && hw.policy_write_failure))) return UINT32_C(0xc0000185);
        int automatic=length==3 && data[0]==0xc2 && hw.bank_pending;
        if (length==22 && data[0]==5 && (at==0xb0 || at==0xe0)) hw.bank_pending=1;
        uint32_t rc=spi(fw_bus,data,length);
        if (automatic) { hw.bank_pending=0;fw_automatic_arm(1); }
        return rc;
    }
    fw_advance();
    if (hw.quiet_waiting && !hw.status) {
        if (length==12 && data[0]==4 && at==0x1a82) ++hw.quiet_reads;
        else if (data[0]!=4 && data[0]!=8) ++hw.quiet_mutations;
    }
    ++fw_bus->calls;
    if (hw.resets && hw.resets%3==0 && !hw.reset_spi_times[hw.resets/3-1])
        hw.reset_spi_times[hw.resets/3-1]=fake.clock_ms;
    if (at==0x1a83 && hw.bank_pending && ((data[0]==4 && hw.policy_read_failure) ||
        (data[0]==5 && hw.policy_write_failure))) return UINT32_C(0xc0000185);
    if (length==5 && data[0]==8) {
        data[4]=data[2]==0x80?fw_bus->mode:fw_bus->bytes[data[2]];return 0;
    }
    if (length==4 && data[0]==9) { fw_bus->bytes[data[2]]=data[3];return 0; }
    if (data[0]==4 && length>=10) {
        if (at==0xb8 || at==0xe8) {
            if (hw.fail_adc) return UINT32_C(0xc0000185);
            uint16_t value=hw.adc_value?hw.adc_value:(uint16_t)(fw_bus->words[0xb0]+33);
            if (hw.recovering) {
                value=fw_bus->words[0x180c]==0?
                    (fw_bus->words[0x1801]==0xfc9b?512:513):512;
                ++hw.recovery_adc;
            } else { ++hw.adc_reads;++hw.adc_since_frame; }
            for (size_t i=6;i+1<length;i+=2) { data[i]=(uint8_t)(value>>8);data[i+1]=(uint8_t)value; }
            if (hw.cancel_at==1 && hw.adc_reads==3) g_cancellable_cancel(hw.cancel);
            return 0;
        }
        if (hw.fail_word) return UINT32_C(0xc0000185);
        if (at==0x1a82 && ++hw.status_reads==hw.late_at) hw.status|=hw.late_irq;
        uint16_t value=at==0x1a82?hw.status:at==0x1a8b?0x9362:fw_bus->words[at];
        if (at==0x1a82 && hw.not_ready && (value&8)) value=0;
        if (at==0x1a82 && (value&0x610)) hw.recovery_seen=1;
        else if (at==0x1a82 && hw.recovery_seen && hw.fail_reset) {
            fake.fail_ioctl=fake.ioctls+1;hw.fail_reset=0;
        }
        memset(data+6,0,length-6);data[6]=(uint8_t)(value>>8);data[7]=(uint8_t)value;return 0;
    }
    if (length==10246 && data[0]==6) {
        int maintenance=hw.arm==2 || hw.adc_since_frame>=3;
        if (maintenance && hw.fail_warm) return UINT32_C(0xc0000185);
        for (size_t i=0;i<5120;++i) {
            uint16_t value=hw.recovering?
                (fw_bus->words[0x1801]==0xfcb6?2048:2052):
                maintenance?hw.warm_value:sample(fw_bus,i);
            if (hw.spike && maintenance && i==5119) value=2500;
            data[6+2*i]=(uint8_t)(value>>8);data[7+2*i]=(uint8_t)value;
        }
        if (hw.recovering) ++hw.recovery_frames;
        else if (maintenance) ++hw.warm_frames;else ++hw.user_frames;
        hw.adc_since_frame=0;
        if (hw.cancel_at==2 && maintenance) g_cancellable_cancel(hw.cancel);
        ++fw_bus->frames;return 0;
    }
    record_write(fw_bus,data,length);
    if (data[0]==5 && length>=8) {
        uint16_t value=(uint16_t)((data[6]<<8)|data[7]);
        if (at==0x1a84) hw.status&=(uint16_t)~value;
        if (at==0x1885) hw.status|=8;
        if (length==22 && (at==0xb0 || at==0xe0)) hw.bank_pending=1;
    }
    if (length==3 && data[0]==0xc4) hw.status|=0x20;
    if (length==3 && data[0]==0xc2 && hw.bank_pending) {
        hw.bank_pending=0;
        fw_automatic_arm(0);
        if (hw.arm==1) {
            ++hw.down_arms;if (!hw.quiet_ms) hw.status|=hw.down_status;
            if (hw.recovering) hw.recovering=0;
            if (hw.cancel_at==3) g_cancellable_cancel(hw.cancel);
            if (hw.deadline_at==3) fake.clock_ms+=10000;
        } else { ++hw.up_arms;if (!hw.quiet_ms) hw.status|=hw.up_status; }
    }
    return 0;
}
static void fw_gpio(int level)
{
    if (fw_bus->next < fw_bus->fixture->count) { CHECK(!gpio(fw_bus,level));return; }
    CHECK(hw.resets<G_N_ELEMENTS(hw.reset_levels));
    hw.reset_levels[hw.resets]=(unsigned)level;hw.reset_times[hw.resets++]=fake.clock_ms;
    hw.recovering=1;hw.status=0;hw.recovery_seen=0;hw.quiet_waiting=0;
}
static void fw_sleep(uint64_t ns)
{
    if (fw_bus->next < fw_bus->fixture->count || fw_wait_remaining) {
        if (!fw_wait_remaining) {
            const struct event *e = &fw_bus->fixture->events[fw_bus->next++];
            CHECK(e->kind == 2 || e->kind == 3);
            fw_wait_remaining = (uint64_t)e->value * (e->kind == 2 ? 1000000 : 1000);
        }
        CHECK(ns <= fw_wait_remaining);
        fw_wait_remaining -= ns;
    }
    fw_bus->clock += ns / 1000;
}
static int fw_poll(int timeout)
{
    fw_advance();
    if (!hw.quiet_ms) return timeout > 0 && !hw.no_irq && hw.status;
    if (timeout<=0 || hw.no_irq || !hw.irq_pending) return 0;
    --hw.irq_pending;++hw.irq_delivered;return 1;
}
static void fw_setup(const struct fixture *f)
{
    fw_bus = calloc(1, sizeof(*fw_bus));
    CHECK(fw_bus);
    fw_bus->fixture = f;
    fw_bus->mode = 0x50;
    fw_wait_remaining = 0;
    memset(&hw,0,sizeof(hw));hw.arm=1;hw.warm_value=2100;hw.up_status=4;hw.down_status=2;
    reset_fake(1);
    bus_spi = fw_spi;
    bus_gpio = fw_gpio;
    bus_sleep = fw_sleep;
    bus_poll = fw_poll;
}
static void fw_teardown(void)
{
    free(fw_bus);
    fw_bus = NULL;
    bus_spi = NULL;
    bus_gpio = NULL;
    bus_sleep = NULL;
    bus_poll = NULL;
}
static struct fte3600_linux_runtime *fw_open_mode(const struct fixture *f,unsigned quiet_ms)
{
    fw_setup(f);
    hw.quiet_ms=quiet_ms;
    char device[PATH_MAX];
    path(device, devroot, "spidev0.0");
    const struct fte3600_linux_runtime_config c = {device, sysroot, devroot, NULL};
    struct fte3600_linux_runtime *r = NULL;
    struct fte3600_linux_runtime_diagnostic d;
    CHECK(!fte3600_linux_runtime_open(&c, NULL, &r, &d));
    CHECK(fw_bus->next == f->count && !fw_wait_remaining);
    CHECK(hw.startup_arms==1);
    if (!quiet_ms) {
        CHECK(fw_bus->linux_policy_reads==1 && fw_bus->linux_policy_writes==1);
        CHECK(!hw.bad_policy);
    }
    hw.status=quiet_ms?0:2;return r;
}
static struct fte3600_linux_runtime *fw_open(const struct fixture *f)
{ return fw_open_mode(f,0); }
static struct fte3600_device_status fw_status(struct fte3600_linux_runtime *r)
{
    struct fte3600_device_status s;CHECK(!fte3600_linux_runtime_get_status(r,&s));return s;
}
static void fw_close(struct fte3600_linux_runtime *r)
{
    CHECK(!fte3600_linux_runtime_close(r,NULL));
    fw_teardown();
}
static void capture_ok(struct fte3600_linux_runtime *r,unsigned sequence)
{
    struct fte3600_linux_runtime_frame frame={0};struct fte3600_linux_runtime_diagnostic d;
    enum fte3600_linux_runtime_error rc=fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d);
    if (rc) fprintf(stderr,"capture rc=%u device=%u component=%u status=%04x adc=%u warm=%u recovery=%u mask=%04x arms=%u quiet_mutations=%u latched=%u\n",
        rc,d.device_error,fw_status(r).component_error,hw.status,hw.adc_reads,hw.warm_frames,hw.resets,
        fw_bus->words[0x1a83],hw.auto_arms,hw.quiet_mutations,hw.latched_events);
    CHECK(!rc);CHECK(frame.status.image_sequence==sequence&&frame.length==5120&&frame.stride==64);
    CHECK(frame.status.baseline_crc==fw_status(r).baseline_crc);
    fte3600_linux_runtime_frame_clear(&frame);
}
static void continuous(const struct fixture *f)
{
    static const uint16_t crcs[]={38016,22101,14908,63721,44983,28002,21024,37109};
    struct fte3600_linux_runtime *r=fw_open(f);
    struct fte3600_linux_runtime_diagnostic d;
    test_name="FW9369 queued UP/DOWN, eight captures and retained maintenance";
    for (unsigned i=0;i<8;++i) {
        fw_bus->pattern=i;capture_ok(r,i+1);
        CHECK(hw.user_frames==i+1&&hw.warm_frames==i&&hw.up_arms==i+1);
        CHECK(hw.status&4); /* UP can arrive before the caller asks for release. */
        hw.warm_value=(uint16_t)(2100+2*i);
        CHECK(!fte3600_linux_runtime_wait_release(r,NULL,10000,&d));
        CHECK(hw.warm_frames==i+1&&hw.adc_reads==(i+1)*5&&hw.down_arms==i+1);
        CHECK(fw_status(r).baseline_crc==crcs[i]&&fw_status(r).image_sequence==i+1);
        CHECK(hw.status&2); /* Touch queued by rearm must survive the next call. */
        uint64_t calls=fw_bus->calls;
        CHECK(!fte3600_linux_runtime_wait_release(r,NULL,10000,&d));CHECK(fw_bus->calls==calls);
    }
    CHECK(!hw.resets&&hw.user_frames==8&&hw.warm_frames==8);fw_close(r);
    /* Public clients need not remember a separate wait_release call. */
    r=fw_open(f);capture_ok(r,1);capture_ok(r,2);
    CHECK(hw.warm_frames==1&&hw.adc_reads==5&&hw.user_frames==2);fw_close(r);
}
static void release_failures(const struct fixture *f)
{
    for (unsigned fault=0;fault<9;++fault) {
        struct fte3600_linux_runtime *r=fw_open(f);
        struct fte3600_linux_runtime_diagnostic d;struct fte3600_linux_runtime_frame frame={0};
        GCancellable *cancel=g_cancellable_new();capture_ok(r,1);
        test_name="FW9369 release failures never publish baseline or permit reuse";
        uint64_t calls=fw_bus->calls;
        if (fault==0) hw.status=hw.up_status=0;
        if (fault==1) hw.fail_adc=1;
        if (fault==2) hw.fail_word=1;
        if (fault==3) hw.fail_warm=1;
        if (fault==4) hw.spike=1;
        if (fault==5) hw.not_ready=1;
        if (fault==6) g_cancellable_cancel(cancel);
        if (fault==7) { hw.cancel=cancel;hw.cancel_at=3; }
        if (fault==8) hw.deadline_at=3;
        enum fte3600_linux_runtime_error rc=fte3600_linux_runtime_wait_release(r,cancel,1000,&d);
        CHECK(rc!=FTE3600_RUNTIME_OK&&rc!=FTE3600_RUNTIME_RELEASE_UNSUPPORTED);
        if (fault==0 || fault==8) CHECK(rc==FTE3600_RUNTIME_TIMEOUT);
        if (fault==6 || fault==7) CHECK(rc==FTE3600_RUNTIME_CANCELLED);
        if (fault==6) CHECK(fw_bus->calls==calls);
        CHECK(fw_status(r).baseline_crc==3634&&fw_status(r).image_sequence==1);
        CHECK(fte3600_linux_runtime_capture(r,NULL,1000,&frame,&d)==FTE3600_RUNTIME_NOT_READY&&!frame.pixels);
        hw.cancel=NULL;g_object_unref(cancel);fw_close(r);
    }
    /* Cancellation in confirmation and warm FIFO is also a failed action. */
    for (unsigned phase=1;phase<=2;++phase) {
        struct fte3600_linux_runtime *r=fw_open(f);capture_ok(r,1);
        struct fte3600_linux_runtime_diagnostic d;GCancellable *cancel=g_cancellable_new();
        hw.cancel=cancel;hw.cancel_at=phase;
        CHECK(fte3600_linux_runtime_wait_release(r,cancel,1000,&d)==FTE3600_RUNTIME_CANCELLED);
        CHECK(fw_status(r).baseline_crc==3634&&fw_status(r).image_sequence==1);
        hw.cancel=NULL;g_object_unref(cancel);fw_close(r);
    }
}
static void recovery(const struct fixture *f)
{
    const uint16_t flags[]={0x10,0x200,0x400,0x12,0x14,0x11};
    for (size_t n=0;n<G_N_ELEMENTS(flags);++n) {
        test_name="FW9369 actual reset pulse/startup recovery preserves pending capture";
        struct fte3600_linux_runtime *r=fw_open(f);hw.status=flags[n];
        capture_ok(r,1);
        CHECK(fw_bus->linux_policy_reads==1 && fw_bus->linux_policy_writes==1);
        CHECK(hw.resets==3&&hw.reset_levels[0]==1&&hw.reset_levels[1]==0&&hw.reset_levels[2]==1);
        CHECK(hw.reset_times[1]-hw.reset_times[0]==10&&hw.reset_times[2]-hw.reset_times[1]==20);
        CHECK(hw.reset_spi_times[0]-hw.reset_times[2]==5);
        CHECK(hw.recovery_adc>=5&&hw.recovery_frames>=3&&hw.user_frames==1&&!hw.warm_frames);
        fw_close(r);
    }
    struct fte3600_linux_runtime_diagnostic d;
    struct fte3600_linux_runtime *r=fw_open(f);capture_ok(r,1);hw.status=0x10;
    CHECK(!fte3600_linux_runtime_wait_release(r,NULL,10000,&d));
    CHECK(hw.resets==3&&hw.user_frames==1&&hw.warm_frames==1&&hw.adc_reads==5);
    CHECK(fw_status(r).image_sequence==1);fw_close(r);
    r=fw_open(f);capture_ok(r,1);hw.status=0x10;hw.up_status=0;
    CHECK(fte3600_linux_runtime_wait_release(r,NULL,1000,&d)==FTE3600_RUNTIME_TIMEOUT);
    CHECK(hw.resets==3&&!hw.warm_frames&&fw_status(r).image_sequence==1);fw_close(r);
    r=fw_open(f);hw.status=0x10;hw.down_status=0x10;
    struct fte3600_linux_runtime_frame frame={0};
    CHECK(fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d)==FTE3600_RUNTIME_SESSION);
    CHECK(hw.resets==9&&!hw.user_frames&&!frame.pixels);fw_close(r);
    r=fw_open(f);hw.status=0x10;hw.fail_reset=1;
    CHECK(fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d)==FTE3600_RUNTIME_IO);
    CHECK(!hw.resets&&!hw.recovery_frames&&!frame.pixels);fw_close(r);
    r=fw_open(f);capture_ok(r,1);hw.adc_value=400;
    CHECK(fte3600_linux_runtime_wait_release(r,NULL,10000,&d)==FTE3600_RUNTIME_SESSION);
    CHECK(hw.resets==9&&!hw.warm_frames&&fw_status(r).image_sequence==1);fw_close(r);
}
static void contact_and_close(const struct fixture *f)
{
    test_name="FW9369 qualification, cancelled actions and close while held";
    struct fte3600_linux_runtime *r=fw_open(f);hw.status=hw.down_status=0;hw.no_irq=1;
    struct fte3600_linux_runtime_frame frame={0};struct fte3600_linux_runtime_diagnostic d;
    CHECK(fte3600_linux_runtime_capture(r,NULL,100,&frame,&d)==FTE3600_RUNTIME_TIMEOUT);
    CHECK(!frame.pixels&&!hw.user_frames);
    CHECK(fte3600_linux_runtime_capture(r,NULL,100,&frame,&d)==FTE3600_RUNTIME_NOT_READY);fw_close(r);
    r=fw_open(f);hw.status=6;capture_ok(r,1);CHECK(!hw.adc_reads);fw_close(r);
    r=fw_open(f);hw.late_at=3;hw.late_irq=4;capture_ok(r,1);
    CHECK(hw.warm_frames==1&&hw.user_frames==1&&hw.adc_reads==5&&fw_status(r).baseline_crc==38016);
    fw_close(r);
    r=fw_open(f);hw.up_status=0;capture_ok(r,1);CHECK(!hw.warm_frames);fw_close(r);
    r=fw_open(f);CHECK(fw_status(r).baseline_crc==3634&&fw_status(r).image_sequence==0);
    capture_ok(r,1);fw_close(r);
}
static void quiet_detection(const struct fixture *f)
{
    for (unsigned lost_irq=0;lost_irq<2;++lost_irq) {
        test_name="FW9369 causal quiet Down/Up and queued touch";
        struct fte3600_linux_runtime *r=fw_open_mode(f,180);
        struct fte3600_linux_runtime_diagnostic d;
        hw.no_irq=lost_irq;
        CHECK(!hw.status && hw.quiet_waiting && hw.auto_arms==1);
        uint64_t began=fake.clock_ms;
        capture_ok(r,1);
        CHECK(fw_bus->linux_policy_reads==1 && fw_bus->linux_policy_writes==1);
        CHECK(fake.clock_ms-began>=180 && hw.quiet_reads>=8);
        CHECK(hw.latched_events==1 && hw.auto_arms==2 && hw.user_frames==1);
        CHECK(!hw.status && !hw.quiet_mutations && !hw.bad_policy);
        CHECK(lost_irq?!hw.irq_delivered:hw.irq_delivered==1);
        /* Preserving upper bits matters when image/manual modes have ORed
         * their own flags into the retained event word. */
        fw_bus->words[0x1a83]|=0xa580;
        began=fake.clock_ms;unsigned reads=hw.quiet_reads;
        CHECK(!fte3600_linux_runtime_wait_release(r,NULL,10000,&d));
        CHECK(fake.clock_ms-began>=180 && hw.quiet_reads-reads>=8);
        CHECK(hw.latched_events==2 && hw.auto_arms==3 && hw.warm_frames==1);
        CHECK(hw.adc_reads==5 && !hw.status && hw.quiet_waiting && !hw.quiet_mutations);
        CHECK(fw_bus->words[0x1a83]==0xa596 && !hw.bad_policy);
        /* Time passes outside the API. Status is latched independently of
         * UIO delivery; the next capture must not clear/rearm it away. */
        fake.clock_ms+=180;fw_advance();
        CHECK(hw.status==2 && hw.irq_pending && hw.latched_events==3);
        unsigned arms=hw.down_arms;
        capture_ok(r,2);
        CHECK(hw.down_arms==arms && hw.auto_arms==4 && hw.up_arms==2);
        CHECK(!hw.status && !hw.bad_policy && !hw.quiet_mutations);
        fw_close(r);
    }
    for (unsigned up=0;up<2;++up) for (unsigned cancel=0;cancel<2;++cancel) {
        test_name="FW9369 empty deadline/cancel leaves automatic detector intact";
        struct fte3600_linux_runtime *r=fw_open_mode(f,180);
        if (up) capture_ok(r,1);
        struct fte3600_linux_runtime_diagnostic d;
        struct fte3600_linux_runtime_frame frame={0};
        GCancellable *token=g_cancellable_new();hw.cancel=token;
        uint64_t began=fake.clock_ms;unsigned arms=hw.auto_arms,reads=hw.quiet_reads;
        if (cancel) hw.cancel_time=began+40;
        enum fte3600_linux_runtime_error rc=up?
            fte3600_linux_runtime_wait_release(r,token,120,&d):
            fte3600_linux_runtime_capture(r,token,120,&frame,&d);
        CHECK(rc==(cancel?FTE3600_RUNTIME_CANCELLED:FTE3600_RUNTIME_TIMEOUT));
        CHECK(fake.clock_ms-began<=(cancel?60u:120u));
        CHECK(hw.auto_arms==arms && hw.quiet_reads>reads && !hw.quiet_mutations);
        CHECK(hw.quiet_waiting && !hw.status && !hw.bad_policy && !frame.pixels);
        CHECK(fw_status(r).image_sequence==up);
        hw.cancel=NULL;g_object_unref(token);fw_close(r);
    }
    for (unsigned up=0;up<2;++up) {
        test_name="FW9369 reset recovery restores Linux policy and quiet detection";
        struct fte3600_linux_runtime *r=fw_open_mode(f,180);
        struct fte3600_linux_runtime_diagnostic d;
        if (up) capture_ok(r,1);
        hw.status=0x10;hw.quiet_waiting=0;
        if (up) CHECK(!fte3600_linux_runtime_wait_release(r,NULL,10000,&d));
        else capture_ok(r,1);
        CHECK(hw.resets==3 && !hw.bad_policy && hw.quiet_waiting);
        CHECK(hw.latched_events==(up?2u:1u) && fw_status(r).image_sequence==1);
        CHECK(up?hw.warm_frames==1:hw.user_frames==1);
        fw_close(r);
    }
}
static void policy_failures(const struct fixture *f)
{
    for (unsigned write=0;write<2;++write) {
        test_name="FW9369 failed initial Linux policy never starts automatic detection";
        fw_setup(f);hw.policy_read_failure=!write;hw.policy_write_failure=write;
        char device[PATH_MAX];path(device,devroot,"spidev0.0");
        const struct fte3600_linux_runtime_config c={device,sysroot,devroot,NULL};
        struct fte3600_linux_runtime *r=NULL;struct fte3600_linux_runtime_diagnostic d;
        CHECK(fte3600_linux_runtime_open(&c,NULL,&r,&d)!=FTE3600_RUNTIME_OK);
        CHECK(!r && !hw.auto_arms);fw_teardown();
        for (unsigned release=0;release<2;++release) {
            test_name="FW9369 failed Up/Down policy never publishes completed action";
            r=fw_open(f);
            struct fte3600_linux_runtime_frame frame={0};
            if (release) capture_ok(r,1);
            unsigned arms=hw.auto_arms;
            hw.policy_read_failure=!write;hw.policy_write_failure=write;
            enum fte3600_linux_runtime_error rc=release?
                fte3600_linux_runtime_wait_release(r,NULL,10000,&d):
                fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d);
            CHECK(rc!=FTE3600_RUNTIME_OK && hw.auto_arms==arms && !frame.pixels);
            CHECK(fw_status(r).image_sequence==release);
            fw_close(r);
        }
    }
}
#ifndef FTE3600_FW_RUNTIME_MAIN
#define FTE3600_FW_RUNTIME_MAIN main
#endif
int FTE3600_FW_RUNTIME_MAIN(void)
{
    const struct fixture *f=NULL;
    for (size_t i=0;i<G_N_ELEMENTS(fixtures);++i)
        if (fixtures[i].model==FTE3600_MODEL_FW9369) { f=&fixtures[i];break; }
    CHECK(f);test_name="public FW9369 runtime";fixture();
    unsigned before=fd_count();
    quiet_detection(f);policy_failures(f);
    continuous(f);release_failures(f);recovery(f);contact_and_close(f);
    CHECK(fd_count()==before);
    puts("public FW9369 runtime: causal quiet Down/Up, Linux event policy, queued/lost IRQ, recovery, cancellation/deadlines, policy failures and eight captures passed");
    return 0;
}
