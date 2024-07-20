/* file.c
 * File I/O routines
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>
#define WS_LOG_DOMAIN LOG_DOMAIN_CAPTURE

#include <time.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <wsutil/file_util.h>
#include <wsutil/filesystem.h>
#include <wsutil/json_dumper.h>
#include <wsutil/wslog.h>
#include <wsutil/ws_assert.h>
#include <wsutil/version_info.h>
#include <wsutil/report_message.h>

#include <wiretap/merge.h>

#include <epan/exceptions.h>
#include <epan/epan.h>
#include <epan/column.h>
#include <epan/packet.h>
#include <epan/column-utils.h>
#include <epan/expert.h>
#include <epan/prefs.h>
#include <epan/dfilter/dfilter.h>
#include <epan/epan_dissect.h>
#include <epan/tap.h>
#include <epan/timestamp.h>
#include <epan/strutil.h>
#include <epan/addr_resolv.h>
#include <epan/color_filters.h>
#include <epan/secrets.h>

#include "cfile.h"
#include "file.h"
#include "fileset.h"
#include "frame_tvbuff.h"

#include "ui/alert_box.h"
#include "ui/simple_dialog.h"
#include "ui/main_statusbar.h"
#include "ui/progress_dlg.h"
#include "ui/urls.h"
#include "ui/ws_ui_util.h"
#include "ui/packet_list_utils.h"

/* Needed for addrinfo */
#include <sys/types.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#endif

static bool read_record(capture_file *cf, wtap_rec *rec, Buffer *buf,
    dfilter_t *dfcode, epan_dissect_t *edt, column_info *cinfo, int64_t offset,
    fifo_string_cache_t *frame_dup_cache, GChecksum *frame_cksum);

static void rescan_packets(capture_file *cf, const char *action, const char *action_item, bool redissect);

typedef enum {
    MR_NOTMATCHED,
    MR_MATCHED,
    MR_ERROR
} match_result;
typedef match_result (*ws_match_function)(capture_file *, frame_data *,
        wtap_rec *, Buffer *, void *);
static match_result match_protocol_tree(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static void match_subtree_text(proto_node *node, void *data);
static void match_subtree_text_reverse(proto_node *node, void *data);
static match_result match_summary_line(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_narrow_and_wide(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_narrow_and_wide_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_narrow_and_wide_case(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_narrow_and_wide_case_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_narrow_case(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_narrow_case_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_wide(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_wide_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_wide_case(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_wide_case_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_binary(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_binary_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_regex(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_regex_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_dfilter(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_marked(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static match_result match_time_reference(capture_file *cf, frame_data *fdata,
        wtap_rec *, Buffer *, void *criterion);
static bool find_packet(capture_file *cf, ws_match_function match_function,
        void *criterion, search_direction dir);

static void cf_rename_failure_alert_box(const char *filename, int err);

/* Seconds spent processing packets between pushing UI updates. */
#define PROGBAR_UPDATE_INTERVAL 0.150

/* Show the progress bar after this many seconds. */
#define PROGBAR_SHOW_DELAY 0.5

/*
 * Maximum number of records we support in a file.
 *
 * It is, at most, the maximum value of a uint32_t, as we use a uint32_t
 * for the frame number.
 *
 * We allow it to be set to a lower value; see issue #16908 for why
 * we're doing this.  Thanks, Qt!
 */
static uint32_t max_records = UINT32_MAX;

void
cf_set_max_records(unsigned max_records_arg)
{
    max_records = max_records_arg;
}

/*
 * We could probably use g_signal_...() instead of the callbacks below but that
 * would require linking our CLI programs to libgobject and creating an object
 * instance for the signals.
 */
typedef struct {
    cf_callback_t cb_fct;
    void *        user_data;
} cf_callback_data_t;

static GList *cf_callbacks;

static void
cf_callback_invoke(int event, void *data)
{
    cf_callback_data_t *cb;
    GList              *cb_item = cf_callbacks;

    /* there should be at least one interested */
    ws_assert(cb_item != NULL);

    while (cb_item != NULL) {
        cb = (cf_callback_data_t *)cb_item->data;
        cb->cb_fct(event, data, cb->user_data);
        cb_item = g_list_next(cb_item);
    }
}

void
cf_callback_add(cf_callback_t func, void *user_data)
{
    cf_callback_data_t *cb;

    cb = g_new(cf_callback_data_t,1);
    cb->cb_fct = func;
    cb->user_data = user_data;

    cf_callbacks = g_list_prepend(cf_callbacks, cb);
}

void
cf_callback_remove(cf_callback_t func, void *user_data)
{
    cf_callback_data_t *cb;
    GList              *cb_item = cf_callbacks;

    while (cb_item != NULL) {
        cb = (cf_callback_data_t *)cb_item->data;
        if (cb->cb_fct == func && cb->user_data == user_data) {
            cf_callbacks = g_list_remove(cf_callbacks, cb);
            g_free(cb);
            return;
        }
        cb_item = g_list_next(cb_item);
    }

    ws_assert_not_reached();
}

unsigned long
cf_get_computed_elapsed(capture_file *cf)
{
    return cf->computed_elapsed;
}

static void
compute_elapsed(capture_file *cf, int64_t start_time)
{
    int64_t delta_time = g_get_monotonic_time() - start_time;

    cf->computed_elapsed = (unsigned long) (delta_time / 1000); /* ms */
}

static epan_t *
ws_epan_new(capture_file *cf)
{
    static const struct packet_provider_funcs funcs = {
        cap_file_provider_get_frame_ts,
        cap_file_provider_get_interface_name,
        cap_file_provider_get_interface_description,
        cap_file_provider_get_modified_block
    };

    return epan_new(&cf->provider, &funcs);
}

cf_status_t
cf_open(capture_file *cf, const char *fname, unsigned int type, bool is_tempfile, int *err)
{
    wtap  *wth;
    char *err_info;

    wth = wtap_open_offline(fname, type, err, &err_info, true);
    if (wth == NULL)
        goto fail;

    /* The open succeeded.  Close whatever capture file we had open,
       and fill in the information for this file. */
    cf_close(cf);

    /* Initialize the record metadata. */
    wtap_rec_init(&cf->rec);

    /* XXX - we really want to initialize this after we've read all
       the packets, so we know how much we'll ultimately need. */
    ws_buffer_init(&cf->buf, 1514);

    /* We're about to start reading the file. */
    cf->state = FILE_READ_IN_PROGRESS;

    /* If there was a pending redissection for the old file (there
     * shouldn't be), clear it. cf_close() should have failed if the
     * old file's read lock was held, but it doesn't hurt to clear it. */
    cf->read_lock = false;
    cf->redissection_queued = RESCAN_NONE;

    cf->provider.wth = wth;
    cf->f_datalen = 0;

    /* Set the file name because we need it to set the follow stream filter.
       XXX - is that still true?  We need it for other reasons, though,
       in any case. */
    cf->filename = g_strdup(fname);

    /* Indicate whether it's a permanent or temporary file. */
    cf->is_tempfile = is_tempfile;

    /* No user changes yet. */
    cf->unsaved_changes = false;

    cf->computed_elapsed = 0;

    cf->cd_t        = wtap_file_type_subtype(cf->provider.wth);
    cf->open_type   = type;
    cf->linktypes = g_array_sized_new(FALSE, FALSE, (unsigned) sizeof(int), 1);
    cf->count     = 0;
    cf->packet_comment_count = 0;
    cf->displayed_count = 0;
    cf->marked_count = 0;
    cf->ignored_count = 0;
    cf->ref_time_count = 0;
    cf->drops_known = false;
    cf->drops     = 0;
    cf->snap      = wtap_snapshot_length(cf->provider.wth);

    /* Allocate a frame_data_sequence for the frames in this file */
    cf->provider.frames = new_frame_data_sequence();

    nstime_set_zero(&cf->elapsed_time);
    cf->provider.ref = NULL;
    cf->provider.prev_dis = NULL;
    cf->provider.prev_cap = NULL;
    cf->cum_bytes = 0;

    /* Create new epan session for dissection.
     * (The old one was freed in cf_close().)
     */
    cf->epan = ws_epan_new(cf);

    packet_list_queue_draw();
    cf_callback_invoke(cf_cb_file_opened, cf);

    wtap_set_cb_new_ipv4(cf->provider.wth, add_ipv4_name);
    wtap_set_cb_new_ipv6(cf->provider.wth, (wtap_new_ipv6_callback_t) add_ipv6_name);
    wtap_set_cb_new_secrets(cf->provider.wth, secrets_wtap_callback);

    return CF_OK;

fail:
    cfile_open_failure_alert_box(fname, *err, err_info);
    return CF_ERROR;
}

/*
 * Add an encapsulation type to cf->linktypes.
 */
static void
cf_add_encapsulation_type(capture_file *cf, int encap)
{
    unsigned i;

    for (i = 0; i < cf->linktypes->len; i++) {
        if (g_array_index(cf->linktypes, int, i) == encap)
            return; /* it's already there */
    }
    /* It's not already there - add it. */
    g_array_append_val(cf->linktypes, encap);
}

/* Reset everything to a pristine state */
void
cf_close(capture_file *cf)
{
    cf->stop_flag = false;
    if (cf->state == FILE_CLOSED || cf->state == FILE_READ_PENDING)
        return; /* Nothing to do */

    /* Die if we're in the middle of reading a file. */
    ws_assert(cf->state != FILE_READ_IN_PROGRESS);
    ws_assert(!cf->read_lock);

    cf_callback_invoke(cf_cb_file_closing, cf);

    /* close things, if not already closed before */
    color_filters_cleanup();

    if (cf->provider.wth) {
        wtap_close(cf->provider.wth);
        cf->provider.wth = NULL;
    }
    /* We have no file open... */
    if (cf->filename != NULL) {
        /* If it's a temporary file, remove it. */
        if (cf->is_tempfile)
            ws_unlink(cf->filename);
        g_free(cf->filename);
        cf->filename = NULL;
    }
    /* ...which means we have no changes to that file to save. */
    cf->unsaved_changes = false;

    /* no open_routine type */
    cf->open_type = WTAP_TYPE_AUTO;

    /* Clean up the record metadata. */
    wtap_rec_cleanup(&cf->rec);

    /* Clear the packet list. */
    packet_list_freeze();
    packet_list_clear();
    packet_list_thaw();

    /* Free up the packet buffer. */
    ws_buffer_free(&cf->buf);

    dfilter_free(cf->rfcode);
    cf->rfcode = NULL;
    if (cf->provider.frames != NULL) {
        free_frame_data_sequence(cf->provider.frames);
        cf->provider.frames = NULL;
    }
    if (cf->provider.frames_modified_blocks) {
        g_tree_destroy(cf->provider.frames_modified_blocks);
        cf->provider.frames_modified_blocks = NULL;
    }
    cf_unselect_packet(cf);   /* nothing to select */
    cf->first_displayed = 0;
    cf->last_displayed = 0;

    /* No frames, no frame selected, no field in that frame selected. */
    cf->count = 0;
    cf->current_frame = NULL;
    cf->finfo_selected = NULL;

    /* No frame link-layer types, either. */
    if (cf->linktypes != NULL) {
        g_array_free(cf->linktypes, TRUE);
        cf->linktypes = NULL;
    }

    cf->f_datalen = 0;
    nstime_set_zero(&cf->elapsed_time);

    reset_tap_listeners();

    epan_free(cf->epan);
    cf->epan = NULL;

    /* We have no file open. */
    cf->state = FILE_CLOSED;

    cf_callback_invoke(cf_cb_file_closed, cf);
}

/*
 * true if the progress dialog doesn't exist and it looks like we'll
 * take > PROGBAR_SHOW_DELAY (500ms) to load, false otherwise.
 */
static inline bool
progress_is_slow(progdlg_t *progdlg, GTimer *prog_timer, int64_t size, int64_t pos)
{
    double elapsed;

    if (progdlg) return false;
    elapsed = g_timer_elapsed(prog_timer, NULL);
    /* This only gets checked between reading records, which doesn't help if
     * a single record takes a very long time, e.g., the first TLS packet if
     * the SSLKEYLOGFILE is very large. (#17051) */
    if ((elapsed * 2 > PROGBAR_SHOW_DELAY && (size / pos) >= 2) /* It looks like we're going to be slow. */
            || elapsed > PROGBAR_SHOW_DELAY) { /* We are indeed slow. */
        return true;
    }
    return false;
}

static float
calc_progbar_val(capture_file *cf, int64_t size, int64_t file_pos, char *status_str, unsigned long status_size)
{
    float progbar_val;

    progbar_val = (float) file_pos / (float) size;
    if (progbar_val > 1.0) {

        /*  The file probably grew while we were reading it.
         *  Update file size, and try again.
         */
        size = wtap_file_size(cf->provider.wth, NULL);

        if (size >= 0)
            progbar_val = (float) file_pos / (float) size;

        /*  If it's still > 1, either "wtap_file_size()" failed (in which
         *  case there's not much we can do about it), or the file
         *  *shrank* (in which case there's not much we can do about
         *  it); just clip the progress value at 1.0.
         */
        if (progbar_val > 1.0f)
            progbar_val = 1.0f;
    }

    snprintf(status_str, status_size,
            "%" PRId64 "KB of %" PRId64 "KB",
            file_pos / 1024, size / 1024);

    return progbar_val;
}

cf_read_status_t
cf_read(capture_file *cf, bool reloading)
{
    int                  err = 0;
    char                *err_info = NULL;
    volatile bool        too_many_records = false;
    char                *name_ptr;
    progdlg_t           *volatile progbar = NULL;
    GTimer              *prog_timer = g_timer_new();
    int64_t              size;
    int64_t              start_time;
    epan_dissect_t       edt;
    wtap_rec             rec;
    Buffer               buf;
    dfilter_t           *dfcode = NULL;
    column_info         *cinfo;
    volatile bool        create_proto_tree;
    unsigned             tap_flags;
    bool                 compiled _U_;
    volatile bool        is_read_aborted = false;

    /* The update_progress_dlg call below might end up accepting a user request to
     * trigger redissection/rescans which can modify/destroy the dissection
     * context ("cf->epan"). That condition should be prevented by callers, but in
     * case it occurs let's fail gracefully.
     */
    if (cf->read_lock) {
        ws_warning("Failing due to recursive cf_read(\"%s\", %d) call!",
                cf->filename, reloading);
        return CF_READ_ERROR;
    }
    /* This is a full dissection, so clear any pending request for one. */
    cf->redissection_queued = RESCAN_NONE;
    cf->read_lock = true;

    /* Compile the current display filter.
     * The code it compiles to might have changed, e.g. if a display
     * filter macro used has changed.
     *
     * We assume this will not fail since cf->dfilter is only set in
     * cf_filter IFF the filter was valid.
     * XXX - This is not necessarily true, if the filter has a FT_IPv4
     * or FT_IPv6 field compared to a resolved hostname in it, because
     * we do a new host lookup, and that *could* timeout this time
     * (though with the read lock above we shouldn't have many lookups at
     * once, reducing the chances of that)... (#19612)
     */
    if (cf->dfilter) {
        compiled = dfilter_compile(cf->dfilter, &dfcode, NULL);
        ws_assert(compiled && dfcode);
    }

    dfilter_free(cf->dfcode);
    cf->dfcode = dfcode;

    /* The compiled dfilter might have a field reference; recompiling it
     * means that the field references won't match anything. That's what
     * we want since this is a new sequential read and we don't have
     * a selected frame with a tree. (Will taps with filters with display
     * references also have cleared display references?)
     */

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    /*
     * Determine whether we need to create a protocol tree.
     * We do if:
     *
     *    we're going to apply a display filter;
     *
     *    one of the tap listeners is going to apply a filter;
     *
     *    one of the tap listeners requires a protocol tree;
     *
     *    a postdissector wants field values or protocols on
     *    the first pass.
     */
    create_proto_tree =
        (cf->dfcode != NULL || have_filtering_tap_listeners() ||
         (tap_flags & TL_REQUIRES_PROTO_TREE) || postdissectors_want_hfids());

    reset_tap_listeners();

    name_ptr = g_filename_display_basename(cf->filename);

    if (reloading)
        cf_callback_invoke(cf_cb_file_reload_started, cf);
    else
        cf_callback_invoke(cf_cb_file_read_started, cf);

    /* Record the file's compression type.
       XXX - do we know this at open time? */
    cf->compression_type = wtap_get_compression_type(cf->provider.wth);

    /* The packet list window will be empty until the file is completely loaded */
    packet_list_freeze();

    cf->stop_flag = false;
    start_time = g_get_monotonic_time();

    epan_dissect_init(&edt, cf->epan, create_proto_tree, false);

    /* If the display filter or any tap listeners require the columns,
     * construct them. */
    cinfo = (tap_listeners_require_columns() ||
        dfilter_requires_columns(cf->dfcode)) ? &cf->cinfo : NULL;

    /* Find the size of the file. */
    size = wtap_file_size(cf->provider.wth, NULL);

    /* If we are to ignore duplicate frames, we need a container to store
     * hashes frame contents */
    fifo_string_cache_t frame_dup_cache;
    GChecksum *volatile cksum = NULL;

    if (prefs.ignore_dup_frames) {
        fifo_string_cache_init(&frame_dup_cache, prefs.ignore_dup_frames_cache_entries, g_free);
        cksum = g_checksum_new(G_CHECKSUM_SHA256);
    }

    g_timer_start(prog_timer);

    wtap_rec_init(&rec);
    ws_buffer_init(&buf, 1514);

    TRY {
        int64_t file_pos;
        int64_t data_offset;

        float   progbar_val;
        char    status_str[100];

        while ((wtap_read(cf->provider.wth, &rec, &buf, &err, &err_info,
                        &data_offset))) {
            if (size >= 0) {
                if (cf->count == max_records) {
                    /*
                     * Quit if we've already read the maximum number of
                     * records allowed.
                     */
                    too_many_records = true;
                    break;
                }
                file_pos = wtap_read_so_far(cf->provider.wth);

                /* Create the progress bar if necessary. */
                if (progress_is_slow(progbar, prog_timer, size, file_pos)) {
                    progbar_val = calc_progbar_val(cf, size, file_pos, status_str, sizeof(status_str));
                    progbar = delayed_create_progress_dlg(cf->window, NULL, NULL, true,
                            &cf->stop_flag, progbar_val);
                }

                /*
                 * Update the progress bar, but do it only after
                 * PROGBAR_UPDATE_INTERVAL has elapsed. Calling update_progress_dlg
                 * and packets_bar_update will likely trigger UI paint events, which
                 * might take a while depending on the platform and display. Reset
                 * our timer *after* painting.
                 */
                if (progbar && g_timer_elapsed(prog_timer, NULL) > PROGBAR_UPDATE_INTERVAL) {
                    progbar_val = calc_progbar_val(cf, size, file_pos, status_str, sizeof(status_str));
                    /* update the packet bar content on the first run or frequently on very large files */
                    update_progress_dlg(progbar, progbar_val, status_str);
                    compute_elapsed(cf, start_time);
                    packets_bar_update();
                    g_timer_start(prog_timer);
                }
                /*
                 * The previous GUI triggers should not have destroyed the running
                 * session. If that did happen, it could blow up when read_record tries
                 * to use the destroyed edt.session, so detect it right here.
                 */
                ws_assert(edt.session == cf->epan);
            }

            if (cf->state == FILE_READ_ABORTED) {
                /* Well, the user decided to exit Wireshark.  Break out of the
                   loop, and let the code below (which is called even if there
                   aren't any packets left to read) exit. */
                is_read_aborted = true;
                break;
            }
            if (cf->stop_flag) {
                /* Well, the user decided to abort the read. He/She will be warned and
                   it might be enough for him/her to work with the already loaded
                   packets.
                   This is especially true for very large capture files, where you don't
                   want to wait loading the whole file (which may last minutes or even
                   hours even on fast machines) just to see that it was the wrong file. */
                break;
            }
            read_record(cf, &rec, &buf, cf->dfcode, &edt, cinfo, data_offset, &frame_dup_cache, cksum);
            wtap_rec_reset(&rec);
        }
    }
    CATCH(OutOfMemoryError) {
        simple_message_box(ESD_TYPE_ERROR, NULL,
                "More information and workarounds can be found at\n"
                WS_WIKI_URL("KnownBugs/OutOfMemory"),
                "Sorry, but Wireshark has run out of memory and has to terminate now.");
#if 0
        /* Could we close the current capture and free up memory from that? */
#else
        /* we have to terminate, as we cannot recover from the memory error */
        exit(1);
#endif
    }
    ENDTRY;

    // If we're ignoring duplicate frames, clear the data structures.
    // We really could look at prefs.ignore_dup_frames here, but it's even
    // safer to check if we had allocated 'cksum'.
    if (cksum != NULL) {
        fifo_string_cache_free(&frame_dup_cache);
        g_checksum_free(cksum);
    }

    /* We're done reading sequentially through the file. */
    cf->state = FILE_READ_DONE;

    /* Destroy the progress bar if it was created. */
    if (progbar != NULL)
        destroy_progress_dlg(progbar);
    g_timer_destroy(prog_timer);

    /* Free the display name */
    g_free(name_ptr);

    epan_dissect_cleanup(&edt);
    wtap_rec_cleanup(&rec);
    ws_buffer_free(&buf);

    /* Close the sequential I/O side, to free up memory it requires. */
    wtap_sequential_close(cf->provider.wth);

    /* Allow the protocol dissectors to free up memory that they
     * don't need after the sequential run-through of the packets. */
    postseq_cleanup_all_protocols();

    /* compute the time it took to load the file */
    compute_elapsed(cf, start_time);

    /* Set the file encapsulation type now; we don't know what it is until
       we've looked at all the packets, as we don't know until then whether
       there's more than one type (and thus whether it's
       WTAP_ENCAP_PER_PACKET). */
    cf->lnk_t = wtap_file_encap(cf->provider.wth);

    cf->current_frame = frame_data_sequence_find(cf->provider.frames, cf->first_displayed);

    packet_list_thaw();

    /* It is safe again to execute redissections or sort. */
    ws_assert(cf->read_lock);
    cf->read_lock = false;

    if (reloading)
        cf_callback_invoke(cf_cb_file_reload_finished, cf);
    else
        cf_callback_invoke(cf_cb_file_read_finished, cf);

    /* If we have any displayed packets to select, select the first of those
       packets by making the first row the selected row. */
    if (cf->first_displayed != 0) {
        packet_list_select_row_from_data(NULL);
    }

    if (is_read_aborted) {
        /*
         * Well, the user decided to exit Wireshark while reading this *offline*
         * capture file (Live captures are handled by something like
         * cf_continue_tail). Clean up accordingly.
         */
        cf_close(cf);
        cf->redissection_queued = RESCAN_NONE;
        return CF_READ_ABORTED;
    }

    if (cf->redissection_queued != RESCAN_NONE) {
        /* Redissection was queued up. Clear the request and perform it now. */
        bool redissect = cf->redissection_queued == RESCAN_REDISSECT;
        rescan_packets(cf, NULL, NULL, redissect);
    }

    if (cf->stop_flag) {
        simple_message_box(ESD_TYPE_WARN, NULL,
                "The remaining packets in the file were discarded.\n"
                "\n"
                "As a lot of packets from the original file will be missing,\n"
                "remember to be careful when saving the current content to a file.\n",
                "File loading was cancelled.");
        return CF_READ_ERROR;
    }

    if (err != 0) {
        /* Put up a message box noting that the read failed somewhere along
           the line.  Don't throw out the stuff we managed to read, though,
           if any. */
        cfile_read_failure_alert_box(NULL, err, err_info);
        return CF_READ_ERROR;
    } else if (too_many_records) {
        simple_message_box(ESD_TYPE_WARN, NULL,
                "The remaining packets in the file were discarded.\n"
                "\n"
                "As a lot of packets from the original file will be missing,\n"
                "remember to be careful when saving the current content to a file.\n"
                "\n"
                "The command-line utility editcap can be used to split "
                "the file into multiple smaller files",
                "The file contains more records than the maximum "
                "supported number of records, %u.", max_records);
        return CF_READ_ERROR;
    } else
        return CF_READ_OK;
}

#ifdef HAVE_LIBPCAP
cf_read_status_t
cf_continue_tail(capture_file *cf, volatile int to_read, wtap_rec *rec,
        Buffer *buf, int *err, fifo_string_cache_t *frame_dup_cache, GChecksum *frame_cksum)
{
    char             *err_info;
    volatile int      newly_displayed_packets = 0;
    epan_dissect_t    edt;
    bool              create_proto_tree;
    unsigned          tap_flags;

    /* Don't compile the current display filter. The current display filter
     * text might compile to different code than when the capture started:
     *
     *    If it has a IP address resolved name, calling get_host_ipaddr every
     *    time new packets arrive can mean a *lot* of gethostbyname calls
     *    in flight at once, eventually leading to a timeout (#19612).
     *    addr_resolv.c says that ares_gethostbyname is "usually interactive",
     *    unlike ares_gethostbyaddr (used in dissection), and violating that
     *    expectation is bad.
     *
     *    If it has a display filter macro, the definition might have changed.
     *
     *    If it has a field reference, the selected frame / current proto tree
     *    might have changed, and we don't have the old one. If we recompile,
     *    we can't set the field references to the old values.
     *
     * For a rescan, redissection, reload, retap, or opening a new file, we
     * want to compile. What about here, when new frames have arrived in a live
     * capture? We might be able to cache the host lookup, and a user might want
     * the new display filter macro definition, but the user almost surely wants
     * the field references to refer to values from the proto tree when the
     * filter was applied, not whatever it happens to be now if the user has
     * clicked on a different packet.
     *
     * To get the new compiled filter, the user should refilter.
     */

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    /*
     * Determine whether we need to create a protocol tree.
     * We do if:
     *
     *    we're going to apply a display filter;
     *
     *    one of the tap listeners is going to apply a filter;
     *
     *    one of the tap listeners requires a protocol tree;
     *
     *    a postdissector wants field values or protocols on
     *    the first pass.
     */
    create_proto_tree =
        (cf->dfcode != NULL || have_filtering_tap_listeners() ||
         (tap_flags & TL_REQUIRES_PROTO_TREE) || postdissectors_want_hfids());

    *err = 0;

    /* Don't freeze/thaw the list when doing live capture */
    /*packet_list_freeze();*/

    epan_dissect_init(&edt, cf->epan, create_proto_tree, false);

    TRY {
        int64_t data_offset = 0;
        column_info *cinfo;

        /* If the display filter or any tap listeners require the columns,
         * construct them. */
        cinfo = (tap_listeners_require_columns() ||
            dfilter_requires_columns(cf->dfcode)) ? &cf->cinfo : NULL;

        while (to_read != 0) {
            wtap_cleareof(cf->provider.wth);
            if (!wtap_read(cf->provider.wth, rec, buf, err, &err_info,
                        &data_offset)) {
                break;
            }
            if (cf->state == FILE_READ_ABORTED) {
                /* Well, the user decided to exit Wireshark.  Break out of the
                   loop, and let the code below (which is called even if there
                   aren't any packets left to read) exit. */
                break;
            }
            if (read_record(cf, rec, buf, cf->dfcode, &edt, cinfo, data_offset, frame_dup_cache, frame_cksum)) {
                newly_displayed_packets++;
            }
            to_read--;
        }
        wtap_rec_reset(rec);
    }
    CATCH(OutOfMemoryError) {
        simple_message_box(ESD_TYPE_ERROR, NULL,
                "More information and workarounds can be found at\n"
                WS_WIKI_URL("KnownBugs/OutOfMemory"),
                "Sorry, but Wireshark has run out of memory and has to terminate now.");
#if 0
        /* Could we close the current capture and free up memory from that? */
        return CF_READ_ABORTED;
#else
        /* we have to terminate, as we cannot recover from the memory error */
        exit(1);
#endif
    }
    ENDTRY;

    /* Update the file encapsulation; it might have changed based on the
       packets we've read. */
    cf->lnk_t = wtap_file_encap(cf->provider.wth);

    epan_dissect_cleanup(&edt);

    /* Don't freeze/thaw the list when doing live capture */
    /*packet_list_thaw();*/
    /* With the new packet list the first packet
     * isn't automatically selected.
     */
    if (!cf->current_frame && !packet_list_multi_select_active())
        packet_list_select_row_from_data(NULL);

    if (cf->state == FILE_READ_ABORTED) {
        /* Well, the user decided to exit Wireshark.  Return CF_READ_ABORTED
           so that our caller can kill off the capture child process;
           this will cause an EOF on the pipe from the child, so
           "cf_finish_tail()" will be called, and it will clean up
           and exit. */
        return CF_READ_ABORTED;
    } else if (*err != 0) {
        /* We got an error reading the capture file.
           XXX - pop up a dialog box instead? */
        if (err_info != NULL) {
            ws_warning("Error \"%s\" while reading \"%s\" (\"%s\")",
                    wtap_strerror(*err), cf->filename, err_info);
            g_free(err_info);
        } else {
            ws_warning("Error \"%s\" while reading \"%s\"",
                    wtap_strerror(*err), cf->filename);
        }
        return CF_READ_ERROR;
    } else
        return CF_READ_OK;
}

void
cf_fake_continue_tail(capture_file *cf)
{
    if (cf->state == FILE_CLOSED) {
        cf->state = FILE_READ_PENDING;
    }
}

cf_read_status_t
cf_finish_tail(capture_file *cf, wtap_rec *rec, Buffer *buf, int *err,
        fifo_string_cache_t *frame_dup_cache, GChecksum *frame_cksum)
{
    char      *err_info;
    int64_t    data_offset;
    column_info *cinfo;
    epan_dissect_t edt;
    bool       create_proto_tree;
    unsigned   tap_flags;

    /* All the comments above in cf_continue_tail apply regarding the
     * current display filter.
     */

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    /* If the display filter or any tap listeners require the columns,
     * construct them. */
    cinfo = (tap_listeners_require_columns() ||
        dfilter_requires_columns(cf->dfcode)) ? &cf->cinfo : NULL;

    /*
     * Determine whether we need to create a protocol tree.
     * We do if:
     *
     *    we're going to apply a display filter;
     *
     *    one of the tap listeners is going to apply a filter;
     *
     *    one of the tap listeners requires a protocol tree;
     *
     *    a postdissector wants field values or protocols on
     *    the first pass.
     */
    create_proto_tree =
        (cf->dfcode != NULL || have_filtering_tap_listeners() ||
         (tap_flags & TL_REQUIRES_PROTO_TREE) || postdissectors_want_hfids());

    if (cf->provider.wth == NULL) {
        cf_close(cf);
        return CF_READ_ERROR;
    }

    /* Don't freeze/thaw the list when doing live capture */
    /*packet_list_freeze();*/

    epan_dissect_init(&edt, cf->epan, create_proto_tree, false);

    while ((wtap_read(cf->provider.wth, rec, buf, err, &err_info, &data_offset))) {
        if (cf->state == FILE_READ_ABORTED) {
            /* Well, the user decided to abort the read.  Break out of the
               loop, and let the code below (which is called even if there
               aren't any packets left to read) exit. */
            break;
        }
        read_record(cf, rec, buf, cf->dfcode, &edt, cinfo, data_offset, frame_dup_cache, frame_cksum);
        wtap_rec_reset(rec);
    }

    epan_dissect_cleanup(&edt);

    /* Don't freeze/thaw the list when doing live capture */
    /*packet_list_thaw();*/

    if (cf->state == FILE_READ_ABORTED) {
        /* Well, the user decided to abort the read.  We're only called
           when the child capture process closes the pipe to us (meaning
           it's probably exited), so we can just close the capture
           file; we return CF_READ_ABORTED so our caller can do whatever
           is appropriate when that happens. */
        cf_close(cf);
        return CF_READ_ABORTED;
    }

    /* We're done reading sequentially through the file. */
    cf->state = FILE_READ_DONE;

    /* We're done reading sequentially through the file; close the
       sequential I/O side, to free up memory it requires. */
    wtap_sequential_close(cf->provider.wth);

    /* Allow the protocol dissectors to free up memory that they
     * don't need after the sequential run-through of the packets. */
    postseq_cleanup_all_protocols();

    /* Update the file encapsulation; it might have changed based on the
       packets we've read. */
    cf->lnk_t = wtap_file_encap(cf->provider.wth);

    /* Update the details in the file-set dialog, as the capture file
     * has likely grown since we first stat-ed it */
    fileset_update_file(cf->filename);

    if (*err != 0) {
        /* We got an error reading the capture file.
           XXX - pop up a dialog box? */
        if (err_info != NULL) {
            ws_warning("Error \"%s\" while reading \"%s\" (\"%s\")",
                    wtap_strerror(*err), cf->filename, err_info);
            g_free(err_info);
        } else {
            ws_warning("Error \"%s\" while reading \"%s\"",
                    wtap_strerror(*err), cf->filename);
        }
        return CF_READ_ERROR;
    } else {
        return CF_READ_OK;
    }
}
#endif /* HAVE_LIBPCAP */

char *
cf_get_display_name(capture_file *cf)
{
    char *displayname;

    /* Return a name to use in displays */
    if (!cf->is_tempfile) {
        /* Get the last component of the file name, and use that. */
        if (cf->filename) {
            displayname = g_filename_display_basename(cf->filename);
        } else {
            displayname=g_strdup("(No file)");
        }
    } else {
        /* The file we read is a temporary file from a live capture or
           a merge operation; we don't mention its name, but, if it's
           from a capture, give the source of the capture. */
        if (cf->source) {
            displayname = g_strdup(cf->source);
        } else {
            displayname = g_strdup("(Untitled)");
        }
    }
    return displayname;
}

char *
cf_get_basename(capture_file *cf)
{
    char *displayname;

    /* Return a name to use in the GUI for the basename for files to
       which we save statistics */
    if (!cf->is_tempfile) {
        /* Get the last component of the file name, and use that. */
        if (cf->filename) {
            displayname = g_filename_display_basename(cf->filename);

            /* If the file name ends with any extension that corresponds
               to a file type we support - including compressed versions
               of those files - strip it off. */
            size_t displayname_len = strlen(displayname);
            GSList *extensions = wtap_get_all_file_extensions_list();
            GSList *suffix;
            for (suffix = extensions; suffix != NULL; suffix = g_slist_next(suffix)) {
                /* Does the file name end with that extension? */
                const char *extension = (char *)suffix->data;
                size_t extension_len = strlen(extension);
                if (displayname_len > extension_len &&
                        displayname[displayname_len - extension_len - 1] == '.' &&
                        strcmp(&displayname[displayname_len - extension_len], extension) == 0) {
                    /* Yes.  Strip the extension off, and return the result. */
                    displayname[displayname_len - extension_len - 1] = '\0';
                    break;
                }
            }
            wtap_free_extensions_list(extensions);
        } else {
            displayname=g_strdup("");
        }
    } else {
        /* The file we read is a temporary file from a live capture or
           a merge operation; we don't mention its name, but, if it's
           from a capture, give the source of the capture. */
        if (cf->source) {
            displayname = g_strdup(cf->source);
        } else {
            displayname = g_strdup("");
        }
    }
    return displayname;
}

void
cf_set_tempfile_source(capture_file *cf, char *source)
{
    if (cf->source) {
        g_free(cf->source);
    }

    if (source) {
        cf->source = g_strdup(source);
    } else {
        cf->source = g_strdup("");
    }
}

const char *
cf_get_tempfile_source(capture_file *cf)
{
    if (!cf->source) {
        return "";
    }

    return cf->source;
}

/* XXX - use a macro instead? */
int
cf_get_packet_count(capture_file *cf)
{
    return cf->count;
}

/* XXX - use a macro instead? */
bool
cf_is_tempfile(capture_file *cf)
{
    return cf->is_tempfile;
}

void
cf_set_tempfile(capture_file *cf, bool is_tempfile)
{
    cf->is_tempfile = is_tempfile;
}


/* XXX - use a macro instead? */
void
cf_set_drops_known(capture_file *cf, bool drops_known)
{
    cf->drops_known = drops_known;
}

/* XXX - use a macro instead? */
void
cf_set_drops(capture_file *cf, uint32_t drops)
{
    cf->drops = drops;
}

/* XXX - use a macro instead? */
bool
cf_get_drops_known(capture_file *cf)
{
    return cf->drops_known;
}

/* XXX - use a macro instead? */
uint32_t
cf_get_drops(capture_file *cf)
{
    return cf->drops;
}

void
cf_set_rfcode(capture_file *cf, dfilter_t *rfcode)
{
    cf->rfcode = rfcode;
}

static void
add_packet_to_packet_list(frame_data *fdata, capture_file *cf,
        epan_dissect_t *edt, dfilter_t *dfcode, column_info *cinfo,
        wtap_rec *rec, Buffer *buf, bool add_to_packet_list)
{
    frame_data_set_before_dissect(fdata, &cf->elapsed_time,
            &cf->provider.ref, cf->provider.prev_dis);
    cf->provider.prev_cap = fdata;

    if (dfcode != NULL) {
        epan_dissect_prime_with_dfilter(edt, dfcode);
    }
#if 0
    /* Prepare coloring rules, this ensures that display filter rules containing
     * frame.color_rule references are still processed.
     * TODO: actually detect that situation or maybe apply other optimizations? */
    if (edt->tree && color_filters_used()) {
        color_filters_prime_edt(edt);
        fdata->need_colorize = 1;
    }
#endif

    if (!fdata->visited) {
        /* This is the first pass, so prime the epan_dissect_t with the
           hfids postdissectors want on the first pass. */
        prime_epan_dissect_with_postdissector_wanted_hfids(edt);
    }

    /* Initialize passed_dfilter here so that dissectors can hide packets. */
    /* XXX We might want to add a separate "visible" bit to frame_data instead. */
    fdata->passed_dfilter = 1;

    /* Dissect the frame. */
    epan_dissect_run_with_taps(edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, cinfo);

    if (fdata->passed_dfilter && dfcode != NULL) {
        fdata->passed_dfilter = dfilter_apply_edt(dfcode, edt) ? 1 : 0;

        if (fdata->passed_dfilter && edt->pi.fd->dependent_frames) {
            /* This frame passed the display filter but it may depend on other
             * (potentially not displayed) frames.  Find those frames and mark them
             * as depended upon.
             */
            g_hash_table_foreach(edt->pi.fd->dependent_frames, find_and_mark_frame_depended_upon, cf->provider.frames);
        }
    }

    if (fdata->passed_dfilter || fdata->ref_time)
        cf->displayed_count++;

    if (add_to_packet_list) {
        /* We fill the needed columns from new_packet_list */
        packet_list_append(cinfo, fdata);
    }

    if (fdata->passed_dfilter || fdata->ref_time)
    {
        frame_data_set_after_dissect(fdata, &cf->cum_bytes);
        /* The only way we use prev_dis is to get the time stamp of
         * the previous displayed frame, so ignore it if it doesn't
         * have a time stamp, because we're presumably interested in
         * the timestamp of the previously displayed frame with a
         * time. XXX: What if in the future we want to use the previously
         * displayed frame for something else, too?
         */
        if (fdata->has_ts) {
            cf->provider.prev_dis = fdata;
        }

        /* If we haven't yet seen the first frame, this is it. */
        if (cf->first_displayed == 0)
            cf->first_displayed = fdata->num;

        /* This is the last frame we've seen so far. */
        cf->last_displayed = fdata->num;
    }

    epan_dissect_reset(edt);
}

/*
 * Read in a new record.
 * Returns true if the packet was added to the packet (record) list,
 * false otherwise.
 */
static bool
read_record(capture_file *cf, wtap_rec *rec, Buffer *buf, dfilter_t *dfcode,
        epan_dissect_t *edt, column_info *cinfo, int64_t offset,
        fifo_string_cache_t *frame_dup_cache, GChecksum *frame_cksum)
{
    frame_data    fdlocal;
    frame_data   *fdata;
    bool          passed = true;
    bool          added = false;
    const char    *cksum_string;
    bool          was_in_cache;

    /* Add this packet's link-layer encapsulation type to cf->linktypes, if
       it's not already there.
       XXX - yes, this is O(N), so if every packet had a different
       link-layer encapsulation type, it'd be O(N^2) to read the file, but
       there are probably going to be a small number of encapsulation types
       in a file. */
    if (rec->rec_type == REC_TYPE_PACKET) {
        cf_add_encapsulation_type(cf, rec->rec_header.packet_header.pkt_encap);
    }

    /* The frame number of this packet, if we add it to the set of frames,
       would be one more than the count of frames in the file so far. */
    frame_data_init(&fdlocal, cf->count + 1, rec, offset, cf->cum_bytes);

    if (cf->rfcode) {
        epan_dissect_t rf_edt;
        column_info *rf_cinfo = NULL;

        epan_dissect_init(&rf_edt, cf->epan, true, false);
        epan_dissect_prime_with_dfilter(&rf_edt, cf->rfcode);
        if (dfilter_requires_columns(cf->rfcode)) {
                rf_cinfo = &cf->cinfo;
        }
        epan_dissect_run(&rf_edt, cf->cd_t, rec,
                frame_tvbuff_new_buffer(&cf->provider, &fdlocal, buf),
                &fdlocal, rf_cinfo);
        passed = dfilter_apply_edt(cf->rfcode, &rf_edt);
        epan_dissect_cleanup(&rf_edt);
    }

    if (passed) {
        added = true;

        /* This does a shallow copy of fdlocal, which is good enough. */
        fdata = frame_data_sequence_add(cf->provider.frames, &fdlocal);

        cf->count++;
        if (rec->block != NULL)
            cf->packet_comment_count += wtap_block_count_option(rec->block, OPT_COMMENT);
         cf->f_datalen = offset + fdlocal.cap_len;

        // Should we check if the frame data is a duplicate, and thus, ignore
        // this frame?
        if (frame_cksum != NULL && rec->rec_type == REC_TYPE_PACKET) {
            g_checksum_reset(frame_cksum);
            g_checksum_update(frame_cksum, ws_buffer_start_ptr(buf), ws_buffer_length(buf));
            cksum_string = g_strdup(g_checksum_get_string(frame_cksum));
            was_in_cache = fifo_string_cache_insert(frame_dup_cache, cksum_string);
            if (was_in_cache) {
                g_free((void *)cksum_string);
                fdata->ignored = true;
                cf->ignored_count++;
            }
        }

        /* When a redissection is in progress (or queued), do not process packets.
         * This will be done once all (new) packets have been scanned. */
        if (!cf->redissecting && cf->redissection_queued == RESCAN_NONE) {
            add_packet_to_packet_list(fdata, cf, edt, dfcode, cinfo, rec, buf, true);
        }
    }

    return added;
}


typedef struct _callback_data_t {
    void *           pd_window;
    int64_t          f_len;
    progdlg_t       *progbar;
    GTimer          *prog_timer;
    bool             stop_flag;
} callback_data_t;


static bool
merge_callback(merge_event event, int num _U_,
        const merge_in_file_t in_files[], const unsigned in_file_count,
        void *data)
{
    unsigned i;
    callback_data_t *cb_data = (callback_data_t*) data;

    ws_assert(cb_data != NULL);

    switch (event) {

        case MERGE_EVENT_INPUT_FILES_OPENED:
            /* do nothing */
            break;

        case MERGE_EVENT_FRAME_TYPE_SELECTED:
            /* do nothing */
            break;

        case MERGE_EVENT_READY_TO_MERGE:
            /* Get the sum of the sizes of all the files. */
            for (i = 0; i < in_file_count; i++)
                cb_data->f_len += in_files[i].size;

            cb_data->prog_timer = g_timer_new();
            g_timer_start(cb_data->prog_timer);
            break;

        case MERGE_EVENT_RECORD_WAS_READ:
            {
                /* Create the progress bar if necessary.
                   We check on every iteration of the loop, so that it takes no
                   longer than the standard time to create it (otherwise, for a
                   large file, we might take considerably longer than that standard
                   time in order to get to the next progress bar step). */
                if (cb_data->progbar == NULL) {
                    cb_data->progbar = delayed_create_progress_dlg(cb_data->pd_window, NULL, NULL,
                            false, &cb_data->stop_flag, 0.0f);
                }

                /*
                 * Update the progress bar, but do it only after
                 * PROGBAR_UPDATE_INTERVAL has elapsed. Calling update_progress_dlg
                 * and packets_bar_update will likely trigger UI paint events, which
                 * might take a while depending on the platform and display. Reset
                 * our timer *after* painting.
                 */
                if (g_timer_elapsed(cb_data->prog_timer, NULL) > PROGBAR_UPDATE_INTERVAL) {
                    float  progbar_val;
                    int64_t file_pos = 0;
                    /* Get the sum of the seek positions in all of the files. */
                    for (i = 0; i < in_file_count; i++)
                        file_pos += wtap_read_so_far(in_files[i].wth);

                    progbar_val = (float) file_pos / (float) cb_data->f_len;
                    if (progbar_val > 1.0f) {
                        /* Some file probably grew while we were reading it.
                           That "shouldn't happen", so we'll just clip the progress
                           value at 1.0. */
                        progbar_val = 1.0f;
                    }

                    if (cb_data->progbar != NULL) {
                        char status_str[100];
                        snprintf(status_str, sizeof(status_str),
                                "%" PRId64 "KB of %" PRId64 "KB",
                                file_pos / 1024, cb_data->f_len / 1024);
                        update_progress_dlg(cb_data->progbar, progbar_val, status_str);
                    }
                    g_timer_start(cb_data->prog_timer);
                }
            }
            break;

        case MERGE_EVENT_DONE:
            /* We're done merging the files; destroy the progress bar if it was created. */
            if (cb_data->progbar != NULL)
                destroy_progress_dlg(cb_data->progbar);
            g_timer_destroy(cb_data->prog_timer);
            break;
    }

    return cb_data->stop_flag;
}



cf_status_t
cf_merge_files_to_tempfile(void *pd_window, const char *temp_dir, char **out_filenamep,
        int in_file_count, const char *const *in_filenames,
        int file_type, bool do_append)
{
    bool                       status;
    merge_progress_callback_t  cb;
    callback_data_t           *cb_data = g_new0(callback_data_t, 1);

    /* prepare our callback routine */
    cb_data->pd_window = pd_window;
    cb.callback_func = merge_callback;
    cb.data = cb_data;

    cf_callback_invoke(cf_cb_file_merge_started, NULL);

    /* merge the files */
    status = merge_files_to_tempfile(temp_dir, out_filenamep, "wireshark", file_type,
            in_filenames,
            in_file_count, do_append,
            IDB_MERGE_MODE_ALL_SAME, 0 /* snaplen */,
            "Wireshark", &cb);

    g_free(cb.data);

    cf_callback_invoke(cf_cb_file_merge_finished, NULL);

    if (!status) {
        /* Callers aren't expected to treat an error or an explicit abort
           differently - the merge code puts up error dialogs itself, so
           they don't have to. */
        return CF_ERROR;
    } else
        return CF_OK;
}

cf_status_t
cf_filter_packets(capture_file *cf, char *dftext, bool force)
{
    const char *filter_new = dftext ? dftext : "";
    const char *filter_old = cf->dfilter ? cf->dfilter : "";
    dfilter_t  *dfcode;
    df_error_t *df_err;

    /* if new filter equals old one, do nothing unless told to do so */
    /* XXX - The text can be the same without compiling to the same code.
     * (Macros, field references, etc.)
     */
    if (!force && strcmp(filter_new, filter_old) == 0) {
        return CF_OK;
    }

    dfcode=NULL;

    if (dftext == NULL) {
        /* The new filter is an empty filter (i.e., display all packets).
         * so leave dfcode==NULL
         */
    } else {
        /*
         * We have a filter; make a copy of it (as we'll be saving it),
         * and try to compile it.
         */
        dftext = g_strdup(dftext);
        if (!dfilter_compile(dftext, &dfcode, &df_err)) {
            /* The attempt failed; report an error. */
            simple_message_box(ESD_TYPE_ERROR, NULL,
                    "See the help for a description of the display filter syntax.",
                    "\"%s\" isn't a valid display filter: %s",
                    dftext, df_err->msg);
            df_error_free(&df_err);
            g_free(dftext);
            return CF_ERROR;
        }

        /* Was it empty? */
        if (dfcode == NULL) {
            /* Yes - free the filter text, and set it to null. */
            g_free(dftext);
            dftext = NULL;
        }
    }

    /* We have a valid filter.  Replace the current filter. */
    g_free(cf->dfilter);
    cf->dfilter = dftext;

    /* We'll recompile this when the rescan starts, or in cf_read()
     * if no file is open currently. However, if no file is open and
     * we start a new capture, we want to use this rather than
     * recompiling in cf_continue_tail() */
    dfilter_free(cf->dfcode);
    cf->dfcode = dfcode;

    /* Now rescan the packet list, applying the new filter, but not
     * throwing away information constructed on a previous pass.
     * If a dissection is already in progress, queue it.
     */
    if (cf->redissection_queued == RESCAN_NONE) {
        if (cf->read_lock) {
            cf->redissection_queued = RESCAN_SCAN;
        } else if (cf->state != FILE_CLOSED) {
            if (dftext == NULL) {
                rescan_packets(cf, "Resetting", "filter", false);
            } else {
                rescan_packets(cf, "Filtering", dftext, false);
            }
        }
    }

    return CF_OK;
}

void
cf_redissect_packets(capture_file *cf)
{
    if (cf->read_lock || cf->redissection_queued == RESCAN_SCAN) {
        /* Dissection in progress, signal redissection rather than rescanning. That
         * would destroy the current (in-progress) dissection in "cf_read" which
         * will cause issues when "cf_read" tries to add packets to the list.
         * If a previous rescan was requested, "upgrade" it to a full redissection.
         */
        cf->redissection_queued = RESCAN_REDISSECT;
    }
    if (cf->redissection_queued != RESCAN_NONE) {
        /* Redissection is (already) queued, wait for "cf_read" to finish. */
        /* XXX - what if whatever set and later clears read_lock is *not*
         * cf_read, e.g. process_specified_records ? We need to handle a
         * queued redissection there too like we do in cf_read.
         */
        return;
    }

    if (cf->state != FILE_CLOSED) {
        /* Restart dissection in case no cf_read is pending. */
        rescan_packets(cf, "Reprocessing", "all packets", true);
    }
}

bool
cf_read_record(capture_file *cf, const frame_data *fdata,
        wtap_rec *rec, Buffer *buf)
{
    int    err;
    char *err_info;

    if (!wtap_seek_read(cf->provider.wth, fdata->file_off, rec, buf, &err, &err_info)) {
        cfile_read_failure_alert_box(cf->filename, err, err_info);
        return false;
    }
    return true;
}

bool
cf_read_record_no_alert(capture_file *cf, const frame_data *fdata,
        wtap_rec *rec, Buffer *buf)
{
    int    err;
    char *err_info;

    if (!wtap_seek_read(cf->provider.wth, fdata->file_off, rec, buf, &err, &err_info)) {
        g_free(err_info);
        return false;
    }
    return true;
}

bool
cf_read_current_record(capture_file *cf)
{
    return cf_read_record(cf, cf->current_frame, &cf->rec, &cf->buf);
}

/* Rescan the list of packets, reconstructing the CList.

   "action" describes why we're doing this; it's used in the progress
   dialog box.

   "action_item" describes what we're doing; it's used in the progress
   dialog box.

   "redissect" is true if we need to make the dissectors reconstruct
   any state information they have (because a preference that affects
   some dissector has changed, meaning some dissector might construct
   its state differently from the way it was constructed the last time). */
static void
rescan_packets(capture_file *cf, const char *action, const char *action_item, bool redissect)
{
    /* Rescan packets new packet list */
    uint32_t    framenum;
    frame_data *fdata;
    wtap_rec    rec;
    Buffer      buf;
    progdlg_t  *progbar = NULL;
    GTimer     *prog_timer = g_timer_new();
    int         count;
    frame_data *selected_frame, *preceding_frame, *following_frame, *prev_frame;
    int         selected_frame_num, preceding_frame_num, following_frame_num, prev_frame_num;
    bool        selected_frame_seen;
    float       progbar_val;
    int64_t     start_time;
    char        status_str[100];
    epan_dissect_t  edt;
    dfilter_t  *dfcode = NULL;
    column_info *cinfo;
    bool        create_proto_tree;
    bool        filtering_tap_listeners = false;
    unsigned    tap_flags;
    bool        add_to_packet_list = false;
    bool        compiled _U_;
    uint32_t    frames_count;
    rescan_type queued_rescan_type = RESCAN_NONE;

    if (cf->state == FILE_CLOSED || cf->state == FILE_READ_PENDING) {
        return;
    }

    /* Rescan in progress, clear pending actions. */
    cf->redissection_queued = RESCAN_NONE;
    ws_assert(!cf->read_lock);
    cf->read_lock = true;

    wtap_rec_init(&rec);
    ws_buffer_init(&buf, 1514);

    /* Compile the current display filter.
     * The code it compiles to might have changed, e.g. if a display
     * filter macro used has changed.
     *
     * We assume this will not fail since cf->dfilter is only set in
     * cf_filter IFF the filter was valid.
     * XXX - This is not necessarily true, if the filter has a FT_IPv4
     * or FT_IPv6 field compared to a resolved hostname in it, because
     * we do a new host lookup, and that *could* timeout this time
     * (though with the read lock above we shouldn't have many lookups at
     * once, reducing the chances of that)... (#19612)
     */
    if (cf->dfilter) {
        compiled = dfilter_compile(cf->dfilter, &dfcode, NULL);
        ws_assert(compiled && dfcode);
    }

    dfilter_free(cf->dfcode);
    cf->dfcode = dfcode;

    /* Do we have any tap listeners with filters? */
    filtering_tap_listeners = have_filtering_tap_listeners();

    /* Update references in filters (if any) for the protocol
     * tree corresponding to the currently selected frame in the GUI. */
    if (cf->edt != NULL && cf->edt->tree != NULL) {
        if (cf->dfcode)
            dfilter_load_field_references(cf->dfcode, cf->edt->tree);
        if (filtering_tap_listeners)
            tap_listeners_load_field_references(cf->edt);
    }

    if (cf->dfcode != NULL) {
        dfilter_log_full(LOG_DOMAIN_DFILTER, LOG_LEVEL_NOISY, NULL, -1, NULL,
                        cf->dfcode, "Rescanning packets with display filter");
    }

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    /* If the display filter or any tap listeners require the columns,
     * construct them. */
    cinfo = (tap_listeners_require_columns() ||
        dfilter_requires_columns(cf->dfcode)) ? &cf->cinfo : NULL;

    /*
     * Determine whether we need to create a protocol tree.
     * We do if:
     *
     *    we're going to apply a display filter;
     *
     *    one of the tap listeners is going to apply a filter;
     *
     *    one of the tap listeners requires a protocol tree;
     *
     *    we're redissecting and a postdissector wants field
     *    values or protocols on the first pass.
     */
    create_proto_tree =
        (cf->dfcode != NULL || filtering_tap_listeners ||
         (tap_flags & TL_REQUIRES_PROTO_TREE) ||
         (redissect && postdissectors_want_hfids()));

    reset_tap_listeners();
    /* Which frame, if any, is the currently selected frame?
       XXX - should the selected frame or the focus frame be the "current"
       frame, that frame being the one from which "Find Frame" searches
       start? */
    selected_frame = cf->current_frame;

    /* Mark frame num as not found */
    selected_frame_num = -1;

    /* Freeze the packet list while we redo it, so we don't get any
       screen updates while it happens. */
    packet_list_freeze();

    if (redissect) {
        /* We need to re-initialize all the state information that protocols
           keep, because some preference that controls a dissector has changed,
           which might cause the state information to be constructed differently
           by that dissector. */

        /* We might receive new packets while redissecting, and we don't
           want to dissect those before their time. */
        cf->redissecting = true;

        /* 'reset' dissection session */
        epan_free(cf->epan);
        if (cf->edt && cf->edt->pi.fd) {
            /* All pointers in "per frame proto data" for the currently selected
               packet are allocated in wmem_file_scope() and deallocated in epan_free().
               Free them here to avoid unintended usage in packet_list_clear(). */
            frame_data_destroy(cf->edt->pi.fd);
        }
        cf->epan = ws_epan_new(cf);
        cf->cinfo.epan = cf->epan;

        /* A new Lua tap listener may be registered in lua_prime_all_fields()
           called via epan_new() / init_dissection() when reloading Lua plugins. */
        if (!create_proto_tree && have_filtering_tap_listeners()) {
            create_proto_tree = true;
        }
        if (!cinfo && tap_listeners_require_columns()) {
            cinfo = &cf->cinfo;
        }

        /* We need to redissect the packets so we have to discard our old
         * packet list store. */
        packet_list_clear();
        add_to_packet_list = true;
    }

    /* We don't yet know which will be the first and last frames displayed. */
    cf->first_displayed = 0;
    cf->last_displayed = 0;

    /* We currently don't display any packets */
    cf->displayed_count = 0;

    /* Iterate through the list of frames.  Call a routine for each frame
       to check whether it should be displayed and, if so, add it to
       the display list. */
    cf->provider.ref = NULL;
    cf->provider.prev_dis = NULL;
    cf->provider.prev_cap = NULL;
    cf->cum_bytes = 0;

    cf_callback_invoke(cf_cb_file_rescan_started, cf);

    g_timer_start(prog_timer);
    /* Count of packets at which we've looked. */
    count = 0;
    /* Progress so far. */
    progbar_val = 0.0f;

    cf->stop_flag = false;
    start_time = g_get_monotonic_time();

    /* no previous row yet */
    prev_frame_num = -1;
    prev_frame = NULL;

    preceding_frame_num = -1;
    preceding_frame = NULL;
    following_frame_num = -1;
    following_frame = NULL;

    selected_frame_seen = false;

    frames_count = cf->count;

    epan_dissect_init(&edt, cf->epan, create_proto_tree, false);

    if (redissect) {
        /*
         * Decryption secrets and name resolution blocks are read while
         * sequentially processing records and then passed to the dissector.
         * During redissection, the previous information is lost (see epan_free
         * above), but they are not read again from the file as only packet
         * records are re-read. Therefore reset the wtap secrets and name
         * resolution callbacks such that wtap resupplies the callbacks with
         * previously read information.
         */
        wtap_set_cb_new_ipv4(cf->provider.wth, add_ipv4_name);
        wtap_set_cb_new_ipv6(cf->provider.wth, (wtap_new_ipv6_callback_t) add_ipv6_name);
        wtap_set_cb_new_secrets(cf->provider.wth, secrets_wtap_callback);
    }

    for (framenum = 1; framenum <= frames_count; framenum++) {
        fdata = frame_data_sequence_find(cf->provider.frames, framenum);

        /* Create the progress bar if necessary.
           We check on every iteration of the loop, so that it takes no
           longer than the standard time to create it (otherwise, for a
           large file, we might take considerably longer than that standard
           time in order to get to the next progress bar step). */
        if (progbar == NULL)
            progbar = delayed_create_progress_dlg(cf->window, action, action_item, true,
                    &cf->stop_flag,
                    progbar_val);

        /*
         * Update the progress bar, but do it only after PROGBAR_UPDATE_INTERVAL
         * has elapsed. Calling update_progress_dlg and packets_bar_update will
         * likely trigger UI paint events, which might take a while depending on
         * the platform and display. Reset our timer *after* painting.
         */
        if (g_timer_elapsed(prog_timer, NULL) > PROGBAR_UPDATE_INTERVAL) {
            /* let's not divide by zero. I should never be started
             * with count == 0, so let's assert that
             */
            ws_assert(cf->count > 0);
            progbar_val = (float) count / frames_count;

            if (progbar != NULL) {
                snprintf(status_str, sizeof(status_str),
                        "%4u of %u frames", count, frames_count);
                update_progress_dlg(progbar, progbar_val, status_str);
            }

            g_timer_start(prog_timer);
        }

        queued_rescan_type = cf->redissection_queued;
        if (queued_rescan_type != RESCAN_NONE) {
            /* A redissection was requested while an existing redissection was
             * pending. */
            break;
        }

        if (cf->stop_flag) {
            /* Well, the user decided to abort the filtering.  Just stop.

               XXX - go back to the previous filter?  Users probably just
               want not to wait for a filtering operation to finish;
               unless we cancel by having no filter, reverting to the
               previous filter will probably be even more expensive than
               continuing the filtering, as it involves going back to the
               beginning and filtering, and even with no filter we currently
               have to re-generate the entire clist, which is also expensive.

               I'm not sure what Network Monitor does, but it doesn't appear
               to give you an unfiltered display if you cancel. */
            break;
        }

        count++;

        if (redissect) {
            /* Since all state for the frame was destroyed, mark the frame
             * as not visited, free the GSList referring to the state
             * data (the per-frame data itself was freed by
             * "init_dissection()"), and null out the GSList pointer. */
            frame_data_reset(fdata);
            frames_count = cf->count;
        }

        /* Frame dependencies from the previous dissection/filtering are no longer valid. */
        fdata->dependent_of_displayed = 0;

        if (!cf_read_record(cf, fdata, &rec, &buf))
            break; /* error reading the frame */

        /* If the previous frame is displayed, and we haven't yet seen the
           selected frame, remember that frame - it's the closest one we've
           yet seen before the selected frame. */
        if (prev_frame_num != -1 && !selected_frame_seen && prev_frame->passed_dfilter) {
            preceding_frame_num = prev_frame_num;
            preceding_frame = prev_frame;
        }

        add_packet_to_packet_list(fdata, cf, &edt, cf->dfcode,
                cinfo, &rec, &buf,
                add_to_packet_list);

        /* If this frame is displayed, and this is the first frame we've
           seen displayed after the selected frame, remember this frame -
           it's the closest one we've yet seen at or after the selected
           frame. */
        if (fdata->passed_dfilter && selected_frame_seen && following_frame_num == -1) {
            following_frame_num = fdata->num;
            following_frame = fdata;
        }
        if (fdata == selected_frame) {
            selected_frame_seen = true;
            if (fdata->passed_dfilter)
                selected_frame_num = fdata->num;
        }

        /* Remember this frame - it'll be the previous frame
           on the next pass through the loop. */
        prev_frame_num = fdata->num;
        prev_frame = fdata;
        wtap_rec_reset(&rec);
    }

    epan_dissect_cleanup(&edt);
    wtap_rec_cleanup(&rec);
    ws_buffer_free(&buf);

    /* We are done redissecting the packet list. */
    cf->redissecting = false;

    if (redissect) {
        frames_count = cf->count;
        /* Clear out what remains of the visited flags and per-frame data
           pointers.

           XXX - that may cause various forms of bogosity when dissecting
           these frames, as they won't have been seen by this sequential
           pass, but the only alternative I see is to keep scanning them
           even though the user requested that the scan stop, and that
           would leave the user stuck with an Wireshark grinding on
           until it finishes.  Should we just stick them with that? */
        for (; framenum <= frames_count; framenum++) {
            fdata = frame_data_sequence_find(cf->provider.frames, framenum);
            frame_data_reset(fdata);
        }
    }

    /* We're done filtering the packets; destroy the progress bar if it
       was created. */
    if (progbar != NULL)
        destroy_progress_dlg(progbar);
    g_timer_destroy(prog_timer);

    /* Unfreeze the packet list. */
    if (!add_to_packet_list)
        packet_list_recreate_visible_rows();

    /* Compute the time it took to filter the file */
    compute_elapsed(cf, start_time);

    packet_list_thaw();

    /* It is safe again to execute redissections or sort. */
    ws_assert(cf->read_lock);
    cf->read_lock = false;

    cf_callback_invoke(cf_cb_file_rescan_finished, cf);

    if (selected_frame_num == -1) {
        /* The selected frame didn't pass the filter. */
        if (selected_frame == NULL) {
            /* That's because there *was* no selected frame.  Make the first
               displayed frame the current frame. */
            selected_frame_num = 0;
        } else {
            /* Find the nearest displayed frame to the selected frame (whether
               it's before or after that frame) and make that the current frame.
               If the next and previous displayed frames are equidistant from the
               selected frame, choose the next one. */
            ws_assert(following_frame == NULL ||
                    following_frame->num >= selected_frame->num);
            ws_assert(preceding_frame == NULL ||
                    preceding_frame->num <= selected_frame->num);
            if (following_frame == NULL) {
                /* No frame after the selected frame passed the filter, so we
                   have to select the last displayed frame before the selected
                   frame. */
                selected_frame_num = preceding_frame_num;
                selected_frame = preceding_frame;
            } else if (preceding_frame == NULL) {
                /* No frame before the selected frame passed the filter, so we
                   have to select the first displayed frame after the selected
                   frame. */
                selected_frame_num = following_frame_num;
                selected_frame = following_frame;
            } else {
                /* Frames before and after the selected frame passed the filter, so
                   we'll select the previous frame */
                selected_frame_num = preceding_frame_num;
                selected_frame = preceding_frame;
            }
        }
    }

    if (selected_frame_num == -1) {
        /* There are no frames displayed at all. */
        cf_unselect_packet(cf);
    } else {
        /* Either the frame that was selected passed the filter, or we've
           found the nearest displayed frame to that frame.  Select it, make
           it the focus row, and make it visible. */
        /* Set to invalid to force update of packet list and packet details */
        if (selected_frame_num == 0) {
            packet_list_select_row_from_data(NULL);
        }else{
            if (!packet_list_select_row_from_data(selected_frame)) {
                /* We didn't find a row corresponding to this frame.
                   This means that the frame isn't being displayed currently,
                   so we can't select it. */
                simple_message_box(ESD_TYPE_INFO, NULL,
                        "The capture file is probably not fully dissected.",
                        "End of capture exceeded.");
            }
        }
    }

    /* If another rescan (due to dfilter change) or redissection (due to profile
     * change) was requested, the rescan above is aborted and restarted here. */
    if (queued_rescan_type != RESCAN_NONE) {
        redissect = redissect || queued_rescan_type == RESCAN_REDISSECT;
        rescan_packets(cf, "Reprocessing", "all packets", redissect);
    }
}


/*
 * Scan through all frame data and recalculate the ref time
 * without rereading the file.
 * XXX - do we need a progress bar or is this fast enough?
 */
void
cf_reftime_packets(capture_file* cf)
{
    uint32_t    framenum;
    frame_data *fdata;
    nstime_t rel_ts;

    cf->provider.ref = NULL;
    cf->provider.prev_dis = NULL;
    cf->cum_bytes = 0;

    for (framenum = 1; framenum <= cf->count; framenum++) {
        fdata = frame_data_sequence_find(cf->provider.frames, framenum);

        /* just add some value here until we know if it is being displayed or not */
        fdata->cum_bytes = cf->cum_bytes + fdata->pkt_len;

        /*
         * Timestamps
         */

        if (fdata->has_ts) {
            /* If we don't have the time stamp of the first packet in the
               capture, it's because this is the first packet.  Save the time
               stamp of this packet as the time stamp of the first packet. */
            if (cf->provider.ref == NULL)
                cf->provider.ref = fdata;
            /* if this frames is marked as a reference time frame, reset
               firstsec and firstusec to this frame */
            if (fdata->ref_time)
                cf->provider.ref = fdata;

            /* Get the time elapsed between the first packet and this one. */
            fdata->frame_ref_num = (fdata != cf->provider.ref) ? cf->provider.ref->num : 0;
            nstime_delta(&rel_ts, &fdata->abs_ts, &cf->provider.ref->abs_ts);

            /* If it's greater than the current elapsed time, set the elapsed
               time to it (we check for "greater than" so as not to be
               confused by time moving backwards). */
            if ((int32_t)cf->elapsed_time.secs < rel_ts.secs
                    || ((int32_t)cf->elapsed_time.secs == rel_ts.secs && (int32_t)cf->elapsed_time.nsecs < rel_ts.nsecs)) {
                cf->elapsed_time = rel_ts;
            }

            /* If this frame is displayed, get the time elapsed between the
               previous displayed packet and this packet. */
            /* XXX: What if in the future we want to use the previously
             * displayed frame for something else, too? Then we'd want
             * to store this frame as prev_dis even if it doesn't have a
             * timestamp. */
            if ( fdata->passed_dfilter ) {
                /* If we don't have the time stamp of the previous displayed
                   packet, it's because this is the first displayed packet.
                   Save the time stamp of this packet as the time stamp of
                   the previous displayed packet. */
                if (cf->provider.prev_dis == NULL) {
                    cf->provider.prev_dis = fdata;
                }

                fdata->prev_dis_num = cf->provider.prev_dis->num;
                cf->provider.prev_dis = fdata;
            }
        } else {
            /* If this frame doesn't have a timestamp, don't calculate
               anything with relative times. */
            /* However, if this frame is marked as a reference time frame,
               clear the reference frame so that the next frame with a
               timestamp becomes the reference frame. */
            if (fdata->ref_time) {
                cf->provider.ref = NULL;
            }
        }

        /*
         * Byte counts
         */
        if ( (fdata->passed_dfilter) || (fdata->ref_time) ) {
            /* This frame either passed the display filter list or is marked as
               a time reference frame.  All time reference frames are displayed
               even if they don't pass the display filter */
            if (fdata->ref_time) {
                /* if this was a TIME REF frame we should reset the cum_bytes field */
                cf->cum_bytes = fdata->pkt_len;
                fdata->cum_bytes = cf->cum_bytes;
            } else {
                /* increase cum_bytes with this packets length */
                cf->cum_bytes += fdata->pkt_len;
            }
        }
    }
}

typedef enum {
    PSP_FINISHED,
    PSP_STOPPED,
    PSP_FAILED
} psp_return_t;

static psp_return_t
process_specified_records(capture_file *cf, packet_range_t *range,
        const char *string1, const char *string2, bool terminate_is_stop,
        bool (*callback)(capture_file *, frame_data *,
            wtap_rec *, Buffer *, void *),
        void *callback_args,
        bool show_progress_bar)
{
    uint32_t         framenum;
    frame_data      *fdata;
    wtap_rec         rec;
    Buffer           buf;
    psp_return_t     ret     = PSP_FINISHED;

    progdlg_t       *progbar = NULL;
    GTimer          *prog_timer = g_timer_new();
    int              progbar_count;
    float            progbar_val;
    char             progbar_status_str[100];
    range_process_e  process_this;

    wtap_rec_init(&rec);
    ws_buffer_init(&buf, 1514);

    g_timer_start(prog_timer);
    /* Count of packets at which we've looked. */
    progbar_count = 0;
    /* Progress so far. */
    progbar_val = 0.0f;

    /* XXX - It should be ok to have multiple readers, so long as nothing
     * frees the epan context, e.g. rescan_packets with redissect true,
     * or anything that closes the file (including reload and certain forms
     * of saving.) This is mostly to stop cf_save_records but should probably
     * be handled by callers in order to allow multiple readers (e.g.,
     * restarting taps after adding or changing one.) We should probably
     * make this a real reader-writer lock.
     */
    if (cf->read_lock) {
        ws_warning("Failing due to nested process_specified_records(\"%s\") call!", cf->filename);
        return PSP_FAILED;
    }
    cf->read_lock = true;

    cf->stop_flag = false;

    if (range != NULL)
        packet_range_process_init(range);

    /* Iterate through all the packets, printing the packets that
       were selected by the current display filter.  */
    for (framenum = 1; framenum <= cf->count; framenum++) {
        fdata = frame_data_sequence_find(cf->provider.frames, framenum);

        /* Create the progress bar if necessary.
           We check on every iteration of the loop, so that it takes no
           longer than the standard time to create it (otherwise, for a
           large file, we might take considerably longer than that standard
           time in order to get to the next progress bar step). */
        if (show_progress_bar && progbar == NULL)
            progbar = delayed_create_progress_dlg(cf->window, string1, string2,
                    terminate_is_stop,
                    &cf->stop_flag,
                    progbar_val);

        /*
         * Update the progress bar, but do it only after PROGBAR_UPDATE_INTERVAL
         * has elapsed. Calling update_progress_dlg and packets_bar_update will
         * likely trigger UI paint events, which might take a while depending on
         * the platform and display. Reset our timer *after* painting.
         */
        if (progbar && g_timer_elapsed(prog_timer, NULL) > PROGBAR_UPDATE_INTERVAL) {
            /* let's not divide by zero. I should never be started
             * with count == 0, so let's assert that
             */
            ws_assert(cf->count > 0);
            progbar_val = (float) progbar_count / cf->count;

            snprintf(progbar_status_str, sizeof(progbar_status_str),
                    "%4u of %u packets", progbar_count, cf->count);
            update_progress_dlg(progbar, progbar_val, progbar_status_str);

            g_timer_start(prog_timer);
        }

        if (cf->stop_flag) {
            /* Well, the user decided to abort the operation.  Just stop,
               and arrange to return PSP_STOPPED to our caller, so they know
               it was stopped explicitly. */
            ret = PSP_STOPPED;
            break;
        }

        progbar_count++;

        if (range != NULL) {
            /* do we have to process this packet? */
            process_this = packet_range_process_packet(range, fdata);
            if (process_this == range_process_next) {
                /* this packet uninteresting, continue with next one */
                continue;
            } else if (process_this == range_processing_finished) {
                /* all interesting packets processed, stop the loop */
                break;
            }
        }

        /* Get the packet */
        if (!cf_read_record(cf, fdata, &rec, &buf)) {
            /* Attempt to get the packet failed. */
            ret = PSP_FAILED;
            break;
        }
        /* Process the packet */
        if (!callback(cf, fdata, &rec, &buf, callback_args)) {
            /* Callback failed.  We assume it reported the error appropriately. */
            ret = PSP_FAILED;
            break;
        }
        wtap_rec_reset(&rec);
    }

    /* We're done printing the packets; destroy the progress bar if
       it was created. */
    if (progbar != NULL)
        destroy_progress_dlg(progbar);
    g_timer_destroy(prog_timer);

    ws_assert(cf->read_lock);
    cf->read_lock = false;

    wtap_rec_cleanup(&rec);
    ws_buffer_free(&buf);

    return ret;
}

typedef struct {
    epan_dissect_t edt;
    column_info *cinfo;
} retap_callback_args_t;

static bool
retap_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec, Buffer *buf,
        void *argsp)
{
    retap_callback_args_t *args = (retap_callback_args_t *)argsp;

    epan_dissect_run_with_taps(&args->edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, args->cinfo);
    epan_dissect_reset(&args->edt);

    return true;
}

cf_read_status_t
cf_retap_packets(capture_file *cf)
{
    packet_range_t        range;
    retap_callback_args_t callback_args;
    bool                  create_proto_tree;
    bool                  filtering_tap_listeners;
    unsigned              tap_flags;
    psp_return_t          ret;

    /* Presumably the user closed the capture file. */
    if (cf == NULL) {
        return CF_READ_ABORTED;
    }

    /* XXX - If cf->read_lock is true, process_specified_records will fail
     * due to a nested call. We fail here so that we don't reset the tap
     * listeners if this tap isn't going to succeed.
     */
    if (cf->read_lock) {
        ws_warning("Failing due to nested process_specified_records(\"%s\") call!", cf->filename);
        return CF_READ_ERROR;
    }

    cf_callback_invoke(cf_cb_file_retap_started, cf);

    /* Do we have any tap listeners with filters? */
    filtering_tap_listeners = have_filtering_tap_listeners();

    /* Update references in filters (if any) for the protocol
     * tree corresponding to the currently selected frame in the GUI. */
    /* XXX - What if we *don't* have a currently selected frame in the GUI,
     * but we did the last time we loaded field references? Then they'll
     * match something instead of nothing (unless they've been recompiled).
     * Should we have a way to clear the field references even with a NULL tree?
     */
    if (cf->edt != NULL && cf->edt->tree != NULL) {
        if (filtering_tap_listeners)
            tap_listeners_load_field_references(cf->edt);
    }

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    /* If any tap listeners require the columns, construct them. */
    callback_args.cinfo = (tap_listeners_require_columns()) ? &cf->cinfo : NULL;

    /*
     * Determine whether we need to create a protocol tree.
     * We do if:
     *
     *    one of the tap listeners is going to apply a filter;
     *
     *    one of the tap listeners requires a protocol tree.
     */
    create_proto_tree =
        (filtering_tap_listeners || (tap_flags & TL_REQUIRES_PROTO_TREE));

    /* Reset the tap listeners. */
    reset_tap_listeners();
    uint32_t count = cf->count;

    epan_dissect_init(&callback_args.edt, cf->epan, create_proto_tree, false);

    /* Iterate through the list of packets, dissecting all packets and
       re-running the taps. */
    packet_range_init(&range, cf);
    packet_range_process_init(&range);

    if (cf->state == FILE_READ_IN_PROGRESS) {
        /* We're not done with the sequential read of the file and might
         * add more frames while process_specified_records is going. We
         * don't want to tap new frames twice, so limit the range to the
         * frames already here.
         *
         * cf_read sets read_lock so we don't tap in case of an offline
         * file, but cf_continue_tail and cf_finish_tail don't, and we
         * don't want them to, because tapping new packets in a live
         * capture is a common use case.
         *
         * Note that most other users of process_specified_records (saving,
         * printing) do want to process new packets, unlike taps.
         */
        if (count) {
            char* range_str = g_strdup_printf("-%u", count);
            packet_range_convert_str(&range, range_str);
            g_free(range_str);
        } else {
            /* range_t treats a missing number as meaning 1, not 0, and
             * reverses the order if backwards; thus the syntax -0 means
             * 0-1, so to only take zero packets we do this.
             */
            packet_range_convert_str(&range, "0");
        }
        range.process = range_process_user_range;
    }

    ret = process_specified_records(cf, &range, "Recalculating statistics on",
            "all packets", true, retap_packet,
            &callback_args, true);

    packet_range_cleanup(&range);
    epan_dissect_cleanup(&callback_args.edt);

    cf_callback_invoke(cf_cb_file_retap_finished, cf);

    switch (ret) {
        case PSP_FINISHED:
            /* Completed successfully. */
            return CF_READ_OK;

        case PSP_STOPPED:
            /* Well, the user decided to abort the refiltering.
               Return CF_READ_ABORTED so our caller knows they did that. */
            return CF_READ_ABORTED;

        case PSP_FAILED:
            /* Error while retapping. */
            return CF_READ_ERROR;
    }

    ws_assert_not_reached();
    return CF_READ_OK;
}

typedef struct {
    print_args_t *print_args;
    bool          print_header_line;
    char         *header_line_buf;
    int           header_line_buf_len;
    bool          print_formfeed;
    bool          print_separator;
    char         *line_buf;
    int           line_buf_len;
    int          *col_widths;
    int           num_visible_cols;
    int          *visible_cols;
    epan_dissect_t edt;
} print_callback_args_t;

static bool
print_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec, Buffer *buf,
        void *argsp)
{
    print_callback_args_t *args = (print_callback_args_t *)argsp;
    int             i;
    char           *cp;
    int             line_len;
    int             column_len;
    int             cp_off;
    char            bookmark_name[9+10+1];  /* "__frameNNNNNNNNNN__\0" */
    char            bookmark_title[6+10+1]; /* "Frame NNNNNNNNNN__\0"  */
    col_item_t*     col_item;
    const char*    col_text;

    /* Fill in the column information if we're printing the summary
       information. */
    if (args->print_args->print_summary) {
        col_custom_prime_edt(&args->edt, &cf->cinfo);
        epan_dissect_run(&args->edt, cf->cd_t, rec,
                frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
                fdata, &cf->cinfo);
        epan_dissect_fill_in_columns(&args->edt, false, true);
    } else
        epan_dissect_run(&args->edt, cf->cd_t, rec,
                frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
                fdata, NULL);

    if (args->print_formfeed) {
        if (!new_page(args->print_args->stream))
            goto fail;

        /*
         * Print another header line if we print a packet summary on the
         * new page.
         */
        if (args->print_args->print_col_headings)
            args->print_header_line = true;
    } else {
        if (args->print_separator) {
            if (!print_line(args->print_args->stream, 0, ""))
                goto fail;
        }
    }

    /*
     * We generate bookmarks, if the output format supports them.
     * The name is "__frameN__".
     */
    snprintf(bookmark_name, sizeof bookmark_name, "__frame%u__", fdata->num);

    if (args->print_args->print_summary) {
        if (!args->print_args->print_col_headings)
            args->print_header_line = false;
        if (args->print_header_line) {
            if (!print_line(args->print_args->stream, 0, args->header_line_buf))
                goto fail;
            args->print_header_line = false;  /* we might not need to print any more */
        }
        cp = &args->line_buf[0];
        line_len = 0;
        for (i = 0; i < args->num_visible_cols; i++) {
            col_item = &cf->cinfo.columns[args->visible_cols[i]];
            col_text = get_column_text(&cf->cinfo, args->visible_cols[i]);
            /* Find the length of the string for this column. */
            column_len = (int) strlen(col_text);
            if (args->col_widths[i] > column_len)
                column_len = args->col_widths[i];

            /* Make sure there's room in the line buffer for the column; if not,
               double its length. */
            line_len += column_len + 1;   /* "+1" for space */
            if (line_len > args->line_buf_len) {
                cp_off = (int) (cp - args->line_buf);
                args->line_buf_len = 2 * line_len;
                args->line_buf = (char *)g_realloc(args->line_buf, args->line_buf_len + 1);
                cp = args->line_buf + cp_off;
            }

            /* Right-justify the packet number column. */
            if (col_item->col_fmt == COL_NUMBER)
                snprintf(cp, column_len+1, "%*s", args->col_widths[i], col_text);
            else
                snprintf(cp, column_len+1, "%-*s", args->col_widths[i], col_text);
            cp += column_len;
            if (i != args->num_visible_cols - 1)
                *cp++ = ' ';
        }
        *cp = '\0';

        /*
         * Generate a bookmark, using the summary line as the title.
         */
        if (!print_bookmark(args->print_args->stream, bookmark_name,
                    args->line_buf))
            goto fail;

        if (!print_line(args->print_args->stream, 0, args->line_buf))
            goto fail;
    } else {
        /*
         * Generate a bookmark, using "Frame N" as the title, as we're not
         * printing the summary line.
         */
        snprintf(bookmark_title, sizeof bookmark_title, "Frame %u", fdata->num);
        if (!print_bookmark(args->print_args->stream, bookmark_name,
                    bookmark_title))
            goto fail;
    } /* if (print_summary) */

    if (args->print_args->print_dissections != print_dissections_none) {
        if (args->print_args->print_summary) {
            /* Separate the summary line from the tree with a blank line. */
            if (!print_line(args->print_args->stream, 0, ""))
                goto fail;
        }

        /* Print the information in that tree. */
        if (!proto_tree_print(args->print_args->print_dissections,
                    args->print_args->print_hex, &args->edt, NULL,
                    args->print_args->stream))
            goto fail;

        /* Print a blank line if we print anything after this (aka more than one packet). */
        args->print_separator = true;

        /* Print a header line if we print any more packet summaries */
        if (args->print_args->print_col_headings)
            args->print_header_line = true;
    }

    if (args->print_args->print_hex) {
        if (args->print_args->print_summary || (args->print_args->print_dissections != print_dissections_none)) {
            if (!print_line(args->print_args->stream, 0, ""))
                goto fail;
        }
        /* Print the full packet data as hex. */
        if (!print_hex_data(args->print_args->stream, &args->edt, args->print_args->hexdump_options))
            goto fail;

        /* Print a blank line if we print anything after this (aka more than one packet). */
        args->print_separator = true;

        /* Print a header line if we print any more packet summaries */
        if (args->print_args->print_col_headings)
            args->print_header_line = true;
    } /* if (args->print_args->print_dissections != print_dissections_none) */

    epan_dissect_reset(&args->edt);

    /* do we want to have a formfeed between each packet from now on? */
    if (args->print_args->print_formfeed) {
        args->print_formfeed = true;
    }

    return true;

fail:
    epan_dissect_reset(&args->edt);
    return false;
}

cf_print_status_t
cf_print_packets(capture_file *cf, print_args_t *print_args,
        bool show_progress_bar)
{
    print_callback_args_t callback_args;
    int           data_width;
    char         *cp;
    int           i, cp_off, column_len, line_len;
    int           num_visible_col = 0, last_visible_col = 0, visible_col_count;
    psp_return_t  ret;
    GList        *clp;
    fmt_data     *cfmt;
    bool          proto_tree_needed;

    callback_args.print_args = print_args;
    callback_args.print_header_line = print_args->print_col_headings;
    callback_args.header_line_buf = NULL;
    callback_args.header_line_buf_len = 256;
    callback_args.print_formfeed = false;
    callback_args.print_separator = false;
    callback_args.line_buf = NULL;
    callback_args.line_buf_len = 256;
    callback_args.col_widths = NULL;
    callback_args.num_visible_cols = 0;
    callback_args.visible_cols = NULL;

    if (!print_preamble(print_args->stream, cf->filename, get_ws_vcs_version_info())) {
        destroy_print_stream(print_args->stream);
        return CF_PRINT_WRITE_ERROR;
    }

    if (print_args->print_summary) {
        /* We're printing packet summaries.  Allocate the header line buffer
           and get the column widths. */
        callback_args.header_line_buf = (char *)g_malloc(callback_args.header_line_buf_len + 1);

        /* Find the number of visible columns and the last visible column */
        for (i = 0; i < prefs.num_cols; i++) {

            clp = g_list_nth(prefs.col_list, i);
            if (clp == NULL) /* Sanity check, Invalid column requested */
                continue;

            cfmt = (fmt_data *) clp->data;
            if (cfmt->visible) {
                num_visible_col++;
                last_visible_col = i;
            }
        }

        /* if num_visible_col is 0, we are done */
        if (num_visible_col == 0) {
            g_free(callback_args.header_line_buf);
            return CF_PRINT_OK;
        }

        /* Find the widths for each of the columns - maximum of the
           width of the title and the width of the data - and construct
           a buffer with a line containing the column titles. */
        callback_args.num_visible_cols = num_visible_col;
        callback_args.col_widths = g_new(int, num_visible_col);
        callback_args.visible_cols = g_new(int, num_visible_col);
        cp = &callback_args.header_line_buf[0];
        line_len = 0;
        visible_col_count = 0;
        for (i = 0; i < cf->cinfo.num_cols; i++) {

            clp = g_list_nth(prefs.col_list, i);
            if (clp == NULL) /* Sanity check, Invalid column requested */
                continue;

            cfmt = (fmt_data *) clp->data;
            if (cfmt->visible == false)
                continue;

            /* Save the order of visible columns */
            callback_args.visible_cols[visible_col_count] = i;

            /* Don't pad the last column. */
            if (i == last_visible_col)
                callback_args.col_widths[visible_col_count] = 0;
            else {
                callback_args.col_widths[visible_col_count] = (int) strlen(cf->cinfo.columns[i].col_title);
                data_width = get_column_char_width(get_column_format(i));
                if (data_width > callback_args.col_widths[visible_col_count])
                    callback_args.col_widths[visible_col_count] = data_width;
            }

            /* Find the length of the string for this column. */
            column_len = (int) strlen(cf->cinfo.columns[i].col_title);
            if (callback_args.col_widths[visible_col_count] > column_len)
                column_len = callback_args.col_widths[visible_col_count];

            /* Make sure there's room in the line buffer for the column; if not,
               double its length. */
            line_len += column_len + 1;   /* "+1" for space */
            if (line_len > callback_args.header_line_buf_len) {
                cp_off = (int) (cp - callback_args.header_line_buf);
                callback_args.header_line_buf_len = 2 * line_len;
                callback_args.header_line_buf = (char *)g_realloc(callback_args.header_line_buf,
                        callback_args.header_line_buf_len + 1);
                cp = callback_args.header_line_buf + cp_off;
            }

            /* Right-justify the packet number column. */
/*          if (cf->cinfo.col_fmt[i] == COL_NUMBER)
                snprintf(cp, column_len+1, "%*s", callback_args.col_widths[visible_col_count], cf->cinfo.columns[i].col_title);
            else*/
            snprintf(cp, column_len+1, "%-*s", callback_args.col_widths[visible_col_count], cf->cinfo.columns[i].col_title);
            cp += column_len;
            if (i != cf->cinfo.num_cols - 1)
                *cp++ = ' ';

            visible_col_count++;
        }
        *cp = '\0';

        /* Now start out the main line buffer with the same length as the
           header line buffer. */
        callback_args.line_buf_len = callback_args.header_line_buf_len;
        callback_args.line_buf = (char *)g_malloc(callback_args.line_buf_len + 1);
    } /* if (print_summary) */

    /* Create the protocol tree, and make it visible, if we're printing
       the dissection or the hex data.
       XXX - do we need it if we're just printing the hex data? */
    proto_tree_needed =
        callback_args.print_args->print_dissections != print_dissections_none ||
        callback_args.print_args->print_hex ||
        have_custom_cols(&cf->cinfo) || have_field_extractors();
    epan_dissect_init(&callback_args.edt, cf->epan, proto_tree_needed, proto_tree_needed);

    /* Iterate through the list of packets, printing the packets we were
       told to print. */
    ret = process_specified_records(cf, &print_args->range, "Printing",
            "selected packets", true, print_packet,
            &callback_args, show_progress_bar);
    epan_dissect_cleanup(&callback_args.edt);
    g_free(callback_args.header_line_buf);
    g_free(callback_args.line_buf);
    g_free(callback_args.col_widths);
    g_free(callback_args.visible_cols);

    switch (ret) {

        case PSP_FINISHED:
            /* Completed successfully. */
            break;

        case PSP_STOPPED:
            /* Well, the user decided to abort the printing.

               XXX - note that what got generated before they did that
               will get printed if we're piping to a print program; we'd
               have to write to a file and then hand that to the print
               program to make it actually not print anything. */
            break;

        case PSP_FAILED:
            /* Error while printing.

               XXX - note that what got generated before they did that
               will get printed if we're piping to a print program; we'd
               have to write to a file and then hand that to the print
               program to make it actually not print anything. */
            destroy_print_stream(print_args->stream);
            return CF_PRINT_WRITE_ERROR;
    }

    if (!print_finale(print_args->stream)) {
        destroy_print_stream(print_args->stream);
        return CF_PRINT_WRITE_ERROR;
    }

    if (!destroy_print_stream(print_args->stream))
        return CF_PRINT_WRITE_ERROR;

    return CF_PRINT_OK;
}

typedef struct {
    FILE *fh;
    epan_dissect_t edt;
    print_args_t *print_args;
    json_dumper jdumper;
} write_packet_callback_args_t;

static bool
write_pdml_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec,
        Buffer *buf, void *argsp)
{
    write_packet_callback_args_t *args = (write_packet_callback_args_t *)argsp;

    /* Create the protocol tree, but don't fill in the column information. */
    epan_dissect_run(&args->edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, NULL);

    /* Write out the information in that tree. */
    write_pdml_proto_tree(NULL, &args->edt, &cf->cinfo, args->fh, false);

    epan_dissect_reset(&args->edt);

    return !ferror(args->fh);
}

cf_print_status_t
cf_write_pdml_packets(capture_file *cf, print_args_t *print_args)
{
    write_packet_callback_args_t callback_args;
    FILE         *fh;
    psp_return_t  ret;

    fh = ws_fopen(print_args->file, "w");
    if (fh == NULL)
        return CF_PRINT_OPEN_ERROR; /* attempt to open destination failed */

    write_pdml_preamble(fh, cf->filename);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    callback_args.fh = fh;
    callback_args.print_args = print_args;
    epan_dissect_init(&callback_args.edt, cf->epan, true, true);

    /* Iterate through the list of packets, printing the packets we were
       told to print. */
    ret = process_specified_records(cf, &print_args->range, "Writing PDML",
            "selected packets", true,
            write_pdml_packet, &callback_args, true);

    epan_dissect_cleanup(&callback_args.edt);

    switch (ret) {

        case PSP_FINISHED:
            /* Completed successfully. */
            break;

        case PSP_STOPPED:
            /* Well, the user decided to abort the printing. */
            break;

        case PSP_FAILED:
            /* Error while printing. */
            fclose(fh);
            return CF_PRINT_WRITE_ERROR;
    }

    write_pdml_finale(fh);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    /* XXX - check for an error */
    fclose(fh);

    return CF_PRINT_OK;
}

static bool
write_psml_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec,
        Buffer *buf, void *argsp)
{
    write_packet_callback_args_t *args = (write_packet_callback_args_t *)argsp;

    /* Fill in the column information */
    col_custom_prime_edt(&args->edt, &cf->cinfo);
    epan_dissect_run(&args->edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, &cf->cinfo);
    epan_dissect_fill_in_columns(&args->edt, false, true);

    /* Write out the column information. */
    write_psml_columns(&args->edt, args->fh, false);

    epan_dissect_reset(&args->edt);

    return !ferror(args->fh);
}

cf_print_status_t
cf_write_psml_packets(capture_file *cf, print_args_t *print_args)
{
    write_packet_callback_args_t callback_args;
    FILE         *fh;
    psp_return_t  ret;

    bool proto_tree_needed;

    fh = ws_fopen(print_args->file, "w");
    if (fh == NULL)
        return CF_PRINT_OPEN_ERROR; /* attempt to open destination failed */

    write_psml_preamble(&cf->cinfo, fh);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    callback_args.fh = fh;
    callback_args.print_args = print_args;

    /* Fill in the column information, only create the protocol tree
       if having custom columns or field extractors. */
    proto_tree_needed = have_custom_cols(&cf->cinfo) || have_field_extractors();
    epan_dissect_init(&callback_args.edt, cf->epan, proto_tree_needed, proto_tree_needed);

    /* Iterate through the list of packets, printing the packets we were
       told to print. */
    ret = process_specified_records(cf, &print_args->range, "Writing PSML",
            "selected packets", true,
            write_psml_packet, &callback_args, true);

    epan_dissect_cleanup(&callback_args.edt);

    switch (ret) {

        case PSP_FINISHED:
            /* Completed successfully. */
            break;

        case PSP_STOPPED:
            /* Well, the user decided to abort the printing. */
            break;

        case PSP_FAILED:
            /* Error while printing. */
            fclose(fh);
            return CF_PRINT_WRITE_ERROR;
    }

    write_psml_finale(fh);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    /* XXX - check for an error */
    fclose(fh);

    return CF_PRINT_OK;
}

static bool
write_csv_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec,
        Buffer *buf, void *argsp)
{
    write_packet_callback_args_t *args = (write_packet_callback_args_t *)argsp;

    /* Fill in the column information */
    col_custom_prime_edt(&args->edt, &cf->cinfo);
    epan_dissect_run(&args->edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, &cf->cinfo);
    epan_dissect_fill_in_columns(&args->edt, false, true);

    /* Write out the column information. */
    write_csv_columns(&args->edt, args->fh);

    epan_dissect_reset(&args->edt);

    return !ferror(args->fh);
}

cf_print_status_t
cf_write_csv_packets(capture_file *cf, print_args_t *print_args)
{
    write_packet_callback_args_t callback_args;
    bool            proto_tree_needed;
    FILE         *fh;
    psp_return_t  ret;

    fh = ws_fopen(print_args->file, "w");
    if (fh == NULL)
        return CF_PRINT_OPEN_ERROR; /* attempt to open destination failed */

    write_csv_column_titles(&cf->cinfo, fh);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    callback_args.fh = fh;
    callback_args.print_args = print_args;

    /* only create the protocol tree if having custom columns or field extractors. */
    proto_tree_needed = have_custom_cols(&cf->cinfo) || have_field_extractors();
    epan_dissect_init(&callback_args.edt, cf->epan, proto_tree_needed, proto_tree_needed);

    /* Iterate through the list of packets, printing the packets we were
       told to print. */
    ret = process_specified_records(cf, &print_args->range, "Writing CSV",
            "selected packets", true,
            write_csv_packet, &callback_args, true);

    epan_dissect_cleanup(&callback_args.edt);

    switch (ret) {

        case PSP_FINISHED:
            /* Completed successfully. */
            break;

        case PSP_STOPPED:
            /* Well, the user decided to abort the printing. */
            break;

        case PSP_FAILED:
            /* Error while printing. */
            fclose(fh);
            return CF_PRINT_WRITE_ERROR;
    }

    /* XXX - check for an error */
    fclose(fh);

    return CF_PRINT_OK;
}

static bool
carrays_write_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec,
        Buffer *buf, void *argsp)
{
    write_packet_callback_args_t *args = (write_packet_callback_args_t *)argsp;

    epan_dissect_run(&args->edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, NULL);
    write_carrays_hex_data(fdata->num, args->fh, &args->edt);
    epan_dissect_reset(&args->edt);

    return !ferror(args->fh);
}

cf_print_status_t
cf_write_carrays_packets(capture_file *cf, print_args_t *print_args)
{
    write_packet_callback_args_t callback_args;
    FILE         *fh;
    psp_return_t  ret;

    fh = ws_fopen(print_args->file, "w");

    if (fh == NULL)
        return CF_PRINT_OPEN_ERROR; /* attempt to open destination failed */

    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    callback_args.fh = fh;
    callback_args.print_args = print_args;
    epan_dissect_init(&callback_args.edt, cf->epan, true, true);

    /* Iterate through the list of packets, printing the packets we were
       told to print. */
    ret = process_specified_records(cf, &print_args->range,
            "Writing C Arrays",
            "selected packets", true,
            carrays_write_packet, &callback_args, true);

    epan_dissect_cleanup(&callback_args.edt);

    switch (ret) {
        case PSP_FINISHED:
            /* Completed successfully. */
            break;
        case PSP_STOPPED:
            /* Well, the user decided to abort the printing. */
            break;
        case PSP_FAILED:
            /* Error while printing. */
            fclose(fh);
            return CF_PRINT_WRITE_ERROR;
    }

    fclose(fh);
    return CF_PRINT_OK;
}

static bool
write_json_packet(capture_file *cf, frame_data *fdata, wtap_rec *rec,
        Buffer *buf, void *argsp)
{
    write_packet_callback_args_t *args = (write_packet_callback_args_t *)argsp;

    /* Create the protocol tree, but don't fill in the column information. */
    epan_dissect_run(&args->edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, NULL);

    /* Write out the information in that tree. */
    write_json_proto_tree(NULL, args->print_args->print_dissections,
            args->print_args->print_hex,
            &args->edt, &cf->cinfo, proto_node_group_children_by_unique,
            &args->jdumper);

    epan_dissect_reset(&args->edt);

    return !ferror(args->fh);
}

cf_print_status_t
cf_write_json_packets(capture_file *cf, print_args_t *print_args)
{
    write_packet_callback_args_t callback_args;
    FILE         *fh;
    psp_return_t  ret;

    fh = ws_fopen(print_args->file, "w");
    if (fh == NULL)
        return CF_PRINT_OPEN_ERROR; /* attempt to open destination failed */

    callback_args.jdumper = write_json_preamble(fh);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    callback_args.fh = fh;
    callback_args.print_args = print_args;
    epan_dissect_init(&callback_args.edt, cf->epan, true, true);

    /* Iterate through the list of packets, printing the packets we were
       told to print. */
    ret = process_specified_records(cf, &print_args->range, "Writing JSON",
            "selected packets", true,
            write_json_packet, &callback_args, true);

    epan_dissect_cleanup(&callback_args.edt);

    switch (ret) {

        case PSP_FINISHED:
            /* Completed successfully. */
            break;

        case PSP_STOPPED:
            /* Well, the user decided to abort the printing. */
            break;

        case PSP_FAILED:
            /* Error while printing. */
            fclose(fh);
            return CF_PRINT_WRITE_ERROR;
    }

    write_json_finale(&callback_args.jdumper);
    if (ferror(fh)) {
        fclose(fh);
        return CF_PRINT_WRITE_ERROR;
    }

    /* XXX - check for an error */
    fclose(fh);

    return CF_PRINT_OK;
}

bool
cf_find_packet_protocol_tree(capture_file *cf, const char *string,
        search_direction dir, bool multiple)
{
    match_data mdata;

    mdata.frame_matched = false;
    mdata.halt = false;
    mdata.string = string;
    mdata.string_len = strlen(string);
    mdata.cf = cf;
    mdata.prev_finfo = cf->finfo_selected;
    if (multiple && cf->finfo_selected && cf->edt) {
        if (dir == SD_FORWARD) {
            proto_tree_children_foreach(cf->edt->tree, match_subtree_text, &mdata);
        } else {
            proto_tree_children_foreach(cf->edt->tree, match_subtree_text_reverse, &mdata);
        }
        if (mdata.frame_matched) {
            packet_list_select_finfo(mdata.finfo);
            return true;
        }
    }
    return find_packet(cf, match_protocol_tree, &mdata, dir);
}

field_info*
cf_find_string_protocol_tree(capture_file *cf, proto_tree *tree)
{
    match_data mdata;
    mdata.frame_matched = false;
    mdata.halt = false;
    mdata.string = convert_string_case(cf->sfilter, cf->case_type);
    mdata.string_len = strlen(mdata.string);
    mdata.cf = cf;
    mdata.prev_finfo = NULL;
    /* Iterate through all the nodes looking for matching text */
    if (cf->dir == SD_FORWARD) {
        proto_tree_children_foreach(tree, match_subtree_text, &mdata);
    } else {
        proto_tree_children_foreach(tree, match_subtree_text_reverse, &mdata);
    }
    g_free((char *)mdata.string);
    return mdata.frame_matched ? mdata.finfo : NULL;
}

static match_result
match_protocol_tree(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    match_data     *mdata = (match_data *)criterion;
    epan_dissect_t  edt;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    /* Construct the protocol tree, including the displayed text */
    epan_dissect_init(&edt, cf->epan, true, true);
    /* We don't need the column information */
    epan_dissect_run(&edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, NULL);

    /* Iterate through all the nodes, seeing if they have text that matches. */
    mdata->cf = cf;
    mdata->frame_matched = false;
    mdata->halt = false;
    mdata->prev_finfo = NULL;
    /* We don't care about the direction here, because we're just looking
     * for one match and we'll destroy this tree anyway. (We find the actual
     * field later in PacketList::selectionChanged().) Forwards is faster.
     */
    proto_tree_children_foreach(edt.tree, match_subtree_text, mdata);
    epan_dissect_cleanup(&edt);
    return mdata->frame_matched ? MR_MATCHED : MR_NOTMATCHED;
}

static void
match_subtree_text(proto_node *node, void *data)
{
    match_data   *mdata      = (match_data *) data;
    const char   *string     = mdata->string;
    size_t        string_len = mdata->string_len;
    capture_file *cf         = mdata->cf;
    field_info   *fi         = PNODE_FINFO(node);
    char          label_str[ITEM_LABEL_LENGTH];
    char         *label_ptr;
    size_t        label_len;
    uint32_t      i, i_restart;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* dissection with an invisible proto tree? */
    ws_assert(fi);

    if (mdata->frame_matched) {
        /* We already had a match; don't bother doing any more work. */
        return;
    }

    /* Don't match invisible entries. */
    if (proto_item_is_hidden(node))
        return;

    if (mdata->prev_finfo) {
        /* Haven't found the old match, so don't match this node. */
        if (fi == mdata->prev_finfo) {
            /* Found the old match, look for the next one after this. */
            mdata->prev_finfo = NULL;
        }
    } else {
        /* was a free format label produced? */
        if (fi->rep) {
            label_ptr = fi->rep->representation;
        } else {
            /* no, make a generic label */
            label_ptr = label_str;
            proto_item_fill_label(fi, label_str);
        }

        if (cf->regex) {
            if (ws_regex_matches(cf->regex, label_ptr)) {
                mdata->frame_matched = true;
                mdata->finfo = fi;
                return;
            }
        } else if (cf->case_type) {
            /* Case insensitive match */
            label_len = strlen(label_ptr);
            i_restart = 0;
            for (i = 0; i < label_len; i++) {
                if (i_restart == 0 && c_match == 0 && (label_len - i < string_len))
                    break;
                c_char = label_ptr[i];
                c_char = g_ascii_toupper(c_char);
                /* If c_match is non-zero, save candidate for retrying full match. */
                if (c_match > 0 && i_restart == 0 && c_char == string[0])
                    i_restart = i;
                if (c_char == string[c_match]) {
                    c_match++;
                    if (c_match == string_len) {
                        mdata->frame_matched = true;
                        mdata->finfo = fi;
                        /* No need to look further; we have a match */
                        return;
                    }
                } else if (i_restart) {
                    i = i_restart;
                    c_match = 1;
                    i_restart = 0;
                } else
                    c_match = 0;
            }
        } else if (strstr(label_ptr, string) != NULL) {
            /* Case sensitive match */
            mdata->frame_matched = true;
            mdata->finfo = fi;
            return;
        }
    }

    /* Recurse into the subtree, if it exists */
    if (node->first_child != NULL)
        proto_tree_children_foreach(node, match_subtree_text, mdata);
}

static void
match_subtree_text_reverse(proto_node *node, void *data)
{
    match_data   *mdata      = (match_data *) data;
    const char   *string     = mdata->string;
    size_t        string_len = mdata->string_len;
    capture_file *cf         = mdata->cf;
    field_info   *fi         = PNODE_FINFO(node);
    char          label_str[ITEM_LABEL_LENGTH];
    char         *label_ptr;
    size_t        label_len;
    uint32_t      i, i_restart;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* dissection with an invisible proto tree? */
    ws_assert(fi);

    /* We don't have an easy way to search backwards in the tree
     * (see also, proto_find_field_from_offset()) because we don't
     * have a previous node pointer, so we search backwards by
     * searching forwards, only stopping if we see the old match
     * (if we have one).
     */

    if (mdata->halt) {
        return;
    }

    /* Don't match invisible entries. */
    if (proto_item_is_hidden(node))
        return;

    if (mdata->prev_finfo && fi == mdata->prev_finfo) {
        /* Found the old match, use the previous match. */
        mdata->halt = true;
        return;
    }

    /* was a free format label produced? */
    if (fi->rep) {
        label_ptr = fi->rep->representation;
    } else {
        /* no, make a generic label */
        label_ptr = label_str;
        proto_item_fill_label(fi, label_str);
    }

    if (cf->regex) {
        if (ws_regex_matches(cf->regex, label_ptr)) {
            mdata->frame_matched = true;
            mdata->finfo = fi;
        }
    } else if (cf->case_type) {
        /* Case insensitive match */
        label_len = strlen(label_ptr);
        i_restart = 0;
        for (i = 0; i < label_len; i++) {
            if (i_restart == 0 && c_match == 0 && (label_len - i < string_len))
                break;
            c_char = label_ptr[i];
            c_char = g_ascii_toupper(c_char);
            /* If c_match is non-zero, save candidate for retrying full match. */
            if (c_match > 0 && i_restart == 0 && c_char == string[0])
                i_restart = i;
            if (c_char == string[c_match]) {
                c_match++;
                if (c_match == string_len) {
                    mdata->frame_matched = true;
                    mdata->finfo = fi;
                    break;
                }
            } else if (i_restart) {
                i = i_restart;
                c_match = 1;
                i_restart = 0;
            } else
                c_match = 0;
        }
    } else if (strstr(label_ptr, string) != NULL) {
        /* Case sensitive match */
        mdata->frame_matched = true;
        mdata->finfo = fi;
    }

    /* Recurse into the subtree, if it exists */
    if (node->first_child != NULL)
        proto_tree_children_foreach(node, match_subtree_text_reverse, mdata);
}

bool
cf_find_packet_summary_line(capture_file *cf, const char *string,
        search_direction dir)
{
    match_data mdata;

    mdata.string = string;
    mdata.string_len = strlen(string);
    return find_packet(cf, match_summary_line, &mdata, dir);
}

static match_result
match_summary_line(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    match_data     *mdata      = (match_data *)criterion;
    const char     *string     = mdata->string;
    size_t          string_len = mdata->string_len;
    epan_dissect_t  edt;
    const char     *info_column;
    size_t          info_column_len;
    match_result    result     = MR_NOTMATCHED;
    int             colx;
    uint32_t        i, i_restart;
    uint8_t         c_char;
    size_t          c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    /* Don't bother constructing the protocol tree */
    epan_dissect_init(&edt, cf->epan, false, false);
    /* Get the column information */
    epan_dissect_run(&edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, &cf->cinfo);

    /* Find the Info column */
    for (colx = 0; colx < cf->cinfo.num_cols; colx++) {
        if (cf->cinfo.columns[colx].fmt_matx[COL_INFO]) {
            /* Found it.  See if we match. */
            info_column = get_column_text(edt.pi.cinfo, colx);
            info_column_len = strlen(info_column);
            if (cf->regex) {
                if (ws_regex_matches(cf->regex, info_column)) {
                    result = MR_MATCHED;
                    break;
                }
            } else if (cf->case_type) {
                /* Case insensitive match */
                i_restart = 0;
                for (i = 0; i < info_column_len; i++) {
                    if (i_restart == 0 && c_match == 0 && (info_column_len - i < string_len))
                        break;
                    c_char = info_column[i];
                    c_char = g_ascii_toupper(c_char);
                    /* If c_match is non-zero, save candidate for retrying full match. */
                    if (c_match > 0 && i_restart == 0 && c_char == string[0])
                        i_restart = i;
                    if (c_char == string[c_match]) {
                        c_match++;
                        if (c_match == string_len) {
                            result = MR_MATCHED;
                            break;
                        }
                    } else if (i_restart) {
                        i = i_restart;
                        c_match = 1;
                        i_restart = 0;
                    } else
                        c_match = 0;
                }
            } else if (strstr(info_column, string) != NULL) {
                /* Case sensitive match */
                result = MR_MATCHED;
            }
            break;
        }
    }
    epan_dissect_cleanup(&edt);
    return result;
}

typedef struct {
    const uint8_t *data;
    size_t        data_len;
    ws_mempbrk_pattern *pattern;
} cbs_t;    /* "Counted byte string" */


/*
 * The current match_* routines only support ASCII case insensitivity and don't
 * convert UTF-8 inputs to UTF-16 for matching.  The UTF-16 support just
 * interleaves with \0 bytes, which works for 7 bit ASCII.
 *
 * We could modify them to use the GLib Unicode routines or the International
 * Components for Unicode library but it's not apparent that we could do so
 * without consuming a lot more CPU and memory or that searching would be
 * significantly better.
 *
 * XXX: We could test the search string to see if it's all ASCII, and if not
 * use Unicode aware routines for case insensitive searches or any UTF-16
 * search.
 */

bool
cf_find_packet_data(capture_file *cf, const uint8_t *string, size_t string_size,
        search_direction dir, bool multiple)
{
    cbs_t  info;
    uint8_t needles[3];
    ws_mempbrk_pattern pattern = {0};
    ws_match_function match_function;

    info.data = string;
    info.data_len = string_size;

    /* Regex, String or hex search? */
    if (cf->regex) {
        /* Regular Expression search */
        match_function = (cf->dir == SD_FORWARD) ? match_regex : match_regex_reverse;
    } else if (cf->string) {
        /* String search - what type of string? */
        if (cf->case_type) {
            needles[0] = string[0];
            needles[1] = g_ascii_tolower(needles[0]);
            needles[2] = '\0';
            ws_mempbrk_compile(&pattern, needles);
            info.pattern = &pattern;
            switch (cf->scs_type) {

                case SCS_NARROW_AND_WIDE:
                    match_function = (cf->dir == SD_FORWARD) ? match_narrow_and_wide_case : match_narrow_and_wide_case_reverse;
                    break;

                case SCS_NARROW:
                    match_function = (cf->dir == SD_FORWARD) ? match_narrow_case : match_narrow_case_reverse;
                    break;

                case SCS_WIDE:
                    match_function = (cf->dir == SD_FORWARD) ? match_wide_case : match_wide_case_reverse;
                    break;

                default:
                    ws_assert_not_reached();
                    return false;
            }

        } else {
            switch (cf->scs_type) {

                case SCS_NARROW_AND_WIDE:
                    match_function = (cf->dir == SD_FORWARD) ? match_narrow_and_wide : match_narrow_and_wide_reverse;
                    break;

                case SCS_NARROW:
                    /* Narrow, case-sensitive match is the same as looking
                     * for a converted hexstring. */
                    match_function = (cf->dir == SD_FORWARD) ? match_binary : match_binary_reverse;
                    break;

                case SCS_WIDE:
                    match_function = (cf->dir == SD_FORWARD) ? match_wide : match_wide_reverse;
                    break;

                default:
                    ws_assert_not_reached();
                    return false;
            }
        }
    } else {
        match_function = (cf->dir == SD_FORWARD) ? match_binary : match_binary_reverse;
    }

    if (multiple && cf->current_frame && (cf->search_pos || cf->search_len)) {
        /* Use the current frame (this will perform the equivalent of
         * cf_read_current_record() in match_function).
         */
        if (match_function(cf, cf->current_frame, &cf->rec, &cf->buf, &info)) {
            cf->search_in_progress = true;
            if (cf->edt) {
                field_info *fi = NULL;
                /* The regex match can match an empty string. */
                if (cf->search_len) {
                    fi = proto_find_field_from_offset(cf->edt->tree, cf->search_pos + cf->search_len - 1, cf->edt->tvb);
                }
                packet_list_select_finfo(fi);
            } else {
                packet_list_select_row_from_data(cf->current_frame);
            }
            cf->search_in_progress = false;
            return true;
        }
    }
    cf->search_pos = 0; /* Reset the position */
    cf->search_len = 0; /* Reset length */
    return find_packet(cf, match_function, &info, dir);
}

static match_result
match_narrow_and_wide(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    result = MR_NOTMATCHED;
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_start;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        pd += cf->search_pos + 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)memchr(pd, ascii_text[0], buf_end - pd);
        if (pd == NULL) break;
        /* Try narrow match at this start location */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = pd[i];
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
            } else {
                break;
            }
        }

        /* Now try wide match at the same start location. */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = pd[i];
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

static match_result
match_narrow_and_wide_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    result = MR_NOTMATCHED;
    /* Has to be room to hold the sought data. */
    if (textlen > fdata->cap_len) {
        return result;
    }
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_end - textlen;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte before the previous match start */
        pd = buf_start + cf->search_pos - 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)ws_memrchr(buf_start, ascii_text[0], pd - buf_start + 1);
        if (pd == NULL) break;
        /* Try narrow match at this start location */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = pd[i];
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
            } else {
                break;
            }
        }

        /* Now try wide match at the same start location. */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = pd[i];
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

/* Case insensitive match */
static match_result
match_narrow_and_wide_case(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    ws_mempbrk_pattern *pattern = info->pattern;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    ws_assert(pattern != NULL);

    result = MR_NOTMATCHED;
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_start;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        pd += cf->search_pos + 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)ws_mempbrk_exec(pd, buf_end - pd, pattern, &c_char);
        if (pd == NULL) break;
        /* Try narrow match at this start location */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
            } else {
                break;
            }
        }

        /* Now try wide match at the same start location. */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

static match_result
match_narrow_and_wide_case_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    ws_mempbrk_pattern *pattern = info->pattern;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    ws_assert(pattern != NULL);

    result = MR_NOTMATCHED;
    /* Has to be room to hold the sought data. */
    if (textlen > fdata->cap_len) {
        return result;
    }
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_end - textlen;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte before the previous match start */
        pd = buf_start + cf->search_pos - 1;
    }
    for (; pd >= buf_start; pd--) {
        pd = (uint8_t *)ws_memrpbrk_exec(buf_start, pd - buf_start + 1, pattern, &c_char);
        if (pd == NULL) break;
        /* Try narrow match at this start location */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
            } else {
                break;
            }
        }

        /* Now try wide match at the same start location. */
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

/* Case insensitive match */
static match_result
match_narrow_case(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    ws_mempbrk_pattern *pattern = info->pattern;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    ws_assert(pattern != NULL);

    result = MR_NOTMATCHED;
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_start;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        pd += cf->search_pos + 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)ws_mempbrk_exec(pd, buf_end - pd, pattern, &c_char);
        if (pd == NULL) break;
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    /* Save position and length for highlighting the field. */
                    result = MR_MATCHED;
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
            } else {
                break;
            }
        }
    }

done:
    return result;
}

static match_result
match_narrow_case_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    ws_mempbrk_pattern *pattern = info->pattern;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    ws_assert(pattern != NULL);

    result = MR_NOTMATCHED;
    /* Has to be room to hold the sought data. */
    if (textlen > fdata->cap_len) {
        return result;
    }
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_end - textlen;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte before the previous match start */
        pd = buf_start + cf->search_pos - 1;
    }
    for (; pd >= buf_start; pd--) {
        pd = (uint8_t *)ws_memrpbrk_exec(buf_start, pd - buf_start + 1, pattern, &c_char);
        if (pd == NULL) break;
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    /* Save position and length for highlighting the field. */
                    result = MR_MATCHED;
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
            } else {
                break;
            }
        }
    }

done:
    return result;
}

static match_result
match_wide(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    result = MR_NOTMATCHED;
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_start;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        pd += cf->search_pos + 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)memchr(pd, ascii_text[0], buf_end - pd);
        if (pd == NULL) break;
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = pd[i];
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

static match_result
match_wide_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    result = MR_NOTMATCHED;
    /* Has to be room to hold the sought data. */
    if (textlen > fdata->cap_len) {
        return result;
    }
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_end - textlen;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte before the previous match start */
        pd = buf_start + cf->search_pos - 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)ws_memrchr(buf_start, ascii_text[0], pd - buf_start + 1);
        if (pd == NULL) break;
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = pd[i];
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

/* Case insensitive match */
static match_result
match_wide_case(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    ws_mempbrk_pattern *pattern = info->pattern;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    ws_assert(pattern != NULL);

    result = MR_NOTMATCHED;
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_start;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        pd += cf->search_pos + 1;
    }
    for (; pd < buf_end; pd++) {
        pd = (uint8_t *)ws_mempbrk_exec(pd, buf_end - pd, pattern, &c_char);
        if (pd == NULL) break;
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

/* Case insensitive match */
static match_result
match_wide_case_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info       = (cbs_t *)criterion;
    const uint8_t *ascii_text = info->data;
    size_t        textlen    = info->data_len;
    ws_mempbrk_pattern *pattern = info->pattern;
    match_result  result;
    uint32_t      buf_len;
    uint8_t      *pd, *buf_start, *buf_end;
    uint32_t      i;
    uint8_t       c_char;
    size_t        c_match    = 0;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    ws_assert(pattern != NULL);

    result = MR_NOTMATCHED;
    /* Has to be room to hold the sought data. */
    if (textlen > fdata->cap_len) {
        return result;
    }
    buf_len = fdata->cap_len;
    buf_start = ws_buffer_start_ptr(buf);
    buf_end = buf_start + buf_len;
    pd = buf_end - textlen;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte before the previous match start */
        pd = buf_start + cf->search_pos - 1;
    }
    for (; pd >= buf_start; pd--) {
        pd = (uint8_t *)ws_memrpbrk_exec(buf_start, pd - buf_start + 1, pattern, &c_char);
        if (pd == NULL) break;
        c_match = 0;
        for (i = 0; pd + i < buf_end; i++) {
            c_char = g_ascii_toupper(pd[i]);
            if (c_char == ascii_text[c_match]) {
                c_match++;
                if (c_match == textlen) {
                    result = MR_MATCHED;
                    /* Save position and length for highlighting the field. */
                    cf->search_pos = (uint32_t)(pd - buf_start);
                    cf->search_len = (uint32_t)(i + 1);
                    goto done;
                }
                i++;
                if (pd + i >= buf_end || pd[i] != '\0') break;
            } else {
                break;
            }
        }
    }

done:
    return result;
}

static match_result
match_binary(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info        = (cbs_t *)criterion;
    size_t        datalen     = info->data_len;
    match_result  result;
    const uint8_t *pd = NULL, *buf_start;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    result = MR_NOTMATCHED;
    buf_start = ws_buffer_start_ptr(buf);
    size_t offset = 0;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        offset = cf->search_pos + 1;
    }
    if (offset < fdata->cap_len) {
        pd = ws_memmem(buf_start + offset, fdata->cap_len - offset, info->data, datalen);
    }
    if (pd != NULL) {
        result = MR_MATCHED;
        /* Save position and length for highlighting the field. */
        cf->search_pos = (uint32_t)(pd - buf_start);
        cf->search_len = (uint32_t)datalen;
    }

    return result;
}

static match_result
match_binary_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    cbs_t        *info        = (cbs_t *)criterion;
    size_t        datalen     = info->data_len;
    match_result  result;
    const uint8_t *pd = NULL, *buf_start;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    result = MR_NOTMATCHED;
    buf_start = ws_buffer_start_ptr(buf);
    /* Has to be room to hold the sought data. */
    if (datalen > fdata->cap_len) {
        return result;
    }
    pd = buf_start + fdata->cap_len - datalen;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte before the previous match start */
        pd = buf_start + cf->search_pos - 1;
    }
    for (; pd >= buf_start; pd--) {
        pd = (uint8_t *)ws_memrchr(buf_start, info->data[0], pd - buf_start + 1);
        if (pd == NULL) break;
        if (memcmp(pd, info->data, datalen) == 0) {
            result = MR_MATCHED;
            /* Save position and length for highlighting the field. */
            cf->search_pos = (uint32_t)(pd - buf_start);
            cf->search_len = (uint32_t)datalen;
            break;
        }
    }

    return result;
}

static match_result
match_regex(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion _U_)
{
    match_result  result = MR_NOTMATCHED;
    size_t result_pos[2] = {0, 0};

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    size_t offset = 0;
    if (cf->search_len || cf->search_pos) {
        /* we want to start searching one byte past the previous match start */
        offset = cf->search_pos + 1;
    }
    if (offset < fdata->cap_len) {
        if (ws_regex_matches_pos(cf->regex,
                                    (const char *)ws_buffer_start_ptr(buf),
                                    fdata->cap_len, offset,
                                    result_pos)) {
            //TODO: A chosen regex can match the empty string (zero length)
            // which doesn't make a lot of sense for searching the packet bytes.
            // Should we search with the PCRE2_NOTEMPTY option?
            //TODO: Fix cast.
            /* Save position and length for highlighting the field. */
            cf->search_pos = (uint32_t)(result_pos[0]);
            cf->search_len = (uint32_t)(result_pos[1] - result_pos[0]);
            result = MR_MATCHED;
        }
    }
    return result;
}

static match_result
match_regex_reverse(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion _U_)
{
    match_result  result = MR_NOTMATCHED;
    size_t result_pos[2] = {0, 0};

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    size_t offset = fdata->cap_len - 1;
    if (cf->search_pos) {
        /* we want to start searching one byte before the previous match */
        offset = cf->search_pos - 1;
    }
    for (; offset > 0; offset--) {
        if (ws_regex_matches_pos(cf->regex,
                                    (const char *)ws_buffer_start_ptr(buf),
                                    fdata->cap_len, offset,
                                    result_pos)) {
            //TODO: A chosen regex can match the empty string (zero length)
            // which doesn't make a lot of sense for searching the packet bytes.
            // Should we search with the PCRE2_NOTEMPTY option?
            //TODO: Fix cast.
            /* Save position and length for highlighting the field. */
            cf->search_pos = (uint32_t)(result_pos[0]);
            cf->search_len = (uint32_t)(result_pos[1] - result_pos[0]);
            result = MR_MATCHED;
            break;
        }
    }
    return result;
}

bool
cf_find_packet_dfilter(capture_file *cf, dfilter_t *sfcode,
        search_direction dir)
{
    return find_packet(cf, match_dfilter, sfcode, dir);
}

bool
cf_find_packet_dfilter_string(capture_file *cf, const char *filter,
        search_direction dir)
{
    dfilter_t *sfcode;
    bool       result;

    if (!dfilter_compile(filter, &sfcode, NULL)) {
        /*
         * XXX - this shouldn't happen, as the filter string is machine
         * generated
         */
        return false;
    }
    if (sfcode == NULL) {
        /*
         * XXX - this shouldn't happen, as the filter string is machine
         * generated.
         */
        return false;
    }
    result = find_packet(cf, match_dfilter, sfcode, dir);
    dfilter_free(sfcode);
    return result;
}

static match_result
match_dfilter(capture_file *cf, frame_data *fdata,
        wtap_rec *rec, Buffer *buf, void *criterion)
{
    dfilter_t      *sfcode = (dfilter_t *)criterion;
    epan_dissect_t  edt;
    match_result    result;

    /* Load the frame's data. */
    if (!cf_read_record(cf, fdata, rec, buf)) {
        /* Attempt to get the packet failed. */
        return MR_ERROR;
    }

    epan_dissect_init(&edt, cf->epan, true, false);
    epan_dissect_prime_with_dfilter(&edt, sfcode);
    epan_dissect_run(&edt, cf->cd_t, rec,
            frame_tvbuff_new_buffer(&cf->provider, fdata, buf),
            fdata, NULL);
    result = dfilter_apply_edt(sfcode, &edt) ? MR_MATCHED : MR_NOTMATCHED;
    epan_dissect_cleanup(&edt);
    return result;
}

bool
cf_find_packet_marked(capture_file *cf, search_direction dir)
{
    return find_packet(cf, match_marked, NULL, dir);
}

static match_result
match_marked(capture_file *cf _U_, frame_data *fdata, wtap_rec *rec _U_,
        Buffer *buf _U_, void *criterion _U_)
{
    return fdata->marked ? MR_MATCHED : MR_NOTMATCHED;
}

bool
cf_find_packet_time_reference(capture_file *cf, search_direction dir)
{
    return find_packet(cf, match_time_reference, NULL, dir);
}

static match_result
match_time_reference(capture_file *cf _U_, frame_data *fdata, wtap_rec *rec _U_,
        Buffer *buf _U_, void *criterion _U_)
{
    return fdata->ref_time ? MR_MATCHED : MR_NOTMATCHED;
}

static bool
find_packet(capture_file *cf, ws_match_function match_function,
        void *criterion, search_direction dir)
{
    frame_data  *start_fd;
    uint32_t     framenum;
    uint32_t     prev_framenum;
    frame_data  *fdata;
    wtap_rec     rec;
    Buffer       buf;
    frame_data  *new_fd = NULL;
    progdlg_t   *progbar = NULL;
    GTimer      *prog_timer = g_timer_new();
    int          count;
    bool         succeeded;
    float        progbar_val;
    char         status_str[100];
    match_result result;

    wtap_rec_init(&rec);
    ws_buffer_init(&buf, 1514);

    start_fd = cf->current_frame;
    if (start_fd != NULL)  {
        prev_framenum = start_fd->num;
    } else {
        prev_framenum = 0;  /* No start packet selected. */
    }

    /* Iterate through the list of packets, starting at the packet we've
       picked, calling a routine to run the filter on the packet, see if
       it matches, and stop if so.  */
    count = 0;
    framenum = prev_framenum;

    g_timer_start(prog_timer);
    /* Progress so far. */
    progbar_val = 0.0f;

    cf->stop_flag = false;

    for (;;) {
        /* Create the progress bar if necessary.
           We check on every iteration of the loop, so that it takes no
           longer than the standard time to create it (otherwise, for a
           large file, we might take considerably longer than that standard
           time in order to get to the next progress bar step). */
        if (progbar == NULL)
            progbar = delayed_create_progress_dlg(cf->window, NULL, NULL,
                    false, &cf->stop_flag, progbar_val);

        /*
         * Update the progress bar, but do it only after PROGBAR_UPDATE_INTERVAL
         * has elapsed. Calling update_progress_dlg and packets_bar_update will
         * likely trigger UI paint events, which might take a while depending on
         * the platform and display. Reset our timer *after* painting.
         */
        if (g_timer_elapsed(prog_timer, NULL) > PROGBAR_UPDATE_INTERVAL) {
            /* let's not divide by zero. I should never be started
             * with count == 0, so let's assert that
             */
            ws_assert(cf->count > 0);

            progbar_val = (float) count / cf->count;

            snprintf(status_str, sizeof(status_str),
                    "%4u of %u packets", count, cf->count);
            update_progress_dlg(progbar, progbar_val, status_str);

            g_timer_start(prog_timer);
        }

        if (cf->stop_flag) {
            /* Well, the user decided to abort the search.  Go back to the
               frame where we started. */
            new_fd = start_fd;
            break;
        }

        /* Go past the current frame. */
        if (dir == SD_BACKWARD) {
            /* Go on to the previous frame. */
            if (framenum <= 1) {
                /*
                 * XXX - other apps have a bit more of a detailed message
                 * for this, and instead of offering "OK" and "Cancel",
                 * they offer things such as "Continue" and "Cancel";
                 * we need an API for popping up alert boxes with
                 * {Verb} and "Cancel".
                 */

                if (prefs.gui_find_wrap) {
                    statusbar_push_temporary_msg("Search reached the beginning. Continuing at end.");
                    framenum = cf->count;     /* wrap around */
                } else {
                    statusbar_push_temporary_msg("Search reached the beginning.");
                    framenum = prev_framenum; /* stay on previous packet */
                }
            } else
                framenum--;
        } else {
            /* Go on to the next frame. */
            if (framenum == cf->count) {
                if (prefs.gui_find_wrap) {
                    statusbar_push_temporary_msg("Search reached the end. Continuing at beginning.");
                    framenum = 1;             /* wrap around */
                } else {
                    statusbar_push_temporary_msg("Search reached the end.");
                    framenum = prev_framenum; /* stay on previous packet */
                }
            } else
                framenum++;
        }

        fdata = frame_data_sequence_find(cf->provider.frames, framenum);
        count++;

        /* Is this packet in the display? */
        if (fdata && fdata->passed_dfilter) {
            /* Yes.  Does it match the search criterion? */
            result = (*match_function)(cf, fdata, &rec, &buf, criterion);
            if (result == MR_ERROR) {
                /* Error; our caller has reported the error.  Go back to the frame
                   where we started. */
                new_fd = start_fd;
                break;
            } else if (result == MR_MATCHED) {
                /* Yes.  Go to the new frame. */
                new_fd = fdata;
                break;
            }
            wtap_rec_reset(&rec);
        }

        if (fdata == start_fd) {
            /* We're back to the frame we were on originally, and that frame
               doesn't match the search filter.  The search failed. */
            break;
        }
    }

    /* We're done scanning the packets; destroy the progress bar if it
       was created. */
    if (progbar != NULL)
        destroy_progress_dlg(progbar);
    g_timer_destroy(prog_timer);

    if (new_fd != NULL) {
        /* We found a frame that's displayed and that matches.
           Try to find and select the packet summary list row for that frame. */
        bool found_row;

        cf->search_in_progress = true;
        found_row = packet_list_select_row_from_data(new_fd);
        cf->search_in_progress = false;
        if (!found_row) {
            /* We didn't find a row corresponding to this frame.
               This means that the frame isn't being displayed currently,
               so we can't select it. */
            cf->search_pos = 0; /* Reset the position */
            cf->search_len = 0; /* Reset length */
            simple_message_box(ESD_TYPE_INFO, NULL,
                    "The capture file is probably not fully dissected.",
                    "End of capture exceeded.");
            succeeded = false; /* The search succeeded but we didn't find the row */
        } else
            succeeded = true; /* The search succeeded and we found the row */
    } else
        succeeded = false;   /* The search failed */
    wtap_rec_cleanup(&rec);
    ws_buffer_free(&buf);
    return succeeded;
}

bool
cf_goto_frame(capture_file *cf, unsigned fnumber, bool exact)
{
    frame_data *fdata;

    if (cf == NULL || cf->provider.frames == NULL) {
        /* we don't have a loaded capture file - fix for bugs 11810 & 11989 */
        statusbar_push_temporary_msg("There is no file loaded");
        return false;   /* we failed to go to that packet */
    }

    fdata = frame_data_sequence_find(cf->provider.frames, fnumber);

    if (fdata == NULL) {
        /* we didn't find a packet with that packet number */
        statusbar_push_temporary_msg("There is no packet number %u.", fnumber);
        return false;   /* we failed to go to that packet */
    }
    if (!fdata->passed_dfilter) {
        /* that packet currently isn't displayed */
        /* XXX - add it to the set of displayed packets? */
        if (cf->first_displayed == 0 || exact) {
            /* We only want that exact frame, or no frames are displayed. */
            statusbar_push_temporary_msg("Packet number %u isn't displayed.", fnumber);
            return false;   /* we failed to go to that packet */
        }
        if (fdata->prev_dis_num == 0) {
            /* There is no previous displayed frame, so this frame is
             * before the first displayed frame. Go to the first line,
             * which is the closest frame.
             */
            fdata = NULL; /* This will select the first row. */
            statusbar_push_temporary_msg("Packet number %u isn't displayed, going to the first displayed packet, %u.", fnumber, cf->first_displayed);
        } else {
            uint32_t delta = fnumber - fdata->prev_dis_num;
            /* The next displayed frame might be closer, we can do an
             * O(log n) binary search for the earliest displayed frame
             * in the open interval (fnumber, fnumber + delta).
             *
             * This is possibly overkill, we could just go to the previous
             * displayed frame.
             */
            frame_data *fdata2;
            uint32_t lower_bound = fnumber + 1;
            uint32_t upper_bound = fnumber + delta - 1;
            bool found = false;
            while (lower_bound <= upper_bound) {
                uint32_t middle = (lower_bound + upper_bound) / 2;
                fdata2 = frame_data_sequence_find(cf->provider.frames, middle);
                if (fdata2 == NULL) {
                    /* We don't have a frame of that number, so search before it. */
                    upper_bound = middle - 1;
                    continue;
                }
                /* We have a frame of that number. What's the displayed
                 * frame before it? */
                if (fdata2->prev_dis_num > fnumber) {
                    /* The previous frame that passed the filter is also after
                     * our target, so our answer is no later than that.
                     */
                    upper_bound = fdata2->prev_dis_num;
                } else {
                    /* The previous displayed frame is before fnumber.
                     * (We already know fnumber itself is not displayed.)
                     * Is this frame itself displayed?
                     */
                    if (fdata2->passed_dfilter) {
                        /* Yes. So this is our answer. */
                        found = true;
                        break;
                    }
                    /* No. So our answer, if any, is after this frame. */
                    lower_bound = middle + 1;
                }
            }
            if (found) {
                fdata = fdata2;
                statusbar_push_temporary_msg("Packet number %u isn't displayed, going to the next displayed packet, %u.", fnumber, fdata->num);
            } else {
                statusbar_push_temporary_msg("Packet number %u isn't displayed, going to the previous displayed packet, %u.", fnumber, fdata->prev_dis_num);
                fdata = frame_data_sequence_find(cf->provider.frames, fdata->prev_dis_num);
            }
        }
    }

    if (!packet_list_select_row_from_data(fdata)) {
        /* We didn't find a row corresponding to this frame.
           This means that the frame isn't being displayed currently,
           so we can't select it. */
        simple_message_box(ESD_TYPE_INFO, NULL,
                "The capture file is probably not fully dissected.",
                "End of capture exceeded.");
        return false;
    }
    return true;  /* we got to that packet */
}

/*
 * Go to frame specified by currently selected protocol tree item.
 */
bool
cf_goto_framenum(capture_file *cf)
{
    const header_field_info *hfinfo;
    uint32_t           framenum;

    if (cf->finfo_selected) {
        hfinfo = cf->finfo_selected->hfinfo;
        ws_assert(hfinfo);
        if (hfinfo->type == FT_FRAMENUM) {
            framenum = fvalue_get_uinteger(cf->finfo_selected->value);
            if (framenum != 0) {
                /* We probably only want to go to the exact match,
                 * even though "Go to Previous Packet in History" exists.
                 */
                return cf_goto_frame(cf, framenum, true);
            }
        }
    }

    return false;
}

/* Select the packet on a given row. */
void
cf_select_packet(capture_file *cf, frame_data *fdata)
{
    epan_dissect_t *old_edt;

    /* check the frame data struct pointer for this frame */
    if (fdata == NULL) {
        return;
    }

    /* Get the data in that frame. */
    if (!cf_read_record(cf, fdata, &cf->rec, &cf->buf)) {
        return;
    }

    /* Record that this frame is the current frame. */
    cf->current_frame = fdata;

    /*
     * The change to defer freeing the current epan_dissect_t was in
     * commit a2bb94c3b33d53f42534aceb7cc67aab1d1fb1f9; to quote
     * that commit's comment:
     *
     *   Clear GtkTreeStore before freeing edt
     *
     *   When building current data for packet details treeview we store two
     *   things.
     *      - Generated string with item label
     *      - Pointer to node field_info structure
     *
     *   After epan_dissect_{free, cleanup} pointer to field_info node is no
     *   longer valid so we should clear GtkTreeStore before freeing.
     *
     * XXX - we're no longer using GTK+; is there a way to ensure that
     * *nothing* refers to any of the current frame information before
     * we replace it?
     */
    old_edt = cf->edt;
    /* Create the logical protocol tree. */
    /* We don't need the columns here. */
    cf->edt = epan_dissect_new(cf->epan, true, true);

    tap_build_interesting(cf->edt);
    epan_dissect_run(cf->edt, cf->cd_t, &cf->rec,
            frame_tvbuff_new_buffer(&cf->provider, cf->current_frame, &cf->buf),
            cf->current_frame, NULL);

    if (old_edt != NULL)
        epan_dissect_free(old_edt);
}

/* Unselect the selected packet, if any. */
void
cf_unselect_packet(capture_file *cf)
{
    epan_dissect_t *old_edt = cf->edt;

    /*
     * See the comment in cf_select_packet() about deferring the freeing
     * of the old cf->edt.
     */
    cf->edt = NULL;

    /* No packet is selected. */
    cf->current_frame = NULL;

    /* Destroy the epan_dissect_t for the unselected packet. */
    if (old_edt != NULL)
        epan_dissect_free(old_edt);
}

/*
 * Mark a particular frame.
 */
void
cf_mark_frame(capture_file *cf, frame_data *frame)
{
    if (! frame->marked) {
        frame->marked = true;
        if (cf->count > cf->marked_count)
            cf->marked_count++;
    }
}

/*
 * Unmark a particular frame.
 */
void
cf_unmark_frame(capture_file *cf, frame_data *frame)
{
    if (frame->marked) {
        frame->marked = false;
        if (cf->marked_count > 0)
            cf->marked_count--;
    }
}

/*
 * Ignore a particular frame.
 */
void
cf_ignore_frame(capture_file *cf, frame_data *frame)
{
    if (! frame->ignored) {
        frame->ignored = true;
        if (cf->count > cf->ignored_count)
            cf->ignored_count++;
    }
}

/*
 * Un-ignore a particular frame.
 */
void
cf_unignore_frame(capture_file *cf, frame_data *frame)
{
    if (frame->ignored) {
        frame->ignored = false;
        if (cf->ignored_count > 0)
            cf->ignored_count--;
    }
}

/*
 * Modify the section comment.
 */
void
cf_update_section_comment(capture_file *cf, char *comment)
{
    wtap_block_t shb_inf;
    char *shb_comment;

    /* Get the first SHB. */
    /* XXX - support multiple SHBs */
    shb_inf = wtap_file_get_shb(cf->provider.wth, 0);

    /* Get the first comment from the SHB. */
    /* XXX - support multiple comments */
    if (wtap_block_get_nth_string_option_value(shb_inf, OPT_COMMENT, 0, &shb_comment) != WTAP_OPTTYPE_SUCCESS) {
        /* There's no comment - add one. */
        wtap_block_add_string_option(shb_inf, OPT_COMMENT, comment, strlen(comment));
    } else {
        /* See if the comment has changed or not */
        if (strcmp(shb_comment, comment) == 0) {
            g_free(comment);
            return;
        }

        /* The comment has changed, let's update it */
        wtap_block_set_nth_string_option_value(shb_inf, OPT_COMMENT, 0, comment, strlen(comment));
    }
    /* Mark the file as having unsaved changes */
    cf->unsaved_changes = true;
}

/*
 * Modify the section comments for a given section.
 */
void
cf_update_section_comments(capture_file *cf, unsigned shb_idx, char **comments)
{
    wtap_block_t shb_inf;
    char *shb_comment;

    shb_inf = wtap_file_get_shb(cf->provider.wth, shb_idx);
    if (shb_inf == NULL) {
        /* Shouldn't happen. XXX: Report it if it does? */
        return;
    }

    unsigned n_comments = g_strv_length(comments);
    unsigned i;
    char* comment;

    for (i = 0; i < n_comments; i++) {
        comment = comments[i];
        if (wtap_block_get_nth_string_option_value(shb_inf, OPT_COMMENT, i, &shb_comment) != WTAP_OPTTYPE_SUCCESS) {
            /* There's no comment - add one. */
            wtap_block_add_string_option_owned(shb_inf, OPT_COMMENT, comment);
            cf->unsaved_changes = true;
        } else {
            /* See if the comment has changed or not */
            if (strcmp(shb_comment, comment) != 0) {
                /* The comment has changed, let's update it */
                wtap_block_set_nth_string_option_value(shb_inf, OPT_COMMENT, 0, comment, strlen(comment));
                cf->unsaved_changes = true;
            }
            g_free(comment);
        }
    }
    /* We either transferred ownership of the comments or freed them
     * above, so free the array of strings but not the strings themselves. */
    g_free(comments);

    /* If there are extra old comments, remove them. Start at the end. */
    for (i = wtap_block_count_option(shb_inf, OPT_COMMENT); i > n_comments; i--) {
        wtap_block_remove_nth_option_instance(shb_inf, OPT_COMMENT, i - 1);
        cf->unsaved_changes = true;
    }
}

/*
 * Get the packet block for a packet (record).
 * If the block has been edited, it returns the result of the edit,
 * otherwise it returns the block from the file.
 * NB. Caller must wtap_block_unref() the result when done.
 */
wtap_block_t
cf_get_packet_block(capture_file *cf, const frame_data *fd)
{
    /* If this block has been modified, fetch the modified version */
    if (fd->has_modified_block)
        return wtap_block_ref(cap_file_provider_get_modified_block(&cf->provider, fd));
    else {
        wtap_rec rec; /* Record metadata */
        Buffer buf;   /* Record data */
        wtap_block_t block;

        /* fetch record block */
        wtap_rec_init(&rec);
        ws_buffer_init(&buf, 1514);

        if (!cf_read_record(cf, fd, &rec, &buf))
        { /* XXX, what we can do here? */ }

        /* rec.block is owned by the record, steal it before it is gone. */
        block = wtap_block_ref(rec.block);

        wtap_rec_cleanup(&rec);
        ws_buffer_free(&buf);
        return block;
    }
}

/*
 * Update(replace) the block on a capture from a frame
 */
bool
cf_set_modified_block(capture_file *cf, frame_data *fd, const wtap_block_t new_block)
{
    wtap_block_t pkt_block = cf_get_packet_block(cf, fd);

    /* It's possible to further modify the modified block "in place" by doing
     * a call to cf_get_packet_block() that returns an already created modified
     * block, modifying that, and calling this function.
     * If the caller did that, then the block pointers will be equal.
     */
    if (pkt_block == new_block) {
        /* No need to save anything here, the caller changes went right
         * onto the block.
         * Unfortunately we don't have a way to know how many comments were
         * in the block before the caller modified it, so tell the caller
         * it is its responsibility to update the comment count.
         */
        return false;
    }
    else {
        if (pkt_block)
            cf->packet_comment_count -= wtap_block_count_option(pkt_block, OPT_COMMENT);

        if (new_block)
            cf->packet_comment_count += wtap_block_count_option(new_block, OPT_COMMENT);

        cap_file_provider_set_modified_block(&cf->provider, fd, new_block);

        expert_update_comment_count(cf->packet_comment_count);
    }

    /* Either way, we have unsaved changes. */
    wtap_block_unref(pkt_block);
    cf->unsaved_changes = true;
    return true;
}

/*
 * What types of comments does this capture file have?
 */
uint32_t
cf_comment_types(capture_file *cf)
{
    uint32_t comment_types = 0;

    /*
     * Does this file have any sections with at least one comment?
     */
    for (unsigned section_number = 0;
            section_number < wtap_file_get_num_shbs(cf->provider.wth);
            section_number++) {
        wtap_block_t shb_inf;
        char *shb_comment;

        shb_inf = wtap_file_get_shb(cf->provider.wth, section_number);

        /* Try to get the first comment from that SHB. */
        if (wtap_block_get_nth_string_option_value(shb_inf, OPT_COMMENT, 0,
                    &shb_comment) == WTAP_OPTTYPE_SUCCESS) {
            /* We succeeded, so this file has at least one section comment. */
            comment_types |= WTAP_COMMENT_PER_SECTION;

            /* We don't need to search any more. */
            break;
        }
    }
    if (cf->packet_comment_count != 0)
        comment_types |= WTAP_COMMENT_PER_PACKET;
    return comment_types;
}

/*
 * Add a resolved address to this file's list of resolved addresses.
 */
bool
cf_add_ip_name_from_string(capture_file *cf, const char *addr, const char *name)
{
    /*
     * XXX - support multiple resolved address lists, and add to the one
     * attached to this file?
     */
    if (!add_ip_name_from_string(addr, name))
        return false;

    /* OK, we have unsaved changes. */
    cf->unsaved_changes = true;
    return true;
}

typedef struct {
    wtap_dumper *pdh;
    const char  *fname;
    int          file_type;
    bool         export;
} save_callback_args_t;

/*
 * Save a capture to a file, in a particular format, saving either
 * all packets, all currently-displayed packets, or all marked packets.
 *
 * Returns true if it succeeds, false otherwise; if it fails, it pops
 * up a message box for the failure.
 */
static bool
save_record(capture_file *cf, frame_data *fdata, wtap_rec *rec,
        Buffer *buf, void *argsp)
{
    save_callback_args_t *args = (save_callback_args_t *)argsp;
    wtap_rec      new_rec;
    int           err;
    char         *err_info;
    wtap_block_t pkt_block;

    /* Copy the record information from what was read in from the file. */
    new_rec = *rec;

    /* Make changes based on anything that the user has done but that
       hasn't been saved yet. */
    if (fdata->has_modified_block)
        pkt_block = cap_file_provider_get_modified_block(&cf->provider, fdata);
    else
        pkt_block = rec->block;
    new_rec.block  = pkt_block;
    new_rec.block_was_modified = fdata->has_modified_block ? true : false;

    if (!nstime_is_zero(&fdata->shift_offset)) {
        if (new_rec.presence_flags & WTAP_HAS_TS) {
            nstime_add(&new_rec.ts, &fdata->shift_offset);
        }
    }

    /* and save the packet */
    if (!wtap_dump(args->pdh, &new_rec, ws_buffer_start_ptr(buf), &err, &err_info)) {
        cfile_write_failure_alert_box(NULL, args->fname, err, err_info, fdata->num,
                args->file_type);
        return false;
    }

    /* If we are saving (i.e., replacing the current file with the one we're
     * writing), then update the frame data to clear the shift offset.
     * This keeps us from having to re-read the entire file.
     * We could do this in rescan_file(), but
     * 1) Ideally we shouldn't have to call rescan_file if all we're doing
     * is changing the timestamps, since that shouldn't change the offsets.
     * 2) The long term goal is to try to do the offset adjustment here
     * instead of using rescan_file, which should be faster (#1257).
     *
     * If we're exporting to a different file, then don't do that.
     */
    if (!args->export && new_rec.presence_flags & WTAP_HAS_TS) {
        nstime_set_zero(&fdata->shift_offset);
    }

    return true;
}

/*
 * Can this capture file be written out in any format using Wiretap
 * rather than by copying the raw data?
 */
bool
cf_can_write_with_wiretap(capture_file *cf)
{
    /* We don't care whether we support the comments in this file or not;
       if we can't, we'll offer the user the option of discarding the
       comments. */
    return wtap_dump_can_write(cf->linktypes, 0);
}

/*
 * Should we let the user do a save?
 *
 * We should if:
 *
 *  the file has unsaved changes, and we can save it in some
 *  format through Wiretap
 *
 * or
 *
 *  the file is a temporary file and has no unsaved changes (so
 *  that "saving" it just means copying it).
 *
 * XXX - we shouldn't allow files to be edited if they can't be saved,
 * so cf->unsaved_changes should be true only if the file can be saved.
 *
 * We don't care whether we support the comments in this file or not;
 * if we can't, we'll offer the user the option of discarding the
 * comments.
 */
bool
cf_can_save(capture_file *cf)
{
    if (cf->unsaved_changes && wtap_dump_can_write(cf->linktypes, 0)) {
        /* Saved changes, and we can write it out with Wiretap. */
        return true;
    }

    if (cf->is_tempfile && !cf->unsaved_changes) {
        /*
         * Temporary file with no unsaved changes, so we can just do a
         * raw binary copy.
         */
        return true;
    }

    /* Nothing to save. */
    return false;
}

/*
 * Should we let the user do a "save as"?
 *
 * That's true if:
 *
 *  we can save it in some format through Wiretap
 *
 * or
 *
 *  the file is a temporary file and has no unsaved changes (so
 *  that "saving" it just means copying it).
 *
 * XXX - we shouldn't allow files to be edited if they can't be saved,
 * so cf->unsaved_changes should be true only if the file can be saved.
 *
 * We don't care whether we support the comments in this file or not;
 * if we can't, we'll offer the user the option of discarding the
 * comments.
 */
bool
cf_can_save_as(capture_file *cf)
{
    if (wtap_dump_can_write(cf->linktypes, 0)) {
        /* We can write it out with Wiretap. */
        return true;
    }

    if (cf->is_tempfile && !cf->unsaved_changes) {
        /*
         * Temporary file with no unsaved changes, so we can just do a
         * raw binary copy.
         */
        return true;
    }

    /* Nothing to save. */
    return false;
}

/*
 * Does this file have unsaved data?
 */
bool
cf_has_unsaved_data(capture_file *cf)
{
    /*
     * If this is a temporary file, or a file with unsaved changes, it
     * has unsaved data.
     */
    return (cf->is_tempfile && cf->count>0) || cf->unsaved_changes;
}

/*
 * Quick scan to find packet offsets.
 */
static cf_read_status_t
rescan_file(capture_file *cf, const char *fname, bool is_tempfile)
{
    wtap_rec             rec;
    Buffer               buf;
    int                  err;
    char                *err_info;
    char                *name_ptr;
    int64_t              data_offset;
    progdlg_t           *progbar        = NULL;
    GTimer              *prog_timer = g_timer_new();
    int64_t              size;
    float                progbar_val;
    int64_t              start_time;
    char                 status_str[100];
    uint32_t             framenum;
    frame_data          *fdata;

    /* Close the old handle. */
    wtap_close(cf->provider.wth);

    /* Open the new file. */
    /* XXX: this will go through all open_routines for a matching one. But right
       now rescan_file() is only used when a file is being saved to a different
       format than the original, and the user is not given a choice of which
       reader to use (only which format to save it in), so doing this makes
       sense for now. (XXX: Now it is also used when saving a changed file,
       e.g. comments or time-shifted frames.) */
    cf->provider.wth = wtap_open_offline(fname, WTAP_TYPE_AUTO, &err, &err_info, true);
    if (cf->provider.wth == NULL) {
        cfile_open_failure_alert_box(fname, err, err_info);
        return CF_READ_ERROR;
    }

    /* We're scanning a file whose contents should be the same as what
       we had before, so we don't discard dissection state etc.. */
    cf->f_datalen = 0;

    /* Set the file name because we need it to set the follow stream filter.
       XXX - is that still true?  We need it for other reasons, though,
       in any case. */
    if (cf->filename != NULL) {
        g_free(cf->filename);
    }
    cf->filename = g_strdup(fname);

    /* Indicate whether it's a permanent or temporary file. */
    cf->is_tempfile = is_tempfile;

    /* No user changes yet. */
    cf->unsaved_changes = false;

    cf->cd_t        = wtap_file_type_subtype(cf->provider.wth);
    if (cf->linktypes != NULL) {
        g_array_free(cf->linktypes, TRUE);
    }
    cf->linktypes = g_array_sized_new(FALSE, FALSE, (unsigned) sizeof(int), 1);

    cf->snap      = wtap_snapshot_length(cf->provider.wth);

    name_ptr = g_filename_display_basename(cf->filename);

    cf_callback_invoke(cf_cb_file_rescan_started, cf);

    /* Record the file's compression type.
       XXX - do we know this at open time? */
    cf->compression_type = wtap_get_compression_type(cf->provider.wth);

    /* Find the size of the file. */
    size = wtap_file_size(cf->provider.wth, NULL);

    g_timer_start(prog_timer);

    cf->stop_flag = false;
    start_time = g_get_monotonic_time();

    framenum = 0;
    wtap_rec_init(&rec);
    ws_buffer_init(&buf, 1514);
    while ((wtap_read(cf->provider.wth, &rec, &buf, &err, &err_info,
                    &data_offset))) {
        framenum++;
        fdata = frame_data_sequence_find(cf->provider.frames, framenum);
        if (G_LIKELY(fdata != NULL)) {
            fdata->file_off = data_offset;
        }
        if (size >= 0) {
            cf->f_datalen = wtap_read_so_far(cf->provider.wth);

            /* Create the progress bar if necessary. */
            if (progress_is_slow(progbar, prog_timer, size, cf->f_datalen)) {
                progbar_val = calc_progbar_val(cf, size, cf->f_datalen, status_str, sizeof(status_str));
                progbar = delayed_create_progress_dlg(cf->window, NULL, NULL,
                        true, &cf->stop_flag, progbar_val);
            }

            /*
             * Update the progress bar, but do it only after PROGBAR_UPDATE_INTERVAL
             * has elapsed. Calling update_progress_dlg and packets_bar_update will
             * likely trigger UI paint events, which might take a while depending on
             * the platform and display. Reset our timer *after* painting.
             */
            if (progbar && g_timer_elapsed(prog_timer, NULL) > PROGBAR_UPDATE_INTERVAL) {
                progbar_val = calc_progbar_val(cf, size, cf->f_datalen, status_str, sizeof(status_str));
                /* update the packet bar content on the first run or frequently on very large files */
                update_progress_dlg(progbar, progbar_val, status_str);
                compute_elapsed(cf, start_time);
                packets_bar_update();
                g_timer_start(prog_timer);
            }
        }

        if (cf->stop_flag) {
            /* Well, the user decided to abort the rescan.  Sadly, as this
               isn't a reread, recovering is difficult, so we'll just
               close the current capture. */
            break;
        }

        /* Add this packet's link-layer encapsulation type to cf->linktypes, if
           it's not already there.
           XXX - yes, this is O(N), so if every packet had a different
           link-layer encapsulation type, it'd be O(N^2) to read the file, but
           there are probably going to be a small number of encapsulation types
           in a file. */
        if (rec.rec_type == REC_TYPE_PACKET) {
            cf_add_encapsulation_type(cf, rec.rec_header.packet_header.pkt_encap);
        }
        wtap_rec_reset(&rec);
    }
    wtap_rec_cleanup(&rec);
    ws_buffer_free(&buf);

    /* Free the display name */
    g_free(name_ptr);

    /* We're done reading the file; destroy the progress bar if it was created. */
    if (progbar != NULL)
        destroy_progress_dlg(progbar);
    g_timer_destroy(prog_timer);

    /* We're done reading sequentially through the file. */
    cf->state = FILE_READ_DONE;

    /* Close the sequential I/O side, to free up memory it requires. */
    wtap_sequential_close(cf->provider.wth);

    /* compute the time it took to load the file */
    compute_elapsed(cf, start_time);

    /* Set the file encapsulation type now; we don't know what it is until
       we've looked at all the packets, as we don't know until then whether
       there's more than one type (and thus whether it's
       WTAP_ENCAP_PER_PACKET). */
    cf->lnk_t = wtap_file_encap(cf->provider.wth);

    cf_callback_invoke(cf_cb_file_rescan_finished, cf);

    if (cf->stop_flag) {
        /* Our caller will give up at this point. */
        return CF_READ_ABORTED;
    }

    if (err != 0) {
        /* Put up a message box noting that the read failed somewhere along
           the line.  Don't throw out the stuff we managed to read, though,
           if any. */
        cfile_read_failure_alert_box(NULL, err, err_info);
        return CF_READ_ERROR;
    } else
        return CF_READ_OK;
}

cf_write_status_t
cf_save_records(capture_file *cf, const char *fname, unsigned save_format,
        wtap_compression_type compression_type,
        bool discard_comments, bool dont_reopen)
{
    char            *err_info = "Unknown error";
    char            *fname_new = NULL;
    wtap_dumper     *pdh;
    frame_data      *fdata;
    addrinfo_lists_t *addr_lists;
    unsigned         framenum;
    int              err;
#ifdef _WIN32
    char            *display_basename;
#endif
    enum {
        SAVE_WITH_MOVE,
        SAVE_WITH_COPY,
        SAVE_WITH_WTAP
    }                    how_to_save;
    save_callback_args_t callback_args;
    callback_args.export = false;
    bool needs_reload = false;

    /* XXX caller should avoid saving the file while a read is pending
     * (e.g. by delaying the save action) */
    if (cf->read_lock) {
        ws_warning("cf_save_records(\"%s\") while the file is being read, potential crash ahead", fname);
    }

    cf_callback_invoke(cf_cb_file_save_started, (void *)fname);

    addr_lists = get_addrinfo_list();

    if (save_format == cf->cd_t && compression_type == cf->compression_type
            && !discard_comments && !cf->unsaved_changes
            && (wtap_addrinfo_list_empty(addr_lists) || wtap_file_type_subtype_supports_block(save_format, WTAP_BLOCK_NAME_RESOLUTION) == BLOCK_NOT_SUPPORTED)) {
        /* We're saving in the format it's already in, and we're not discarding
           comments, and there are no changes we have in memory that aren't saved
           to the file, and we have no name resolution information to write or
           the file format we're saving in doesn't support writing name
           resolution information, so we can just move or copy the raw data. */

        if (cf->is_tempfile) {
            /* The file being saved is a temporary file from a live
               capture, so it doesn't need to stay around under that name;
               first, try renaming the capture buffer file to the new name.
               This acts as a "safe save", in that, if the file already
               exists, the existing file will be removed only if the rename
               succeeds.

               Sadly, on Windows, as we have the current capture file
               open, even MoveFileEx() with MOVEFILE_REPLACE_EXISTING
               (to cause the rename to remove an existing target), as
               done by ws_stdio_rename() (ws_rename() is #defined to
               be ws_stdio_rename() on Windows) will fail.

               According to the MSDN documentation for CreateFile(), if,
               when we open a capture file, we were to directly do a CreateFile(),
               opening with FILE_SHARE_DELETE|FILE_SHARE_READ, and then
               convert it to a file descriptor with _open_osfhandle(),
               that would allow the file to be renamed out from under us.

               However, that doesn't work in practice.  Perhaps the problem
               is that the process doing the rename is the process that
               has the file open. */
#ifndef _WIN32
            if (ws_rename(cf->filename, fname) == 0) {
                /* That succeeded - there's no need to copy the source file. */
                how_to_save = SAVE_WITH_MOVE;
            } else {
                if (errno == EXDEV) {
                    /* They're on different file systems, so we have to copy the
                       file. */
                    how_to_save = SAVE_WITH_COPY;
                } else {
                    /* The rename failed, but not because they're on different
                       file systems - put up an error message.  (Or should we
                       just punt and try to copy?  The only reason why I'd
                       expect the rename to fail and the copy to succeed would
                       be if we didn't have permission to remove the file from
                       the temporary directory, and that might be fixable - but
                       is it worth requiring the user to go off and fix it?) */
                    cf_rename_failure_alert_box(fname, errno);
                    goto fail;
                }
            }
#else
            /* Windows - copy the file to its new location. */
            how_to_save = SAVE_WITH_COPY;
#endif
        } else {
            /* It's a permanent file, so we should copy it, and not remove the
               original. */
            how_to_save = SAVE_WITH_COPY;
        }

        if (how_to_save == SAVE_WITH_COPY) {
            /* Copy the file, if we haven't moved it.  If we're overwriting
               an existing file, we do it with a "safe save", by writing
               to a new file and, if the write succeeds, renaming the
               new file on top of the old file. */
            if (file_exists(fname)) {
                fname_new = ws_strdup_printf("%s~", fname);
                if (!copy_file_binary_mode(cf->filename, fname_new))
                    goto fail;
            } else {
                if (!copy_file_binary_mode(cf->filename, fname))
                    goto fail;
            }
        }
    } else {
        /* Either we're saving in a different format or we're saving changes,
           such as added, modified, or removed comments, that haven't yet
           been written to the underlying file; we can't do that by copying
           or moving the capture file, we have to do it by writing the packets
           out in Wiretap. */

        wtap_dump_params params;
        int encap;

        how_to_save = SAVE_WITH_WTAP;
        wtap_dump_params_init(&params, cf->provider.wth);

        /* Determine what file encapsulation type we should use. */
        encap = wtap_dump_required_file_encap_type(cf->linktypes);
        params.encap = encap;

        /* Use the snaplen from cf (XXX - does wtap_dump_params_init handle that?) */
        params.snaplen = cf->snap;

        if (file_exists(fname)) {
            /* We're overwriting an existing file; write out to a new file,
               and, if that succeeds, rename the new file on top of the
               old file.  That makes this a "safe save", so that we don't
               lose the old file if we have a problem writing out the new
               file.  (If the existing file is the current capture file,
               we *HAVE* to do that, otherwise we're overwriting the file
               from which we're reading the packets that we're writing!) */
            fname_new = ws_strdup_printf("%s~", fname);
            pdh = wtap_dump_open(fname_new, save_format, compression_type, &params,
                    &err, &err_info);
        } else {
            pdh = wtap_dump_open(fname, save_format, compression_type, &params,
                    &err, &err_info);
        }
        /* XXX idb_inf is documented to be used until wtap_dump_close. */
        g_free(params.idb_inf);
        params.idb_inf = NULL;

        if (pdh == NULL) {
            cfile_dump_open_failure_alert_box(fname, err, err_info, save_format);
            goto fail;
        }

        /* Add address resolution */
        wtap_dump_set_addrinfo_list(pdh, addr_lists);

        /* Iterate through the list of packets, processing all the packets. */
        callback_args.pdh = pdh;
        callback_args.fname = fname;
        callback_args.file_type = save_format;
        switch (process_specified_records(cf, NULL, "Saving", "packets",
                    true, save_record, &callback_args, true)) {

            case PSP_FINISHED:
                /* Completed successfully. */
                break;

            case PSP_STOPPED:
                /* The user decided to abort the saving.
                   If we're writing to a temporary file, remove it.
                   XXX - should we do so even if we're not writing to a
                   temporary file? */
                wtap_dump_close(pdh, NULL, &err, &err_info);
                if (fname_new != NULL)
                    ws_unlink(fname_new);
                cf_callback_invoke(cf_cb_file_save_stopped, NULL);
                wtap_dump_params_cleanup(&params);
                return CF_WRITE_ABORTED;

            case PSP_FAILED:
                /* Error while saving.
                   If we're writing to a temporary file, remove it. */
                if (fname_new != NULL)
                    ws_unlink(fname_new);
                wtap_dump_close(pdh, NULL, &err, &err_info);
                wtap_dump_params_cleanup(&params);
                goto fail;
        }

        if (!wtap_dump_close(pdh, &needs_reload, &err, &err_info)) {
            cfile_close_failure_alert_box(fname, err, err_info);
            wtap_dump_params_cleanup(&params);
            goto fail;
        }

        wtap_dump_params_cleanup(&params);
    }

    if (fname_new != NULL) {
        /* We wrote out to fname_new, and should rename it on top of
           fname.  fname_new is now closed, so that should be possible even
           on Windows.  However, on Windows, we first need to close whatever
           file descriptors we have open for fname. */
#ifdef _WIN32
        wtap_fdclose(cf->provider.wth);
#endif
        /* Now do the rename. */
        if (ws_rename(fname_new, fname) == -1) {
            /* Well, the rename failed. */
            cf_rename_failure_alert_box(fname, errno);
#ifdef _WIN32
            /* Attempt to reopen the random file descriptor using the
               current file's filename.  (At this point, the sequential
               file descriptor is closed.) */
            if (!wtap_fdreopen(cf->provider.wth, cf->filename, &err)) {
                /* Oh, well, we're screwed. */
                display_basename = g_filename_display_basename(cf->filename);
                simple_error_message_box(
                        file_open_error_message(err, false), display_basename);
                g_free(display_basename);
            }
#endif
            goto fail;
        }
        g_free(fname_new);
    }

    /* If this was a temporary file, and we didn't do the save by doing
       a move, so the tempoary file is still around under its old name,
       remove it. */
    if (cf->is_tempfile && how_to_save != SAVE_WITH_MOVE) {
        /* If this fails, there's not much we can do, so just ignore errors. */
        ws_unlink(cf->filename);
    }

    cf_callback_invoke(cf_cb_file_save_finished, NULL);
    cf->unsaved_changes = false;

    if (!dont_reopen) {
        switch (how_to_save) {

            case SAVE_WITH_MOVE:
                /* We just moved the file, so the wtap structure refers to the
                   new file, and all the information other than the filename
                   and the "is temporary" status applies to the new file; just
                   update that. */
                g_free(cf->filename);
                cf->filename = g_strdup(fname);
                cf->is_tempfile = false;
                cf_callback_invoke(cf_cb_file_fast_save_finished, cf);
                break;

            case SAVE_WITH_COPY:
                /* We just copied the file, so all the information other than
                   the file descriptors, the filename, and the "is temporary"
                   status applies to the new file; just update that. */
                wtap_fdclose(cf->provider.wth);
                /* Attempt to reopen the random file descriptor using the
                   new file's filename.  (At this point, the sequential
                   file descriptor is closed.) */
                if (!wtap_fdreopen(cf->provider.wth, fname, &err)) {
                    cfile_open_failure_alert_box(fname, err, err_info);
                    cf_close(cf);
                } else {
                    g_free(cf->filename);
                    cf->filename = g_strdup(fname);
                    cf->is_tempfile = false;
                }
                cf_callback_invoke(cf_cb_file_fast_save_finished, cf);
                break;

            case SAVE_WITH_WTAP:
                /* Open and read the file we saved to.

                   XXX - this is somewhat of a waste; we already have the
                   packets, all this gets us is updated file type information
                   (which we could just stuff into "cf"), and having the new
                   file be the one we have opened and from which we're reading
                   the data, and it means we have to spend time opening and
                   reading the file, which could be a significant amount of
                   time if the file is large.

                   If the capture-file-writing code were to return the
                   seek offset of each packet it writes, we could save that
                   in the frame_data structure for the frame, and just open
                   the file without reading it again...

                   ...as long as, for gzipped files, the process of writing
                   out the file *also* generates the information needed to
                   support fast random access to the compressed file. */
                /* rescan_file will cause us to try all open_routines, so
                   reset cfile's open_type */
                cf->open_type = WTAP_TYPE_AUTO;
                /* There are cases when SAVE_WITH_WTAP can result in new packets
                   being written to the file, e.g ERF records
                   In that case, we need to reload the whole file */
                if(needs_reload) {
                    if (cf_open(cf, fname, WTAP_TYPE_AUTO, false, &err) == CF_OK) {
                        if (cf_read(cf, /*reloading=*/true) != CF_READ_OK) {
                            /* The rescan failed; just close the file.  Either
                               a dialog was popped up for the failure, so the
                               user knows what happened, or they stopped the
                               rescan, in which case they know what happened.  */
                            /* XXX: This is inconsistent with normal open/reload behaviour. */
                            cf_close(cf);
                        }
                    }
                }
                else {
                    if (rescan_file(cf, fname, false) != CF_READ_OK) {
                        /* The rescan failed; just close the file.  Either
                           a dialog was popped up for the failure, so the
                           user knows what happened, or they stopped the
                           rescan, in which case they know what happened.  */
                        cf_close(cf);
                    }
                }
                break;
        }

        /* If we were told to discard the comments, do so. */
        if (discard_comments) {
            /* Remove SHB comment, if any. */
            wtap_write_shb_comment(cf->provider.wth, NULL);

            /* remove all user comments */
            for (framenum = 1; framenum <= cf->count; framenum++) {
                fdata = frame_data_sequence_find(cf->provider.frames, framenum);

                // XXX: This also ignores non-comment options like verdict
                fdata->has_modified_block = false;
            }

            if (cf->provider.frames_modified_blocks) {
                g_tree_destroy(cf->provider.frames_modified_blocks);
                cf->provider.frames_modified_blocks = NULL;
            }

            cf->packet_comment_count = 0;
        }
    }
    return CF_WRITE_OK;

fail:
    if (fname_new != NULL) {
        /* We were trying to write to a temporary file; get rid of it if it
           exists.  (We don't care whether this fails, as, if it fails,
           there's not much we can do about it.  I guess if it failed for
           a reason other than "it doesn't exist", we could report an
           error, so the user knows there's a junk file that they might
           want to clean up.) */
        ws_unlink(fname_new);
        g_free(fname_new);
    }
    cf_callback_invoke(cf_cb_file_save_failed, NULL);
    return CF_WRITE_ERROR;
}

cf_write_status_t
cf_export_specified_packets(capture_file *cf, const char *fname,
        packet_range_t *range, unsigned save_format,
        wtap_compression_type compression_type)
{
    char                        *fname_new = NULL;
    int                          err;
    char                        *err_info;
    wtap_dumper                 *pdh;
    save_callback_args_t         callback_args;
    wtap_dump_params             params;
    int                          encap;

    callback_args.export = true;
    packet_range_process_init(range);

    /* We're writing out specified packets from the specified capture
       file to another file.  Even if all captured packets are to be
       written, don't special-case the operation - read each packet
       and then write it out if it's one of the specified ones. */

    wtap_dump_params_init(&params, cf->provider.wth);

    /* Determine what file encapsulation type we should use. */
    encap = wtap_dump_required_file_encap_type(cf->linktypes);
    params.encap = encap;

    /* Use the snaplen from cf (XXX - does wtap_dump_params_init handle that?) */
    params.snaplen = cf->snap;

    if (file_exists(fname)) {
        /* We're overwriting an existing file; write out to a new file,
           and, if that succeeds, rename the new file on top of the
           old file.  That makes this a "safe save", so that we don't
           lose the old file if we have a problem writing out the new
           file.  (If the existing file is the current capture file,
           we *HAVE* to do that, otherwise we're overwriting the file
           from which we're reading the packets that we're writing!) */
        fname_new = ws_strdup_printf("%s~", fname);
        pdh = wtap_dump_open(fname_new, save_format, compression_type, &params,
                &err, &err_info);
    } else {
        pdh = wtap_dump_open(fname, save_format, compression_type, &params,
                &err, &err_info);
    }
    /* XXX idb_inf is documented to be used until wtap_dump_close. */
    g_free(params.idb_inf);
    params.idb_inf = NULL;

    if (pdh == NULL) {
        cfile_dump_open_failure_alert_box(fname, err, err_info, save_format);
        goto fail;
    }

    /* Add address resolution */
    wtap_dump_set_addrinfo_list(pdh, get_addrinfo_list());

    /* Iterate through the list of packets, processing the packets we were
       told to process.

       XXX - we've already called "packet_range_process_init(range)", but
       "process_specified_records()" will do it again.  Fortunately,
       that's harmless in this case, as we haven't done anything to
       "range" since we initialized it. */
    callback_args.pdh = pdh;
    callback_args.fname = fname;
    callback_args.file_type = save_format;
    switch (process_specified_records(cf, range, "Writing", "specified records",
                true, save_record, &callback_args, true)) {

        case PSP_FINISHED:
            /* Completed successfully. */
            break;

        case PSP_STOPPED:
            /* The user decided to abort the saving.
               If we're writing to a temporary file, remove it.
               XXX - should we do so even if we're not writing to a
               temporary file? */
            wtap_dump_close(pdh, NULL, &err, &err_info);
            if (fname_new != NULL) {
                ws_unlink(fname_new);
                g_free(fname_new);
            }
            wtap_dump_params_cleanup(&params);

            return CF_WRITE_ABORTED;

        case PSP_FAILED:
            /* Error while saving. */
            wtap_dump_close(pdh, NULL, &err, &err_info);
            /*
             * We don't report any error from closing; the error that caused
             * process_specified_records() to fail has already been reported.
             */
            goto fail;
    }

    if (!wtap_dump_close(pdh, NULL, &err, &err_info)) {
        cfile_close_failure_alert_box(fname, err, err_info);
        goto fail;
    }

    if (fname_new != NULL) {
        /* We wrote out to fname_new, and should rename it on top of
           fname; fname is now closed, so that should be possible even
           on Windows.  Do the rename. */
        if (ws_rename(fname_new, fname) == -1) {
            /* Well, the rename failed. */
            cf_rename_failure_alert_box(fname, errno);
            goto fail;
        }
        g_free(fname_new);
    }
    wtap_dump_params_cleanup(&params);

    return CF_WRITE_OK;

fail:
    if (fname_new != NULL) {
        /* We were trying to write to a temporary file; get rid of it if it
           exists.  (We don't care whether this fails, as, if it fails,
           there's not much we can do about it.  I guess if it failed for
           a reason other than "it doesn't exist", we could report an
           error, so the user knows there's a junk file that they might
           want to clean up.) */
        ws_unlink(fname_new);
        g_free(fname_new);
    }
    wtap_dump_params_cleanup(&params);

    return CF_WRITE_ERROR;
}

/*
 * XXX - whether we mention the source pathname, the target pathname,
 * or both depends on the error and on what we find if we look for
 * one or both of them.
 */
static void
cf_rename_failure_alert_box(const char *filename, int err)
{
    char *display_basename;

    display_basename = g_filename_display_basename(filename);
    switch (err) {

        case ENOENT:
            /* XXX - should check whether the source exists and, if not,
               report it as the problem and, if so, report the destination
               as the problem. */
            simple_error_message_box("The path to the file \"%s\" doesn't exist.",
                    display_basename);
            break;

        case EACCES:
            /* XXX - if we're doing a rename after a safe save, we should
               probably say something else. */
            simple_error_message_box("You don't have permission to move the capture file to \"%s\".",
                    display_basename);
            break;

        default:
            /* XXX - this should probably mention both the source and destination
               pathnames. */
            simple_error_message_box("The file \"%s\" could not be moved: %s.",
                    display_basename, wtap_strerror(err));
            break;
    }
    g_free(display_basename);
}

/* Reload the current capture file. */
cf_status_t
cf_reload(capture_file *cf)
{
    char     *filename;
    bool      is_tempfile;
    cf_status_t cf_status = CF_OK;
    int       err;

    if (cf->read_lock) {
        ws_warning("Failing cf_reload(\"%s\") since a read is in progress", cf->filename);
        return CF_ERROR;
    }

    /* If the file could be opened, "cf_open()" calls "cf_close()"
       to get rid of state for the old capture file before filling in state
       for the new capture file.  "cf_close()" will remove the file if
       it's a temporary file; we don't want that to happen (for one thing,
       it'd prevent subsequent reopens from working).  Remember whether it's
       a temporary file, mark it as not being a temporary file, and then
       reopen it as the type of file it was.

       Also, "cf_close()" will free "cf->filename", so we must make
       a copy of it first. */
    filename = g_strdup(cf->filename);
    is_tempfile = cf->is_tempfile;
    cf->is_tempfile = false;
    if (cf_open(cf, filename, cf->open_type, is_tempfile, &err) == CF_OK) {
        switch (cf_read(cf, /*reloading=*/true)) {

            case CF_READ_OK:
            case CF_READ_ERROR:
                /* Just because we got an error, that doesn't mean we were unable
                   to read any of the file; we handle what we could get from the
                   file. */
                break;

            case CF_READ_ABORTED:
                /* The user bailed out of re-reading the capture file; the
                   capture file has been closed. */
                break;
        }
    } else {
        /* The open failed, so "cf->is_tempfile" wasn't set to "is_tempfile".
           Instead, the file was left open, so we should restore "cf->is_tempfile"
           ourselves.

           XXX - change the menu?  Presumably "cf_open()" will do that;
           make sure it does! */
        cf->is_tempfile = is_tempfile;
        cf_status = CF_ERROR;
    }
    /* "cf_open()" made a copy of the file name we handed it, so
       we should free up our copy. */
    g_free(filename);
    return cf_status;
}
