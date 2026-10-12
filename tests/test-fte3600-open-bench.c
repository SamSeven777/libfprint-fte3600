/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Public API doubles with the real GLib default main context and timer. */
#define main benchmark_main
#include "../tools/fte3600-open-bench.c"
#undef main

enum outcome { CANCELLED, OPEN_WINS, OTHER_ERROR, CANCELLED_BUT_OPEN };
static enum outcome outcome;
static GPtrArray *devices;
static gboolean is_open;
static unsigned opens,closes,contexts_freed,tokens_freed;
static void freed(gpointer data,GObject *object)
{ (void)object;++*(unsigned *)data; }
FpContext *__wrap_fp_context_new(void)
{
    GObject *object=g_object_new(G_TYPE_OBJECT,NULL);
    g_object_weak_ref(object,freed,&contexts_freed);return (FpContext *)object;
}
GPtrArray *__wrap_fp_context_get_devices(FpContext *context)
{ (void)context;return devices; }
const gchar *__wrap_fp_device_get_driver(FpDevice *device)
{ (void)device;return "fte3600"; }
gboolean __wrap_fp_device_is_open(FpDevice *device)
{ (void)device;return is_open; }
gboolean __wrap_fp_device_open_sync(FpDevice *device,GCancellable *cancel,GError **error)
{
    (void)device;g_assert_false(is_open);++opens;
    if (!cancel) { is_open=TRUE;return TRUE; }
    g_object_weak_ref(G_OBJECT(cancel),freed,&tokens_freed);
    if (outcome==OPEN_WINS) { is_open=TRUE;return TRUE; }
    if (outcome==OTHER_ERROR) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"unrelated failure");return FALSE;
    }
    while (!g_cancellable_is_cancelled(cancel)) g_main_context_iteration(NULL,TRUE);
    g_assert_true(g_cancellable_set_error_if_cancelled(cancel,error));
    if (outcome==CANCELLED_BUT_OPEN) is_open=TRUE;
    return FALSE;
}
gboolean __wrap_fp_device_close_sync(FpDevice *device,GCancellable *cancel,GError **error)
{
    (void)device;(void)error;g_assert_null(cancel);g_assert_true(is_open);
    ++closes;is_open=FALSE;return TRUE;
}
static void run_case(enum outcome next,int expected,unsigned expected_opens,unsigned expected_closes)
{
    outcome=next;is_open=FALSE;opens=closes=contexts_freed=tokens_freed=0;
    devices=g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(devices,g_object_new(G_TYPE_OBJECT,NULL));
    char program[]="test-open-bench",cancel[]="--cancel-open-ms=1",cycles[]="2";
    char *args[]={program,cancel,cycles,NULL};
    g_assert_cmpint(benchmark_main(3,args),==,expected);
    g_assert_cmpuint(opens,==,expected_opens);g_assert_cmpuint(closes,==,expected_closes);
    g_assert_cmpuint(contexts_freed,==,1);g_assert_cmpuint(tokens_freed,==,1);g_assert_false(is_open);
    /* Pending sources from a winning open/error must have been removed;
     * otherwise this dispatch would dereference expired stack/token state. */
    g_usleep(5000);while (g_main_context_iteration(NULL,FALSE)) {}
    g_ptr_array_unref(devices);
}
int main(void)
{
    outcome=CANCELLED;is_open=FALSE;opens=closes=contexts_freed=tokens_freed=0;
    devices=g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(devices,g_object_new(G_TYPE_OBJECT,NULL));
    char program[]="test-open-bench";
    char *defaults[]={program,NULL};
    g_assert_cmpint(benchmark_main(1,defaults),==,0);
    g_assert_cmpuint(opens,==,3);g_assert_cmpuint(closes,==,3);
    g_assert_cmpuint(contexts_freed,==,1);g_assert_cmpuint(tokens_freed,==,0);
    g_ptr_array_unref(devices);
    const char *invalid[]={"--cancel-open-ms=0","--cancel-open-ms=1001","--cancel-open-ms=-1","--cancel-open-ms=", "0","21"};
    for (size_t i=0;i<G_N_ELEMENTS(invalid);++i) {
        char *args[]={program,(char *)invalid[i],NULL};
        g_assert_cmpint(benchmark_main(2,args),==,2);
    }
    run_case(CANCELLED,0,3,2);
    run_case(OPEN_WINS,1,1,1);
    run_case(OTHER_ERROR,1,1,0);
    run_case(CANCELLED_BUT_OPEN,1,1,1);
    puts("PASS open benchmark cancellation, reopen, race, error and timer cleanup");return 0;
}
