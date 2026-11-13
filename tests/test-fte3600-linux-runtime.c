/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-only composition: actual Linux board, runtime and device-session paths,
 * synthetic sysfs/cdev identities and frozen protocol startup transcripts. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
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
#include "../libfprint/drivers/fte3600/linux_runtime.c"
#include "../libfprint/drivers/fte3600/linux_firmware.c"

static struct bus *runtime_bus;
static uint64_t wait_remaining;
static int irq_enabled=1;
static int byte_contact=1,byte_unhealthy;
static GCancellable *cancel_in_poll;
static gint poll_waiting;
static uint32_t runtime_spi(uint8_t *data,size_t length)
{
    int post_start=runtime_bus->next==runtime_bus->fixture->count;
    uint8_t command=data[0];uint32_t rc=spi(runtime_bus,data,length);
    if(post_start&&command==0x91&&length==39&&runtime_bus->fixture->model==FTE3600_MODEL_FT9368) {
        data[8]=data[9]=byte_contact?0x11:0;
        if(byte_unhealthy) data[26]=0;
    }
    return rc;
}
static void runtime_gpio(int level) { CHECK(!gpio(runtime_bus,level)); }
static void runtime_sleep(uint64_t ns)
{
    if(runtime_bus->next<runtime_bus->fixture->count||wait_remaining) {
        if(!wait_remaining) {
            const struct event *e=&runtime_bus->fixture->events[runtime_bus->next++];
            CHECK(e->kind==2||e->kind==3);
            wait_remaining=(uint64_t)e->value*(e->kind==2?1000000:1000);
        }
        CHECK(ns<=wait_remaining);wait_remaining-=ns;
    }
    runtime_bus->clock+=ns/1000;
}
static int runtime_poll(int timeout)
{
    if(timeout>0&&cancel_in_poll) {
        g_atomic_int_set(&poll_waiting,1);
        while(!g_cancellable_is_cancelled(cancel_in_poll)) g_thread_yield();
        return 0;
    }
    if(timeout>0&&irq_enabled&&runtime_bus)runtime_bus->finger=1; /* the touch arrives with the interrupt */
    return timeout>0&&irq_enabled;
}
static int runtime_expect_release_sync(void)
{
    if(runtime_bus->next<runtime_bus->fixture->count) {
        const struct event *e=&runtime_bus->fixture->events[runtime_bus->next];
        return e->kind==0&&e->length==2&&e->tx[0]==0x55&&e->tx[1]==0xaa;
    }
    return runtime_bus->recovery_failure&&runtime_bus->recovery_gpio_calls==3;
}
static void setup_bus(const struct fixture *f)
{
    runtime_bus=calloc(1,sizeof(*runtime_bus));CHECK(runtime_bus);
    runtime_bus->fixture=f;runtime_bus->mode=0x50;wait_remaining=0;
    test_name=f->name;reset_fake(1);irq_enabled=1;runtime_bus->irq_finger=1;byte_contact=1;byte_unhealthy=0;
    bus_spi=runtime_spi;bus_gpio=runtime_gpio;bus_sleep=runtime_sleep;bus_poll=runtime_poll;
    expect_release_sync=runtime_expect_release_sync;
}
static void teardown_bus(void)
{ CHECK(!sync_gap);expect_release_sync=NULL;free(runtime_bus);runtime_bus=NULL;bus_spi=NULL;bus_gpio=NULL;bus_sleep=NULL;bus_poll=NULL; }
static struct fte3600_linux_runtime_config runtime_config(void)
{
    static char device[PATH_MAX];path(device,devroot,"spidev0.0");
    return (struct fte3600_linux_runtime_config){device,sysroot,devroot,"/nonexistent-fte3600-fixture-firmware"};
}
static void pairing(void)
{
    struct fte3600_linux_runtime_config c=runtime_config();
    struct fte3600_linux_runtime_pair pair={0};struct fte3600_linux_runtime_diagnostic d;
    unsigned before=fd_count();test_name="runtime sysfs discovery";
    CHECK(fte3600_linux_runtime_discover(&c,&pair,&d)==0);
    CHECK(!strcmp(pair.spi,"spidev0.0")&&!strcmp(pair.gpio,"gpiochip7")&&!strcmp(pair.uio,"uio3"));
    CHECK(fake.ioctls==0&&fd_count()==before);
    c.spidev_path="/dev/spidev999.999";CHECK(fte3600_linux_runtime_discover(&c,&pair,&d)==FTE3600_RUNTIME_PAIRING);
    c=runtime_config();write_value(glue,"fte3600_glue_abi","1\n");
    CHECK(fte3600_linux_runtime_discover(&c,&pair,&d)==FTE3600_RUNTIME_PAIRING);
    write_value(glue,"fte3600_glue_abi","2\n");
    char wrong[PATH_MAX];path(wrong,sysroot,"devices/LNXSYSTM/FTE3600:00");write_value(wrong,"hid","FTE3600evil\n");
    CHECK(fte3600_linux_runtime_discover(&c,&pair,&d)==FTE3600_RUNTIME_PAIRING);write_value(wrong,"hid","FTE3600\n");
    /* A second name resolving to a valid cdev must not choose arbitrarily. */
    char gpio_path[PATH_MAX],alias[PATH_MAX],original[PATH_MAX];
    path(gpio_path,glue,"gpiochip7");
    link_value(sysroot,"bus/gpio/devices/gpiochip8",gpio_path);
    path(original,devroot,"gpiochip7");path(alias,devroot,"gpiochip8");CHECK(!link(original,alias));
    CHECK(fte3600_linux_runtime_discover(&c,&pair,&d)==FTE3600_RUNTIME_PAIRING&&d.system_error==EEXIST);
    CHECK(!unlink(alias));path(alias,sysroot,"bus/gpio/devices/gpiochip8");CHECK(!unlink(alias));
    /* IRQ from a different physical child cannot complete the tuple. */
    char irq_link[PATH_MAX];path(irq_link,glue,"uio/uio3/device");CHECK(!unlink(irq_link));
    CHECK(!symlink(wrong,irq_link));
    CHECK(fte3600_linux_runtime_discover(&c,&pair,&d)==FTE3600_RUNTIME_PAIRING);
    CHECK(!unlink(irq_link));CHECK(!symlink(glue,irq_link));
    CHECK(fd_count()==before);
}
static void verified_firmware(void)
{
    char tmp[]="/tmp/fte3600-runtime-firmware-XXXXXX";struct firmware_identity id={FTE3600_MODEL_UNKNOWN,"small.bin",3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    struct fte3600_linux_runtime_diagnostic d;struct fte3600_device_config c={0};uint8_t *owned[2]={0};uint8_t *bytes;
    test_name="runtime verified firmware";CHECK(mkdtemp(tmp));write_value(tmp,"small.bin","abc");
    int dir=open(tmp,O_RDONLY|O_DIRECTORY);CHECK(dir>=0);
    bytes=load_image(dir,&id,3,&d);CHECK(bytes&&!memcmp(bytes,"abc",3));free(bytes);
    id.size=2;CHECK(!load_image(dir,&id,2,&d)&&errno==EINVAL);id.size=3;
    id.hash="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    CHECK(!load_image(dir,&id,3,&d)&&errno==EBADMSG);
    CHECK(!symlinkat("small.bin",dir,"link.bin"));id.name="link.bin";
    CHECK(!load_image(dir,&id,3,&d)&&errno==ELOOP);CHECK(!close(dir));
    CHECK(fte3600_linux_firmware_load(tmp,FTE3600_MODEL_FT9368,&c,owned,&d)==FTE3600_RUNTIME_FIRMWARE);
    CHECK(d.system_error==ENOENT&&!strcmp(d.firmware_file,"ft9368-app.bin")&&!owned[0]&&!owned[1]&&!c.startup.pram.data);
    write_value(tmp,"ft9361.bin","invalid");
    CHECK(fte3600_linux_firmware_load(tmp,FTE3600_MODEL_FT9361,&c,owned,&d)==FTE3600_RUNTIME_FIRMWARE);
    CHECK(d.system_error==EINVAL&&!strcmp(d.firmware_file,"ft9361.bin")&&c.legacy_firmware_model==FTE3600_MODEL_UNKNOWN);
}
static void runtime_cases(const struct fixture *f)
{
    struct fte3600_linux_runtime *r=NULL;struct fte3600_linux_runtime_config c=runtime_config();
    struct fte3600_linux_runtime_diagnostic d;struct fte3600_linux_runtime_frame frame={0};
    struct fte3600_device_status s;unsigned before=fd_count();setup_bus(f);
    enum fte3600_linux_runtime_error rc=fte3600_linux_runtime_open(&c,NULL,&r,&d);
    if(is_legacy(f)||f->model==FTE3600_MODEL_FT9368) {
        CHECK(rc==FTE3600_RUNTIME_FIRMWARE&&d.system_error==ENOENT&&r==NULL);
        CHECK(fake.mode_writes==2&&fake.written_modes[0]==0&&fake.written_modes[1]==0);
        CHECK(!runtime_bus->uploads&&fd_count()==before);teardown_bus();return;
    }
    if(rc) fprintf(stderr,"runtime open error %d errno %d device %d\n",rc,d.system_error,d.device_error);
    CHECK(rc==0&&r);CHECK(runtime_bus->next==f->count&&!wait_remaining);
    CHECK(fake.mode_writes==1&&fake.mode==0);
    CHECK(fte3600_linux_runtime_get_status(r,&s)==0&&s.chip_id==f->chip&&s.width==f->width&&s.height==f->height);
    for(unsigned i=1;i<=2;++i) {
        runtime_bus->pattern=i;
        CHECK(fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d)==0);
        CHECK(frame.status.image_sequence==i&&frame.length==(size_t)f->width*f->height&&frame.stride==f->width);
        CHECK(runtime_bus->frames==i*per_request(f));fte3600_linux_runtime_frame_clear(&frame);
    }
    /* The raw-array flow checks lift with one more scan against the arm reference. */
    CHECK(fte3600_linux_runtime_wait_release(r,NULL,raw_array(f)?1000:10,&d)==(raw_array(f)?FTE3600_RUNTIME_OK:FTE3600_RUNTIME_RELEASE_UNSUPPORTED));
    if(raw_array(f))CHECK(runtime_bus->frames==2*per_request(f)+1);
    irq_enabled=0;
    CHECK(fte3600_linux_runtime_capture(r,NULL,10,&frame,&d)==FTE3600_RUNTIME_TIMEOUT);
    CHECK(frame.pixels==NULL);
    CHECK(fte3600_linux_runtime_close(r,&d)==0&&fd_count()==before);teardown_bus();
    setup_bus(f);CHECK(fte3600_linux_runtime_open(&c,NULL,&r,&d)==0);
    runtime_bus->fail_fifo=1;
    CHECK(fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d)==FTE3600_RUNTIME_IO);
    CHECK(!frame.pixels);CHECK(fte3600_linux_runtime_close(r,&d)==0&&fd_count()==before);teardown_bus();
    /* A new real board+session owner is the recovery boundary. */
    setup_bus(f);CHECK(fte3600_linux_runtime_open(&c,NULL,&r,&d)==0);
    CHECK(fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d)==0);
    fte3600_linux_runtime_frame_clear(&frame);CHECK(!fte3600_linux_runtime_close(r,&d));teardown_bus();
    CHECK(fd_count()==before);printf("runtime PASS %s\n",f->name);
}
static gpointer glib_cancel_thread(gpointer cancel)
{
    while(!g_atomic_int_get(&poll_waiting)) g_thread_yield();
    g_cancellable_cancel(cancel);return NULL;
}
static void threaded_cancel(const struct fixture *f)
{
    struct fte3600_linux_runtime_config c=runtime_config();struct fte3600_linux_runtime *r=NULL;
    struct fte3600_linux_runtime_frame frame={0};struct fte3600_linux_runtime_diagnostic d;
    GCancellable *cancel=g_cancellable_new();setup_bus(f);
    CHECK(fte3600_linux_runtime_open(&c,cancel,&r,&d)==0);
    cancel_in_poll=cancel;g_atomic_int_set(&poll_waiting,0);
    GThread *thread=g_thread_new("runtime-cancel",glib_cancel_thread,cancel);
    CHECK(fte3600_linux_runtime_capture(r,cancel,10000,&frame,&d)==FTE3600_RUNTIME_CANCELLED);
    g_thread_join(thread);cancel_in_poll=NULL;
    CHECK(!frame.pixels&&d.system_error==ECANCELED);
    CHECK(!fte3600_linux_runtime_close(r,&d));g_object_unref(cancel);teardown_bus();
}
/* Only this test constructor supplies frozen synthetic payloads directly to
 * the reviewed session. Production open must and does reject these payloads. */
static void supplied_firmware_case(const struct fixture *f)
{
    struct fte3600_linux_runtime *r=calloc(1,sizeof(*r));CHECK(r);
    struct fte3600_linux_runtime_diagnostic d;struct fte3600_linux_runtime_frame frame={0};
    struct fte3600_linux_board_config board=board_test_config();unsigned before=fd_count();
    setup_bus(f);board.is_cancelled=cancelled;board.cancel_context=r;
    CHECK(!fte3600_linux_board_open(&board,&r->board));
    r->device=config(runtime_bus,1);r->device.io=*fte3600_linux_board_callbacks(r->board);
    r->firmware_dir=strdup("/test-only-no-production-bypass");CHECK(r->firmware_dir);
    CHECK(!fte3600_linux_runtime_start(r,NULL,&d));
    CHECK(runtime_bus->next==f->count);
    CHECK(!fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d));
    CHECK(frame.status.image_sequence==1&&frame.length==(size_t)f->width*f->height);
    fte3600_linux_runtime_frame_clear(&frame);
    if(f->model==FTE3600_MODEL_FT9368) {
        byte_contact=0;CHECK(!fte3600_linux_runtime_wait_release(r,NULL,1000,&d));
        byte_unhealthy=1;
        CHECK(fte3600_linux_runtime_wait_release(r,NULL,1000,&d)==FTE3600_RUNTIME_IO&&d.system_error==EPROTO);
        byte_unhealthy=0;byte_contact=1;
    }
    runtime_bus->pattern=5;CHECK(!fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d));
    CHECK(frame.status.image_sequence==2);fte3600_linux_runtime_frame_clear(&frame);
    if(is_legacy(f)&&f->model!=FTE3600_MODEL_FT9536) {
        /* Force the real lifecycle reinitialization/LOAD path after capture;
         * version mismatch reaches Boot A or A8 synchronization, then the bus
         * fails that sync. Sticky board errors prevent the remaining retries. */
        unsigned pairs=sync_pairs;
        runtime_bus->unhealthy=1;runtime_bus->recovery_failure=UINT32_C(0xc0000185);
        CHECK(fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d)==FTE3600_RUNTIME_IO);
        CHECK(d.system_error==EIO&&sync_pairs==pairs+1);
        CHECK(runtime_bus->recovery_gpio_calls==3&&runtime_bus->recovery_sync_calls==1);
        struct fte3600_device_status status;
        CHECK(!fte3600_linux_runtime_get_status(r,&status));
        CHECK(status.failed_spi_calls==1&&status.last_spi_status==UINT32_C(0x80000000));
    }
    CHECK(!fte3600_linux_runtime_close(r,&d)&&fd_count()==before);teardown_bus();
    printf("runtime supplied-fixture PASS %s\n",f->name);
}
static void rejected_startups(void)
{
    struct fte3600_linux_runtime_config c=runtime_config();
    struct fte3600_linux_runtime_diagnostic d;struct fte3600_linux_runtime *r=NULL;
    unsigned before=fd_count();
    for(size_t i=0;i<G_N_ELEMENTS(fixtures);++i) {
        if(fixtures[i].unconfirmed_stop) {
            setup_bus(&fixtures[i]);
            write_value(glue,"fte3600_cs_control","0\n");
            CHECK(fte3600_linux_runtime_open(&c,NULL,&r,&d)==FTE3600_RUNTIME_SESSION);
            CHECK(d.device_error==FTE3600_DEVICE_UNSUPPORTED&&!r&&!runtime_bus->uploads);
            CHECK(runtime_bus->next==fixtures[i].unconfirmed_stop&&fd_count()==before);
            CHECK(!fake.mode_writes);
            teardown_bus();
        } else if(fixtures[i].negative_count) {
            struct fixture f=fixtures[i];f.events=f.negative;f.count=f.negative_count;
            setup_bus(&f);
            CHECK(fte3600_linux_runtime_open(&c,NULL,&r,&d)==FTE3600_RUNTIME_SESSION);
            CHECK(d.device_error==FTE3600_DEVICE_CALIBRATION_ERROR&&!r&&fd_count()==before);
            CHECK(fake.mode_writes==2&&fake.written_modes[0]==0&&fake.written_modes[1]==0);
            teardown_bus();
        }
    }
}
/* Counterexample at the actual spidev seam: wrong polarity completes every
 * ioctl but yields zero bytes. Only the permitted alternate polarity exposes
 * the frozen FW9369 startup transcript; no model or identity is injected. */
static struct {
    uint32_t responsive_mode;
    unsigned zero_calls,live_calls;
    uint16_t partial_family;
    unsigned family_reads,sync_calls,unconfirmed_bulk,wake70,ping_reads,sync_step;
    int silent,fail_first,fail_trial,cancel_first,cancel_mode,cancel_trial;
    GCancellable *cancel;
} cs_test;
static int cs_responsive(void)
{ return !cs_test.silent&&fake.mode==cs_test.responsive_mode; }
static uint32_t cs_spi(uint8_t *data,size_t length)
{
    if(!cs_responsive()) {
        int family=length==8&&!memcmp(data,"\x04\xfb\x85\xc0",4);
        if(length==1&&data[0]==0x70) ++cs_test.wake70;
        if(length==6&&!memcmp(data,"\x10\xef\x20",3)) ++cs_test.ping_reads;
        if(family) {
            /* The first ROM family read must follow the complete six-attempt
             * legacy wake/ping prefix, even on an unresponsive CS polarity. */
            CHECK(cs_test.wake70>=12&&cs_test.ping_reads>=6);
            cs_test.sync_step=1;
        }
        if(length==2&&data[0]==0x55&&data[1]==0xaa) {
            CHECK(cs_test.sync_step==6);cs_test.sync_step=0;++cs_test.sync_calls;
        }
        if(length>128) ++cs_test.unconfirmed_bulk;
        ++cs_test.zero_calls;
        if(cs_test.cancel_first) g_cancellable_cancel(cs_test.cancel);
        if(cs_test.fail_first) return UINT32_C(0x80000000);
        memset(data,0,length);
        if(family) { data[6]=(uint8_t)(cs_test.partial_family>>8);data[7]=(uint8_t)cs_test.partial_family;++cs_test.family_reads; }
        return 0;
    }
    ++cs_test.live_calls;
    if(cs_test.cancel_trial) g_cancellable_cancel(cs_test.cancel);
    if(cs_test.fail_trial) return UINT32_C(0x80000000);
    return runtime_spi(data,length);
}
static void cs_gpio(int level)
{
    if(cs_responsive()) runtime_gpio(level);
    else if(cs_test.sync_step) {
        CHECK((cs_test.sync_step==1&&level==1)||(cs_test.sync_step==3&&level==0)||
            (cs_test.sync_step==5&&level==1));
        ++cs_test.sync_step;
    }
}
static void cs_sleep(uint64_t ns)
{
    if(cs_responsive()) runtime_sleep(ns);
    else if(cs_test.sync_step) {
        CHECK((cs_test.sync_step==2&&ns==UINT64_C(10000000))||
            (cs_test.sync_step==4&&ns==UINT64_C(20000000)));
        ++cs_test.sync_step;
    }
}
static int cs_expect_release_sync(void)
{ return cs_responsive()?runtime_expect_release_sync():cs_test.sync_step==6; }
static void cs_mode(void)
{ if(fake.mode_writes==2&&cs_test.cancel_mode) g_cancellable_cancel(cs_test.cancel); }
static void cs_setup(const struct fixture *f)
{
    setup_bus(f);memset(&cs_test,0,sizeof(cs_test));cs_test.responsive_mode=SPI_CS_HIGH;
    bus_spi=cs_spi;bus_gpio=cs_gpio;bus_sleep=cs_sleep;after_mode=cs_mode;
    expect_release_sync=cs_expect_release_sync;
}
static void cs_teardown(void)
{ CHECK(!cs_test.unconfirmed_bulk);after_mode=NULL;teardown_bus(); }
static void chip_select_negotiation(void)
{
    const struct fixture *f=NULL;
    for(size_t i=0;i<G_N_ELEMENTS(fixtures);++i)
        if(fixtures[i].model==FTE3600_MODEL_FW9369) { f=&fixtures[i];break; }
    CHECK(f);unsigned before=fd_count();
    struct fte3600_linux_runtime_config c=runtime_config();
    struct fte3600_linux_runtime_diagnostic d;struct fte3600_linux_runtime *r=NULL;
    test_name="CS polarity: zero identity then fresh FW9369 startup";
    for(unsigned high=0;high<2;++high) {
        struct fte3600_device_status s;cs_setup(f);
        /* Also exercise reversed direction while preserving CPOL/CPHA. */
        uint32_t original=high?SPI_CPOL|SPI_CPHA|SPI_CS_HIGH:0;
        write_value(glue,"fte3600_acpi_mode",high?"7\n":"0\n");
        cs_test.responsive_mode=original^SPI_CS_HIGH;
        CHECK(!fte3600_linux_runtime_open(&c,NULL,&r,&d)&&r);
        CHECK(!fte3600_linux_runtime_get_status(r,&s));
        CHECK(s.raw_id==0x9362&&s.chip_id==0x9369&&s.width==64&&s.height==80);
        CHECK(s.startup_complete&&s.calibration_complete&&s.failed_spi_calls==0);
        CHECK(cs_test.zero_calls>0&&s.spi_calls==cs_test.live_calls);
        CHECK(runtime_bus->next==f->count&&!wait_remaining&&fake.mode==cs_test.responsive_mode);
        CHECK(fake.mode_writes==2&&fake.written_modes[0]==original&&fake.written_modes[1]==cs_test.responsive_mode);
        if(!high) {
            GCancellable *cancel=g_cancellable_new();struct fte3600_linux_runtime_frame frame={0};
            g_cancellable_cancel(cancel);
            CHECK(fte3600_linux_runtime_capture(r,cancel,10000,&frame,&d)==FTE3600_RUNTIME_CANCELLED);
            CHECK(!frame.pixels&&fake.mode==cs_test.responsive_mode);g_object_unref(cancel);
        }
        CHECK(!fte3600_linux_runtime_close(r,&d)&&fake.mode==original&&fd_count()==before);
        CHECK(fake.mode_writes==3&&fake.bits==16&&fake.speed==1000000);
        cs_teardown();
    }
    write_value(glue,"fte3600_acpi_mode","0\n");
    for(unsigned fault=0;fault<12;++fault) {
        test_name="CS retry gates and unsuccessful-open restoration";cs_setup(f);
        GCancellable *cancel=g_cancellable_new();cs_test.cancel=cancel;
        if(fault==0) cs_test.silent=1;
        if(fault==1) { cs_test.silent=1;write_value(glue,"fte3600_cs_control","0\n"); }
        if(fault==2) cs_test.fail_first=1;
        if(fault==3) cs_test.fail_trial=1;
        if(fault==4) cs_test.cancel_first=1;
        if(fault==5) cs_test.cancel_mode=1;
        if(fault==6) cs_test.cancel_trial=1;
        if(fault==7) fake.fail_mode_write=2;
        if(fault==8) fake.ignore_mode_write=2;
        if(fault==9) { cs_test.silent=1;fake.fail_mode_write=3;fake.fail_mode_error=EBUSY; }
        if(fault>=10) { cs_test.silent=1;cs_test.partial_family=fault==10?0x1574:0x1534; }
        enum fte3600_linux_runtime_error rc=fte3600_linux_runtime_open(&c,cancel,&r,&d);
        CHECK(!r&&fd_count()==before&&fake.bits==16&&fake.speed==1000000);
        if(fault<=1||fault>=10) CHECK(rc==FTE3600_RUNTIME_SESSION&&d.device_error==FTE3600_DEVICE_UNSUPPORTED);
        else if(fault>=4&&fault<=6) CHECK(rc==FTE3600_RUNTIME_CANCELLED&&d.system_error==ECANCELED);
        else CHECK(rc==FTE3600_RUNTIME_IO);
        if(fault==9) CHECK(d.system_error==EBUSY&&fake.mode==SPI_CS_HIGH);
        else CHECK(fake.mode==(fault==1?3u:0u));
        int writes=fault==1?0:(fault==2||fault==4)?2:3;
        CHECK(fake.mode_writes==writes);
        if(writes==2) CHECK(fake.written_modes[1]==0&&!cs_test.live_calls);
        if(writes==3) CHECK(fake.written_modes[0]==0&&fake.written_modes[1]==SPI_CS_HIGH&&fake.written_modes[2]==0);
        CHECK(d.cs_control==(fault==1?0u:1u)&&d.acpi_mode==0);
        CHECK(d.cs_retry_attempted==(writes==3?1u:0u));
        if(fault<=1||fault>=10) {
            CHECK(d.has_session_status&&d.component_error==FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY);
            CHECK(!d.raw_id&&!d.chip_id);
            CHECK(d.last_configured_mode==(fault==1?3u:SPI_CS_HIGH));
        }
        if(fault==7||fault==8||fault==9)
            CHECK(d.has_session_status&&d.component_error==FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY);
        if(fault>=10) CHECK(cs_test.family_reads&&cs_test.sync_calls&&!runtime_bus->uploads&&!cs_test.live_calls);
        g_object_unref(cancel);cs_teardown();
    }
    /* A successful retry does not make a later failed close successful. */
    test_name="CS successful-open restore error";cs_setup(f);
    CHECK(!fte3600_linux_runtime_open(&c,NULL,&r,&d));fake.ignore_mode_write=3;
    CHECK(fte3600_linux_runtime_close(r,&d)==FTE3600_RUNTIME_IO&&d.system_error==EPROTO);
    CHECK(fd_count()==before);cs_teardown();
    puts("runtime CS: zero-response retry, FW9369 identity, reversed polarity, strict gates and restoration passed");
}
static void legacy_chip_select_negotiation(void)
{
    const struct fixture *f=NULL;
    for(size_t i=0;i<G_N_ELEMENTS(fixtures);++i)
        if(fixtures[i].model==FTE3600_MODEL_FT9338) { f=&fixtures[i];break; }
    CHECK(f);unsigned before=fd_count();
    struct fte3600_linux_runtime_config c=runtime_config();
    struct fte3600_linux_runtime_diagnostic d;struct fte3600_linux_runtime *r=NULL;
    test_name="FT9338 identified at alternate CS still requires verified firmware";cs_setup(f);cs_test.partial_family=0x1574;
    CHECK(fte3600_linux_runtime_open(&c,NULL,&r,&d)==FTE3600_RUNTIME_FIRMWARE);
    CHECK(!r&&d.system_error==ENOENT);
    CHECK(cs_test.zero_calls&&cs_test.live_calls&&!runtime_bus->uploads);
    CHECK(cs_test.family_reads&&cs_test.sync_calls);
    CHECK(fake.mode_writes==3&&fake.mode==0&&fd_count()==before);cs_teardown();

    /* The same private fixture supply as supplied_firmware_case: production
     * verification above stays intact. Full frozen reset/identity and
     * startup sequence must still match after retry; CS never selects model. */
    test_name="FT9338 alternate-CS cold startup and capture";cs_setup(f);cs_test.partial_family=0x1574;
    r=calloc(1,sizeof(*r));CHECK(r);
    struct fte3600_linux_board_config board=board_test_config();
    board.is_cancelled=cancelled;board.cancel_context=r;
    CHECK(!fte3600_linux_board_open(&board,&r->board));
    r->device=config(runtime_bus,1);r->device.io=*fte3600_linux_board_callbacks(r->board);
    r->firmware_dir=strdup("/test-only-no-production-bypass");CHECK(r->firmware_dir);
    CHECK(!fte3600_linux_runtime_start(r,NULL,&d));
    struct fte3600_device_status s;CHECK(!fte3600_linux_runtime_get_status(r,&s));
    CHECK(s.model==FTE3600_MODEL_FT9338&&s.chip_id==0x9338&&s.width==88&&s.height==88);
    CHECK(s.startup_complete&&s.calibration_complete&&runtime_bus->next==f->count);
    CHECK(cs_test.zero_calls&&s.spi_calls==cs_test.live_calls&&fake.mode==SPI_CS_HIGH);
    struct fte3600_linux_runtime_frame frame={0};
    CHECK(!fte3600_linux_runtime_capture(r,NULL,10000,&frame,&d));
    CHECK(frame.length==88*88&&frame.status.image_sequence==1);fte3600_linux_runtime_frame_clear(&frame);
    CHECK(!fte3600_linux_runtime_close(r,&d)&&fake.mode==0&&fd_count()==before);cs_teardown();
    puts("runtime CS: FT9338 checked family startup, firmware gate and capture passed");
}
int main(void)
{
    CHECK(board_suite_main()==0);reset_fake(1);pairing();verified_firmware();
    /* FW9369's reviewed event protocol has a separate production-linked suite. */
    for(size_t i=0;i<G_N_ELEMENTS(fixtures);++i)
        if(!fixtures[i].unconfirmed_stop&&fixtures[i].model!=FTE3600_MODEL_FW9369) runtime_cases(&fixtures[i]);
    for(size_t i=0;i<G_N_ELEMENTS(fixtures);++i)
        if(!fixtures[i].unconfirmed_stop&&(is_legacy(&fixtures[i])||fixtures[i].model==FTE3600_MODEL_FT9368))
            supplied_firmware_case(&fixtures[i]);
    for(size_t i=0;i<G_N_ELEMENTS(fixtures);++i) if(fixtures[i].model==FTE3600_MODEL_FT9365) { threaded_cancel(&fixtures[i]);break; }
    rejected_startups();chip_select_negotiation();legacy_chip_select_negotiation();
    CHECK(sync_pairs>0);
    printf("runtime: %u release/sync pairs passed syscall-order guard through full startup and lifecycle LOAD\n",sync_pairs);
    puts("runtime: strict pairing, verified firmware, actual startup/capture, timeout, fault/reopen and cross-thread cancellation passed");
    return 0;
}
