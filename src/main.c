#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct AppConfig {
    gchar *ndi_name;
    gchar *srt_uri;     // e.g. srt://host:port?mode=caller or srt://:port?mode=listener
    gboolean with_audio;
    gchar *encoder;     // x264enc|vtenc_h264|openh264enc
    gint bitrate_kbps;
    guint gop_size;     // GOP size in frames (0 = auto)
    gchar *audio_codec; // aac|mp3|ac3|smpte302m
    gint audio_bitrate_kbps; // audio bitrate (0 = auto/default, ignored for SMPTE 302M)
    gboolean zerolatency;
    gboolean inject_sei;
    guint timeout_seconds; // 0 disables auto-exit
    gchar *dump_ts_path;   // optional mpegts dump path

    gboolean stdout_mode;  // output mpegts to stdout instead of SRT
    gchar *timestamp_mode; // ndisrc timestamp-mode (auto|timecode|timestamp|...)
    gboolean verbose;      // enable debug stderr messages
    gboolean discover;     // discover and list NDI sources
} AppConfig;

// Forward declarations
static gboolean element_has_property(GstElement *element, const gchar *prop_name);
GByteArray* build_pic_timing_sei_nal_from_au(const guint8 *annexb, gsize size, gboolean drop_frame, guint frame, guint seconds, guint minutes, guint hours);
static gint find_startcode(const guint8 *data, gint size, gint from);
static gint startcode_len_at(const guint8 *data, gint size, gint pos);
static GByteArray* ebsp_to_rbsp(const guint8 *ebsp, gsize size);
// SpsVuiInfo is defined below; forward declare parser signature after struct

// VUI/SPS info used to format pic_timing properly
typedef struct {
    gboolean vui_present;
    gboolean pic_struct_present_flag;
    gboolean cpb_dpb_delays_present_flag;
    guint cpb_removal_delay_length;
    guint dpb_output_delay_length;
    guint time_offset_length;
    gboolean timing_info_present_flag;
    guint32 num_units_in_tick;
    guint32 time_scale;
    gboolean fixed_frame_rate_flag;
} SpsVuiInfo;

// now that SpsVuiInfo is defined, forward declare parser we reference above
static gboolean parse_sps_vui_info_from_rbsp(const guint8 *rbsp, gsize size, SpsVuiInfo *out);

// Functions to parse SPS/VUI and build pic_timing accordingly
static gboolean extract_sps_vui_from_au(const guint8 *annexb, gsize size, SpsVuiInfo *out);
static GByteArray* build_pic_timing_sei_nal_from_sps(const SpsVuiInfo *info, gboolean drop_frame, guint frame, guint seconds, guint minutes, guint hours);
static GByteArray* patch_sps_pic_struct_flag_to_one(const guint8 *ebsp, gsize ebsp_size, guint8 header_byte);
static GByteArray* patch_sps_pic_struct_and_timing(const guint8 *ebsp, gsize ebsp_size, guint8 header_byte, guint fps_n, guint fps_d);

// Debug helper: parse and log SPS VUI fields from an Annex B SPS NAL
static void log_sps_vui_from_annexb(const guint8 *annexb, gsize size) {
    if (!annexb || size < 5) return;
    // find start code
    gint sc = find_startcode(annexb, (gint)size, 0);
    if (sc < 0) return;
    gint sc_len = startcode_len_at(annexb, (gint)size, sc);
    gint nal_start = sc + sc_len;
    if (nal_start + 1 >= (gint)size) return;
    // Convert EBSP to RBSP skipping header byte
    GByteArray *rbsp = ebsp_to_rbsp(annexb + nal_start + 1, size - (nal_start + 1));
    if (!rbsp) return;
    SpsVuiInfo info; gboolean ok = parse_sps_vui_info_from_rbsp(rbsp->data, rbsp->len, &info);
    if (ok) {
        g_printerr("Patched SPS VUI: pic_struct_present=%d, HRD=%d, to_len=%u, timing_info=%d, num_units_in_tick=%u, time_scale=%u, fixed_frame_rate=%d\n",
                   info.pic_struct_present_flag ? 1 : 0,
                   info.cpb_dpb_delays_present_flag ? 1 : 0,
                   info.time_offset_length,
                   info.timing_info_present_flag ? 1 : 0,
                   info.num_units_in_tick,
                   info.time_scale,
                   info.fixed_frame_rate_flag ? 1 : 0);
    }
    g_byte_array_unref(rbsp);
}

// Cache last seen SPS/VUI to format pic_timing on frames without in-band SPS
static SpsVuiInfo g_last_sps_info;
static gboolean g_last_sps_valid = FALSE;
// Cached patched SPS (Annex B EBSP) with pic_struct_present_flag forced to 1
static GByteArray *g_patched_sps_ebsp = NULL;



typedef struct SeiConfig {
    gboolean inject_sei;
    guint fps_n;
    guint fps_d;
    gboolean prefer_pts;
    gboolean verbose;      // enable debug stderr messages
    // Dynamic estimation when fps is unknown
    GstClockTime last_pts_ns;
    guint last_sec;
    guint est_fps;
} SeiConfig;

static void print_usage(const char *prog) {
    g_printerr("Usage: %s --ndi-name <name> [options]\n\n", prog);
    g_printerr("Required:\n");
    g_printerr("  --ndi-name <name>     NDI source name to connect to\n\n");
    g_printerr("Output Options:\n");
    g_printerr("  --srt-uri <uri>       SRT endpoint URI (srt://host:port?mode=caller)\n");
    g_printerr("  --stdout              Output MPEG-TS to stdout instead of SRT\n\n");
    g_printerr("Encoding Options:\n");
    g_printerr("  --encoder <name>      Video encoder: x264enc, vtenc_h264, openh264enc\n");
    g_printerr("  --bitrate <kbps>      Video bitrate in kbps (default: 6000)\n");
    g_printerr("  --gop-size <frames>   GOP size in frames (0 = auto, default: 0)\n");
    g_printerr("  --audio-codec <name>  Audio codec: aac, mp3, ac3, smpte302m (default: aac)\n");
    g_printerr("  --audio-bitrate <k>   Audio bitrate in kbps (0 = auto, ignored for smpte302m)\n\n");
    g_printerr("Behavior Options:\n");
    g_printerr("  --no-audio            Disable audio processing\n");
    g_printerr("  --zerolatency         Enable ultra-low latency mode (default: on)\n");
    g_printerr("  --no-sei              Disable SEI timecode injection\n");
    g_printerr("  --timeout <seconds>   Auto-exit after specified seconds (0 = disabled)\n");
    g_printerr("  --dump-ts <path>      Save MPEG-TS to file for debugging\n");
    g_printerr("  --timestamp-mode <m>  NDI timestamp mode: auto, timecode, timestamp, etc.\n");
    g_printerr("  --verbose             Enable debug stderr messages\n");
    g_printerr("  --discover            Discover and list available NDI sources\n");
    g_printerr("  --help, -h            Show this help message\n\n");
    g_printerr("Examples:\n");
    g_printerr("  %s --discover                                    # List available NDI sources\n", prog);
    g_printerr("  %s --ndi-name \"Camera 1\" --srt-uri \"srt://receiver:9000?mode=caller\"\n", prog);
    g_printerr("  %s --ndi-name \"Camera 1\" --stdout --gop-size 25 --bitrate 8000\n", prog);
    g_printerr("  %s --ndi-name \"Camera 1\" --stdout --audio-codec smpte302m     # SMPTE 302M audio\n", prog);
}

static gboolean parse_args(int argc, char **argv, AppConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->with_audio = TRUE;
    cfg->encoder = g_strdup("x264enc");
    cfg->bitrate_kbps = 6000;
    cfg->gop_size = 0;      // 0 = auto (default)
    cfg->audio_codec = g_strdup("aac");  // default audio codec
    cfg->audio_bitrate_kbps = 0;    // 0 = auto/default
    cfg->zerolatency = TRUE;
    cfg->inject_sei = TRUE;
    cfg->timeout_seconds = 0;
    cfg->dump_ts_path = NULL;
    cfg->stdout_mode = FALSE;
    cfg->timestamp_mode = g_strdup("timecode");
    cfg->verbose = FALSE;
    cfg->discover = FALSE;

    for (int i = 1; i < argc; ++i) {
        if (g_strcmp0(argv[i], "--ndi-name") == 0 && i + 1 < argc) {
            cfg->ndi_name = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--srt-uri") == 0 && i + 1 < argc) {
            cfg->srt_uri = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--encoder") == 0 && i + 1 < argc) {
            g_free(cfg->encoder);
            cfg->encoder = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            cfg->bitrate_kbps = atoi(argv[++i]);
        } else if (g_strcmp0(argv[i], "--gop-size") == 0 && i + 1 < argc) {
            int gop = atoi(argv[++i]);
            if (gop < 0) gop = 0;
            cfg->gop_size = (guint)gop;
        } else if (g_strcmp0(argv[i], "--audio-codec") == 0 && i + 1 < argc) {
            g_free(cfg->audio_codec);
            cfg->audio_codec = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--audio-bitrate") == 0 && i + 1 < argc) {
            cfg->audio_bitrate_kbps = atoi(argv[++i]);
        } else if (g_strcmp0(argv[i], "--no-audio") == 0) {
            cfg->with_audio = FALSE;
        } else if (g_strcmp0(argv[i], "--zerolatency") == 0) {
            cfg->zerolatency = TRUE;
        } else if (g_strcmp0(argv[i], "--no-sei") == 0) {
            cfg->inject_sei = FALSE;
        } else if (g_strcmp0(argv[i], "--timeout") == 0 && i + 1 < argc) {
            int t = atoi(argv[++i]);
            if (t < 0) t = 0;
            cfg->timeout_seconds = (guint)t;
        } else if (g_strcmp0(argv[i], "--dump-ts") == 0 && i + 1 < argc) {
            cfg->dump_ts_path = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--verbose") == 0) {
            cfg->verbose = TRUE;
        } else if (g_strcmp0(argv[i], "--discover") == 0) {
            cfg->discover = TRUE;
        } else if (g_strcmp0(argv[i], "--stdout") == 0) {
            cfg->stdout_mode = TRUE;
        } else if (g_strcmp0(argv[i], "--timestamp-mode") == 0 && i + 1 < argc) {
            g_free(cfg->timestamp_mode);
            cfg->timestamp_mode = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0) {
            return FALSE;
        } else {
            g_printerr("Unknown arg: %s\n", argv[i]);
            return FALSE;
        }
    }

    // If discover mode is enabled, don't require other parameters
    if (cfg->discover) {
        return TRUE;
    }
    
    if (!cfg->ndi_name || (!cfg->srt_uri && !cfg->stdout_mode)) {
        return FALSE;
    }

    return TRUE;
}

static gboolean bus_msg_cb(GstBus *bus, GstMessage *msg, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL; gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            g_printerr("ERROR: %s\n", err ? err->message : "(unknown)");
            if (dbg) { g_printerr("DEBUG: %s\n", dbg); g_free(dbg);} 
            if (err) g_error_free(err);
            g_main_loop_quit((GMainLoop*)user_data);
            break;
        }
        case GST_MESSAGE_EOS:
            g_main_loop_quit((GMainLoop*)user_data);
            break;
        default:
            break;
    }
    return TRUE;
}

static gboolean quit_loop_cb(gpointer user_data) {
    GMainLoop *loop = (GMainLoop*)user_data;
    if (loop) {
        g_main_loop_quit(loop);
    }
    return FALSE; // one-shot
}



static gboolean element_has_property(GstElement *element, const gchar *prop_name) {
    GObjectClass *klass = G_OBJECT_GET_CLASS(element);
    GParamSpec *pspec = g_object_class_find_property(klass, prop_name);
    return pspec != NULL;
}

static gchar* build_audio_pipeline(const gchar *audio_codec, gint audio_bitrate_kbps) {
    if (g_strcmp0(audio_codec, "aac") == 0) {
        if (audio_bitrate_kbps > 0) {
            return g_strdup_printf("audioconvert ! avenc_aac bitrate=%d", audio_bitrate_kbps * 1000);
        } else {
            return g_strdup("audioconvert ! avenc_aac");
        }
    } else if (g_strcmp0(audio_codec, "mp3") == 0) {
        if (audio_bitrate_kbps > 0) {
            return g_strdup_printf("audioconvert ! lamemp3enc bitrate=%d", audio_bitrate_kbps);
        } else {
            return g_strdup("audioconvert ! lamemp3enc");
        }
    } else if (g_strcmp0(audio_codec, "ac3") == 0) {
        if (audio_bitrate_kbps > 0) {
            return g_strdup_printf("audioconvert ! avenc_ac3 bitrate=%d", audio_bitrate_kbps * 1000);
        } else {
            return g_strdup("audioconvert ! avenc_ac3");
        }
    } else if (g_strcmp0(audio_codec, "smpte302m") == 0) {
        // SMPTE 302M doesn't use bitrate - it's uncompressed PCM wrapped (S16LE or S32LE)  
        return g_strdup("audioconvert ! audio/x-raw,format=S16LE,channels=2,rate=48000 ! avenc_s302m");
    } else {
        g_printerr("Warning: Unknown audio codec '%s', falling back to AAC\n", audio_codec);
        if (audio_bitrate_kbps > 0) {
            return g_strdup_printf("audioconvert ! avenc_aac bitrate=%d", audio_bitrate_kbps * 1000);
        } else {
            return g_strdup("audioconvert ! avenc_aac");
        }
    }
}

static void discover_ndi_sources(void) {
    g_printerr("Discovering NDI sources...\n");
    g_printerr("Note: This requires NDI SDK and GStreamer NDI plugin to be properly installed.\n\n");
    
    // Create a device monitor for NDI sources
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    
    // Add filter for NDI sources (Source/Network:application/x-ndi)
    GstCaps *caps = gst_caps_new_empty_simple("application/x-ndi");
    gst_device_monitor_add_filter(monitor, "Source/Network", caps);
    gst_caps_unref(caps);
    
    g_printerr("Scanning for NDI sources (this may take a few seconds)...\n");
    
    // Start monitoring
    if (!gst_device_monitor_start(monitor)) {
        g_printerr("ERROR: Failed to start device monitor. NDI plugin may not be available.\n");
        gst_object_unref(monitor);
        return;
    }
    
    // Wait a bit for discovery
    g_usleep(3000000); // 3 seconds
    
    // Get discovered devices
    GList *devices = gst_device_monitor_get_devices(monitor);
    
    g_printerr("\nAvailable NDI sources:\n");
    g_printerr("=====================\n");
    
    if (devices == NULL) {
        g_printerr("No NDI sources found.\n\n");
        g_printerr("Possible reasons:\n");
        g_printerr("  - No NDI sources are currently running on the network\n");
        g_printerr("  - NDI SDK or GStreamer NDI plugin not properly installed\n");
        g_printerr("  - Network firewall blocking NDI discovery\n");
        g_printerr("  - NDI sources may be in a different network segment\n\n");
        g_printerr("Common NDI source names to try manually:\n");
        g_printerr("  - \"OBS Virtual Camera\"\n");
        g_printerr("  - \"NDI Video Input\"\n");
        g_printerr("  - \"Screen Capture\"\n");
        g_printerr("  - \"[Computer Name] (NDI Signal Generator)\"\n");
        g_printerr("  - \"[Computer Name] (OBS)\"\n\n");
    } else {
        guint count = 0;
        for (GList *l = devices; l != NULL; l = l->next) {
            GstDevice *device = GST_DEVICE(l->data);
            gchar *name = gst_device_get_display_name(device);
            gchar *device_class = gst_device_get_device_class(device);
            
            g_printerr("  %u. \"%s\" (class: %s)\n", ++count, name, device_class);
            
            // Get device properties for additional info
            GstStructure *props = gst_device_get_properties(device);
            if (props) {
                const gchar *ndi_name = gst_structure_get_string(props, "ndi-name");
                const gchar *url_address = gst_structure_get_string(props, "url-address");
                if (ndi_name) {
                    g_printerr("      NDI Name: \"%s\"\n", ndi_name);
                }
                if (url_address) {
                    g_printerr("      URL Address: %s\n", url_address);
                }
                gst_structure_free(props);
            }
            
            g_free(name);
            g_free(device_class);
        }
        g_printerr("\nTo use a discovered source:\n");
        g_printerr("  ./ndi2srt --ndi-name \"Source Name\" --stdout --timeout 5\n\n");
    }
    
    // Cleanup
    g_list_free_full(devices, gst_object_unref);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    
    g_printerr("Discovery complete.\n");
}



static inline void epb_safe_append(GByteArray *arr, guint8 byte) {
    gsize n = arr->len;
    if (n >= 2) {
        guint8 b1 = arr->data[n-2];
        guint8 b2 = arr->data[n-1];
        if (b1 == 0x00 && b2 == 0x00 && byte <= 0x03) {
            g_byte_array_append(arr, (guint8[]){0x03}, 1);
        }
    }
    g_byte_array_append(arr, &byte, 1);
}

// Build Annex B NAL from RBSP and provided header byte (applies EPB)
static GByteArray* build_annexb_from_rbsp_and_header(const guint8 *rbsp, gsize size, guint8 header_byte) {
    GByteArray *out = g_byte_array_new();
    // startcode and header
    g_byte_array_append(out, (guint8[]){0x00,0x00,0x00,0x01}, 4);
    epb_safe_append(out, header_byte);
    for (gsize i = 0; i < size; ++i) {
        epb_safe_append(out, rbsp[i]);
    }
    return out;
}



static gint find_startcode(const guint8 *data, gint size, gint from) {
    for (gint i = from; i + 3 < size; ++i) {
        if (data[i] == 0x00 && data[i+1] == 0x00) {
            if (data[i+2] == 0x01) return i; // 00 00 01
            if (i + 4 < size && data[i+2] == 0x00 && data[i+3] == 0x01) return i; // 00 00 00 01
        }
    }
    return -1;
}

static gint startcode_len_at(const guint8 *data, gint size, gint pos) {
    if (pos + 3 < size && data[pos] == 0x00 && data[pos+1] == 0x00) {
        if (data[pos+2] == 0x01) return 3;
        if (pos + 4 < size && data[pos+2] == 0x00 && data[pos+3] == 0x01) return 4;
    }
    return 0;
}

static GstBuffer* prepend_h264_sei_timecode(SeiConfig *scfg, GstBuffer *inbuf) {
    guint hours = 0, minutes = 0, seconds = 0, frame = 0;
    gboolean have_tc = FALSE;
    gboolean drop_frame = FALSE;
    
    // Prefer source UTC LTC (GstVideoTimeCodeMeta) if present
    {
        GstVideoTimeCodeMeta *tcmeta = gst_buffer_get_video_time_code_meta(inbuf);
        if (tcmeta) {
            const GstVideoTimeCode *tc = &tcmeta->tc;
            hours = tc->hours;
            minutes = tc->minutes;
            seconds = tc->seconds;
            frame = tc->frames;
            // Use meta rate if available to detect drop-frame standards
            guint fpsn = scfg ? scfg->fps_n : 0;
            guint fpsd = scfg ? (scfg->fps_d ? scfg->fps_d : 1) : 1;
#ifdef GST_VIDEO_TIME_CODE_FLAG_DROP_FRAME
            if (tc->flags & GST_VIDEO_TIME_CODE_FLAG_DROP_FRAME) drop_frame = TRUE;
#else
            if ((fpsn == 30000 && fpsd == 1001) || (fpsn == 60000 && fpsd == 1001)) drop_frame = TRUE;
#endif
            have_tc = TRUE;
        }
    }
    
    // Fallback: derive from PTS (wallclock-based), if requested
    if (!have_tc && scfg && scfg->prefer_pts && GST_BUFFER_PTS_IS_VALID(inbuf)) {
        GstClockTime pts = GST_BUFFER_PTS(inbuf);
        guint64 sec_total = pts / GST_SECOND;
        hours = (sec_total / 3600ULL) % 24ULL;
        minutes = (sec_total / 60ULL) % 60ULL;
        seconds = sec_total % 60ULL;
        
        // frame number from fractional part of second
        if (scfg->fps_n > 0 && scfg->fps_d > 0) {
            guint64 num = (pts % GST_SECOND) * scfg->fps_n;
            guint64 den = (guint64)GST_SECOND * scfg->fps_d;
            frame = (guint)(num / den);
            if ((scfg->fps_n == 30000 && scfg->fps_d == 1001) || (scfg->fps_n == 60000 && scfg->fps_d == 1001)) drop_frame = TRUE;
        } else {
            if (scfg->last_pts_ns != 0) {
                GstClockTime delta = pts - scfg->last_pts_ns;
                if (delta > 0) {
                    guint est = (guint)(GST_SECOND / delta);
                    if (est >= 12 && est <= 120) scfg->est_fps = est;
                }
            }
            scfg->last_pts_ns = pts;
            guint fps = scfg->est_fps ? scfg->est_fps : 25;
            guint64 frac = pts % GST_SECOND;
            frame = (guint)((frac * fps) / GST_SECOND);
        }
        have_tc = TRUE;
    }
    
    if (!have_tc) {
        return gst_buffer_ref(inbuf);
    }
    
    // Build Picture Timing SEI based on SPS/VUI
    GByteArray *sei = NULL;
    {
        GstMapInfo spsmap;
        if (gst_buffer_map(inbuf, &spsmap, GST_MAP_READ)) {
            SpsVuiInfo info; memset(&info, 0, sizeof(info));
            if (extract_sps_vui_from_au(spsmap.data, spsmap.size, &info)) {
                // We will emit pic_timing regardless; force effective flag to 1
                info.pic_struct_present_flag = TRUE;
                // Ensure no HRD-derived fields are expected in pic_timing
                info.cpb_dpb_delays_present_flag = FALSE;
                info.cpb_removal_delay_length = 0;
                info.dpb_output_delay_length = 0;
                info.time_offset_length = 0;
                g_last_sps_info = info; g_last_sps_valid = TRUE;
                // Debug: print effective SPS flags
                if (scfg->verbose) {
                    g_printerr("SPS VUI: pic_struct_present=%d, HRD=%d, cpb_len=%u, dpb_len=%u, to_len=%u, timing_info=%d, num_units_in_tick=%u, time_scale=%u, fixed_frame_rate=%d\n",
                        info.pic_struct_present_flag, info.cpb_dpb_delays_present_flag,
                        info.cpb_removal_delay_length, info.dpb_output_delay_length, info.time_offset_length,
                        info.timing_info_present_flag ? 1 : 0,
                        info.num_units_in_tick, info.time_scale,
                        info.fixed_frame_rate_flag ? 1 : 0);
                }
                sei = build_pic_timing_sei_nal_from_sps(&info, drop_frame, frame, seconds, minutes, hours);
            }
            gst_buffer_unmap(inbuf, &spsmap);
        }
    }
    if (!sei) {
        if (g_last_sps_valid) {
            // clear HRD expectations on cached info too
            g_last_sps_info.cpb_dpb_delays_present_flag = FALSE;
            g_last_sps_info.cpb_removal_delay_length = 0;
            g_last_sps_info.dpb_output_delay_length = 0;
            g_last_sps_info.time_offset_length = 0;
            sei = build_pic_timing_sei_nal_from_sps(&g_last_sps_info, drop_frame, frame, seconds, minutes, hours);
        } else {
            // If SPS not present in this AU, emit minimal pic_timing
            SpsVuiInfo def = { FALSE, FALSE, FALSE, 0, 0, 0 };
            sei = build_pic_timing_sei_nal_from_sps(&def, drop_frame, frame, seconds, minutes, hours);
        }
    }

    // Allocate output and append original buffer data
    gsize sei_len = sei ? sei->len : 0;
    gsize in_size = gst_buffer_get_size(inbuf);
    GstMapInfo inmap;
    if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ)) {
        if (sei) g_byte_array_unref(sei);
        return gst_buffer_ref(inbuf);
    }

    // If first NAL is AUD (type 9), insert SEI after it; also scan AU for SPS/IDR
    gint pos0 = find_startcode(inmap.data, (gint)inmap.size, 0);
    gint sc_len = pos0 >= 0 ? startcode_len_at(inmap.data, (gint)inmap.size, pos0) : 0;
    gboolean insert_after_aud = FALSE;
    gint aud_end = -1;
    gboolean sps_present = FALSE;
    gboolean idr_present = FALSE;
    if (sc_len > 0) {
        // iterate nal units within this AU
        gint nal_start = pos0 + sc_len;
        while (nal_start < (gint)inmap.size) {
            guint8 nal_hdr = inmap.data[nal_start];
            guint8 nal_type = nal_hdr & 0x1F;
            // determine next NAL start
            gint next = find_startcode(inmap.data, (gint)inmap.size, nal_start + 1);
            if (next < 0) next = (gint)inmap.size;
            if (nal_type == 9 && !insert_after_aud) {
                insert_after_aud = TRUE;
                aud_end = next;
            } else if (nal_type == 7) {
                sps_present = TRUE;
                // opportunistically build patched SPS cache if not yet cached
                if (g_patched_sps_ebsp == NULL && next > nal_start + 1) {
                    guint fpsn = scfg ? (scfg->fps_n ? scfg->fps_n : (scfg->est_fps ? scfg->est_fps : 25)) : 25;
                    guint fpsd = scfg ? (scfg->fps_d ? scfg->fps_d : 1) : 1;
                    g_patched_sps_ebsp = patch_sps_pic_struct_and_timing(inmap.data + nal_start + 1,
                                                                          (gsize)(next - (nal_start + 1)), nal_hdr,
                                                                          fpsn, fpsd);
                    if (g_patched_sps_ebsp && scfg->verbose) {
                        log_sps_vui_from_annexb(g_patched_sps_ebsp->data, g_patched_sps_ebsp->len);
                    }
                }
            } else if (nal_type == 5) {
                idr_present = TRUE;
            }
            // advance
            if (next == (gint)inmap.size) break;
            // skip over next startcode length
            gint next_sc_len = startcode_len_at(inmap.data, (gint)inmap.size, next);
            nal_start = next + next_sc_len;
        }
    }
    
    // If stream does not look like Annex B, skip injection
    if (pos0 < 0) {
        gst_buffer_unmap(inbuf, &inmap);
        if (sei) g_byte_array_unref(sei);
        return gst_buffer_ref(inbuf);
    }

    // Inject patched SPS before SEI on every AU that either contains an SPS or follows an IDR
    gboolean inject_patched_sps = (g_patched_sps_ebsp && g_patched_sps_ebsp->len > 0) && (sps_present || idr_present);

    // Build the new AU dynamically for exact length
    GByteArray *out_arr = g_byte_array_new();
    gboolean sps_replaced = FALSE;
    if (insert_after_aud) {
        // copy AUD region
        g_byte_array_append(out_arr, inmap.data, aud_end);
        // If this AU contains SPS or we need to inject before IDR, ensure patched SPS comes before SEI
        gboolean want_before_sei = (g_patched_sps_ebsp && g_patched_sps_ebsp->len > 0) && (sps_present || inject_patched_sps);
        if (want_before_sei) {
            g_byte_array_append(out_arr, g_patched_sps_ebsp->data, g_patched_sps_ebsp->len);
            if (scfg->verbose) {
                g_printerr("Injected patched SPS (after AUD) tc=%02u:%02u:%02u:%02u drop=%d\n",
                           hours, minutes, seconds, frame, drop_frame ? 1 : 0);
            }
            sps_replaced = TRUE;
        }
        // insert SEI
        if (sei && sei->len > 0) {
            g_byte_array_append(out_arr, sei->data, sei_len);
            if (scfg->verbose) {
                g_printerr("Emitted pic_timing SEI (after AUD) tc=%02u:%02u:%02u:%02u drop=%d\n",
                           hours, minutes, seconds, frame, drop_frame ? 1 : 0);
            }
        } else {
            g_printerr("SEI injection disabled - using GStreamer timecode handling\n");
        }
        // Append remaining NALs skipping SPS
        gint p = aud_end;
        while (p < (gint)inmap.size) {
            gint sc = find_startcode(inmap.data, (gint)inmap.size, p);
            if (sc < 0) break;
            gint sc_len2 = startcode_len_at(inmap.data, (gint)inmap.size, sc);
            gint nal_start2 = sc + sc_len2;
            if (nal_start2 >= (gint)inmap.size) break;
            gint next2 = find_startcode(inmap.data, (gint)inmap.size, nal_start2 + 1);
            if (next2 < 0) next2 = (gint)inmap.size;
            guint8 nal_hdr2 = inmap.data[nal_start2];
            guint8 nal_type2 = nal_hdr2 & 0x1F;
            if (nal_type2 == 7) {
                if (!sps_replaced && g_patched_sps_ebsp && g_patched_sps_ebsp->len > 0) {
                    g_byte_array_append(out_arr, g_patched_sps_ebsp->data, g_patched_sps_ebsp->len);
                    sps_replaced = TRUE;
                }
                // else skip original SPS
            } else if (nal_type2 == 6) {
                // Skip original SEI NAL units to avoid duplication with our injected SEI
                // else skip original SEI
            } else {
                g_byte_array_append(out_arr, inmap.data + sc, (guint)(next2 - sc));
            }
            p = next2;
        }
    } else {
        // Prepend patched SPS (if any) and SEI, then original AU skipping SPS
        gboolean want_before_sei = (g_patched_sps_ebsp && g_patched_sps_ebsp->len > 0) && (sps_present || inject_patched_sps);
        if (want_before_sei) {
            g_byte_array_append(out_arr, g_patched_sps_ebsp->data, g_patched_sps_ebsp->len);
            if (scfg->verbose) {
                g_printerr("Injected patched SPS (prepend) tc=%02u:%02u:%02u:%02u drop=%d\n",
                           hours, minutes, seconds, frame, drop_frame ? 1 : 0);
            }
            sps_replaced = TRUE;
        }
        if (sei && sei->len > 0) {
            g_byte_array_append(out_arr, sei->data, sei_len);
            if (scfg->verbose) {
                g_printerr("Emitted pic_timing SEI (prepend) tc=%02u:%02u:%02u:%02u drop=%d\n",
                           hours, minutes, seconds, frame, drop_frame ? 1 : 0);
            }
        } else {
            g_printerr("SEI injection disabled - using GStreamer timecode handling\n");
        }
        gint p = 0;
        while (p < (gint)inmap.size) {
            gint sc = find_startcode(inmap.data, (gint)inmap.size, p);
            if (sc < 0) break;
            gint sc_len2 = startcode_len_at(inmap.data, (gint)inmap.size, sc);
            gint nal_start2 = sc + sc_len2;
            if (nal_start2 >= (gint)inmap.size) break;
            gint next2 = find_startcode(inmap.data, (gint)inmap.size, nal_start2 + 1);
            if (next2 < 0) next2 = (gint)inmap.size;
            guint8 nal_hdr2 = inmap.data[nal_start2];
            guint8 nal_type2 = nal_hdr2 & 0x1F;
            if (nal_type2 == 7) {
                if (!sps_replaced && g_patched_sps_ebsp && g_patched_sps_ebsp->len > 0) {
                    g_byte_array_append(out_arr, g_patched_sps_ebsp->data, g_patched_sps_ebsp->len);
                    sps_replaced = TRUE;
                }
                // else skip original SPS
            } else if (nal_type2 == 6) {
                // Skip original SEI NAL units to avoid duplication with our injected SEI
                // else skip original SEI
            } else {
                g_byte_array_append(out_arr, inmap.data + sc, (guint)(next2 - sc));
            }
            p = next2;
        }
    }
    // Allocate exact-sized buffer and copy metadata
    GstBuffer *out = gst_buffer_new_allocate(NULL, out_arr->len, NULL);
    if (!out) { gst_buffer_unmap(inbuf, &inmap); g_byte_array_unref(sei); g_byte_array_unref(out_arr); return gst_buffer_ref(inbuf); }
    gst_buffer_copy_into(out, inbuf, (GstBufferCopyFlags)(GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_TIMESTAMPS), 0, -1);
    gst_buffer_fill(out, 0, out_arr->data, out_arr->len);
    g_byte_array_unref(out_arr);
    
    gst_buffer_unmap(inbuf, &inmap);
    if (sei) g_byte_array_unref(sei);
    return out;
}

static GstPadProbeReturn h264_sei_inject_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    SeiConfig *scfg = (SeiConfig*)user_data;
    if (!scfg || !scfg->inject_sei) return GST_PAD_PROBE_OK;
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) return GST_PAD_PROBE_OK;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    GstBuffer *newbuf = prepend_h264_sei_timecode(scfg, buf);
    if (newbuf != buf) {
        GST_PAD_PROBE_INFO_DATA(info) = newbuf;
    }
    return GST_PAD_PROBE_OK;
}

static gboolean caps_is_video_raw(GstCaps *caps) {
    if (!caps || gst_caps_is_empty(caps)) return FALSE;
    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        const GstStructure *s = gst_caps_get_structure(caps, i);
        const gchar *name = gst_structure_get_name(s);
        if (g_str_has_prefix(name, "video/x-raw")) return TRUE;
    }
    return FALSE;
}







// no demux block probe





static void format_tc(const GstVideoTimeCode *tc, gchar out[16]) {
    if (!tc) { g_strlcpy(out, "--:--:--:--", 16); return; }
    g_snprintf(out, 16, "%02u:%02u:%02u:%02u", tc->hours, tc->minutes, tc->seconds, tc->frames);
}





// Bit writer for RBSP payloads
typedef struct {
	GByteArray *bytes;
	guint8 current;
	gint bits_filled; // number of bits already filled in current (0..7)
} BitWriter;

static inline void bw_init(BitWriter *bw, GByteArray *out) {
	bw->bytes = out;
	bw->current = 0;
	bw->bits_filled = 0;
}

static inline void bw_put_bit(BitWriter *bw, guint bit) {
	bw->current = (guint8)((bw->current << 1) | (bit & 1));
	bw->bits_filled++;
	if (bw->bits_filled == 8) {
		// write raw byte (no EPB here; apply EPB at NAL assembly)
		g_byte_array_append(bw->bytes, &bw->current, 1);
		bw->current = 0;
		bw->bits_filled = 0;
	}
}

static inline void bw_put_bits(BitWriter *bw, guint32 value, guint num_bits) {
	for (gint i = (gint)num_bits - 1; i >= 0; --i) {
		bw_put_bit(bw, (value >> i) & 1);
	}
}

static inline void bw_put_rbsp_trailing_bits(BitWriter *bw) {
	// append a single '1' bit then zero bits to byte-align
	bw_put_bit(bw, 1);
	while (bw->bits_filled != 0) {
		bw_put_bit(bw, 0);
	}
}

static inline void bw_flush_zero_align(BitWriter *bw) {
	if (bw->bits_filled > 0) {
		while (bw->bits_filled != 0) {
			bw->current <<= 1;
			bw->bits_filled++;
			if (bw->bits_filled == 8) {
				g_byte_array_append(bw->bytes, &bw->current, 1);
				bw->current = 0;
				bw->bits_filled = 0;
				break;
			}
		}
	}
}

// Convert to BCD format (Binary Coded Decimal) as required by SMPTE
static guint8 to_bcd(guint value, guint bits) {
    if (bits == 6) {
        // 6-bit BCD: 4 bits for units, 2 bits for tens
        return ((value / 10) << 4) | (value % 10);
    } else {
        // 7-bit BCD: 4 bits for units, 3 bits for tens  
        return ((value / 10) << 4) | (value % 10);
    }
}

// Build SMPTE timecode in the exact format ffmpeg expects for side data extraction
static void build_ffmpeg_timecode_sei(gboolean drop_frame, guint frame, guint seconds, guint minutes, guint hours, guint8 *out) {
    // Build the exact format that ffmpeg's H.264 decoder recognizes as timecode
    // This should create extractable side data: timecode|value=HH:MM:SS:FF
    
    // Method 1: Try GOP timecode format (25-bit as used in MPEG)
    // Convert to 25-bit GOP timecode format that ffmpeg recognizes
    guint32 tc_25bit = 0;
    
    // GOP timecode format (25 bits):
    // bits 0-5: frames (6 bits) - BCD
    // bits 6-12: seconds (7 bits) - BCD  
    // bits 13-18: minutes (6 bits) - BCD
    // bits 19-23: hours (5 bits) - BCD
    // bit 24: drop frame flag
    
    tc_25bit |= (to_bcd(frame, 6) & 0x3F);           // frames: bits 0-5
    tc_25bit |= ((to_bcd(seconds, 7) & 0x7F) << 6);  // seconds: bits 6-12  
    tc_25bit |= ((to_bcd(minutes, 6) & 0x3F) << 13); // minutes: bits 13-18
    tc_25bit |= ((to_bcd(hours, 5) & 0x1F) << 19);   // hours: bits 19-23
    if (drop_frame) tc_25bit |= (1 << 24);           // drop frame: bit 24
    
    // Use the specific UUID that ffmpeg might recognize for timecode extraction
    // Based on common broadcast UUIDs for SMPTE timecode
    out[0] = 0x4F; out[1] = 0x78; out[2] = 0xCA; out[3] = 0x42;  // Custom UUID for timecode
    out[4] = 0x4C; out[5] = 0x47; out[6] = 0x11; out[7] = 0xD9;
    out[8] = 0x94; out[9] = 0x08; out[10] = 0x00; out[11] = 0x20;
    out[12] = 0x0C; out[13] = 0x9A; out[14] = 0x66; out[15] = 0x00;
    
    // SMPTE timecode signature that ffmpeg looks for
    out[16] = 0x47; // 'G'  
    out[17] = 0x41; // 'A'
    out[18] = 0x39; // '9'
    out[19] = 0x34; // '4'
    out[20] = 0x03; // Data type identifier for timecode
    
    // 25-bit GOP timecode (3 bytes + 1 bit, padded to 4 bytes)
    out[21] = (tc_25bit >> 24) & 0xFF;
    out[22] = (tc_25bit >> 16) & 0xFF; 
    out[23] = (tc_25bit >> 8) & 0xFF;
    out[24] = tc_25bit & 0xFF;
}

// Note: These functions are no longer used - we only inject Picture Timing SEI (payload type 1)
// which provides proper SMPTE 12-1 timecode side data that ffprobe can extract

// Build a complete SEI NAL (Annex B) for Picture Timing with clock timestamp (full timestamp)
GByteArray* build_pic_timing_sei_nal(gboolean drop_frame, guint frame, guint seconds, guint minutes, guint hours, gboolean include_time_offset) {
	// Build RBSP payload bytes (no EPB) and byte-align within payload
	GByteArray *payload = g_byte_array_new();
	BitWriter bw;
	bw_init(&bw, payload);
	// pic_struct u(4) = 0 (frame)
	bw_put_bits(&bw, 0, 4);
	// clock_timestamp_flag[0] u(1) = 1
	bw_put_bits(&bw, 1, 1);
	// ct_type u(2)=0, nuit_field_based_flag u(1)=0, counting_type u(5)=0
	bw_put_bits(&bw, 0, 2);
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, 0, 5);
	// full_timestamp_flag u(1)=1, discontinuity_flag u(1)=0, cnt_dropped_flag u(1)=drop_frame
	bw_put_bits(&bw, 1, 1);
	bw_put_bits(&bw, 0, 1);
	bw_put_bits(&bw, drop_frame ? 1 : 0, 1);
	// n_frames u(8)
	bw_put_bits(&bw, frame & 0xFF, 8);
	// seconds_value u(6), minutes_value u(6), hours_value u(5)
	bw_put_bits(&bw, seconds & 0x3F, 6);
	bw_put_bits(&bw, minutes & 0x3F, 6);
	bw_put_bits(&bw, hours & 0x1F, 5);
	if (include_time_offset) {
		bw_put_bits(&bw, 0, 24);
	}
	// Byte-align payload (zero pad)
	bw_flush_zero_align(&bw);

	// Assemble SEI NAL
	GByteArray *sei = g_byte_array_new();
	g_byte_array_append(sei, (guint8[]){0x00,0x00,0x00,0x01}, 4);
	g_byte_array_append(sei, (guint8[]){0x06}, 1);
	// payloadType 1
	epb_safe_append(sei, 1);
	// payloadSize (RBSP bytes count)
	guint total = payload->len;
	while (total >= 255) { epb_safe_append(sei, 255); total -= 255; }
	epb_safe_append(sei, (guint8)total);
	// append payload bytes with EPB at NAL level
	for (guint i = 0; i < payload->len; ++i) {
		epb_safe_append(sei, payload->data[i]);
	}
	g_byte_array_unref(payload);
	// rbsp_trailing_bits for NAL
	epb_safe_append(sei, 0x80);
	return sei;
}

// --- RBSP/EBSP bit reader helpers ---
typedef struct {
	const guint8 *data;
	gsize size;
	gsize bitpos; // bit index from start
} BitReader;

static inline void br_init(BitReader *br, const guint8 *data, gsize size) {
	br->data = data; br->size = size; br->bitpos = 0;
}

static inline guint br_read_bit(BitReader *br, gboolean *ok) {
	if (br->bitpos >= br->size * 8) { if (ok) *ok = FALSE; return 0; }
	gsize byte_index = br->bitpos >> 3;
	guint shift = 7 - (br->bitpos & 7);
	guint bit = (br->data[byte_index] >> shift) & 1;
	br->bitpos++;
	return bit;
}

static inline guint32 br_read_bits(BitReader *br, guint nbits, gboolean *ok) {
	guint32 val = 0;
	for (guint i = 0; i < nbits; ++i) {
		val = (val << 1) | br_read_bit(br, ok);
		if (ok && !*ok) return 0;
	}
	return val;
}

static inline guint32 br_read_ue(BitReader *br, gboolean *ok) {
	guint zeros = 0;
	while (br_read_bit(br, ok) == 0) {
		if (ok && !*ok) return 0;
		zeros++;
		if (zeros > 31) { if (ok) *ok = FALSE; return 0; }
	}
	if (zeros == 0) return 0;
	guint32 suffix = br_read_bits(br, zeros, ok);
	return (1u << zeros) - 1 + suffix;
}

static GByteArray* ebsp_to_rbsp(const guint8 *ebsp, gsize size) {
	GByteArray *rbsp = g_byte_array_new();
	guint zeros = 0;
	for (gsize i = 0; i < size; ++i) {
		guint8 b = ebsp[i];
		if (zeros >= 2 && b == 0x03) {
			// skip EPB
			zeros = 0;
			continue;
		}
		g_byte_array_append(rbsp, &b, 1);
		if (b == 0x00) zeros++; else zeros = 0;
	}
	return rbsp;
}

// Try to patch SPS RBSP to force VUI pic_struct_present_flag=1 and return Annex B EBSP
static GByteArray* patch_sps_pic_struct_flag_to_one(const guint8 *ebsp, gsize ebsp_size, guint8 header_byte) {
    GByteArray *rbsp = ebsp_to_rbsp(ebsp, ebsp_size);
    if (!rbsp) return NULL;
    // Parse to ensure VUI present and locate flag bit
    BitReader br; gboolean ok = TRUE; br_init(&br, rbsp->data, rbsp->len);
    // profile/constraints/level
    br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok);
    br_read_ue(&br, &ok);
    guint profile_idc = rbsp->data[0];
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 || profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 || profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 || profile_idc == 135) {
        guint chroma_format_idc = br_read_ue(&br, &ok); if (chroma_format_idc == 3) br_read_bits(&br, 1, &ok);
        br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_bits(&br, 1, &ok);
        guint seq_scaling_matrix_present_flag = br_read_bits(&br, 1, &ok);
        if (seq_scaling_matrix_present_flag) {
            guint count = (chroma_format_idc != 3) ? 8 : 12;
            for (guint i = 0; i < count; ++i) {
                guint f = br_read_bits(&br, 1, &ok);
                if (f) {
                    guint sizeOfScalingList = (i < 6) ? 16 : 64;
                    for (guint j = 0; j < sizeOfScalingList; ++j) (void)br_read_ue(&br, &ok);
                }
            }
        }
    }
    br_read_ue(&br, &ok); // log2_max_frame_num_minus4
    guint pic_order_cnt_type = br_read_ue(&br, &ok);
    if (pic_order_cnt_type == 0) { br_read_ue(&br, &ok); }
    else if (pic_order_cnt_type == 1) { br_read_bits(&br, 1, &ok); br_read_ue(&br, &ok); br_read_ue(&br, &ok); guint num_ref = br_read_ue(&br, &ok); for (guint i = 0; i < num_ref; ++i) br_read_ue(&br, &ok); }
    br_read_ue(&br, &ok); br_read_bits(&br, 1, &ok); br_read_ue(&br, &ok); br_read_ue(&br, &ok);
    guint frame_mbs_only_flag = br_read_bits(&br, 1, &ok); if (!frame_mbs_only_flag) br_read_bits(&br, 1, &ok);
    br_read_bits(&br, 1, &ok);
    guint frame_cropping_flag = br_read_bits(&br, 1, &ok); if (frame_cropping_flag) { br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_ue(&br, &ok); }
    guint vui_parameters_present_flag = br_read_bits(&br, 1, &ok);
    if (!ok || !vui_parameters_present_flag) { g_byte_array_unref(rbsp); return NULL; }
    // Skip through VUI to the pic_struct_present_flag
    guint aspect_ratio_info_present_flag = br_read_bits(&br, 1, &ok);
    if (aspect_ratio_info_present_flag) { guint ari = br_read_bits(&br, 8, &ok); if (ari == 255) { br_read_bits(&br, 16, &ok); br_read_bits(&br, 16, &ok); } }
    guint overscan_info_present_flag = br_read_bits(&br, 1, &ok); if (overscan_info_present_flag) br_read_bits(&br, 1, &ok);
    guint video_signal_type_present_flag = br_read_bits(&br, 1, &ok);
    if (video_signal_type_present_flag) { br_read_bits(&br, 3, &ok); guint colour_description_present_flag = br_read_bits(&br, 1, &ok); if (colour_description_present_flag) { br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok); } }
    guint chroma_loc_info_present_flag = br_read_bits(&br, 1, &ok); if (chroma_loc_info_present_flag) { br_read_ue(&br, &ok); br_read_ue(&br, &ok); }
    guint timing_info_present_flag = br_read_bits(&br, 1, &ok); if (timing_info_present_flag) { br_read_bits(&br, 32, &ok); br_read_bits(&br, 32, &ok); br_read_bits(&br, 1, &ok); }
    guint nal_hrd = br_read_bits(&br, 1, &ok);
    if (nal_hrd) { guint cpb_cnt_minus1 = br_read_ue(&br, &ok); br_read_bits(&br, 4, &ok); br_read_bits(&br, 4, &ok); for (guint i = 0; i <= cpb_cnt_minus1; ++i) { br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_bits(&br, 1, &ok); } br_read_bits(&br, 5, &ok); br_read_bits(&br, 5, &ok); br_read_bits(&br, 5, &ok); br_read_bits(&br, 5, &ok); }
    guint vcl_hrd = br_read_bits(&br, 1, &ok);
    if (vcl_hrd) { guint cpb_cnt_minus1 = br_read_ue(&br, &ok); br_read_bits(&br, 4, &ok); br_read_bits(&br, 4, &ok); for (guint i = 0; i <= cpb_cnt_minus1; ++i) { br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_bits(&br, 1, &ok); } br_read_bits(&br, 5, &ok); br_read_bits(&br, 5, &ok); br_read_bits(&br, 5, &ok); br_read_bits(&br, 5, &ok); }
    if (nal_hrd || vcl_hrd) { br_read_bits(&br, 1, &ok); }
    if (!ok) { g_byte_array_unref(rbsp); return NULL; }
    gsize bitpos = br.bitpos;
    // Set the flag to 1 in a copy
    GByteArray *patched_rbsp = g_byte_array_sized_new(rbsp->len);
    g_byte_array_append(patched_rbsp, rbsp->data, rbsp->len);
    g_byte_array_unref(rbsp);
    gsize byte_idx = bitpos >> 3; guint bit_in_byte = 7 - (bitpos & 7);
    patched_rbsp->data[byte_idx] |= (1u << bit_in_byte);
    // Build Annex B EBSP
    GByteArray *annexb = build_annexb_from_rbsp_and_header(patched_rbsp->data, patched_rbsp->len, header_byte);
    g_byte_array_unref(patched_rbsp);
    return annexb;
}

// Patch SPS to set pic_struct_present_flag=1 and timing_info_present_flag with fps
static GByteArray* patch_sps_pic_struct_and_timing(const guint8 *ebsp, gsize ebsp_size, guint8 header_byte, guint fps_n, guint fps_d) {
    if (fps_n == 0 || fps_d == 0) return patch_sps_pic_struct_flag_to_one(ebsp, ebsp_size, header_byte);
    GByteArray *rbsp = ebsp_to_rbsp(ebsp, ebsp_size);
    if (!rbsp) return NULL;
    gboolean ok = TRUE;
    // Find bit position of vui_parameters_present_flag
    BitReader br2; br_init(&br2, rbsp->data, rbsp->len);
    br_read_bits(&br2, 8, &ok); br_read_bits(&br2, 8, &ok); br_read_bits(&br2, 8, &ok); br_read_ue(&br2, &ok);
    guint profile_idc = rbsp->data[0];
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 || profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 || profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 || profile_idc == 135) {
        guint chroma_format_idc = br_read_ue(&br2, &ok); if (chroma_format_idc == 3) br_read_bits(&br2, 1, &ok);
        br_read_ue(&br2, &ok); br_read_ue(&br2, &ok); br_read_bits(&br2, 1, &ok);
        guint seq_scaling_matrix_present_flag = br_read_bits(&br2, 1, &ok);
        if (seq_scaling_matrix_present_flag) {
            guint count = (chroma_format_idc != 3) ? 8 : 12;
            for (guint i = 0; i < count; ++i) {
                guint f = br_read_bits(&br2, 1, &ok);
                if (f) {
                    guint sizeOfScalingList = (i < 6) ? 16 : 64;
                    for (guint j = 0; j < sizeOfScalingList; ++j) (void)br_read_ue(&br2, &ok);
                }
            }
        }
    }
    br_read_ue(&br2, &ok);
    guint pic_order_cnt_type = br_read_ue(&br2, &ok);
    if (pic_order_cnt_type == 0) { br_read_ue(&br2, &ok); }
    else if (pic_order_cnt_type == 1) { br_read_bits(&br2, 1, &ok); br_read_ue(&br2, &ok); br_read_ue(&br2, &ok); guint num_ref = br_read_ue(&br2, &ok); for (guint i = 0; i < num_ref; ++i) br_read_ue(&br2, &ok); }
    br_read_ue(&br2, &ok); br_read_bits(&br2, 1, &ok); br_read_ue(&br2, &ok); br_read_ue(&br2, &ok);
    guint frame_mbs_only_flag = br_read_bits(&br2, 1, &ok); if (!frame_mbs_only_flag) br_read_bits(&br2, 1, &ok);
    br_read_bits(&br2, 1, &ok);
    guint frame_cropping_flag = br_read_bits(&br2, 1, &ok); if (frame_cropping_flag) { br_read_ue(&br2, &ok); br_read_ue(&br2, &ok); br_read_ue(&br2, &ok); br_read_ue(&br2, &ok); }
    gsize vui_flag_bitpos = br2.bitpos; // position of vui_parameters_present_flag
    guint vui_parameters_present_flag = br_read_bits(&br2, 1, &ok);
    if (!ok) { g_byte_array_unref(rbsp); return NULL; }

    // Rebuild SPS RBSP: copy bits up to the VUI flag, then write flag=1 and our VUI
    BitReader br_copy; br_init(&br_copy, rbsp->data, rbsp->len);
    GByteArray *new_rbsp = g_byte_array_sized_new(rbsp->len + 64);
    BitWriter bw; bw_init(&bw, new_rbsp);
    for (gsize i = 0; i < vui_flag_bitpos; ++i) {
        guint bit = br_read_bit(&br_copy, &ok);
        if (!ok) { g_byte_array_unref(rbsp); g_byte_array_unref(new_rbsp); return NULL; }
        bw_put_bit(&bw, bit);
    }
    // Write vui_parameters_present_flag = 1
    bw_put_bit(&bw, 1);
    // Write minimal VUI with timing_info and pic_struct_present_flag
    // aspect_ratio_info_present_flag
    bw_put_bits(&bw, 0, 1);
    // overscan_info_present_flag
    bw_put_bits(&bw, 0, 1);
    // video_signal_type_present_flag
    bw_put_bits(&bw, 0, 1);
    // chroma_loc_info_present_flag
    bw_put_bits(&bw, 0, 1);
    // timing_info_present_flag
    bw_put_bits(&bw, 1, 1);
    // num_units_in_tick (32), time_scale (32), fixed_frame_rate_flag (1)
    guint32 num_units_in_tick = fps_d;
    guint32 time_scale = fps_n * 2u;
    bw_put_bits(&bw, num_units_in_tick, 32);
    bw_put_bits(&bw, time_scale, 32);
    bw_put_bits(&bw, 1, 1); // fixed_frame_rate_flag
    // nal_hrd_parameters_present_flag
    bw_put_bits(&bw, 0, 1);
    // vcl_hrd_parameters_present_flag
    bw_put_bits(&bw, 0, 1);
    // if any HRD present, low_delay_hrd_flag would follow; none here
    // pic_struct_present_flag
    bw_put_bits(&bw, 1, 1);
    // bitstream_restriction_flag
    bw_put_bits(&bw, 0, 1);
    // Trailing bits to end SPS
    bw_put_rbsp_trailing_bits(&bw);

    // Assemble Annex B from new RBSP
    GByteArray *annexb = build_annexb_from_rbsp_and_header(new_rbsp->data, new_rbsp->len, header_byte);
    g_byte_array_unref(new_rbsp);
    g_byte_array_unref(rbsp);
    return annexb;
}

// (Removed duplicate typedef SpsVuiInfo; defined near top of file)

static gboolean parse_sps_vui_info_from_rbsp(const guint8 *rbsp, gsize size, SpsVuiInfo *out) {
	memset(out, 0, sizeof(*out));
	BitReader br; gboolean ok = TRUE; br_init(&br, rbsp, size);
	// profile_idc, constraint flags, level_idc
	br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok);
	guint sps_id = br_read_ue(&br, &ok);
	// High profiles
	guint profile_idc = rbsp[0];
	if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 || profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 || profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 || profile_idc == 135) {
		guint chroma_format_idc = br_read_ue(&br, &ok);
		if (chroma_format_idc == 3) { br_read_bits(&br, 1, &ok); }
		br_read_ue(&br, &ok); // bit_depth_luma_minus8
		br_read_ue(&br, &ok); // bit_depth_chroma_minus8
		br_read_bits(&br, 1, &ok); // qpprime_y_zero_transform_bypass_flag
		guint seq_scaling_matrix_present_flag = br_read_bits(&br, 1, &ok);
		if (seq_scaling_matrix_present_flag) {
			guint count = (chroma_format_idc != 3) ? 8 : 12;
			for (guint i = 0; i < count; ++i) {
				guint f = br_read_bits(&br, 1, &ok);
				if (f) {
					// skip scaling list; approximate skipping using exp-golomb deltas
					guint sizeOfScalingList = (i < 6) ? 16 : 64;
					gint lastScale = 8, nextScale = 8;
					for (guint j = 0; j < sizeOfScalingList; ++j) {
						if (nextScale != 0) {
							// se(v); implement quick signed Exp-Golomb read
							// read_ue and map to signed
							guint ue = br_read_ue(&br, &ok);
							gint se = (ue & 1) ? (gint)((ue + 1) / 2) : -(gint)(ue / 2);
							nextScale = (lastScale + se + 256) % 256;
						}
						lastScale = (nextScale == 0) ? lastScale : nextScale;
					}
				}
			}
		}
	}
	br_read_ue(&br, &ok); // log2_max_frame_num_minus4
	guint pic_order_cnt_type = br_read_ue(&br, &ok);
	if (pic_order_cnt_type == 0) { br_read_ue(&br, &ok); }
	else if (pic_order_cnt_type == 1) {
		br_read_bits(&br, 1, &ok);
		br_read_ue(&br, &ok); br_read_ue(&br, &ok);
		guint num_ref = br_read_ue(&br, &ok);
		for (guint i = 0; i < num_ref; ++i) br_read_ue(&br, &ok);
	}
	br_read_ue(&br, &ok); // max_num_ref_frames
	br_read_bits(&br, 1, &ok); // gaps_in_frame_num_value_allowed_flag
	br_read_ue(&br, &ok); // pic_width_in_mbs_minus1
	br_read_ue(&br, &ok); // pic_height_in_map_units_minus1
	guint frame_mbs_only_flag = br_read_bits(&br, 1, &ok);
	if (!frame_mbs_only_flag) { br_read_bits(&br, 1, &ok); }
	br_read_bits(&br, 1, &ok); // direct_8x8_inference_flag
	guint frame_cropping_flag = br_read_bits(&br, 1, &ok);
	if (frame_cropping_flag) {
		br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_ue(&br, &ok);
	}
	guint vui_parameters_present_flag = br_read_bits(&br, 1, &ok);
	out->vui_present = vui_parameters_present_flag;
	if (!vui_parameters_present_flag || !ok) {
		// default conservative: no HRD, set pic_struct_present = 1 so our payload includes timestamp
		out->pic_struct_present_flag = TRUE;
		out->cpb_dpb_delays_present_flag = FALSE;
		out->cpb_removal_delay_length = 0;
		out->dpb_output_delay_length = 0;
		out->time_offset_length = 0;
		out->timing_info_present_flag = FALSE;
		out->num_units_in_tick = 0;
		out->time_scale = 0;
		out->fixed_frame_rate_flag = FALSE;
		return TRUE;
	}
	// VUI
	guint aspect_ratio_info_present_flag = br_read_bits(&br, 1, &ok);
	if (aspect_ratio_info_present_flag) {
		guint aspect_ratio_idc = br_read_bits(&br, 8, &ok);
		if (aspect_ratio_idc == 255) { br_read_bits(&br, 16, &ok); br_read_bits(&br, 16, &ok); }
	}
	guint overscan_info_present_flag = br_read_bits(&br, 1, &ok);
	if (overscan_info_present_flag) br_read_bits(&br, 1, &ok);
	guint video_signal_type_present_flag = br_read_bits(&br, 1, &ok);
	if (video_signal_type_present_flag) {
		br_read_bits(&br, 3, &ok);
		guint colour_description_present_flag = br_read_bits(&br, 1, &ok);
		if (colour_description_present_flag) { br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok); br_read_bits(&br, 8, &ok); }
	}
	guint chroma_loc_info_present_flag = br_read_bits(&br, 1, &ok);
	if (chroma_loc_info_present_flag) { br_read_ue(&br, &ok); br_read_ue(&br, &ok); }
	guint timing_info_present_flag = br_read_bits(&br, 1, &ok);
	if (timing_info_present_flag) {
		guint32 num_units_in_tick = br_read_bits(&br, 32, &ok);
		guint32 time_scale = br_read_bits(&br, 32, &ok);
		guint fixed_frame_rate_flag = br_read_bits(&br, 1, &ok);
		out->timing_info_present_flag = TRUE;
		out->num_units_in_tick = num_units_in_tick;
		out->time_scale = time_scale;
		out->fixed_frame_rate_flag = fixed_frame_rate_flag ? TRUE : FALSE;
	}
	guint nal_hrd_parameters_present_flag = br_read_bits(&br, 1, &ok);
	guint cpb_removal_delay_length_minus1 = 23, dpb_output_delay_length_minus1 = 23, time_offset_length = 24; // defaults
	if (nal_hrd_parameters_present_flag) {
		guint cpb_cnt_minus1 = br_read_ue(&br, &ok);
		br_read_bits(&br, 4, &ok); // bit_rate_scale
		br_read_bits(&br, 4, &ok); // cpb_size_scale
		for (guint i = 0; i <= cpb_cnt_minus1; ++i) {
			br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_bits(&br, 1, &ok);
		}
		br_read_bits(&br, 5, &ok); // initial_cpb_removal_delay_length_minus1
		cpb_removal_delay_length_minus1 = br_read_bits(&br, 5, &ok);
		dpb_output_delay_length_minus1 = br_read_bits(&br, 5, &ok);
		time_offset_length = br_read_bits(&br, 5, &ok);
	}
	guint vcl_hrd_parameters_present_flag = br_read_bits(&br, 1, &ok);
	if (vcl_hrd_parameters_present_flag) {
		guint cpb_cnt_minus1 = br_read_ue(&br, &ok);
		br_read_bits(&br, 4, &ok); br_read_bits(&br, 4, &ok);
		for (guint i = 0; i <= cpb_cnt_minus1; ++i) { br_read_ue(&br, &ok); br_read_ue(&br, &ok); br_read_bits(&br, 1, &ok); }
		br_read_bits(&br, 5, &ok);
		cpb_removal_delay_length_minus1 = br_read_bits(&br, 5, &ok);
		dpb_output_delay_length_minus1 = br_read_bits(&br, 5, &ok);
		time_offset_length = br_read_bits(&br, 5, &ok);
	}
	if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
		br_read_bits(&br, 1, &ok); // low_delay_hrd_flag
	}
	guint pic_struct_present_flag = br_read_bits(&br, 1, &ok);
	out->pic_struct_present_flag = pic_struct_present_flag;
	out->cpb_dpb_delays_present_flag = (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag);
	out->cpb_removal_delay_length = cpb_removal_delay_length_minus1 + 1;
	out->dpb_output_delay_length = dpb_output_delay_length_minus1 + 1;
	out->time_offset_length = (out->cpb_dpb_delays_present_flag ? time_offset_length : 0);
	return ok;
}

static gboolean extract_sps_vui_from_au(const guint8 *annexb, gsize size, SpsVuiInfo *out) {
	gint pos = 0;
	while (pos + 4 < (gint)size) {
		// find next start code
		gint sc = find_startcode(annexb, (gint)size, pos);
		if (sc < 0) break;
		gint sc_len = startcode_len_at(annexb, (gint)size, sc);
		gint nal_start = sc + sc_len;
		if (nal_start >= (gint)size) break;
		gint next = find_startcode(annexb, (gint)size, nal_start);
		gint nal_end = (next < 0) ? (gint)size : next;
		guint8 nal_hdr = annexb[nal_start];
		guint8 nal_type = nal_hdr & 0x1F;
		if (nal_type == 7) {
			GByteArray *rbsp = ebsp_to_rbsp(annexb + nal_start + 1, (gsize)(nal_end - (nal_start + 1)));
			gboolean ok = parse_sps_vui_info_from_rbsp(rbsp->data, rbsp->len, out);
			g_byte_array_unref(rbsp);
			return ok;
		}
		pos = nal_end;
	}
	return FALSE;
}

static GByteArray* build_pic_timing_sei_nal_from_sps(const SpsVuiInfo *info, gboolean drop_frame, guint frame, guint seconds, guint minutes, guint hours) {
	// Re-enable SEI injection using the working implementation
	// The include_time_offset parameter should be TRUE if HRD is present and time_offset_length > 0
	gboolean include_time_offset = (info->cpb_dpb_delays_present_flag && info->time_offset_length > 0);
	return build_pic_timing_sei_nal(drop_frame, frame, seconds, minutes, hours, include_time_offset);
}

GByteArray* build_pic_timing_sei_nal_from_au(const guint8 *annexb, gsize size, gboolean drop_frame, guint frame, guint seconds, guint minutes, guint hours) {
    SpsVuiInfo info; memset(&info, 0, sizeof(info));
    if (!extract_sps_vui_from_au(annexb, size, &info)) {
        // Fallback to default lengths: no HRD, pic_struct present
        info.vui_present = TRUE;
        info.pic_struct_present_flag = TRUE;
        info.cpb_dpb_delays_present_flag = FALSE;
        info.cpb_removal_delay_length = 0;
        info.dpb_output_delay_length = 0;
        info.time_offset_length = 0;
    } else {
        // Force pic_struct_present_flag=1 since we patch the SPS to have this flag set
        info.pic_struct_present_flag = TRUE;
        // Do not force time_offset bits if not present in HRD
    }
    return build_pic_timing_sei_nal_from_sps(&info, drop_frame, frame, seconds, minutes, hours);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    // No in-binary element registration needed

    AppConfig cfg;
    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    // Handle discover mode
    if (cfg.discover) {
        discover_ndi_sources();
        return 0;
    }
    
    // Build exact working pipeline via gst_parse_launch
    gchar *output_section;
    if (cfg.stdout_mode) {
        output_section = g_strdup("fdsink fd=1 sync=false");
    } else {
        output_section = g_strdup_printf("srtsink uri=\"%s\" wait-for-connection=false sync=false", cfg.srt_uri);
    }
    
    // Build GOP size parameter string
    gchar *gop_param = cfg.gop_size > 0 ? g_strdup_printf("key-int-max=%u ", cfg.gop_size) : g_strdup("");
    
    // Build audio pipeline section based on codec choice
    gchar *audio_section;
    if (cfg.with_audio) {
        gchar *audio_pipeline = build_audio_pipeline(cfg.audio_codec, cfg.audio_bitrate_kbps);
        audio_section = g_strdup_printf("src.audio ! queue ! %s ! mux.", audio_pipeline);
        g_free(audio_pipeline);
    } else {
        audio_section = g_strdup("src.audio ! queue ! fakesink sync=false");
    }
    
    gchar *pipeline_desc = g_strdup_printf(
        "ndisrc ndi-name=\"%s\" timestamp-mode=%s ! ndisrcdemux name=src "
        "src.video ! queue ! videoconvert ! video/x-raw,format=I420 ! "
        "x264enc name=enc tune=zerolatency speed-preset=ultrafast %sbitrate=%d aud=false byte-stream=true insert-vui=false interlaced=false nal-hrd=none ! "
        "h264parse name=h264parse disable-passthrough=true config-interval=1 ! video/x-h264,stream-format=byte-stream,alignment=au ! mpegtsmux name=mux "
        "%s "
        "mux. ! queue leaky=2 max-size-time=2000000000 ! %s",
        cfg.ndi_name, cfg.timestamp_mode, gop_param, cfg.bitrate_kbps, audio_section, output_section);
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_desc, &err);
    if (!pipeline || err) {
        g_printerr("Failed to build pipeline: %s\n", err ? err->message : "unknown error");
        if (err) g_error_free(err);
        g_free(pipeline_desc);
        g_free(output_section);
        g_free(gop_param);
        return 1;
    }
    g_free(pipeline_desc);
    g_free(output_section);
    g_free(audio_section);
    g_free(gop_param);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_msg_cb, loop);
    gst_object_unref(bus);

    // Pause first to allow negotiation and install SEI probe
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

    // Install SEI injector on encoder src (Annex B byte-stream with AUD)
    gulong sei_probe_id = 0;
    SeiConfig *sei_cfg = NULL;
    if (cfg.inject_sei) {
        GstElement *enc_elem = gst_bin_get_by_name(GST_BIN(pipeline), "enc");
        if (enc_elem) {
            GstPad *enc_src = gst_element_get_static_pad(enc_elem, "src");
            GstPad *enc_sink = gst_element_get_static_pad(enc_elem, "sink");
            guint fps_n = 0, fps_d = 1;
            if (enc_sink) {
                GstPad *peer = gst_pad_get_peer(enc_sink);
                GstCaps *pcaps = peer ? gst_pad_get_current_caps(peer) : NULL;
                if (!pcaps && peer) pcaps = gst_pad_query_caps(peer, NULL);
                if (pcaps) {
                    const GstStructure *s = gst_caps_get_structure(pcaps, 0);
                    if (s) {
                        const GValue *fr = gst_structure_get_value(s, "framerate");
                        if (fr && GST_VALUE_HOLDS_FRACTION(fr)) {
                            fps_n = gst_value_get_fraction_numerator(fr);
                            fps_d = gst_value_get_fraction_denominator(fr);
                        }
                    }
                    gst_caps_unref(pcaps);
                }
                if (peer) gst_object_unref(peer);
                gst_object_unref(enc_sink);
            }
            if (enc_src) {
                sei_cfg = g_new0(SeiConfig, 1);
                sei_cfg->inject_sei = TRUE;
                sei_cfg->prefer_pts = TRUE;
                sei_cfg->fps_n = fps_n;
                sei_cfg->fps_d = fps_d;
                sei_cfg->verbose = cfg.verbose;
                sei_probe_id = gst_pad_add_probe(enc_src, GST_PAD_PROBE_TYPE_BUFFER, h264_sei_inject_probe, sei_cfg, NULL);
                gst_object_unref(enc_src);
            }
            gst_object_unref(enc_elem);
        }
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    if (cfg.stdout_mode) {
        g_printerr("Running... NDI: %s -> stdout\n", cfg.ndi_name);
    } else {
        g_printerr("Running... NDI: %s -> SRT: %s\n", cfg.ndi_name, cfg.srt_uri);
    }
    if (cfg.timeout_seconds > 0) {
        g_timeout_add_seconds(cfg.timeout_seconds, quit_loop_cb, loop);
    }
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    // Remove probe and free
    if (cfg.inject_sei && sei_probe_id != 0) {
        GstElement *enc_elem2 = gst_bin_get_by_name(GST_BIN(pipeline), "enc");
        if (enc_elem2) {
            GstPad *enc_src2 = gst_element_get_static_pad(enc_elem2, "src");
            if (enc_src2) {
                gst_pad_remove_probe(enc_src2, sei_probe_id);
                gst_object_unref(enc_src2);
            }
            gst_object_unref(enc_elem2);
        }
    }
    if (sei_cfg) g_free(sei_cfg);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);

    g_free(cfg.ndi_name);
    g_free(cfg.encoder);
    g_free(cfg.audio_codec);
    if (cfg.timestamp_mode) g_free(cfg.timestamp_mode);
    if (cfg.dump_ts_path) g_free(cfg.dump_ts_path);
    return 0;
}
