/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_device_session.h"
#include "fte3600_fw9369_grayscale.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *test_name;
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"%s:%d %s: %s\n",__FILE__,__LINE__,test_name,#x);abort();} } while(0)
struct event { int kind;uint32_t status,value;size_t length;const uint8_t *tx,*response;size_t size,offset; };
struct fixture { const char *name;const struct event *events;size_t count;enum fte3600_factory_model model;uint16_t chip,raw,width,height,extra;
    const uint8_t *pram;size_t pram_length;const uint8_t *app;size_t app_length;const uint8_t *firmware;size_t firmware_length;const struct event *negative;size_t negative_count,unconfirmed_stop; };
#include "device_session_fixtures.h"
struct bus {
    const struct fixture *fixture;size_t next;
    uint16_t words[32768];uint8_t bytes[256],mode;
    uint64_t clock,calls;unsigned frames,measurements,version_reads,uploads;
    size_t frame_position;unsigned pattern;
    int fail_fifo,wait_failure,latched,frame_absent,unhealthy;
    uint32_t recovery_failure;
    unsigned recovery_gpio_calls,recovery_sync_calls;
    int fail_recovery_gpio;
};
static const struct event *event(struct bus *b,int kind)
{
    if (b->next>=b->fixture->count) return NULL;
    const struct event *e=&b->fixture->events[b->next++];CHECK(e->kind==kind);return e;
}
static int is_legacy(const struct fixture *f) { return f->model==1 || f->model==2 || f->model==3 || f->model==5; }
static void record_write(struct bus *b,const uint8_t *tx,size_t n)
{
    if (n>=8 && tx[0]==5 && tx[1]==0xfa) {
        size_t at=((size_t)(tx[2]&127)<<8)|tx[3];
        for (size_t i=6;i+1<n && at<32768;i+=2,++at) b->words[at]=(uint16_t)((tx[i]<<8)|tx[i+1]);
    }
    if (n==5 && tx[0]==0x11 && tx[1]==0xee) b->bytes[tx[2]]=tx[3];
    if (n==3) {
        if (tx[0]==0x5a || tx[0]==0xc0) b->mode=0x50;
        if (tx[0]==0xc4 || tx[0]==0xc8) { b->mode=0x54;b->frame_position=0; }
    }
}
static uint16_t sample(struct bus *b,size_t i)
{
    if (b->fixture->model==6) return (uint16_t)(1850+(i*17+b->pattern*41)%170);
    return (uint16_t)(1200+(i*53+b->pattern*89)%2400);
}
static uint32_t spi(void *context,uint8_t *tx,size_t n)
{
    struct bus *b=context;++b->calls;
    const struct event *e=event(b,0);
    if (e) {
        if (n!=e->length || memcmp(tx,e->tx,n)) {
            fprintf(stderr,"%s event %zu n=%zu expected=%zu first=%02x/%02x\n",test_name,b->next-1,n,e->length,tx[0],e->tx[0]);
            for(size_t j=0;j<n && j<16;++j) {fprintf(stderr,"%02x/%02x ",tx[j],j<e->length?e->tx[j]:0);}
            fputc(10,stderr);abort();
        }
        record_write(b,tx,n);
        if (e->size) memcpy(tx+e->offset,e->response,e->size);
        return e->status;
    }
    if ((n>1000 && (tx[0]==6 || tx[0]==0x90 || (tx[0]==4 && tx[2]==0x34))) && b->fail_fifo) {
        b->latched=5;return UINT32_C(0xc0000185);
    }
    if (b->recovery_failure && n==2 && tx[0]==0x55 && tx[1]==0xaa) {
        ++b->recovery_sync_calls;return b->recovery_failure;
    }
    if (n==10246 && tx[0]==6) {
        for (size_t i=0;i<5120;++i) {uint16_t v=sample(b,i);tx[6+2*i]=(uint8_t)(v>>8);tx[7+2*i]=(uint8_t)v;}
        ++b->frames;return 0;
    }
    if (tx[0]==6 && n>=8 && tx[1]==0xf9) {
        size_t count=(n-8)/2;
        for (size_t i=0;i<count;++i) {uint16_t v=sample(b,b->frame_position+i);tx[6+2*i]=(uint8_t)(v>>8);tx[7+2*i]=(uint8_t)v;}
        tx[n-2]=tx[n-1]=0;b->frame_position+=count;
        size_t samples=(size_t)b->fixture->width*(b->fixture->height+b->fixture->extra);
        if (b->frame_position==samples) ++b->frames;
        return 0;
    }
    if (n>=4 && (tx[0]==0xff || tx[0]==0x91 || tx[0]==0x90)) {
        if (tx[0]==0xff && n>=9) {tx[7]=0x55;tx[8]=0xaa;}
        else if (tx[0]==0x91 && n==39) {memset(tx+7,0,32);tx[26]=0x93;tx[27]=0x68;tx[28]=0x13;}
        else if (tx[0]==0x90 && n==5127) {for (size_t i=0;i<5120;++i) tx[7+i]=(uint8_t)(i+b->pattern);++b->frames;}
        return 0;
    }
    if (n>1000 && tx[0]==4 && tx[2]==0x34) {
        for (size_t i=8;i<n;++i) tx[i]=(uint8_t)(i-8+b->pattern);
        ++b->frames;return 0;
    }
    if (n>=5 && tx[0]==0x10 && tx[1]==0xef) {
        uint8_t reg=tx[2];
        if (reg==0x20 && n==6) {tx[4]=b->unhealthy?0:0xa5;tx[5]=b->unhealthy?0:0x5a;}
        else if (reg==0x1d) tx[4]=b->frame_absent?0:1;
        else if (reg==0x1a || reg==0x3c) {
            static const uint8_t firmware[]={0,0x40,0x30,0x30,0,0x23};
            static const uint8_t agc[]={0,0x10,0x31,0x31,0,0x13};
            tx[4]=reg==0x1a?firmware[b->fixture->model]:agc[b->fixture->model];++b->version_reads;
            if (b->recovery_failure) tx[4]^=1; /* Force the reviewed LOAD path. */
            b->unhealthy=0;
        } else tx[4]=b->bytes[reg];
        return 0;
    }
    if (n==5 && tx[0]==8 && tx[1]==0xf7) {tx[4]=tx[2]==0x80?b->mode:b->bytes[tx[2]];return 0;}
    if (n>=10 && tx[0]==4 && tx[1]==0xfb) {
        size_t at=((size_t)(tx[2]&127)<<8)|tx[3];
        uint16_t v=at==0x1a82?0x28:b->words[at];
        memset(tx+6,0,n-6);tx[6]=(uint8_t)(v>>8);tx[7]=(uint8_t)v;
        if (n==14 || n==16) ++b->measurements;
        return 0;
    }
    if (n>22 && tx[0]==5) ++b->uploads;
    record_write(b,tx,n);return 0;
}
static void ms(void *context,uint32_t n)
{struct bus *b=context;const struct event *e=event(b,2);if(e) CHECK(e->value==n);b->clock+=(uint64_t)n*1000;if(b->wait_failure)b->latched=4;}
static void us(void *context,uint32_t n)
{struct bus *b=context;const struct event *e=event(b,3);if(e) CHECK(e->value==n);b->clock+=n;}
static uint32_t gpio(void *context,int level)
{
    struct bus *b=context;const struct event *e=event(b,1);
    if(e){CHECK((int)e->value==level);return e->status;}
    if(b->recovery_failure) {
        ++b->recovery_gpio_calls;
        if(b->fail_recovery_gpio) return b->recovery_failure;
    }
    return 0;
}
static uint32_t boundary(void *context,enum fte3600_factory_boundary value)
{(void)context;CHECK(value>=0 && value<=FTE3600_BOUNDARY_FRAMEWORK_FINISH);return 0;}
static uint32_t now(void *context) {return (uint32_t)(((struct bus *)context)->clock/1000);}
static int error(void *context) {return ((struct bus *)context)->latched;}
static void clear(void *context) {((struct bus *)context)->latched=0;}
static struct fte3600_device_config config(struct bus *b,int firmware)
{
    struct fte3600_device_config c={.io={b,spi,ms,us,gpio,boundary,now,error,clear}};
    if (firmware && b->fixture->model==4) {
        c.startup.pram=(struct fte3600_ft9368_boot_image){b->fixture->pram,b->fixture->pram_length,(b->fixture->pram_length+127)/128*128};
        c.startup.application=(struct fte3600_ft9368_boot_image){b->fixture->app,b->fixture->app_length,b->fixture->app_length};
    } else if (firmware && is_legacy(b->fixture)) {
        c.startup.firmware=b->fixture->firmware;c.startup.firmware_length=b->fixture->firmware_length;
        c.legacy_firmware_model=(enum fte3600_factory_model)b->fixture->model;
    }
    return c;
}
static struct fte3600_device_status status(struct fte3600_device_session *s)
{struct fte3600_device_status r;CHECK(fte3600_device_get_status(s,&r)==0);return r;}
static void success(enum fte3600_device_error e,struct fte3600_device_session *s)
{if(e){struct fte3600_device_status r=status(s);fprintf(stderr,"error %d component %u compat %08x\n",e,r.component_error,r.compatibility_status);}CHECK(e==0);}
static void fw_baseline_expected(struct bus *source,const uint8_t *actual)
{
    /* An independent expected state is initialized from the pinned startup
     * fixture's measured 2052 baseline. No session state is injected/read. */
    struct bus b=*source;b.next=b.fixture->count;b.frames=0;
    struct fte3600_fw9369_startup_state *s=calloc(1,sizeof(*s));CHECK(s);
    *s=(struct fte3600_fw9369_startup_state)FTE3600_FW9369_STARTUP_STATE_INITIALIZER;
    for(size_t i=0;i<5120;++i)s->baseline[i]=2052;
    s->image_dac=55;
    struct fte3600_fw9369_grayscale_workspace *w=calloc(1,sizeof(*w));CHECK(w);
    uint16_t processed[5120];uint8_t expected[5120];
    const struct fte3600_fw9369_grayscale_buffers buffers={NULL,0,processed,5120,expected,5120};
    const struct fte3600_capture_io io={&b,spi,ms};
    CHECK(fte3600_fw9369_grayscale_run(FTE3600_FW9369_GRAYSCALE_CAPTURE,64,80,s,&buffers,w,sizeof(*w),&io,NULL,NULL)==0);
    CHECK(!memcmp(actual,expected,5120));
    memset(s->baseline,0,sizeof(s->baseline));
    CHECK(fte3600_fw9369_grayscale_run(FTE3600_FW9369_GRAYSCALE_CAPTURE,64,80,s,&buffers,w,sizeof(*w),&io,NULL,NULL)==0);
    CHECK(memcmp(actual,expected,5120));free(w);free(s);
}
static void scenario(const struct fixture *f)
{
    test_name=f->name;struct bus *b=calloc(1,sizeof(*b));CHECK(b);b->fixture=f;b->mode=0x50;
    struct fte3600_device_config c=config(b,1);struct fte3600_device_session *s=NULL;
    success(fte3600_device_create(&c,&s),s);CHECK(b->calls==0);
    CHECK(fte3600_device_capture(s)==FTE3600_DEVICE_NOT_READY);
    CHECK(fte3600_device_cancel(s)==0);CHECK(b->calls==0);
    success(fte3600_device_start(s),s);CHECK(b->next==f->count);
    struct fte3600_device_status r=status(s);
    CHECK(r.model==f->model && r.chip_id==f->chip && r.raw_id==f->raw);
    CHECK(r.width==f->width && r.height==f->height && r.extra_rows==f->extra);
    CHECK(r.startup_complete && r.calibration_complete && !r.image_available);
    if(is_legacy(f))CHECK(b->version_reads>=2);
    if(f->model==6)CHECK(r.baseline_crc==3634 && r.calibrated_image_dac==55);
    unsigned measurements=b->measurements;
    uint8_t first[10240],second[10240],untouched[10240];memset(untouched,0xaa,sizeof(untouched));
    success(fte3600_device_arm(s),s);CHECK(fte3600_device_arm(s)==FTE3600_DEVICE_BUSY);
    CHECK(fte3600_device_start(s)==FTE3600_DEVICE_BUSY);
    CHECK(fte3600_device_cancel(s)==FTE3600_DEVICE_CANCELLED);
    uint64_t calls=b->calls;CHECK(fte3600_device_irq(s)==FTE3600_DEVICE_NOT_READY);CHECK(b->calls==calls);
    CHECK(fte3600_device_cancel(s)==0);
    success(fte3600_device_capture(s),s);r=status(s);CHECK(r.image_sequence==1 && r.image_available);
    success(fte3600_device_copy_image(s,1,first,sizeof(first)),s);
    CHECK(fte3600_device_copy_image(s,2,untouched,sizeof(untouched))==FTE3600_DEVICE_NO_FRAME);CHECK(untouched[0]==0xaa);
    if(f->model==6)fw_baseline_expected(b,first);
    b->pattern=1;success(fte3600_device_arm(s),s);success(fte3600_device_irq(s),s);
    r=status(s);if(r.image_sequence!=2 || b->frames!=2 || b->measurements!=measurements)fprintf(stderr,"seq=%llu frames=%u measurements=%u/%u\n",(unsigned long long)r.image_sequence,b->frames,b->measurements,measurements);
    CHECK(r.image_sequence==2 && b->frames==2 && b->measurements==measurements);
    success(fte3600_device_copy_image(s,2,second,sizeof(second)),s);
    CHECK(memcmp(first,second,(size_t)f->width*f->height));
    CHECK(r.width==f->width && r.height==f->height && r.extra_rows==f->extra);
    if(f->model==6) {CHECK(r.baseline_crc==3634);fw_baseline_expected(b,second);}
    if(is_legacy(f)) {
        b->frame_absent=1;CHECK(fte3600_device_capture(s)==FTE3600_DEVICE_NO_FRAME);
        CHECK(status(s).state==FTE3600_DEVICE_ARMED && status(s).image_sequence==2);
        b->frame_absent=0;success(fte3600_device_irq(s),s);CHECK(status(s).image_sequence==3);
        success(fte3600_device_arm(s),s);unsigned versions=b->version_reads;b->unhealthy=1;
        CHECK(fte3600_device_irq(s)==FTE3600_DEVICE_NO_FRAME);
        CHECK(b->version_reads==versions+2 && status(s).image_sequence==3 && status(s).state==FTE3600_DEVICE_ARMED);
        success(fte3600_device_irq(s),s);CHECK(status(s).image_sequence==4);
    }
    r=status(s);uint64_t old=r.image_sequence;
    success(fte3600_device_arm(s),s);b->fail_fifo=1;
    CHECK(fte3600_device_irq(s)==FTE3600_DEVICE_PLATFORM_ERROR);
    CHECK(status(s).image_sequence==old && status(s).state==FTE3600_DEVICE_FAULT);
    success(fte3600_device_copy_image(s,old,untouched,sizeof(untouched)),s);
    CHECK(!memcmp(untouched,second,(size_t)f->width*f->height));
    calls=b->calls;CHECK(fte3600_device_capture(s)==FTE3600_DEVICE_NOT_READY);CHECK(b->calls==calls);
    /* Recovery reruns the actual frozen startup on this same bus/session. */
    b->fail_fifo=0;b->next=0;b->mode=0x50;b->pattern=2;
    success(fte3600_device_start(s),s);CHECK(!status(s).image_available && status(s).image_sequence==old);
    success(fte3600_device_capture(s),s);CHECK(status(s).image_sequence==old+1);
    b->wait_failure=1;CHECK(fte3600_device_capture(s)==FTE3600_DEVICE_PLATFORM_ERROR);
    CHECK(status(s).image_sequence==old+1);
    fte3600_device_close(s);free(b);printf("PASS %s\n",f->name);
}
static void missing_firmware(void)
{
    test_name="explicit missing and mismatched firmware";
    for(size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {
        const struct fixture *f=&fixtures[i];if(f->unconfirmed_stop || (!is_legacy(f) && f->model!=4))continue;
        struct bus *b=calloc(1,sizeof(*b));CHECK(b);b->fixture=f;b->mode=0x50;
        struct fte3600_device_config c=config(b,0);struct fte3600_device_session *s=NULL;
        success(fte3600_device_create(&c,&s),s);
        CHECK(fte3600_device_start(s)==FTE3600_DEVICE_MISSING_FIRMWARE);
        CHECK(status(s).model==f->model && !status(s).startup_complete && !status(s).image_available && !b->uploads);
        fte3600_device_close(s);
        if(is_legacy(f)) {
            b->next=0;c=config(b,1);c.legacy_firmware_model=f->model==1?FTE3600_MODEL_FT9348:FTE3600_MODEL_FT9338;
            success(fte3600_device_create(&c,&s),s);CHECK(fte3600_device_start(s)==FTE3600_DEVICE_FIRMWARE_MISMATCH);
            CHECK(!b->version_reads && !b->uploads);fte3600_device_close(s);
        }
        free(b);
    }
}
static void incomplete_calibration(void)
{
    test_name="compatibility success is not acknowledged calibration";
    for(size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {
        if(!fixtures[i].negative)continue;
        struct fixture f=fixtures[i];f.events=f.negative;f.count=f.negative_count;
        struct bus *b=calloc(1,sizeof(*b));CHECK(b);b->fixture=&f;b->mode=0x50;
        struct fte3600_device_config c=config(b,0);struct fte3600_device_session *s=NULL;
        success(fte3600_device_create(&c,&s),s);
        CHECK(fte3600_device_start(s)==FTE3600_DEVICE_CALIBRATION_ERROR);
        struct fte3600_device_status r=status(s);
        CHECK(!r.compatibility_status && !r.startup_complete && !r.image_available);
        CHECK(r.chip_id==f.chip && r.raw_id==f.raw && r.width==f.width && r.height==f.height);
        uint64_t calls=b->calls;CHECK(fte3600_device_capture(s)==FTE3600_DEVICE_NOT_READY);CHECK(b->calls==calls);
        fte3600_device_close(s);free(b);
    }
}
static void refuse_guessed_firmware(void)
{
    test_name="no forced firmware into unidentified device";
    for(size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {
        const struct fixture *f=&fixtures[i];if(!f->unconfirmed_stop)continue;
        for(int supplied=0;supplied<=1;++supplied) {
            struct bus *b=calloc(1,sizeof(*b));CHECK(b);b->fixture=f;b->mode=0x50;
            struct fte3600_device_config c=config(b,supplied);struct fte3600_device_session *s=NULL;
            success(fte3600_device_create(&c,&s),s);
            CHECK(fte3600_device_start(s)==FTE3600_DEVICE_UNSUPPORTED);
            struct fte3600_device_status r=status(s);
            CHECK(r.component_error==FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY);
            CHECK(r.model==FTE3600_MODEL_UNKNOWN && !r.startup_complete && !r.image_available);
            CHECK(b->next==f->unconfirmed_stop && !b->uploads);
            fte3600_device_close(s);free(b);
        }
    }
}
static void legacy_terminal_recovery_errors(void)
{
    test_name="legacy recovery preserves terminal absence";
    for(size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {
        const struct fixture *f=&fixtures[i];if(f->unconfirmed_stop || !is_legacy(f))continue;
        for(int absent=0;absent<=1;++absent) for(int gpio_error=0;gpio_error<=1;++gpio_error) {
            struct bus *b=calloc(1,sizeof(*b));CHECK(b);b->fixture=f;b->mode=0x50;
            struct fte3600_device_config c=config(b,1);struct fte3600_device_session *s=NULL;
            success(fte3600_device_create(&c,&s),s);success(fte3600_device_start(s),s);
            success(fte3600_device_arm(s),s);
            b->unhealthy=1;b->recovery_failure=absent?UINT32_C(0xc000000e):UINT32_C(0xc0000185);
            b->fail_recovery_gpio=gpio_error;
            CHECK(fte3600_device_irq(s)==FTE3600_DEVICE_PLATFORM_ERROR);
            struct fte3600_device_status r=status(s);unsigned attempts=absent?1:5;
            CHECK(r.component_error==(unsigned)(absent?FTE3600_LEGACY_BOOT_DEVICE_ABSENT:FTE3600_LEGACY_BOOT_IO_ERROR));
            CHECK(b->recovery_gpio_calls==attempts*(gpio_error?1:3));
            CHECK(b->recovery_sync_calls==(gpio_error?0:attempts));
            CHECK((gpio_error?r.last_gpio_status:r.last_spi_status)==b->recovery_failure);
            CHECK(r.state==FTE3600_DEVICE_FAULT && !r.image_available && !r.image_sequence && !b->frames);
            uint64_t calls=b->calls;CHECK(fte3600_device_capture(s)==FTE3600_DEVICE_NOT_READY);CHECK(b->calls==calls);
            fte3600_device_close(s);free(b);
        }
    }
    puts("PASS terminal legacy SPI/GPIO absence stops recovery; ordinary errors retain five attempts");
}
int main(void)
{
    for(size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {if(!fixtures[i].unconfirmed_stop)scenario(&fixtures[i]);}
    missing_firmware();incomplete_calibration();refuse_guessed_firmware();legacy_terminal_recovery_errors();puts("PASS missing/mismatched firmware; real start→arm→capture, baseline reuse, geometry, cancel, failure and recovery");return 0;
}
