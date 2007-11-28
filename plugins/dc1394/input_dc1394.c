#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dc1394/control.h>
#include <dc1394/vendor/avt.h>
#include <libraw1394/raw1394.h>
#include <math.h>

//#define USE_CONFIG
#ifdef USE_CONFIG
#include <dgc/config_util.h>
#endif

#ifdef USE_RAW1394
#include "capture_raw1394.h"
#endif

#include <libcam/dbg.h>
#include <libcam/pixels.h>
#include <libcam/plugin.h>
#include "input_dc1394.h"

#define NUM_BUFFERS 60

#define VENDOR_ID_POINT_GREY 0xb09d


#define err(args...) fprintf (stderr, args)

static CamUnit * driver_create_unit (CamUnitDriver * super,
        const CamUnitDescription * udesc);
static void driver_finalize (GObject * obj);
static int driver_start (CamUnitDriver * super);
static int driver_stop (CamUnitDriver * super);
static int add_all_camera_controls (CamUnit * super);

CAM_PLUGIN_TYPE (CamDC1394Driver, cam_dc1394_driver, CAM_TYPE_UNIT_DRIVER);

static void
cam_dc1394_driver_init (CamDC1394Driver * self)
{
    dbg (DBG_DRIVER, "dc1394 driver constructor\n");
    CamUnitDriver * super = CAM_UNIT_DRIVER (self);
    cam_unit_driver_set_name (super, "input", "dc1394");

    self->cameras = NULL;
    self->num_cameras = 0;
}

static void
cam_dc1394_driver_class_init (CamDC1394DriverClass * klass)
{
    dbg (DBG_DRIVER, "dc1394 driver class initializer\n");
    GObjectClass * gobject_class = G_OBJECT_CLASS (klass);
    gobject_class->finalize = driver_finalize;
    klass->parent_class.create_unit = driver_create_unit;
    klass->parent_class.start = driver_start;
    klass->parent_class.stop = driver_stop;
}

static void
driver_finalize (GObject * obj)
{
    dbg (DBG_DRIVER, "dc1394 driver finalize\n");
    CamDC1394Driver * self = CAM_DC1394_DRIVER (obj);

    if (self->num_cameras)
        err ("Warning: dc1394 driver finalized before stopping\n");

    G_OBJECT_CLASS (cam_dc1394_driver_parent_class)->finalize (obj);
}

static int
driver_start (CamUnitDriver * super)
{
    CamDC1394Driver * self = CAM_DC1394_DRIVER (super);

    if (dc1394_find_cameras (&self->cameras, &self->num_cameras)
            != DC1394_SUCCESS)
        return -1;

    int i;
    for (i = 0; i < self->num_cameras; i++) {
        char name[256], id[32];

        snprintf (name, sizeof (name), "%s %s",
                self->cameras[i]->vendor,
                self->cameras[i]->model);

        int64_t uid = self->cameras[i]->euid_64;
        snprintf (id, sizeof (id), "%"PRIx64, uid);

        CamUnitDescription * udesc = 
            cam_unit_driver_add_unit_description (super, name, id, 
                CAM_UNIT_EVENT_METHOD_FD);

        g_object_set_data (G_OBJECT(udesc), "dc1394-driver-index",
                GINT_TO_POINTER(i));
    }
    
    return 0;
}

static int
driver_stop (CamUnitDriver * super)
{
    CamDC1394Driver * self = CAM_DC1394_DRIVER (super);

    int i;
    for (i = 0; i < self->num_cameras; i++) {
        dc1394_free_camera (self->cameras[i]);
    }
    free (self->cameras);

    self->num_cameras = 0;
    self->cameras = NULL;

    return CAM_UNIT_DRIVER_CLASS (cam_dc1394_driver_parent_class)->stop (super);
}

static CamUnit *
driver_create_unit (CamUnitDriver * super, const CamUnitDescription * udesc)
{
    CamDC1394Driver * self = CAM_DC1394_DRIVER (super);

    dbg (DBG_DRIVER, "dc1394 driver creating new unit\n");

    g_assert (udesc->driver == super);

    int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT(udesc), 
                "dc1394-driver-index"));

    if (idx >= self->num_cameras) {
        fprintf (stderr, "Error: invalid unit id %s\n", udesc->unit_id);
        return NULL;
    }

    CamDC1394 * result = cam_dc1394_new (self->cameras[idx]);

    return CAM_UNIT (result);
}

CAM_PLUGIN_TYPE (CamDC1394, cam_dc1394, CAM_TYPE_UNIT);

void
cam_plugin_initialize (GTypeModule * module)
{
    cam_dc1394_driver_register_type (module);
    cam_dc1394_register_type (module);
}

CamUnitDriver *
cam_plugin_create (GTypeModule * module)
{
    return CAM_UNIT_DRIVER (g_object_new (CAM_DC1394_DRIVER_TYPE, NULL));
}

#define CAM_DC1394_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CAM_DC1394_TYPE, CamDC1394Private))
typedef struct _CamDC1394Private CamDC1394Private;

struct _CamDC1394Private {
#ifdef USE_RAW1394
    CaptureRaw1394 * raw1394;
#endif
    int embedded_timestamp;
    raw1394handle_t raw1394_handle;
    int raw1394_fd;
};

static void
cam_dc1394_init (CamDC1394 * self)
{
    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);
    dbg (DBG_INPUT, "dc1394 constructor\n");

    priv->embedded_timestamp = 0;
    self->num_buffers = NUM_BUFFERS;
}

static void dc1394_finalize (GObject * obj);
static int dc1394_stream_init (CamUnit * super, const CamUnitFormat * format);
static int dc1394_stream_shutdown (CamUnit * super);
static int dc1394_stream_on (CamUnit * super);
static int dc1394_stream_off (CamUnit * super);
static void dc1394_try_produce_frame (CamUnit * super);
static int dc1394_get_fileno (CamUnit * super);
static gboolean dc1394_try_set_control(CamUnit *super,
        const CamUnitControl *ctl, const GValue *proposed, GValue *actual);

static void
cam_dc1394_class_init (CamDC1394Class * klass)
{
    dbg (DBG_INPUT, "dc1394 class initializer\n");
    GObjectClass * gobject_class = G_OBJECT_CLASS (klass);
    gobject_class->finalize = dc1394_finalize;

    klass->parent_class.stream_init = dc1394_stream_init;
    klass->parent_class.stream_shutdown = dc1394_stream_shutdown;
    klass->parent_class.stream_on = dc1394_stream_on;
    klass->parent_class.stream_off = dc1394_stream_off;
    klass->parent_class.try_produce_frame = dc1394_try_produce_frame;
    klass->parent_class.get_fileno = dc1394_get_fileno;
    klass->parent_class.try_set_control = dc1394_try_set_control;

    g_type_class_add_private (gobject_class, sizeof (CamDC1394Private));
}

static void
dc1394_finalize (GObject * obj)
{
    dbg (DBG_INPUT, "dc1394 finalize\n");
    CamUnit * super = CAM_UNIT (obj);

    if (super->status != CAM_UNIT_STATUS_IDLE) {
        dbg (DBG_INPUT, "forcibly shutting down dc1394 unit\n");
        dc1394_stream_shutdown (super);
    }

    G_OBJECT_CLASS (cam_dc1394_parent_class)->finalize (obj);
}

static CamPixelFormat
dc1394_pixel_format (dc1394color_coding_t color,
        dc1394color_filter_t filter)
{
    switch (color) {
        case DC1394_COLOR_CODING_MONO8:
        case DC1394_COLOR_CODING_RAW8:
            switch (filter) {
                case DC1394_COLOR_FILTER_RGGB:
                    return CAM_PIXEL_FORMAT_BAYER_RGGB;
                case DC1394_COLOR_FILTER_GBRG:
                    return CAM_PIXEL_FORMAT_BAYER_GBRG;
                case DC1394_COLOR_FILTER_GRBG:
                    return CAM_PIXEL_FORMAT_BAYER_GRBG;
                case DC1394_COLOR_FILTER_BGGR:
                    return CAM_PIXEL_FORMAT_BAYER_BGGR;
                default:
                    return CAM_PIXEL_FORMAT_GRAY;
            }
        case DC1394_COLOR_CODING_YUV411:
            return CAM_PIXEL_FORMAT_IYU1;
        case DC1394_COLOR_CODING_YUV422:
            return CAM_PIXEL_FORMAT_UYVY;
        case DC1394_COLOR_CODING_YUV444:
            return CAM_PIXEL_FORMAT_IYU2;
        case DC1394_COLOR_CODING_RGB8:
            return CAM_PIXEL_FORMAT_RGB;
        case DC1394_COLOR_CODING_MONO16:
            return CAM_PIXEL_FORMAT_GRAY16;
        case DC1394_COLOR_CODING_RGB16:
            return CAM_PIXEL_FORMAT_RGB16;
        case DC1394_COLOR_CODING_MONO16S:
            return CAM_PIXEL_FORMAT_SIGNED_GRAY16;
        case DC1394_COLOR_CODING_RGB16S:
            return CAM_PIXEL_FORMAT_SIGNED_RGB16;
            //return CAM_PIXEL_FORMAT_BAYER;
        case DC1394_COLOR_CODING_RAW16:
            return CAM_PIXEL_FORMAT_GRAY16;
            //return CAM_PIXEL_FORMAT_BAYER16;
    }
    return 0;
}

static int
setup_embedded_timestamps (CamDC1394 * self)
{
    uint32_t value;
    if (GetCameraAdvControlRegister (self->cam, 0x2F8, &value) !=
            DC1394_SUCCESS)
        return -1;

    if (!(value & 0x80000000))
        return -1;

    value |= 0x1;

    if (SetCameraAdvControlRegister (self->cam, 0x2F8, value) !=
            DC1394_SUCCESS)
        return -1;

    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);
    priv->embedded_timestamp = 1;
    printf ("Enabled embedded timestamps for Point Grey camera\n");
    return 0;
}

CamDC1394 *
cam_dc1394_new (dc1394camera_t * cam)
{
    CamDC1394 * self =
        CAM_DC1394 (g_object_new (CAM_DC1394_TYPE, NULL));

    self->cam = cam;

    printf ("Found camera with UID 0x%"PRIx64"\n", cam->euid_64);

    dc1394format7modeset_t info;

    if (dc1394_video_set_mode (cam, DC1394_VIDEO_MODE_FORMAT7_0) !=
            DC1394_SUCCESS)
        goto fail;
    if (dc1394_format7_get_modeset (cam, &info) != DC1394_SUCCESS)
        goto fail;
    
    int i;
    for (i = 0; i < DC1394_VIDEO_MODE_FORMAT7_NUM; i++) {
        char name[256];
        dc1394format7mode_t * mode = info.mode + i;
        if (!info.mode[i].present)
            continue;

        int j;
        for (j = 0; j < mode->color_codings.num; j++) {
            CamPixelFormat pix =
                dc1394_pixel_format (mode->color_codings.codings[j],
                        mode->color_filter);
            sprintf (name, "%dx%d %s", mode->max_size_x, mode->max_size_y,
                    cam_pixel_format_str (pix));

            int stride = mode->max_size_x * cam_pixel_format_bpp(pix) / 8;

            CamUnitFormat * fmt = 
                cam_unit_add_output_format_full (CAM_UNIT (self), pix,
                    name, mode->max_size_x, mode->max_size_y, 
                    stride, 
                    mode->max_size_y * stride);

            g_object_set_data (G_OBJECT (fmt), "input_dc1394-format7-mode",
                    GINT_TO_POINTER (i));
            g_object_set_data (G_OBJECT (fmt), "input_dc1394-color-coding",
                    GINT_TO_POINTER (j));
        }
    }

    add_all_camera_controls (CAM_UNIT (self));

    if (self->cam->vendor_id == VENDOR_ID_POINT_GREY)
        setup_embedded_timestamps (self);

    return self;

fail:
    g_object_unref (G_OBJECT (self));
    return NULL;
}

static int
dc1394_stream_init (CamUnit * super, const CamUnitFormat * format)
{
    CamDC1394 * self = CAM_DC1394 (super);
    dbg (DBG_INPUT, "Initializing DC1394 stream (pxlfmt 0x%x %dx%d)\n",
            format->pixelformat, format->width, format->height);

    dc1394format7modeset_t info;
    dc1394_format7_get_modeset (self->cam, &info);

    int i = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (format), 
                "input_dc1394-format7-mode"));
    int j = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (format),
                "input_dc1394-color-coding"));
    dc1394format7mode_t * mode = info.mode + i;
    dc1394color_coding_t color_coding = mode->color_codings.codings[j];
    
    if (format->pixelformat != dc1394_pixel_format (color_coding,
                mode->color_filter) ||
            format->width != mode->max_size_x ||
            format->height != mode->max_size_y)
        goto fail;

    dc1394video_mode_t vidmode = DC1394_VIDEO_MODE_FORMAT7_0 + i;

    dc1394_video_set_mode (self->cam, vidmode);
    dc1394_video_set_iso_speed (self->cam, DC1394_ISO_SPEED_400);

    int width, height;
    dc1394_format7_set_image_size (self->cam, vidmode,
            format->width, format->height);
    dc1394_format7_set_image_position (self->cam, vidmode, 0, 0);
    width = format->width;
    height = format->height;

    dc1394_format7_set_color_coding (self->cam, vidmode, color_coding);

    uint32_t psize_unit, psize_max;
    dc1394_format7_get_packet_para (self->cam, vidmode, &psize_unit,
            &psize_max);

    CamUnitControl * ctl = cam_unit_find_control (super, "packet-size");
    int desired_packet = cam_unit_control_get_int (ctl);

    desired_packet = (desired_packet / psize_unit) * psize_unit;
    if (desired_packet > psize_max)
        desired_packet = psize_max;
    if (desired_packet < psize_unit)
        desired_packet = psize_unit;

    self->packet_size = desired_packet;

    cam_unit_control_force_set_int (ctl, desired_packet);


#if 0
    dc1394_format7_get_recommended_byte_per_packet (self->cam,
            vidmode, &self->packet_size);
    dbg (DBG_INPUT, "DC1394: Using device-recommended packet size of %d\n",
            self->packet_size);
#endif
    if (self->packet_size == 0)
        self->packet_size = 4096;

    dc1394_format7_set_byte_per_packet (self->cam, vidmode, self->packet_size);

    uint64_t bytes_per_frame;
    dc1394_format7_get_total_bytes (self->cam, vidmode, &bytes_per_frame);

//  FIXME
//    super->stride = bytes_per_frame / height;

    if (bytes_per_frame * self->num_buffers > 25000000) {
        printf ("Reducing dc1394 buffers from %d to ", self->num_buffers);
        self->num_buffers = 25000000 / bytes_per_frame;
        printf ("%d\n", self->num_buffers);
    }

#ifdef USE_RAW1394
    /* Using libraw1394 for the iso streaming */
    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);
    priv->raw1394 = capture_raw1394_new (self->cam, super,
            self->packet_size,
            self->num_buffers * bytes_per_frame / self->packet_size);
            //320);
    if (!priv->raw1394)
        goto fail;

    self->fd = capture_raw1394_get_fileno (priv->raw1394);
#else
    /* Using libdc1394 for iso streaming */
    if (dc1394_capture_setup (self->cam, self->num_buffers,
                DC1394_CAPTURE_FLAGS_DEFAULT) != DC1394_SUCCESS)
        goto fail;

    self->fd = dc1394_capture_get_fileno (self->cam);
#endif

    return 0;

fail:
    fprintf (stderr, "Error: failed to initialize dc1394 stream\n");
    fprintf (stderr, "\nIF YOU HAVE HAD A CAMERA FAIL TO EXIT CLEANLY OR\n");
    fprintf (stderr, " THE BANDWIDTH HAS BEEN OVER SUBSCRIBED TRY (to reset):\n");
    fprintf (stderr, "dc1394_reset_bus\n\n");
    return -1;
}

static int
dc1394_stream_shutdown (CamUnit * super)
{
    CamDC1394 * self = CAM_DC1394 (super);

    dbg (DBG_INPUT, "Shutting down DC1394 stream\n");

#ifdef USE_RAW1394
    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);
    capture_raw1394_free (priv->raw1394);
    priv->raw1394 = NULL;
#else
    dc1394_capture_stop (self->cam);
#endif

    /* chain up to parent, which handles some of the work */
    return CAM_UNIT_CLASS (cam_dc1394_parent_class)->stream_shutdown (super);
}

static int
dc1394_stream_on (CamUnit * super)
{
    CamDC1394 * self = CAM_DC1394 (super);

    dbg (DBG_INPUT, "DC1394 stream on\n");

    if (dc1394_video_set_transmission (self->cam, DC1394_ON) !=
            DC1394_SUCCESS)
        return -1;

    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);
    priv->raw1394_handle = raw1394_new_handle ();
    raw1394_set_port (priv->raw1394_handle, 0);
    priv->raw1394_fd = raw1394_get_fd (priv->raw1394_handle);

    return 0;
}

static int
dc1394_stream_off (CamUnit * super)
{
    CamDC1394 * self = CAM_DC1394 (super);

    dbg (DBG_INPUT, "DC1394 stream off\n");

    dc1394_video_set_transmission (self->cam, DC1394_OFF);

    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);
    raw1394_destroy_handle (priv->raw1394_handle);

    return 0;
}

#define CYCLE_TIMER_TO_USEC(cycle,secmask) (\
        (((uint32_t)cycle >> 25) & secmask) * 1000000 + \
        (((uint32_t)cycle & 0x01fff000) >> 12) * 125 + \
        ((uint32_t)cycle & 0x00000fff) * 125 / 3072)

#define CYCLE_TIMER_MAX_USEC(secmask)  ((secmask+1)*1000000)

enum {
    TS_SHORT,
    TS_LONG
};

#ifndef raw1394_cycle_timer
struct raw1394_cycle_timer {
	/* contents of Isochronous Cycle Timer register,
	   as in OHCI 1.1 clause 5.13 (also with non-OHCI hosts) */
	uint32_t cycle_timer;

	/* local time in microseconds since Epoch,
	   simultaneously read with cycle timer */
	uint64_t local_time;
};
#define RAW1394_IOC_GET_CYCLE_TIMER		\
	_IOR ('#', 0x30, struct raw1394_cycle_timer)
#endif

static void
dc1394_try_produce_frame (CamUnit * super)
{
    CamDC1394 * self = CAM_DC1394 (super);

    dbg (DBG_INPUT, "DC1394 stream iterate\n");

    if (super->status != CAM_UNIT_STATUS_STREAMING) return;

#ifdef USE_RAW1394
    capture_raw1394_iterate (priv->raw1394);
#else
    dc1394video_frame_t * frame;
    if (dc1394_capture_dequeue (self->cam, DC1394_CAPTURE_POLICY_WAIT, &frame)
            != DC1394_SUCCESS) {
        err ("DC1394 dequeue failed\n");
        return;
    }

    // TODO don't malloc
    CamFrameBuffer * buf = 
        cam_framebuffer_new (frame->image, frame->image_bytes);

    if (frame->frames_behind >= self->num_buffers-2)
        fprintf (stderr, "Warning: video1394 buffer contains %d frames, "
                "probably dropped frames...\n",
                frame->frames_behind);
    
    buf->data = frame->image;
    buf->length = frame->image_bytes;
    buf->bytesused = frame->image_bytes;

    CamDC1394Private * priv = CAM_DC1394_GET_PRIVATE (self);

    struct raw1394_cycle_timer ct = { 0xffffffff, 0 };

    // XXX gross hack...
    for (int i=0; i<100 && ct.cycle_timer == 0xffffffff; i++) {
        ioctl (priv->raw1394_fd, RAW1394_IOC_GET_CYCLE_TIMER, &ct);
    }
//    printf ("cyctime: %x\n", ct.cycle_timer);

    if (priv->embedded_timestamp && ct.cycle_timer != 0xffffffff) {
        uint32_t bus_timestamp = (buf->data[0] << 24) |
            (buf->data[1] << 16) | (buf->data[2] << 8) |
            buf->data[3];
        /* bottom 4 bits of cycle offset will be a frame count */
        bus_timestamp &= 0xfffffff0;

        uint32_t cycle_usec_now = CYCLE_TIMER_TO_USEC (ct.cycle_timer, 0x7f);

        int usec_diff = cycle_usec_now -
            CYCLE_TIMER_TO_USEC (bus_timestamp, 0x7f);
        if (usec_diff < 0)
            usec_diff += CYCLE_TIMER_MAX_USEC (0x7f);

        buf->timestamp = ct.local_time - usec_diff;
    }
    else {
        buf->timestamp = frame->timestamp;
    }
    char str[20];
    sprintf (str, "0x%016"PRIx64, self->cam->euid_64);
    cam_framebuffer_metadata_set (buf, "Source GUID", (uint8_t *) str,
            strlen (str));

    cam_unit_produce_frame (super, buf, super->fmt);

    dc1394_capture_enqueue (self->cam, frame);

    g_object_unref (buf);
#endif

//    int ts_type = TS_SHORT;
// TODO David - FIXME
//    int i;
//    uint32_t cycle_usec_now;
//    uint64_t systime = 0;
//    int sec_mask = 0;
//    uint32_t cyctime;
//    for (i = 0; i < g_queue_get_length (super->outgoing_q); i++) {
//        CamFrameBuffer * b = g_queue_peek_nth (super->outgoing_q, i);
//        if (b->timestamp != 0 ||
//                (b->bus_timestamp == 0 && !priv->embedded_timestamp))
//            continue;
//
//        if (priv->embedded_timestamp) {
//            ts_type = TS_LONG;
//            b->bus_timestamp = (b->data[0] << 24) |
//                (b->data[1] << 16) | (b->data[2] << 8) |
//                b->data[3];
//            /* bottom 4 bits of cycle offset will be a frame count */
//            b->bus_timestamp &= 0xfffffff0;
//        }
//
//        if (systime == 0) {
//            if (dc1394_read_cycle_timer (self->cam, &cyctime,
//                        &systime) != DC1394_SUCCESS)
//                break;
//            sec_mask = (ts_type == TS_SHORT) ? 0x7 : 0x7f;
//            cycle_usec_now = CYCLE_TIMER_TO_USEC (cyctime, sec_mask);
//        }
//
//        int usec_diff = cycle_usec_now -
//            CYCLE_TIMER_TO_USEC (b->bus_timestamp, sec_mask);
//        if (usec_diff < 0)
//            usec_diff += CYCLE_TIMER_MAX_USEC (sec_mask);
//
//        b->timestamp = systime - usec_diff;
//
//#if 0
//        printf ("%.3f %.3f bus %x nowbus %x d %d\n",
//                (double) systime / 1000000.0,
//                (double) b->timestamp / 1000000.0,
//                b->bus_timestamp, cyctime, usec_diff / 1000);
//#endif
//    }

    return;
}

static int
dc1394_get_fileno (CamUnit * super)
{
    CamDC1394 * self = CAM_DC1394 (super);

    if (super->status != CAM_UNIT_STATUS_IDLE)
        return self->fd;
    else
        return -1;
}

static const char * trigger_mode_desc[] = {
    "Off",
    "Start integration (Mode 0)",
    "Bulb shutter (Mode 1)",
    "Integrate to Nth (Mode 2)",
    "Every Nth frame (Mode 3)",
    "Mult. exposures (Mode 4)",
    "Mult. bulb exposures (Mode 5)",
    "Vendor-specific (Mode 14)",
    "Vendor-specific (Mode 15)",
    NULL
};
#define NUM_TRIGGER_MODES 9

static const char * feature_ids[] = {
    "brightness",
    "exposure",
    "sharpness",
    "white-balance",
    "hue",
    "saturation",
    "gamma",
    "shutter",
    "gain",
    "iris",
    "focus",
    "temperature",
    "trigger",
    "trigger-delay",
    "white-shading",
    "frame-rate",
    "zoom",
    "pan",
    "tilt",
    "optical-filter",
    "capture-size",
    "capture-quality",
};

static const char * feature_desc[] = {
    "Brightness",
    "Exposure",
    "Sharpness",
    "White Bal.",
    "Hue",
    "Saturation",
    "Gamma",
    "Shutter",
    "Gain",
    "Iris",
    "Focus",
    "Temperature",
    "Trigger",
    "Trig. Delay",
    "White Shading",
    "Frame Rate",
    "Zoom",
    "Pan",
    "Tilt",
    "Optical Filter",
    "Capture Size",
    "Capture Qual.",
};

static const char * trigger_source_desc[] = {
    "Trigger Source 0",
    "Trigger Source 1",
    "Trigger Source 2",
    "Trigger Source 3",
    "Software Trigger",
    NULL
};

static const char * feature_state_desc[] = {
    "Off",
    "Auto",
    "Manual",
    NULL
};
#define NUM_FEATURE_STATES 3

#define NUM_FLOAT_STEPS 100

static int
add_all_camera_controls (CamUnit * super)
{
    CamDC1394 * self = CAM_DC1394 (super);
    dc1394featureset_t features;
    CamUnitControl * ctl;

    dc1394_get_camera_feature_set (self->cam, &features);

    int i, reread = 0;
    for (i = 0; i < DC1394_FEATURE_NUM; i++) {
        dc1394feature_info_t * f = features.feature + i;
        if (f->available && f->absolute_capable && !f->abs_control) {
            fprintf (stderr, "Enabling absolute control of \"%s\"\n",
                    feature_desc[i]);
            dc1394_feature_set_absolute_control (self->cam, f->id, DC1394_ON);
            reread = 1;
        }
    }
    if (reread)
        dc1394_get_camera_feature_set (self->cam, &features);

    cam_unit_add_control_int (super, "packet-size",
            "Packet Size", 1, 4192, 1, 4192, 1);

    for (i = 0; i < DC1394_FEATURE_NUM; i++) {
        dc1394feature_info_t * f = features.feature + i;

        if (!f->available)
            continue;
#if 0
        fprintf (stderr, "%s, one-push %d abs %d read %d on_off %d auto %d manual %d\n",
                dc1394_feature_desc[i], f->one_push, f->absolute_capable, f->readout_capable,
                f->on_off_capable, f->auto_capable, f->manual_capable);
        fprintf (stderr, "  on %d polar %d auto_active %d min %d max %d value %d\n",
                f->is_on, f->polarity_capable, f->auto_active, f->min, f->max, f->value);
        fprintf (stderr, "  value %f max %f min %f\n", f->abs_value, f->abs_max, f->abs_min);
#endif

#if 0
        if (f->one_push)
            fprintf (stderr, "Warning: One-push available on control \"%s\"\n",
                    feature_desc[i]);
#endif

        if (f->id == DC1394_FEATURE_TRIGGER) {
            int entries_enabled[NUM_TRIGGER_MODES];
            memset (entries_enabled, 0, NUM_TRIGGER_MODES * sizeof (int));
            int j, cur_val = 0;
            for (j = 0; j < f->trigger_modes.num; j++) {
                entries_enabled[f->trigger_modes.modes[j] -
                    DC1394_TRIGGER_MODE_0 + 1] = 1;
                if (f->trigger_mode == f->trigger_modes.modes[j])
                    cur_val = f->trigger_modes.modes[j] -
                        DC1394_TRIGGER_MODE_0 + 1;
            }
            if (f->on_off_capable) {
                entries_enabled[0] = 1;
                if (f->is_on == DC1394_OFF)
                    cur_val = 0;
            }
            cam_unit_add_control_enum (super, "trigger", "Trigger", 
                    cur_val, 1,
                    trigger_mode_desc, entries_enabled);

            int aux_enabled = 0;
            if (cur_val > 0)
                aux_enabled = 1;

            /* Add trigger polarity selection */
            if (f->polarity_capable)
                cam_unit_add_control_boolean (super,
                        "trigger-polarity", "Polarity",
                        f->trigger_polarity, aux_enabled);

            /* Add trigger source selection */
            int sources_enabled[DC1394_TRIGGER_SOURCE_NUM];
            memset (sources_enabled, 0, DC1394_TRIGGER_SOURCE_NUM * sizeof (int));
            for (j = 0; j < f->trigger_sources.num; j++) {
                int source = f->trigger_sources.sources[j];
                sources_enabled[source - DC1394_TRIGGER_SOURCE_MIN] = 1;
                if (f->trigger_source == source)
                    cur_val = source - DC1394_TRIGGER_SOURCE_MIN;
            }
            cam_unit_add_control_enum (super, "trigger-source",
                    "Source", cur_val, aux_enabled, trigger_source_desc,
                    sources_enabled);

            /* Add one-shot software trigger */
            if (sources_enabled[CAM_DC1394_TRIGGER_SOURCE_SOFTWARE]) {
                CamUnitControl * ctl = cam_unit_add_control_boolean (super,
                        "trigger-now", "Trigger",
                        0, aux_enabled);
                cam_unit_control_set_ui_hints(ctl, CAM_UNIT_CONTROL_ONE_SHOT);
            }

            continue;
        }

        if (!f->on_off_capable && !f->auto_capable && !f->manual_capable) {
            fprintf (stderr, "Warning: Control \"%s\" has neither auto, "
                    "manual, or off mode\n", feature_desc[i]);
            continue;
        }

        if (f->on_off_capable && !f->auto_capable && !f->manual_capable) {
            fprintf (stderr, "Warning: Control \"%s\" has neither auto "
                    "nor manual mode\n", feature_desc[i]);
            continue;
        }

        if (!f->on_off_capable && f->auto_capable && !f->manual_capable) {
            fprintf (stderr, "Warning: Control \"%s\" has only auto mode\n",
                    feature_desc[i]);
            continue;
        }

        if (!(!f->on_off_capable && !f->auto_capable && f->manual_capable)) {
            int entries_enabled[] = {
                f->on_off_capable, f->auto_capable, f->manual_capable
            };
            int cur_val = CAM_DC1394_MENU_OFF;
            if (f->is_on && f->auto_active)
                cur_val = CAM_DC1394_MENU_AUTO;
            else if (f->is_on)
                cur_val = CAM_DC1394_MENU_MANUAL;

            char *ctl_id = g_strdup_printf ("%s-mode", feature_ids[i]);

            ctl = cam_unit_add_control_enum (super, 
                    ctl_id,
//                    CAM_DC1394_CNTL_FLAG_STATE | f->id,
                    (char *) feature_desc[i], cur_val, 1,
                    feature_state_desc, entries_enabled);
            g_object_set_data (G_OBJECT (ctl), "dc1394-control-id",
                    GINT_TO_POINTER (f->id));
            free (ctl_id);
        }

        int enabled = (f->is_on && !f->auto_active) ||
            (!f->on_off_capable && !f->auto_capable && f->manual_capable);

        if (!f->readout_capable && f->manual_capable) {
            fprintf (stderr, "Control \"%s\" is not readout capable but can "
                    "still be set\n", feature_desc[i]);
        }

        if (f->id == DC1394_FEATURE_WHITE_BALANCE) {
            ctl = cam_unit_add_control_int (super, "white-balance-red",
                    "W.B. Red", f->min, f->max, 1, f->RV_value,
                    enabled);
            g_object_set_data (G_OBJECT (ctl), "dc1394-control-id",
                    GINT_TO_POINTER (f->id));
            ctl = cam_unit_add_control_int (super, "white-balance-blue",
                    "W.B. Blue", f->min, f->max, 1, f->BU_value,
                    enabled);
            g_object_set_data (G_OBJECT (ctl), "dc1394-control-id",
                    GINT_TO_POINTER (f->id));
            continue;
        }

        if (f->absolute_capable && f->abs_control) {
            if (f->abs_max <= f->abs_min) {
                fprintf (stderr, 
                        "Disabling control \"%s\" because min >= max\n",
                        feature_desc[i]);
                cam_unit_add_control_float (super, feature_ids[i],
                        (char *) feature_desc[i], 0, 1,
                        1, 0, 0);
                continue;
            }
            ctl = cam_unit_add_control_float (super, feature_ids[i],
                    (char *) feature_desc[i], f->abs_min,
                    f->abs_max, (f->abs_max - f->abs_min) / NUM_FLOAT_STEPS,
                    f->abs_value, enabled);
            g_object_set_data (G_OBJECT (ctl), "dc1394-control-id",
                    GINT_TO_POINTER (f->id));
        }
        else {
            if (f->max <= f->min) {
                fprintf (stderr, 
                        "Disabling control \"%s\" because min >= max\n",
                        feature_desc[i]);
                cam_unit_add_control_int (super, feature_ids[i],
                        (char *) feature_desc[i], 0, 1,
                        1, 0, 0);
                continue;
            }

            ctl = cam_unit_add_control_int (super, feature_ids[i],
                    (char *) feature_desc[i], f->min, f->max,
                    1, f->value, enabled);
            g_object_set_data (G_OBJECT (ctl), "dc1394-control-id",
                    GINT_TO_POINTER (f->id));
        }
    }

    return 0;
}

static gboolean
dc1394_try_set_control(CamUnit *super, const CamUnitControl *ctl,
        const GValue *proposed, GValue *actual)
{
    CamDC1394 * self = CAM_DC1394 (super);
    dc1394feature_info_t f;
    int val = 0;

    if (!strcmp (ctl->id, "packet-size")) {
        g_value_copy (proposed, actual);
        return TRUE;
    }

    if (G_VALUE_TYPE (proposed) == G_TYPE_INT)
        val = g_value_get_int (proposed);

    if (!strcmp (ctl->id, "trigger-polarity")) {
        dc1394_external_trigger_set_polarity (self->cam,
                g_value_get_boolean (proposed));
        dc1394trigger_polarity_t newpol;
        dc1394_external_trigger_get_polarity (self->cam, &newpol);
        g_value_set_boolean (actual, newpol);
        return TRUE;
    }

    if (!strcmp (ctl->id, "trigger-source")) {
        dc1394_external_trigger_set_source (self->cam,
                val + DC1394_TRIGGER_SOURCE_MIN);
        dc1394trigger_source_t source;
        dc1394_external_trigger_get_source (self->cam, &source);
        g_value_set_int (actual, source - DC1394_TRIGGER_SOURCE_MIN);
        return TRUE;
    }

    if (!strcmp (ctl->id, "trigger-now")) {
        dc1394_software_trigger_set_power (self->cam,
                g_value_get_boolean (proposed));
        dc1394switch_t power;
        dc1394_software_trigger_get_power (self->cam,
                &power);
        g_value_set_boolean (actual, power);
        return TRUE;
    }

    if (!strcmp (ctl->id, "trigger")) {
        if (val == 0)
            dc1394_external_trigger_set_power (self->cam, DC1394_OFF);
        else {
            dc1394_external_trigger_set_power (self->cam, DC1394_ON);
            dc1394_external_trigger_set_mode (self->cam, val - 1 +
                    DC1394_TRIGGER_MODE_0);
        }
        f.id = DC1394_FEATURE_TRIGGER;
        dc1394_get_camera_feature (self->cam, &f);
        if (f.is_on)
            g_value_set_int (actual,
                    f.trigger_mode - DC1394_TRIGGER_MODE_0 + 1);
        else
            g_value_set_int (actual, 0);

        /* Enable or disable other trigger-related controls accordingly */
        CamUnitControl * ctl2;
        ctl2 = cam_unit_find_control (super, "trigger-polarity");
        if (ctl2)
            cam_unit_control_set_enabled (ctl2, f.is_on);

        ctl2 = cam_unit_find_control (super, "trigger-source");
        if (ctl2)
            cam_unit_control_set_enabled (ctl2, f.is_on);

        ctl2 = cam_unit_find_control (super, "trigger-now");
        if (ctl2)
            cam_unit_control_set_enabled (ctl2, f.is_on);
        return TRUE;
    }

    f.id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (ctl),
                "dc1394-control-id"));

    char ctlid[64];
    strncpy (ctlid, ctl->id, 64);
    char * suffix = strstr (ctlid, "-mode");
    if (suffix) {
        if (val == 0)
            dc1394_feature_set_power (self->cam, f.id, DC1394_OFF);
        else {
            dc1394_feature_set_power (self->cam, f.id, DC1394_ON);
            dc1394_feature_set_mode (self->cam, f.id, (val == 1) ?
                    DC1394_FEATURE_MODE_AUTO : DC1394_FEATURE_MODE_MANUAL);
        }
        dc1394_get_camera_feature (self->cam, &f);
        if (!f.is_on)
            g_value_set_int (actual, CAM_DC1394_MENU_OFF);
        else if (f.auto_active)
            g_value_set_int (actual, CAM_DC1394_MENU_AUTO);
        else
            g_value_set_int (actual, CAM_DC1394_MENU_MANUAL);

        *suffix = '\0';
        if (!strcmp (ctlid, "white-balance")) {
            CamUnitControl * ctl2 = cam_unit_find_control (super,
                    "white-balance-red");
            cam_unit_control_modify_int (ctl2, f.min, f.max, 1,
                    f.is_on && !f.auto_active);
            cam_unit_control_force_set_int (ctl2, f.RV_value);
            CamUnitControl * ctl3 = cam_unit_find_control (super,
                    "white-balance-blue");
            cam_unit_control_modify_int (ctl3, f.min, f.max, 1,
                    f.is_on && !f.auto_active);
            cam_unit_control_force_set_int (ctl3, f.BU_value);
            return TRUE;
        }

        CamUnitControl * ctl2 = cam_unit_find_control (super, ctlid);
        if (!ctl2) return TRUE;

        if (ctl2->type == CAM_UNIT_CONTROL_TYPE_INT) {
            cam_unit_control_modify_int (ctl2, f.min, f.max, 1,
                    f.is_on && !f.auto_active);
            cam_unit_control_force_set_int (ctl2, f.value);
        }
        else {
            cam_unit_control_modify_float (ctl2, f.abs_min, f.abs_max,
                    (f.abs_max - f.abs_min) / NUM_FLOAT_STEPS,
                    f.is_on && !f.auto_active);
            cam_unit_control_force_set_float (ctl2, f.abs_value);
        }
        return TRUE;
    }

    if (f.id == DC1394_FEATURE_WHITE_BALANCE) {
        dc1394_get_camera_feature (self->cam, &f);
        if (strstr (ctl->id, "blue"))
            dc1394_feature_whitebalance_set_value (self->cam,
                    val, f.RV_value);
        else
            dc1394_feature_whitebalance_set_value (self->cam,
                    f.BU_value, val);
        dc1394_get_camera_feature (self->cam, &f);
        if (strstr (ctl->id, "blue"))
            g_value_set_int (actual, f.BU_value);
        else
            g_value_set_int (actual, f.RV_value);
        return TRUE;
    }

    if (G_VALUE_TYPE (proposed) == G_TYPE_FLOAT) {
        float fval = g_value_get_float (proposed);
        dc1394_feature_set_absolute_value (self->cam, f.id, fval);
        dc1394_get_camera_feature (self->cam, &f);
        if (f.readout_capable)
            g_value_set_float (actual, f.abs_value);
        else
            g_value_copy (proposed, actual);
    }
    else {
        dc1394_feature_set_value (self->cam, f.id, val);
        dc1394_get_camera_feature (self->cam, &f);
        if (f.readout_capable)
            g_value_set_int (actual, f.value);
        else
            g_value_copy (proposed, actual);
    }
    return TRUE;
}