#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>

#include <glib/gstdio.h>

#include <libcam/plugin.h>

#define err(args...) fprintf(stderr, args)

enum {
    FILE_FORMAT_NONE,
    FILE_FORMAT_JPEG, 
    FILE_FORMAT_PPM, 
    FILE_FORMAT_PGM
};

static const char *_file_format_options[] = { 
    "None",
    "JPEG", 
    "PPM", 
    "PGM",
    NULL 
};

static const char *_suffixes[] = {
    "",
    "jpg",
    "ppm",
    "pgm",
    NULL
};

typedef struct _CamutilFileWriter CamutilFileWriter;
typedef struct _CamutilFileWriterClass CamutilFileWriterClass;

// boilerplate
#define CAMUTIL_TYPE_FILE_WRITER  camutil_file_writer_get_type()
#define CAMUTIL_FILE_WRITER(obj)  (G_TYPE_CHECK_INSTANCE_CAST( (obj), \
        CAMUTIL_TYPE_FILE_WRITER, CamutilFileWriter))
#define CAMUTIL_FILE_WRITER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), \
            CAMUTIL_TYPE_FILE_WRITER, CamutilFileWriterClass ))
#define IS_CAMUTIL_FILE_WRITER(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
            CAMUTIL_TYPE_FILE_WRITER ))
#define IS_CAMUTIL_FILE_WRITER_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE( \
            (klass), CAMUTIL_TYPE_FILE_WRITER))
#define CAMUTIL_FILE_WRITER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), \
            CAMUTIL_TYPE_FILE_WRITER, CamutilFileWriterClass))

void cam_plugin_initialize (GTypeModule * module);
CamUnitDriver * cam_plugin_create (GTypeModule * module);
struct _CamutilFileWriter {
    CamUnit parent;
    CamUnitControl *file_format_ctl;
    CamUnitControl *file_prefix_ctl;
    CamUnitControl *write_ctl;
    CamUnitControl *last_file_written_ctl;
    int counter;
};

struct _CamutilFileWriterClass {
    CamUnitClass parent_class;
};

GType camutil_file_writer_get_type (void);

static CamutilFileWriter * camutil_file_writer_new(void);
static void on_input_frame_ready (CamUnit * super, const CamFrameBuffer *inbuf,
        const CamUnitFormat *infmt);
static void on_input_format_changed (CamUnit *super, 
        const CamUnitFormat *infmt);
static gboolean _try_set_control (CamUnit *super, const CamUnitControl *ctl, 
        const GValue *proposed, GValue *actual);

CAM_PLUGIN_TYPE (CamutilFileWriter, camutil_file_writer, CAM_TYPE_UNIT);

/* These next two functions are required as entry points for the
 * plug-in API. */
void
cam_plugin_initialize (GTypeModule * module)
{
    camutil_file_writer_register_type (module);
}

CamUnitDriver *
cam_plugin_create (GTypeModule * module)
{
    return cam_unit_driver_new_stock_full ("output", "image_files", 
            "Image File Writer", 0, (CamUnitConstructor)camutil_file_writer_new, 
            module);
}

static void
camutil_file_writer_class_init (CamutilFileWriterClass *klass)
{
    // class initializer.  setup the class vtable here.
    klass->parent_class.on_input_frame_ready = on_input_frame_ready;
    klass->parent_class.try_set_control = _try_set_control;
}

static void
camutil_file_writer_init (CamutilFileWriter *self)
{
    // "private" constructor.  Initialize the unit with some reasonable
    // defaults here.
    CamUnit *super = CAM_UNIT (self);

    int format_enabled[] = { 1, 1, 1, 1, 0 };

    self->file_format_ctl = cam_unit_add_control_enum (super,
            "file-format", "File Format", 0, 0, _file_format_options,
            format_enabled);

    self->file_prefix_ctl = cam_unit_add_control_string (super, "file-prefix",
            "File Prefix", "", 1);
    self->write_ctl = cam_unit_add_control_boolean (super, "write",
            "Write", 0, 1);
    self->counter = 0;

    self->last_file_written_ctl = cam_unit_add_control_string (super, 
            "last-file-written", "Last File Written", "", 0);

    g_signal_connect (G_OBJECT (self), "input-format-changed",
            G_CALLBACK (on_input_format_changed), self);
}

static CamutilFileWriter * 
camutil_file_writer_new()
{
    // "public" constructor
    return CAMUTIL_FILE_WRITER(g_object_new(CAMUTIL_TYPE_FILE_WRITER, NULL));
}

static void
on_input_format_changed (CamUnit *super, const CamUnitFormat *infmt)
{
    CamutilFileWriter *self = CAMUTIL_FILE_WRITER (super);
    cam_unit_remove_all_output_formats (super);
    if (! infmt) return;

//    if (! (infmt->pixelformat == CAM_PIXEL_FORMAT_MJPEG ||
//           infmt->pixelformat == CAM_PIXEL_FORMAT_RGB ||
//           infmt->pixelformat == CAM_PIXEL_FORMAT_GRAY))
//        return;

    int fse = FILE_FORMAT_NONE;
//    int format_enabled[] = { 0, 0, 0, 0, 0 };

    if (infmt->pixelformat == CAM_PIXEL_FORMAT_MJPEG) {
        fse = FILE_FORMAT_JPEG;
    } else if (infmt->pixelformat == CAM_PIXEL_FORMAT_RGB) {
        fse = FILE_FORMAT_PPM;
    } else if (infmt->pixelformat == CAM_PIXEL_FORMAT_GRAY) {
        fse = FILE_FORMAT_PGM;
    } else {
        fse = FILE_FORMAT_NONE;
    }

    cam_unit_control_force_set_enum (self->file_format_ctl, fse);

//    format_enabled[fse] = 1;
//    cam_unit_control_modify_enum (self->file_format_ctl, 1,
//            _file_format_options, format_enabled);
//
    if (fse == FILE_FORMAT_NONE)
        return;

    cam_unit_add_output_format_full (super, infmt->pixelformat,
            infmt->name, infmt->width, infmt->height, 
            infmt->row_stride, infmt->max_data_size);
}

static int 
_ppm_write (FILE *fp, const uint8_t *pixels,
        int width, int height, 
        int rowstride)
{
    fprintf(fp, "P6 %d %d %d\n", width, height, 255);
    for (int i=0; i<height; i++) {
        if (1 != fwrite(pixels + i*rowstride, width*3, 1, fp)) 
            return -1;
    }
    return 0;
}

static int 
_pgm_write (FILE *fp, const uint8_t *pixels,
        int width, int height, 
        int rowstride)
{
    fprintf(fp, "P5\n%d\n%d\n%d\n", width, height, 255);
    for (int i=0; i<height; i++){
        if (1 != fwrite(pixels + i*rowstride, width, 1, fp))
            return -1;
    }
    return 0;
}

static void 
on_input_frame_ready (CamUnit *super, const CamFrameBuffer *inbuf, 
        const CamUnitFormat *infmt)
{
    CamutilFileWriter *self = CAMUTIL_FILE_WRITER(super);

    int out_fmt = cam_unit_control_get_enum (self->file_format_ctl);

    if ((! cam_unit_control_get_boolean (self->write_ctl)) ||
         (out_fmt == FILE_FORMAT_NONE)) {
        cam_unit_produce_frame (super, inbuf, infmt);
        return;
    }

    const char *prefix = cam_unit_control_get_string (self->file_prefix_ctl);
    const char *suffix = _suffixes[out_fmt];

    char *dirpart = g_path_get_dirname(prefix);
    if (! g_file_test (dirpart, G_FILE_TEST_EXISTS)) {
        g_mkdir_with_parents (dirpart, 0777);
    }
    free (dirpart);

    // pick an unused filename
    char *fname = g_strdup_printf ("%s%06d.%s", prefix, self->counter, suffix);
    while (g_file_test (fname, G_FILE_TEST_EXISTS)) {
        free (fname);
        self->counter++;
        fname = g_strdup_printf ("%s%06d.%s", prefix, self->counter, suffix);
    }

    FILE *fp = fopen (fname, "wb");
    if (!fp) {
        cam_unit_produce_frame (super, inbuf, infmt);
        free (fname);
        return;
    }

    // write the image out to file.
    if (out_fmt == FILE_FORMAT_JPEG) {
            fwrite (inbuf->data, inbuf->bytesused, 1, fp);
    } else if (out_fmt == FILE_FORMAT_PPM) {
        _ppm_write (fp, inbuf->data, infmt->width, infmt->height, 
                infmt->row_stride);
    } else if (out_fmt == FILE_FORMAT_PGM) {
        _pgm_write (fp, inbuf->data, infmt->width, infmt->height, 
                infmt->row_stride);
    }
    fclose (fp);

    char *bname = g_path_get_basename (fname);
    cam_unit_control_force_set_string (self->last_file_written_ctl, bname);
    free (bname);
    free (fname);

    cam_unit_produce_frame (super, inbuf, infmt);
}

static gboolean
_try_set_control (CamUnit *super, const CamUnitControl *ctl, 
        const GValue *proposed, GValue *actual)
{
    CamutilFileWriter *self = CAMUTIL_FILE_WRITER(super);
    if (ctl == self->last_file_written_ctl) {
        return FALSE;
    }
    g_value_copy (proposed, actual);
    return TRUE;
}