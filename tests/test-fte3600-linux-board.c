/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
/* Meson enables large-file APIs for both the production archive and this
 * translation unit. Keep the doubles on those same redirected ABI symbols. */
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
#define __real_stat __real_stat64
#define __wrap_stat __wrap_stat64
#define __real_fstat __real_fstat64
#define __wrap_fstat __wrap_fstat64
#define __real_lstat __real_lstat64
#define __wrap_lstat __wrap_lstat64
#define __real_open __real_open64
#define __wrap_open __wrap_open64
#endif
#include "fte3600_linux_board.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%s:%d: %s errno=%d\n",__FILE__,__LINE__,#c,errno); exit(1); } } while(0)
static char root[PATH_MAX],sysroot[PATH_MAX],devroot[PATH_MAX],glue[PATH_MAX];
/* Optional synthetic bus hooks used by the real-session runtime tests. */
static uint32_t (*bus_spi)(uint8_t *,size_t);
static void (*bus_gpio)(int);
static void (*bus_sleep)(uint64_t);
static int (*bus_poll)(int);
static void (*after_spi)(void);
static void (*after_mode)(void);
static int count_calls;
static struct {
    unsigned realpaths, stats, lstats, metadata_opens, metadata_reads, metadata_closes;
} calls;
static struct {
    int reset_fd,uid,mode_writes,spi_calls,gpio_calls,ioctls,fail_ioctl;
    int fail_mode_write,fail_mode_error,ignore_mode_write;
    uint32_t written_modes[8];
    int short_spi,stale_spi,stale_request,stale_gpio,clock_error,sleep_error;
    int poll_event,poll_hup,short_uio,stale_uio,closed[8],closes;
    unsigned long long clock_ms;
    uint32_t mode,speed;
    uint8_t bits;
} fake;
static void path(char *out,const char *a,const char *b)
{ CHECK(snprintf(out,PATH_MAX,"%s/%s",a,b)<PATH_MAX); }
static void directories(const char *p)
{
    char copy[PATH_MAX]; CHECK(strlen(p)<sizeof(copy)); strcpy(copy,p);
    for(char *q=copy+1;*q;++q) if(*q=='/') { *q=0; CHECK(!mkdir(copy,0700)||errno==EEXIST); *q='/'; }
    CHECK(!mkdir(copy,0700)||errno==EEXIST);
}
static void write_value(const char *base,const char *name,const char *value)
{
    char p[PATH_MAX],dir[PATH_MAX],*last; FILE *f;
    path(p,base,name); strcpy(dir,p); last=strrchr(dir,'/'); CHECK(last); *last=0; directories(dir);
    f=fopen(p,"w"); CHECK(f); CHECK(fputs(value,f)>=0); CHECK(!fclose(f));
}
static void link_value(const char *base,const char *name,const char *target)
{
    char p[PATH_MAX],dir[PATH_MAX],*last;
    path(p,base,name); strcpy(dir,p); last=strrchr(dir,'/'); CHECK(last); *last=0; directories(dir);
    CHECK(!symlink(target,p));
}
static void fixture(void)
{
    char tmp[]="/tmp/fte3600-board-XXXXXX",physical[PATH_MAX],gpio[PATH_MAX],uio[PATH_MAX],spi[PATH_MAX],p[PATH_MAX];
    CHECK(mkdtemp(tmp)); strcpy(root,tmp); path(sysroot,root,"sys"); path(devroot,root,"dev");
    path(physical,sysroot,"devices/platform/spi0.0"); path(glue,physical,"fte3600-glue.0");
    path(spi,physical,"spidev/spidev0.0"); path(gpio,glue,"gpiochip7"); path(uio,glue,"uio/uio3");
    directories(spi); directories(gpio); directories(uio); directories(devroot);
    path(p,sysroot,"bus/spi/drivers/spidev"); directories(p); link_value(physical,"driver",p);
    path(p,sysroot,"bus/platform/drivers/fte3600-glue"); directories(p); link_value(glue,"driver",p);
    path(p,sysroot,"devices/LNXSYSTM/FTE3600:00"); directories(p); write_value(p,"hid","FTE3600\n"); link_value(physical,"firmware_node",p);
    link_value(sysroot,"class/spidev/spidev0.0",spi); link_value(spi,"device",physical);
    link_value(sysroot,"bus/gpio/devices/gpiochip7",gpio);
    link_value(sysroot,"class/uio/uio3",uio); link_value(uio,"device",glue);
    write_value(spi,"dev","153:0\n"); write_value(gpio,"dev","254:7\n"); write_value(uio,"dev","247:3\n");
    link_value(sysroot,"dev/char/153:0",spi); link_value(sysroot,"dev/char/254:7",gpio); link_value(sysroot,"dev/char/247:3",uio);
    write_value(uio,"name","fte3600-irq\n"); write_value(uio,"version","2\n");
    write_value(sysroot,"module/spidev/parameters/bufsiz","65536\n");
    write_value(glue,"fte3600_glue_abi","2\n"); write_value(glue,"fte3600_ngpio","1\n");
    write_value(glue,"fte3600_generation","0\n"); write_value(glue,"fte3600_status","ready\n");
    write_value(glue,"fte3600_acpi_mode","0\n"); write_value(glue,"fte3600_acpi_speed_hz","12000000\n");
    write_value(glue,"fte3600_cs_control","1\n"); write_value(glue,"fte3600_irq_active_low","1\n"); write_value(glue,"fte3600_irq_source","gpio\n");
    write_value(devroot,"spidev0.0",""); write_value(devroot,"gpiochip7",""); write_value(devroot,"uio3","");
}
static int classify(const char *p)
{
    if(strncmp(p,devroot,strlen(devroot))||p[strlen(devroot)]!='/') return 0;
    p+=strlen(devroot)+1;
    return !strcmp(p,"spidev0.0")?1:(!strcmp(p,"gpiochip7")||!strcmp(p,"gpiochip8"))?2:!strcmp(p,"uio3")?3:0;
}
static int fd_path(int fd,char *target)
{
    char p[64]; ssize_t n;
    snprintf(p,sizeof(p),"/proc/self/fd/%d",fd); n=readlink(p,target,PATH_MAX-1);
    if(n<0) return 0;
    target[n]=0; return 1;
}
static int fd_kind(int fd) { char target[PATH_MAX]; return fd_path(fd,target)?classify(target):0; }
static int metadata_path(const char *p)
{ return !strncmp(p,sysroot,strlen(sysroot))&&p[strlen(sysroot)]=='/'; }
static int metadata_fd(int fd)
{ char target[PATH_MAX];return fd_path(fd,target)&&metadata_path(target); }
char *__real_realpath(const char *,char *);
char *__wrap_realpath(const char *p,char *out)
{ if(count_calls) ++calls.realpaths;return __real_realpath(p,out); }
char *__real___realpath_chk(const char *,char *,size_t);
char *__wrap___realpath_chk(const char *p,char *out,size_t capacity)
{ if(count_calls) ++calls.realpaths;return __real___realpath_chk(p,out,capacity); }
int __real_open(const char *,int,...);
int __wrap_open(const char *p,int flags,...)
{
    if(count_calls&&metadata_path(p)) ++calls.metadata_opens;
    if(flags&O_CREAT) {
        va_list args;mode_t mode;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);
        return __real_open(p,flags,mode);
    }
    return __real_open(p,flags);
}
static void char_stat(struct stat *st,int which)
{
    if(!which) return;
    st->st_mode=(st->st_mode&0777)|S_IFCHR;
    st->st_rdev=which==1?makedev(153,0):which==2?makedev(254,7):makedev(247,3);
}
int __real_stat(const char *,struct stat *);
int __wrap_stat(const char *p,struct stat *st) { if(count_calls) ++calls.stats;int r=__real_stat(p,st); if(!r) char_stat(st,classify(p)); return r; }
int __real_lstat(const char *,struct stat *);
int __wrap_lstat(const char *p,struct stat *st)
{ if(count_calls) ++calls.lstats;return __real_lstat(p,st); }
int __real_fstat(int,struct stat *);
int __wrap_fstat(int fd,struct stat *st) { int r=__real_fstat(fd,st); if(!r) char_stat(st,fd_kind(fd)); return r; }
uid_t __wrap_geteuid(void) { return (uid_t)fake.uid; }
int __real_close(int);
int __wrap_close(int fd)
{
    int kind=fd_kind(fd);
    if(count_calls&&metadata_fd(fd)) ++calls.metadata_closes;
    if(kind && fake.closes<8) fake.closed[fake.closes++]=fd==fake.reset_fd?4:kind;
    return __real_close(fd);
}
int __wrap_ioctl(int fd,unsigned long command,...)
{
    va_list args; void *arg;
    va_start(args,command); arg=va_arg(args,void *); va_end(args);
    ++fake.ioctls; if(fake.fail_ioctl==fake.ioctls) { errno=EIO; return -1; }
    switch(command) {
    case GPIO_GET_CHIPINFO_IOCTL: CHECK(fd_kind(fd)==2); ((struct gpiochip_info *)arg)->lines=1; return 0;
    case GPIO_V2_GET_LINEINFO_IOCTL: strcpy(((struct gpio_v2_line_info *)arg)->name,"reset"); return 0;
    case GPIO_V2_GET_LINE_IOCTL: {
        struct gpio_v2_line_request *r=arg;
        CHECK(r->num_lines==1&&r->offsets[0]==0);
        CHECK(r->config.flags==(GPIO_V2_LINE_FLAG_OUTPUT|GPIO_V2_LINE_FLAG_ACTIVE_LOW));
        CHECK(r->config.num_attrs==1&&r->config.attrs[0].attr.id==GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
        CHECK(r->config.attrs[0].attr.values==0&&r->config.attrs[0].mask==1);
        fake.reset_fd=r->fd=fcntl(fd,F_DUPFD_CLOEXEC,0); CHECK(r->fd>=0);
        if(fake.stale_request) write_value(glue,"fte3600_generation","1\n");
        return 0;
    }
    case GPIO_V2_LINE_SET_VALUES_IOCTL: {
        struct gpio_v2_line_values *v=arg; CHECK(fd==fake.reset_fd&&v->mask==1);
        if(bus_gpio) bus_gpio(v->bits?0:1);
        else CHECK(v->bits==(fake.gpio_calls?0u:1u));
        ++fake.gpio_calls;
        if(fake.stale_gpio) write_value(glue,"fte3600_generation","1\n");
        return 0;
    }
    case SPI_IOC_RD_MODE32: *(uint32_t *)arg=fake.mode; return 0;
    case SPI_IOC_WR_MODE32:
        ++fake.mode_writes;
        CHECK(fake.mode_writes<=(int)(sizeof(fake.written_modes)/sizeof(fake.written_modes[0])));
        fake.written_modes[fake.mode_writes-1]=*(uint32_t *)arg;
        if(fake.mode_writes==fake.fail_mode_write) { errno=fake.fail_mode_error?fake.fail_mode_error:EIO;return -1; }
        if(fake.mode_writes!=fake.ignore_mode_write) fake.mode=*(uint32_t *)arg;
        if(after_mode) after_mode();
        return 0;
    case SPI_IOC_RD_MAX_SPEED_HZ: *(uint32_t *)arg=fake.speed; return 0;
    case SPI_IOC_WR_MAX_SPEED_HZ: fake.speed=*(uint32_t *)arg; return 0;
    case SPI_IOC_RD_BITS_PER_WORD: *(uint8_t *)arg=fake.bits; return 0;
    case SPI_IOC_WR_BITS_PER_WORD: fake.bits=*(uint8_t *)arg; return 0;
    default: {
        struct spi_ioc_transfer *tr=arg;
        CHECK(command==SPI_IOC_MESSAGE(1)&&fd_kind(fd)==1); ++fake.spi_calls;
        CHECK(tr->tx_buf!=tr->rx_buf&&tr->speed_hz==fake.speed&&tr->bits_per_word==8);
        if(bus_spi) {
            memcpy((void *)(uintptr_t)tr->rx_buf,(void *)(uintptr_t)tr->tx_buf,tr->len);
            if(bus_spi((void *)(uintptr_t)tr->rx_buf,tr->len)>>31) { errno=EIO;return -1; }
        } else memset((void *)(uintptr_t)tr->rx_buf,0xa7,tr->len);
        if(fake.stale_spi) write_value(glue,"fte3600_generation","1\n");
        if(after_spi) after_spi();
        return (int)tr->len-(fake.short_spi?1:0);
    }
    }
}
int __wrap_poll(struct pollfd *p,nfds_t n,int ms)
{
    CHECK(n==1&&fd_kind(p->fd)==3); fake.clock_ms+=(unsigned)ms;
    p->revents=fake.poll_hup?POLLHUP:(bus_poll?bus_poll(ms):fake.poll_event)?POLLIN:0; return p->revents?1:0;
}
int __wrap___poll_chk(struct pollfd *p,nfds_t n,int ms,size_t capacity)
{ CHECK(n*sizeof(*p)<=capacity); return __wrap_poll(p,n,ms); }
ssize_t __real_read(int,void *,size_t);
ssize_t __wrap_read(int fd,void *buffer,size_t n)
{
    if(count_calls&&metadata_fd(fd)) ++calls.metadata_reads;
    if(fd_kind(fd)!=3) return __real_read(fd,buffer,n);
    CHECK(n==4); *(uint32_t *)buffer=37;
    if(fake.stale_uio) write_value(glue,"fte3600_generation","1\n");
    return fake.short_uio?2:4;
}
ssize_t __wrap___read_chk(int fd,void *buffer,size_t n,size_t capacity)
{ CHECK(n<=capacity); return __wrap_read(fd,buffer,n); }
int __wrap_clock_gettime(clockid_t clock,struct timespec *t)
{
    CHECK(clock==CLOCK_MONOTONIC);
    if(fake.clock_error) { errno=EIO; return -1; }
    t->tv_sec=(time_t)(fake.clock_ms/1000); t->tv_nsec=(long)(fake.clock_ms%1000)*1000000; return 0;
}
int __wrap_clock_nanosleep(clockid_t clock,int flags,const struct timespec *request,struct timespec *left)
{
    (void)left; CHECK(clock==CLOCK_MONOTONIC&&flags==0);
    if(fake.sleep_error) return EIO;
    if(bus_sleep) bus_sleep((uint64_t)request->tv_sec*1000000000+(uint64_t)request->tv_nsec);
    fake.clock_ms+=(unsigned long long)request->tv_sec*1000+(unsigned long long)request->tv_nsec/1000000; return 0;
}
static unsigned fd_count(void)
{
    unsigned n=0; DIR *d=opendir("/proc/self/fd"); CHECK(d); while(readdir(d)) ++n; CHECK(!closedir(d)); return n;
}
static struct fte3600_linux_board_config config(void)
{
    return (struct fte3600_linux_board_config){.sys_root=sysroot,.dev_root=devroot,
        .spi="spidev0.0",.gpio="gpiochip7",.uio="uio3",.transfer_capacity=65535};
}
static int atomic_cancelled(void *context)
{ return atomic_load_explicit((atomic_int *)context,memory_order_acquire); }
static void *cancel_thread(void *context)
{ atomic_store_explicit((atomic_int *)context,1,memory_order_release);return NULL; }
static void reset_fake(int native)
{
    memset(&fake,0,sizeof(fake)); fake.reset_fd=-1; fake.mode=3; fake.speed=1000000; fake.bits=16;
    write_value(glue,"fte3600_generation","0\n"); write_value(glue,"fte3600_status","ready\n");
    write_value(glue,"fte3600_cs_control",native?"1\n":"0\n");
    write_value(sysroot,"module/spidev/parameters/bufsiz","65536\n");
}
static struct fte3600_linux_board *start(int native)
{
    struct fte3600_linux_board_config c=config(); struct fte3600_linux_board *b=NULL;
    reset_fake(native); CHECK(!fte3600_linux_board_open(&c,&b)); CHECK(b); return b;
}
/* This mode also links against the unchanged baseline board source. Counts
 * describe public C calls, not libc's internal path-resolution syscalls.
 * Timings exclude the counting pass and are reported without assertions. */
int __real_clock_gettime(clockid_t,struct timespec *);
static void benchmark(void)
{
    const unsigned transfers=1000;struct timespec first,last;uint8_t data[16]={0};
    struct fte3600_linux_board *b=start(1);
    const struct fte3600_device_io *io=fte3600_linux_board_callbacks(b);
    memset(&calls,0,sizeof(calls));count_calls=1;
    CHECK(!io->spi(io->context,data,sizeof(data)));count_calls=0;
    printf("per SPI: realpath=%u stat=%u lstat=%u metadata open/read/close=%u/%u/%u\n",
        calls.realpaths,calls.stats,calls.lstats,calls.metadata_opens,calls.metadata_reads,calls.metadata_closes);
    CHECK(calls.metadata_opens==32&&calls.metadata_reads==32&&calls.metadata_closes==32);
    CHECK(calls.realpaths==0 ? calls.stats==42&&calls.lstats==46 :
        calls.realpaths==26&&calls.stats==16&&calls.lstats==0);
    CHECK(!__real_clock_gettime(CLOCK_MONOTONIC,&first));
    for(unsigned i=0;i<transfers;++i) CHECK(!io->spi(io->context,data,sizeof(data)));
    CHECK(!__real_clock_gettime(CLOCK_MONOTONIC,&last));
    double seconds=(double)(last.tv_sec-first.tv_sec)+(double)(last.tv_nsec-first.tv_nsec)/1e9;
    printf("synthetic SPI: %u transfers, %.6f seconds, %.1f transfers/second\n",transfers,seconds,transfers/seconds);
    CHECK(!fte3600_linux_board_close(b));
}
static char mutation_path[PATH_MAX],mutation_value[128];
static void change_metadata(void)
{ write_value(sysroot,mutation_path,mutation_value); }
static void remove_metadata(void)
{ char p[PATH_MAX];path(p,sysroot,mutation_path);CHECK(!unlink(p)); }
static void rejected_spi(struct fte3600_linux_board *b,int post)
{
    const struct fte3600_device_io *io=fte3600_linux_board_callbacks(b);
    uint8_t data[16],original[16];memset(data,0x45,sizeof(data));memcpy(original,data,sizeof(data));
    CHECK(io->spi(io->context,data,sizeof(data))&&io->error(io->context)==ESTALE);
    CHECK(fake.spi_calls==post&&!memcmp(data,original,sizeof(data)));
    io->clear_error(io->context);CHECK(io->spi(io->context,data,sizeof(data))&&fake.spi_calls==post);
    int prior=fake.ioctls;CHECK(fte3600_linux_board_close(b)==-1&&errno==ESTALE);CHECK(fake.ioctls==prior);
    after_spi=NULL;
}
static void metadata_changes(void)
{
    static const struct {const char *path,*before,*after;} cases[]={
        {"module/spidev/parameters/bufsiz","65536\n","32768\n"},
        {"devices/LNXSYSTM/FTE3600:00/hid","FTE3600\n","FTE3600evil\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/uio/uio3/name","fte3600-irq\n","other\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/uio/uio3/version","2\n","1\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_glue_abi","2\n","1\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_ngpio","1\n","2\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_status","ready\n","suspended\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_generation","0\n","1\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_acpi_mode","0\n","1\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_acpi_speed_hz","12000000\n","11000000\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_cs_control","1\n","0\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_irq_active_low","1\n","0\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/fte3600_irq_source","gpio\n","unknown\n"},
        {"devices/platform/spi0.0/spidev/spidev0.0/dev","153:0\n","153:1\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/gpiochip7/dev","254:7\n","254:8\n"},
        {"devices/platform/spi0.0/fte3600-glue.0/uio/uio3/dev","247:3\n","247:4\n"}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) for(int post=0;post<2;++post) {
        struct fte3600_linux_board *b=start(1);
        strcpy(mutation_path,cases[i].path);strcpy(mutation_value,cases[i].after);
        if(post) after_spi=change_metadata;else change_metadata();
        rejected_spi(b,post);write_value(sysroot,cases[i].path,cases[i].before);
    }
    for(unsigned i=6;i<=7;++i) for(int post=0;post<2;++post) {
        struct fte3600_linux_board *b=start(1);strcpy(mutation_path,cases[i].path);
        if(post) after_spi=remove_metadata;else remove_metadata();
        rejected_spi(b,post);write_value(sysroot,cases[i].path,cases[i].before);
    }
}
static char alias_path[PATH_MAX],alias_target[PATH_MAX];
static void redirect_alias(void)
{ CHECK(!unlink(alias_path));CHECK(!symlink(alias_target,alias_path)); }
static void alias_changes(void)
{
    static const char *const aliases[]={
        "class/spidev/spidev0.0","devices/platform/spi0.0/spidev/spidev0.0/device",
        "devices/platform/spi0.0/driver","devices/platform/spi0.0/firmware_node",
        "bus/gpio/devices/gpiochip7","devices/platform/spi0.0/fte3600-glue.0/driver",
        "class/uio/uio3","devices/platform/spi0.0/fte3600-glue.0/uio/uio3/device",
        "dev/char/153:0","dev/char/254:7","dev/char/247:3"
    };
    char original[PATH_MAX];path(alias_target,sysroot,"other-object");directories(alias_target);
    for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);++i) for(int post=0;post<2;++post) {
        struct fte3600_linux_board *b=start(1);path(alias_path,sysroot,aliases[i]);
        ssize_t n=readlink(alias_path,original,sizeof(original)-1);CHECK(n>0);original[n]=0;
        if(post) after_spi=redirect_alias;else redirect_alias();
        rejected_spi(b,post);CHECK(!unlink(alias_path));CHECK(!symlink(original,alias_path));
    }
}
static char replaced_path[PATH_MAX],retained_path[PATH_MAX];
static void move_children(const char *from,const char *to)
{
    DIR *d=opendir(from);struct dirent *entry;CHECK(d);
    while((entry=readdir(d))) {
        char a[PATH_MAX],b[PATH_MAX];
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
        path(a,from,entry->d_name);path(b,to,entry->d_name);CHECK(!rename(a,b));
    }
    CHECK(!closedir(d));
}
static void replace_directory(void)
{
    CHECK(!rename(replaced_path,retained_path));CHECK(!mkdir(replaced_path,0700));
    /* Move, rather than recreate, the contents so every attribute, child and
     * symlink is unchanged. Only this directory's inode is different. */
    move_children(retained_path,replaced_path);
}
static void replace_node(void)
{ CHECK(!rename(replaced_path,retained_path));write_value(devroot,mutation_path,""); }
static void forward_directory(void)
{ CHECK(!rename(replaced_path,retained_path));CHECK(!symlink(retained_path,replaced_path)); }
static void inode_changes(void)
{
    static const char *const directories_to_replace[]={
        "devices/platform/spi0.0","devices/platform/spi0.0/fte3600-glue.0",
        "devices/platform/spi0.0/spidev/spidev0.0",
        "devices/platform/spi0.0/fte3600-glue.0/gpiochip7",
        "devices/platform/spi0.0/fte3600-glue.0/uio/uio3",
        "bus/spi/drivers/spidev","bus/platform/drivers/fte3600-glue",
        "devices/LNXSYSTM/FTE3600:00"
    };
    static const char *const nodes[]={"spidev0.0","gpiochip7","uio3"};
    path(retained_path,root,"retained-instance");
    for(unsigned i=0;i<sizeof(directories_to_replace)/sizeof(directories_to_replace[0]);++i)
        for(int post=0;post<2;++post) {
            struct fte3600_linux_board *b=start(1);path(replaced_path,sysroot,directories_to_replace[i]);
            if(post) after_spi=replace_directory;else replace_directory();
            rejected_spi(b,post);move_children(replaced_path,retained_path);
            CHECK(!rmdir(replaced_path));CHECK(!rename(retained_path,replaced_path));
        }
    for(unsigned i=0;i<sizeof(nodes)/sizeof(nodes[0]);++i) for(int post=0;post<2;++post) {
        struct fte3600_linux_board *b=start(1);path(replaced_path,devroot,nodes[i]);strcpy(mutation_path,nodes[i]);
        if(post) after_spi=replace_node;else replace_node();
        rejected_spi(b,post);CHECK(!unlink(replaced_path));CHECK(!rename(retained_path,replaced_path));
    }
    /* Forwarding symlinks preserve the original target inode. Cover both a
     * meaningful leaf (driver name) and a shared ancestor of the device. */
    for(unsigned i=0;i<2;++i) for(int post=0;post<2;++post) {
        struct fte3600_linux_board *b=start(1);
        path(replaced_path,sysroot,i?"devices/platform":"bus/spi/drivers/spidev");
        if(post) after_spi=forward_directory;else forward_directory();
        rejected_spi(b,post);CHECK(!unlink(replaced_path));CHECK(!rename(retained_path,replaced_path));
    }
}
int main(void)
{
    struct fte3600_linux_board *b; struct fte3600_linux_board_info info; struct fte3600_linux_board_config c;
    const struct fte3600_device_io *io; uint8_t data[16],original[16]; uint32_t count=0; unsigned before;
    fixture(); reset_fake(1); c=config(); before=fd_count();
    if(getenv("FTE3600_BOARD_BENCHMARK")) { benchmark();return 0; }
    fake.uid=1000; CHECK(!fte3600_linux_board_validate(&c,&info)); CHECK(fake.ioctls==0);
    CHECK(fte3600_linux_board_open(&c,&b)==-1&&errno==EPERM&&b==NULL); fake.uid=0;
    c.gpio="../gpiochip7"; CHECK(fte3600_linux_board_validate(&c,&info)==-1); c=config();
    write_value(glue,"fte3600_glue_abi","1\n"); CHECK(fte3600_linux_board_validate(&c,&info)==-1);
    write_value(glue,"fte3600_glue_abi","2\n");
    b=start(1); io=fte3600_linux_board_callbacks(b); CHECK(fake.mode==0&&fake.bits==8&&fake.speed==1000000);
    CHECK(!io->gpio(io->context,0)&&!io->gpio(io->context,1));
    memset(data,0x12,sizeof(data));memset(&calls,0,sizeof(calls));count_calls=1;
    CHECK(!io->spi(io->context,data,sizeof(data))&&data[0]==0xa7&&fake.spi_calls==1);count_calls=0;
    CHECK(calls.realpaths==0&&calls.stats==42&&calls.lstats==46);
    CHECK(calls.metadata_opens==32&&calls.metadata_reads==32&&calls.metadata_closes==32);
    CHECK(!fte3600_linux_board_set_cs(b,1)&&fake.mode==SPI_CS_HIGH);
    fake.poll_event=1; CHECK(fte3600_linux_board_wait_irq(b,100,&count)==1&&count==37);
    CHECK(!fte3600_linux_board_close(b)); CHECK(fake.mode==0&&fake.bits==16&&fake.speed==1000000);
    CHECK(fake.closes==4&&fake.closed[0]==3&&fake.closed[1]==4&&fake.closed[2]==1&&fake.closed[3]==2);
    CHECK(fd_count()==before);
    /* Preserve an existing lower speed ceiling, also on fixed-CS devices.
     * Zero is unspecified; higher settings are capped by ACPI. */
    static const uint32_t saved_speeds[]={1000000,12000000,20000000,0};
    for(unsigned native=0;native<2;++native) for(unsigned n=0;n<4;++n) {
        reset_fake((int)native);fake.speed=saved_speeds[n];
        uint32_t expected=n==0?1000000:12000000;
        CHECK(!fte3600_linux_board_open(&c,&b));io=fte3600_linux_board_callbacks(b);
        CHECK(fake.speed==expected);
        CHECK(!io->boundary(io->context,FTE3600_BOUNDARY_SETTINGS));
        CHECK(!io->spi(io->context,data,sizeof(data)));
        CHECK(!fte3600_linux_board_close(b)&&fake.speed==saved_speeds[n]&&fd_count()==before);
        CHECK(fake.mode_writes==(native?2:0));
    }
    /* A successful ioctl is insufficient when restoration did not take.
     * Failed restore still releases every descriptor and is observable. */
    for(int failure=0;failure<2;++failure) {
        b=start(1);CHECK(!fte3600_linux_board_set_cs(b,1));
        if(failure) { fake.fail_mode_write=3;fake.fail_mode_error=EBUSY; }
        else fake.ignore_mode_write=3;
        CHECK(fte3600_linux_board_close(b)==-1&&errno==(failure?EBUSY:EPROTO));
        CHECK(fake.mode==SPI_CS_HIGH&&fake.bits==16&&fake.speed==1000000&&fd_count()==before);
    }
    b=start(0); CHECK(fte3600_linux_board_set_cs(b,1)==-1&&errno==ENOTSUP);
    CHECK(!fte3600_linux_board_close(b)&&fake.mode_writes==0&&fake.mode==3);
    reset_fake(1); write_value(sysroot,"module/spidev/parameters/bufsiz","8\n");
    CHECK(!fte3600_linux_board_open(&c,&b)); io=fte3600_linux_board_callbacks(b);
    CHECK(io->spi(io->context,data,sizeof(data))&&io->error(io->context)==EMSGSIZE&&fake.spi_calls==0);
    CHECK(!fte3600_linux_board_close(b));
    for(int failure=1;failure<=12;++failure) {
        reset_fake(1); fake.fail_ioctl=failure;
        CHECK(fte3600_linux_board_open(&c,&b)==-1&&b==NULL); CHECK(fd_count()==before);
    }
    reset_fake(1);fake.fail_ioctl=8;fake.fail_mode_write=2;fake.fail_mode_error=EBUSY;
    CHECK(fte3600_linux_board_open(&c,&b)==-1&&errno==EBUSY&&!b&&fd_count()==before);
    reset_fake(1); fake.stale_request=1; CHECK(fte3600_linux_board_open(&c,&b)==-1&&b==NULL); CHECK(fd_count()==before);
    for(int stale=0;stale<2;++stale) {
        b=start(1); io=fte3600_linux_board_callbacks(b); fake.short_spi=!stale; fake.stale_spi=stale;
        memset(data,0x45,sizeof(data)); memcpy(original,data,sizeof(data));
        CHECK(io->spi(io->context,data,sizeof(data))&UINT32_C(0x80000000)); CHECK(!memcmp(data,original,sizeof(data)));
        io->clear_error(io->context); CHECK(io->error(io->context)!=0);
        CHECK(io->spi(io->context,data,sizeof(data))&&fake.spi_calls==1);
        (void)fte3600_linux_board_close(b); CHECK(fd_count()==before);
    }
    b=start(1); io=fte3600_linux_board_callbacks(b); fake.stale_gpio=1;
    CHECK(io->gpio(io->context,0)&&io->error(io->context)==ESTALE); (void)fte3600_linux_board_close(b);
    b=start(1); CHECK(fte3600_linux_board_wait_irq(b,101,&count)==0&&fake.clock_ms==101); CHECK(!fte3600_linux_board_close(b));
    for(int kind=0;kind<3;++kind) {
        b=start(1); fake.poll_event=1; fake.short_uio=kind==0; fake.stale_uio=kind==1; fake.poll_hup=kind==2;
        count=99; CHECK(fte3600_linux_board_wait_irq(b,100,&count)==-1&&count==99); (void)fte3600_linux_board_close(b);
    }
    for(int kind=0;kind<2;++kind) {
        b=start(1); io=fte3600_linux_board_callbacks(b); fake.clock_error=kind==0; fake.sleep_error=kind==1;
        if(kind) io->wait_ms(io->context,1); else (void)io->clock_ms(io->context);
        CHECK(io->error(io->context)==EIO&&io->spi(io->context,data,sizeof(data))&&fake.spi_calls==0);
        (void)fte3600_linux_board_close(b);
    }
    reset_fake(1); { volatile sig_atomic_t cancel=0; c=config(); c.cancelled=&cancel;
      CHECK(!fte3600_linux_board_open(&c,&b)); cancel=1;
      CHECK(fte3600_linux_board_wait_irq(b,100,&count)==-1&&errno==ECANCELED); CHECK(!fte3600_linux_board_close(b)); }
    reset_fake(1); { atomic_int cancel=ATOMIC_VAR_INIT(0);pthread_t thread;
      c=config();c.is_cancelled=atomic_cancelled;c.cancel_context=&cancel;
      CHECK(!fte3600_linux_board_open(&c,&b));
      CHECK(!pthread_create(&thread,NULL,cancel_thread,&cancel));CHECK(!pthread_join(thread,NULL));
      CHECK(fte3600_linux_board_wait_irq(b,100,&count)==-1&&errno==ECANCELED);
      CHECK(!fte3600_linux_board_close(b)); }
    metadata_changes();alias_changes();inode_changes();
    CHECK(fd_count()==before); puts("linux board: identity, leases, polarity, SPI, IRQ, cancellation and fd checks passed");
    return 0;
}
