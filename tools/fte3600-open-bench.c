/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include <fprint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Public libfprint API only. Opening performs the driver's normal startup;
 * this program never requests acquisition, enrollment or print storage. */
struct timing { double wall_ms, cpu_ms; };
static double milliseconds(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock,&value)) { perror("clock_gettime"); exit(2); }
    return (double)value.tv_sec*1000.0+(double)value.tv_nsec/1000000.0;
}
static struct timing now(void)
{
    return (struct timing){milliseconds(CLOCK_MONOTONIC),milliseconds(CLOCK_PROCESS_CPUTIME_ID)};
}
static void json_string(const char *value)
{
    if (!value) { fputs("null",stdout); return; }
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)value;*p;++p) {
        if (*p=='"' || *p=='\\') { putchar('\\');putchar(*p); }
        else if (*p<32) printf("\\u%04x",(unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}
static void report(unsigned cycle,const char *operation,gboolean ok,
                   struct timing start,struct timing stop,const GError *error)
{
    printf("{\"cycle\":%u,\"operation\":",cycle);json_string(operation);
    printf(",\"ok\":%s,\"wall_ms\":%.3f,\"cpu_ms\":%.3f,\"error_domain\":",
           ok?"true":"false",stop.wall_ms-start.wall_ms,stop.cpu_ms-start.cpu_ms);
    json_string(error?g_quark_to_string(error->domain):NULL);
    printf(",\"error_code\":%d,\"error_message\":",error?error->code:0);
    json_string(error?error->message:NULL);puts("}");fflush(stdout);
}
static int bounded_number(const char *text,unsigned maximum,unsigned *value)
{
    char *end=NULL;errno=0;unsigned long parsed=strtoul(text,&end,10);
    if (errno || !text[0] || text[0]<'0' || text[0]>'9' || *end || parsed<1 || parsed>maximum) return 0;
    *value=(unsigned)parsed;return 1;
}
struct cancellation { GCancellable *token;guint source;gboolean fired; };
static gboolean cancel_open(gpointer data)
{
    struct cancellation *cancel=data;
    cancel->source=0;cancel->fired=TRUE;g_cancellable_cancel(cancel->token);
    return G_SOURCE_REMOVE;
}
static int cancelled_open_case(FpDevice *device,unsigned delay)
{
    GError *error=NULL;struct cancellation cancel={g_cancellable_new(),0,FALSE};
    struct timing start=now();
    cancel.source=g_timeout_add(delay,cancel_open,&cancel);
    gboolean opened=fp_device_open_sync(device,cancel.token,&error);
    struct timing stop=now();
    /* A successful or failed open may beat the timer. Never leave a callback
     * pointing to this stack frame or carrying over into the normal cycles. */
    if (cancel.source) g_source_remove(cancel.source);
    report(0,"cancel_open",opened,start,stop,error);
    gboolean still_open=fp_device_is_open(device);
    gboolean passed=cancel.fired && !opened && !still_open &&
        g_error_matches(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
    printf("{\"operation\":\"cancel_open_check\",\"ok\":%s,\"delay_ms\":%u,\"timer_fired\":%s,\"device_open\":%s,\"reason\":",
           passed?"true":"false",delay,cancel.fired?"true":"false",still_open?"true":"false");
    json_string(passed?NULL:opened?"open_won_cancellation_race":"expected_cancellation_not_observed");
    puts("}");fflush(stdout);g_clear_error(&error);g_object_unref(cancel.token);
    if (still_open) {
        start=now();gboolean closed=fp_device_close_sync(device,NULL,&error);stop=now();
        report(0,"cancel_open_cleanup_close",closed,start,stop,error);g_clear_error(&error);
        if (!closed) return 1;
    }
    return passed?0:1;
}
int main(int argc,char **argv)
{
    unsigned cycles=3,cancel_delay=0;int supplied_cycles=0;
    for (int i=1;i<argc;++i) {
        if (g_str_has_prefix(argv[i],"--cancel-open-ms=")) {
            if (cancel_delay || !bounded_number(argv[i]+17,1000,&cancel_delay)) {
                fputs("--cancel-open-ms must be supplied once with an integer from 1 to 1000\n",stderr);return 2;
            }
        } else {
            if (supplied_cycles || !bounded_number(argv[i],20,&cycles)) {
                fprintf(stderr,"Usage: %s [cycles: 1..20; default 3] [--cancel-open-ms=1..1000]\n",argv[0]);return 2;
            }
            supplied_cycles=1;
        }
    }
    /* Restrict discovery before constructing the context, then also require
     * one exact driver match rather than choosing among multiple sensors. */
    if (!g_setenv("FP_DRIVERS_ALLOWLIST","fte3600",TRUE)) return 2;
    struct timing start=now();FpContext *context=fp_context_new();
    if (!context) { fputs("Cannot create libfprint context\n",stderr);return 2; }
    GPtrArray *devices=fp_context_get_devices(context);FpDevice *device=NULL;unsigned matches=0;
    for (guint i=0;i<devices->len;++i) {
        FpDevice *candidate=g_ptr_array_index(devices,i);
        if (!g_strcmp0(fp_device_get_driver(candidate),"fte3600")) { device=candidate;++matches; }
    }
    struct timing stop=now();
    printf("{\"operation\":\"discover\",\"driver\":\"fte3600\",\"matches\":%u,\"wall_ms\":%.3f,\"cpu_ms\":%.3f}\n",
           matches,stop.wall_ms-start.wall_ms,stop.cpu_ms-start.cpu_ms);fflush(stdout);
    if (matches!=1) { g_object_unref(context);return 2; }
    if (cancel_delay && cancelled_open_case(device,cancel_delay)) { g_object_unref(context);return 1; }
    int result=0;
    for (unsigned cycle=1;cycle<=cycles;++cycle) {
        GError *error=NULL;start=now();
        gboolean ok=fp_device_open_sync(device,NULL,&error);stop=now();
        report(cycle,"open",ok,start,stop,error);g_clear_error(&error);
        if (!ok) { result=1;break; }
        start=now();ok=fp_device_close_sync(device,NULL,&error);stop=now();
        report(cycle,"close",ok,start,stop,error);g_clear_error(&error);
        if (!ok) { result=1;break; }
    }
    g_object_unref(context);return result;
}
