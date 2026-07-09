/*
* Audacity: A Digital Audio Editor
*
* Stable C ABI for dynamically loaded Audacity plugins.
*
* This header is the complete contract between Audacity and a plugin: a
* plugin built against it must keep loading and working in later Audacity
* versions. To make that hold, everything here follows these rules:
*
*  - Pure C: no C++ types, exceptions or memory allocation cross this
*    boundary in either direction.
*  - Additive evolution only: data structs grow by appending fields, guarded
*    by struct_size; function tables never change once published — any
*    extension or redesign gets a new factory/extension id
*    ("audacity.params/2") and the old id keeps working. Behavior of
*    existing fields never changes.
*  - Capabilities are discovered by string id (get_factory/get_extension),
*    never by casting. Unknown ids return NULL and the caller degrades
*    gracefully.
*  - Strings are UTF-8. Unless stated otherwise, a string returned by a
*    function is owned by the callee and stays valid until the next call to
*    the same function on the same object, or until the object is destroyed,
*    whichever comes first. Strings in descriptors stay valid for the
*    lifetime of the plugin.
*  - Audio is 32-bit float, planar (one buffer per channel).
*  - Threading: the host calls all plugin functions from a single thread
*    unless a function's documentation states otherwise.
*
* A plugin implements one exported symbol (in C++ the extern "C" is
* required, or the symbol gets internal linkage and is never exported):
*
*    extern "C" AUPLUG_EXPORT const auplug_entry_t auplug_entry = { ... };
*
* Load sequence (host side):
*  1. dlopen / LoadLibrary; the library stays loaded for the lifetime of
*     the host process
*  2. resolve "auplug_entry"; reject if entry->api_version > host API version
*  3. call entry->init(host); the plugin may inspect host->api_version and
*     refuse (return false) if the host is too old for it
*  4. query factories by id, e.g. AUPLUG_EFFECT_FACTORY_ID
*
* At shutdown the host calls deinit(), but remaining effect instances may
* still be destroyed afterwards: fx->destroy must stay safe after deinit.
*/

#ifndef AUDACITY_PLUGIN_API_H
#define AUDACITY_PLUGIN_API_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define AUPLUG_EXPORT __declspec(dllexport)
#else
#define AUPLUG_EXPORT __attribute__((visibility("default")))
#endif

/* The API version this header describes. The host supports all versions up
 * to and including its own; a plugin declares the version it was built
 * against in auplug_entry_t::api_version. */
#define AUPLUG_API_VERSION 1

#define AUPLUG_ENTRY_SYMBOL "auplug_entry"

/* ====================================================================== */
/* Host                                                                   */
/* ====================================================================== */

enum auplug_log_level {
    AUPLUG_LOG_DEBUG = 0,
    AUPLUG_LOG_INFO = 1,
    AUPLUG_LOG_WARN = 2,
    AUPLUG_LOG_ERROR = 3
};

/* Passed to the plugin at init; valid until deinit. All members are
 * non-NULL for the versions they were introduced in. */
typedef struct auplug_host_t {
    uint32_t struct_size;   /* sizeof(auplug_host_t) at host build time */
    uint32_t api_version;   /* host's AUPLUG_API_VERSION */
    void* ctx;              /* opaque host context, pass to callbacks */

    /* log and data_dir may be called from any thread. */
    void (*log)(void* ctx, int32_t level, const char* msg);

    /* Directory for plugin data (AI models etc.). Valid until deinit. */
    const char* (*data_dir)(void* ctx);

    /* Host-side capability discovery; NULL if the id is unknown.
     * Known ids: AUPLUG_DSP_EXT_ID. */
    const void* (*get_extension)(void* ctx, const char* extension_id);
} auplug_host_t;

/* ====================================================================== */
/* Plugin entry                                                           */
/* ====================================================================== */

typedef struct auplug_desc_t {
    uint32_t struct_size;
    const char* id;           /* stable unique id, e.g. "org.audacity.example" */
    const char* name;         /* display name, e.g. "Example Effects" */
    const char* vendor;
    const char* version;      /* plugin's own version */
    const char* description;
} auplug_desc_t;

typedef struct auplug_entry_t {
    uint32_t api_version;   /* AUPLUG_API_VERSION the plugin was built with */

    const auplug_desc_t* desc;

    /* Called once after load, before anything else. Returning false aborts
     * the load (e.g. the host is too old for this plugin). The host pointer
     * stays valid until deinit. */
    bool (*init)(const auplug_host_t* host);
    void (*deinit)(void);

    /* Plugin-level capability discovery; NULL if the id is unknown.
     * Known ids: AUPLUG_EFFECT_FACTORY_ID, AUPLUG_IMPORTER_FACTORY_ID,
     * AUPLUG_EXPORTER_FACTORY_ID, AUPLUG_VIEWS_ID. */
    const void* (*get_factory)(const char* factory_id);
} auplug_entry_t;

/* ====================================================================== */
/* Effect factory: "audacity.effect-factory/1"                            */
/* ====================================================================== */

#define AUPLUG_EFFECT_FACTORY_ID "audacity.effect-factory/1"

enum auplug_effect_type {
    AUPLUG_EFFECT_PROCESSOR = 0,   /* transforms selected audio */
    AUPLUG_EFFECT_GENERATOR = 1,   /* creates audio */
    AUPLUG_EFFECT_ANALYZER = 2     /* produces labels/analysis, audio unchanged */
};

typedef struct auplug_effect_desc_t {
    uint32_t struct_size;
    const char* id;           /* stable unique id, e.g. "example.gain" */
    const char* name;         /* display name (English) */
    const char* vendor;
    const char* version;
    const char* description;
    int32_t type;             /* auplug_effect_type */
    uint32_t flags;           /* reserved, set to 0 */
} auplug_effect_desc_t;

typedef struct auplug_effect_t {
    void* impl;   /* plugin-private */

    void (*destroy)(struct auplug_effect_t* fx);

    /* Instance-level capability discovery; NULL if the id is unknown.
     * Known ids: AUPLUG_PARAMS_EXT_ID, AUPLUG_PROCESS_OFFLINE_EXT_ID. */
    const void* (*get_extension)(struct auplug_effect_t* fx, const char* extension_id);
} auplug_effect_t;

typedef struct auplug_effect_factory_t {
    uint32_t (*count)(void);
    const auplug_effect_desc_t* (*get_descriptor)(uint32_t index);

    /* Create an instance by descriptor id; NULL on failure. The caller
     * destroys it with fx->destroy. */
    auplug_effect_t* (*create)(const char* effect_id);
} auplug_effect_factory_t;

/* ====================================================================== */
/* Parameters extension: "audacity.params/1" (per effect instance)        */
/*                                                                        */
/* The host discovers each parameter's key/label/id via get_info(index),   */
/* then addresses it by auplug_param_id_t from then on — key/label are     */
/* used only for the host's own persisted settings store and its UI, never */
/* passed back to the plugin. Modeled on CLAP's params extension:          */
/* min_value/max_value are authoritative for every numeric type, including */
/* AUPLUG_PARAM_ENUM, and one bidirectional text-conversion pair covers     */
/* both numeric formatting and enum choice labels instead of a separate    */
/* enum-only accessor.                                                    */
/* ====================================================================== */

#define AUPLUG_PARAMS_EXT_ID "audacity.params/1"

/* Opaque to the host: never interpreted, only compared and passed back.
 * Plugin-assigned and stable for the plugin's lifetime; the simplest valid
 * choice is just the param's declaration index. */
typedef uint64_t auplug_param_id_t;

enum auplug_param_type {
    AUPLUG_PARAM_DOUBLE = 0,
    AUPLUG_PARAM_INT = 1,
    AUPLUG_PARAM_BOOL = 2,
    AUPLUG_PARAM_ENUM = 3,     /* value = choice index, an integer in [min_value, max_value] */
    AUPLUG_PARAM_STRING = 4
};

typedef struct auplug_param_info_t {
    uint32_t struct_size;
    auplug_param_id_t id;     /* passed to get_number/set_number/etc. below */
    const char* key;          /* stable settings key, ASCII, no spaces */
    const char* label;        /* display label (English) */
    int32_t type;             /* auplug_param_type */
    double min_value;         /* numeric and enum types; 0 otherwise. For
                                * AUPLUG_PARAM_ENUM this also defines the
                                * choice count (max_value - min_value + 1);
                                * may be computed at instance creation (e.g.
                                * models found on disk) — max_value < min_value
                                * means no choices are currently available. */
    double max_value;
    double default_value;
    const char* default_string;   /* AUPLUG_PARAM_STRING; NULL otherwise */
} auplug_param_info_t;

typedef struct auplug_params_ext_t {
    uint32_t (*count)(auplug_effect_t* fx);

    /* Fills 'out' (caller sets out->struct_size first). False if index is
     * out of range. */
    bool (*get_info)(auplug_effect_t* fx, uint32_t index, auplug_param_info_t* out);

    /* Values by id. Numbers cover double/int/bool(0|1)/enum(index). */
    double (*get_number)(auplug_effect_t* fx, auplug_param_id_t id);
    bool (*set_number)(auplug_effect_t* fx, auplug_param_id_t id, double value);
    const char* (*get_string)(auplug_effect_t* fx, auplug_param_id_t id);
    bool (*set_string)(auplug_effect_t* fx, auplug_param_id_t id, const char* value);

    /* Formats 'value' for display, e.g. an AUPLUG_PARAM_ENUM index -> its
     * choice label ("Sine"), or a numeric value -> a custom rendering
     * ("100 = no limit"). Fills out_buffer (out_capacity bytes,
     * NUL-terminated). False if the plugin has no display text for this
     * value; the host falls back to a plain numeric display, and for
     * AUPLUG_PARAM_ENUM to the value's decimal index. */
    bool (*value_to_text)(auplug_effect_t* fx, auplug_param_id_t id, double value,
                          char* out_buffer, uint32_t out_capacity);

    /* Inverse of value_to_text, e.g. "Square" -> 1.0 for an enum. False if
     * 'text' doesn't parse to a value for this id. */
    bool (*text_to_value)(auplug_effect_t* fx, auplug_param_id_t id, const char* text, double* out_value);
} auplug_params_ext_t;

/* ====================================================================== */
/* Offline processing extension: "audacity.process.offline/1"             */
/*                                                                        */
/* Whole-selection processing. The host prepares the selected audio and    */
/* passes a context with pull/push callbacks; the plugin reads input       */
/* tracks, computes, and writes results (audio in place, new tracks,      */
/* labels). All callbacks are called from the thread that called          */
/* process() and are valid only during that call.                         */
/* ====================================================================== */

#define AUPLUG_PROCESS_OFFLINE_EXT_ID "audacity.process.offline/1"

typedef struct auplug_audio_buffer_t {
    uint32_t struct_size;
    uint32_t channel_count;
    uint64_t frame_count;
    double sample_rate;
    float* const* channels;   /* planar; channels[c][frame] */
} auplug_audio_buffer_t;

typedef struct auplug_track_info_t {
    uint32_t struct_size;
    const char* name;         /* track display name; valid until process() returns */
    uint32_t channel_count;
    double sample_rate;       /* native rate */

    /* All times in seconds relative to the selection start. */
    double sel_start_sec;     /* this track's selected region (>= 0) */
    double sel_end_sec;
    double track_start_sec;   /* the track's full extent (may be negative) */
    double track_end_sec;
} auplug_track_info_t;

/* All callbacks are valid only during the process() call that received the
 * context. add_label() may be called from any thread; the other callbacks
 * (including progress()) must be called from the thread that called
 * process() — marshal worker-thread progress to that thread. */
typedef struct auplug_process_ctx_t {
    uint32_t struct_size;
    void* ctx;

    /* For generators: the requested output duration. For other effect types
     * the length of the selection, in seconds. */
    double duration_sec;

    /* --- input: one entry per selected track --- */
    uint32_t (*track_count)(void* ctx);

    /* Fills 'out' (caller sets out->struct_size). False if out of range. */
    bool (*track_info)(void* ctx, uint32_t track_index, auplug_track_info_t* out);

    /* Fills 'out' with host-owned buffers for the selected region of the
     * track, resampled to desired_sample_rate (0 = native rate). Buffers
     * stay valid until the next read on the same track index, or until
     * process() returns. Caller sets out->struct_size. */
    bool (*read_track)(void* ctx, uint32_t track_index, double desired_sample_rate, auplug_audio_buffer_t* out);

    /* Same, for an arbitrary [start_sec, end_sec) range of the track, in
     * seconds relative to the selection start (may be negative). The range
     * is clamped to the track extent. */
    bool (*read_track_range)(void* ctx, uint32_t track_index, double start_sec, double end_sec,
                             double desired_sample_rate, auplug_audio_buffer_t* out);

    /* --- output --- */

    /* Replace the selected region of the track. frame_count and sample_rate
     * may differ from the input: the host resamples to the track rate (or
     * adopts the buffer's rate if the track is empty) and, for generators,
     * shifts later clips when the output duration differs from the
     * selection. Channel counts are adapted (mixdown/duplication). */
    bool (*write_track)(void* ctx, uint32_t track_index, const auplug_audio_buffer_t* audio);

    /* Append a new audio track to the project (generators, separation),
     * with exactly the buffer's rate and channel count, placed at start_sec
     * seconds relative to the selection start (may be negative). */
    bool (*add_audio_track)(void* ctx, const char* name, double start_sec, const auplug_audio_buffer_t* audio);

    /* Add a label to a label track with the given name (created on first
     * use). Times are seconds relative to the selection start. */
    bool (*add_label)(void* ctx, const char* track_name, double start_sec, double end_sec, const char* text);

    /* --- progress --- */

    /* fraction in [0,1]; optional message may be NULL. Returns false if the
     * user cancelled: stop work and return AUPLUG_PROCESS_CANCELLED. */
    bool (*progress)(void* ctx, double fraction, const char* message);
} auplug_process_ctx_t;

enum auplug_process_result {
    AUPLUG_PROCESS_OK = 0,
    AUPLUG_PROCESS_ERROR = 1,
    AUPLUG_PROCESS_CANCELLED = 2
};

typedef struct auplug_process_offline_ext_t {
    /* Runs the effect with current parameter values. Blocking; the host
     * calls it off the UI thread or pumps progress itself. */
    int32_t (*process)(auplug_effect_t* fx, const auplug_process_ctx_t* pctx);

    /* Human-readable description of the last AUPLUG_PROCESS_ERROR. */
    const char* (*last_error)(auplug_effect_t* fx);
} auplug_process_offline_ext_t;

/* ====================================================================== */
/* Importer factory: "audacity.importer-factory/1"                        */
/*                                                                        */
/* File-format importers. The host consults descriptors when the user     */
/* opens a file; on an extension match it probes with can_open and calls  */
/* import(), which decodes the file and pushes audio through the context. */
/* ====================================================================== */

#define AUPLUG_IMPORTER_FACTORY_ID "audacity.importer-factory/1"

typedef struct auplug_importer_desc_t {
    uint32_t struct_size;
    const char* id;           /* stable unique id, e.g. "ogg.vorbis.import" */
    const char* format_name;  /* short format name (English), e.g. "Ogg Vorbis" */
    const char* extensions;   /* semicolon-separated, e.g. "ogg;oga" */
} auplug_importer_desc_t;

/* Sink for decoded audio; valid only during the import() call. */
typedef struct auplug_import_ctx_t {
    uint32_t struct_size;
    void* ctx;

    /* Start a stream of audio: following append() calls feed it. May be
     * called again (e.g. chained files); each call starts a new set of
     * tracks. */
    bool (*begin_stream)(void* ctx, uint32_t channel_count, double sample_rate);

    /* Append decoded audio to the current stream. The buffer's channel
     * count and rate must match begin_stream. */
    bool (*append)(void* ctx, const auplug_audio_buffer_t* audio);

    /* File metadata, e.g. from Vorbis comments. */
    bool (*add_tag)(void* ctx, const char* key, const char* value);

    /* fraction in [0,1]. Returns false if the user cancelled: stop work
     * and return AUPLUG_PROCESS_CANCELLED. */
    bool (*progress)(void* ctx, double fraction);
} auplug_import_ctx_t;

typedef struct auplug_importer_factory_t {
    uint32_t (*count)(void);

    /* Fills 'out' (caller sets out->struct_size). False if out of range. */
    bool (*get_descriptor)(uint32_t index, auplug_importer_desc_t* out);

    /* Cheap content probe (magic bytes): can this importer open the file?
     * The host calls it before import() for files with a matching
     * extension. */
    bool (*can_open)(uint32_t index, const char* path);

    /* Decode 'path', pushing audio and tags through the context. Returns
     * an auplug_process_result. Blocking. */
    int32_t (*import)(uint32_t index, const char* path, const auplug_import_ctx_t* ictx);

    /* Human-readable description of the last AUPLUG_PROCESS_ERROR. */
    const char* (*last_error)(uint32_t index);
} auplug_importer_factory_t;

/* ====================================================================== */
/* Exporter factory: "audacity.exporter-factory/1"                        */
/*                                                                        */
/* File-format exporters. The format appears in the host's export dialog; */
/* on export the host passes a context yielding the mixed-down audio and  */
/* the plugin encodes it into the target file.                            */
/* ====================================================================== */

#define AUPLUG_EXPORTER_FACTORY_ID "audacity.exporter-factory/1"

typedef struct auplug_exporter_desc_t {
    uint32_t struct_size;
    const char* id;           /* stable unique id, e.g. "ogg.vorbis.export" */
    const char* format_name;  /* shown in the format list (English), e.g. "Ogg Vorbis Files" */
    const char* extension;    /* default file extension, e.g. "ogg" */
    uint32_t max_channels;
} auplug_exporter_desc_t;

/* Source of mixed audio; valid only during the export_file() call. */
typedef struct auplug_export_ctx_t {
    uint32_t struct_size;
    void* ctx;

    double sample_rate;       /* output rate chosen in the export dialog */
    uint32_t channel_count;   /* output channel count */

    /* Pull the next chunk of at most max_frames. Fills 'out' (caller sets
     * out->struct_size) with host-owned planar buffers that stay valid
     * until the next read() call. Returns false when no audio is left --
     * also on user cancel, so finalize the file and return
     * AUPLUG_PROCESS_OK; the host reports the cancellation itself. The
     * host updates the progress indicator on each call. */
    bool (*read)(void* ctx, uint32_t max_frames, auplug_audio_buffer_t* out);

    /* Metadata to embed, if the format supports it. */
    uint32_t (*tag_count)(void* ctx);
    bool (*get_tag)(void* ctx, uint32_t index, const char** key, const char** value);
} auplug_export_ctx_t;

typedef struct auplug_exporter_factory_t {
    uint32_t (*count)(void);

    /* Fills 'out' (caller sets out->struct_size). False if out of range. */
    bool (*get_descriptor)(uint32_t index, auplug_exporter_desc_t* out);

    /* Encode the context's audio into 'path'. Returns an
     * auplug_process_result. Blocking. */
    int32_t (*export_file)(uint32_t index, const char* path, const auplug_export_ctx_t* ectx);

    /* Human-readable description of the last AUPLUG_PROCESS_ERROR. */
    const char* (*last_error)(uint32_t index);
} auplug_exporter_factory_t;

/* ====================================================================== */
/* Host DSP extension: "audacity.dsp/1"                                   */
/*                                                                        */
/* Utility DSP provided by the host (via auplug_host_t::get_extension) so   */
/* plugins don't have to ship their own resampler. Available from init     */
/* until deinit.                                                          */
/* ====================================================================== */

#define AUPLUG_DSP_EXT_ID "audacity.dsp/1"

typedef struct auplug_dsp_ext_t {
    /* High-quality resample of 'in' to target_rate. Fills 'out' (caller
     * sets out->struct_size) with host-owned buffers that stay valid until
     * the next resample() call on the same thread. Chaining is supported:
     * 'in' may alias this function's previous result on the same thread.
     * Callable from any thread; calls on different threads don't
     * interfere. */
    bool (*resample)(void* host_ctx, const auplug_audio_buffer_t* in, double target_rate, auplug_audio_buffer_t* out);
} auplug_dsp_ext_t;

/* ====================================================================== */
/* Views: "audacity.views/1" (plugin level)                               */
/*                                                                        */
/* A plugin may ship QML views as UTF-8 source text. The host instantiates */
/* them in a sandboxed QML engine; the QML must not import application-    */
/* internal QML modules. Views bind to host-provided context objects (an   */
/* 'effect' object exposing the parameter model for effect views).        */
/* ====================================================================== */

#define AUPLUG_VIEWS_ID "audacity.views/1"

enum auplug_view_role {
    AUPLUG_VIEW_EFFECT = 0,          /* settings view; ref = effect id */
    AUPLUG_VIEW_PLUGIN_CONFIG = 1    /* plugin configuration view; ref unused */
};

typedef struct auplug_view_desc_t {
    uint32_t struct_size;
    int32_t role;             /* auplug_view_role */
    const char* ref;          /* effect id for AUPLUG_VIEW_EFFECT; else NULL */
    const char* qml;          /* complete QML source, UTF-8; plugin lifetime */
} auplug_view_desc_t;

typedef struct auplug_views_t {
    uint32_t (*count)(void);

    /* Fills 'out' (caller sets out->struct_size). False if out of range. */
    bool (*get_view)(uint32_t index, auplug_view_desc_t* out);
} auplug_views_t;

#ifdef __cplusplus
}
#endif

#endif /* AUDACITY_PLUGIN_API_H */
