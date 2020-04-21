#include <libavutil/avutil.h>

typedef struct FormatReport {
    /* Must be set for sanity */
    enum AVMediaType type;

    /* Video - all must be set */
    int width;
    int height;
    enum AVPixelFormat pix_fmt;
    AVRational avg_frame_rate;

    /* Audio - all must be set */
    int sample_rate;
    enum AVSampleFormat sample_fmt;
    uint64_t channel_layout;

    /* Common - all must be set */
    AVRational time_base;
} FormatReport;

typedef struct SourceInfo {
    char *name;
    char *desc;
    uint64_t identifier;
} SourceInfo;

typedef int (report_format)(void *opaque, FormatReport *info);
typedef void (report_error)(void *opaque, int error_code);

typedef const struct CaptureSource {
    /**
     * Capture API name
     */
    const char *name;

    /**
     * Initialize the API
     * The report_error callback may be called at any time from any thread
     */
    int  (*init)(void **s, report_error *err_cb, void *err_opaque);

    /**
     * Get a list of sources, memory is handled by the capture source
     */
    void (*sources)(void *s, SourceInfo **sources, int *num);

    /**
     * Start encoding and dump frames into the FIFO given.
     * Will error out if called multiple times on the same identifier.
     * The info function:
     *     - will be called every single time the format changes
     *     - will be called before the first frame is pushed to the FIFO
     *     - can be called from a different thread (or not)
     * To avoid race conditions, just call peek_from_fifo() right after
     */
    int  (*start)(void *s, uint64_t identifier, AVDictionary *opts,
                  AVFrameFIFO *dst, report_format *info_cb, void *info_cb_ctx);

    /**
     * Stops capture from a given identifier. Returns an error if not found.
     */
    int  (*stop)(void  *s, uint64_t identifier);

    /**
     * Stops and frees the capture API
     */
    void (*free)(void **s);
} CaptureSource;

extern const CaptureSource src_pulse;
extern const CaptureSource src_wayland;
