/*****************************************************************************
 * glspectrum.c: spectrum visualization module based on OpenGL
 *****************************************************************************
 * Copyright © 2009-2013 VLC authors and VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_window.h>
#include <vlc_opengl.h>
#include <vlc_filter.h>
#include <vlc_queue.h>
#include <vlc_rand.h>

#include <math.h>

#include "visual/fft.h"
#include "visual/window.h"

#include "../video_output/opengl/gl_common.h"


/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int Open(vlc_object_t *);
static void Close(filter_t *);

#define WIDTH_TEXT N_("Video width")
#define WIDTH_LONGTEXT N_("The width of the visualization window, in pixels.")

#define HEIGHT_TEXT N_("Video height")
#define HEIGHT_LONGTEXT N_("The height of the visualization window, in pixels.")

#define LOG_OFFSET 0.1

vlc_module_begin()
    set_shortname(N_("glSpectrum"))
    set_description(N_("3D OpenGL spectrum visualization"))
    set_capability("visualization", 0)
    set_subcategory(SUBCAT_AUDIO_VISUAL)

    add_integer("glspectrum-width", 400, WIDTH_TEXT, WIDTH_LONGTEXT)
    add_integer("glspectrum-height", 300, HEIGHT_TEXT, HEIGHT_LONGTEXT)

    add_shortcut("glspectrum")
    set_callback(Open)
vlc_module_end()

/* Legacy glBegin()/glEnd() API */
typedef void (APIENTRY *PFNGLMATRIXMODEPROC)(GLenum);
typedef void (APIENTRY *PFNGLPUSHMATRIXPROC)(void);
typedef void (APIENTRY *PFNGLPOPMATRIXPROC)(void);
typedef void (APIENTRY *PFNGLFRUSTUMPROC)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (APIENTRY *PFNGLTRANSLATEFPROC)(float, float, float);
typedef void (APIENTRY *PFNGLSCALEFPROC)(float, float, float);
typedef void (APIENTRY *PFNGLROTATEFPROC)(float, float, float, float);
typedef void (APIENTRY *PFNGLCOLORMATERIALPROC)(GLenum, GLenum);
typedef void (APIENTRY *PFNGLCOLOR4FPROC)(float, float, float, float);
typedef void (APIENTRY *PFNGLLIGHTFVPROC)(GLenum, GLenum, const float*);
typedef void (APIENTRY *PFNGLSHADEMODELPROC)(GLenum);

typedef void (APIENTRY *PFNGLVERTEXPOINTERPROC)(GLint, GLenum, GLsizei, const void *);
typedef void (APIENTRY *PFNGLNORMALPOINTERPROC)(GLenum, GLsizei, const void *);
typedef void (APIENTRY *PFNGLENABLECLIENTSTATEPROC)(GLenum);
typedef void (APIENTRY *PFNGLDISABLECLIENTSTATEPROC)(GLenum);

#define OPENGL_VTABLE_F(F) \
    /* Usual functions */ \
    F(PFNGLENABLEPROC              , Enable) \
    F(PFNGLCLEARPROC               , Clear) \
    F(PFNGLDRAWARRAYSPROC          , DrawArrays) \
    F(PFNGLDEPTHMASKPROC           , DepthMask) \
    F(PFNGLBLENDFUNCPROC           , BlendFunc) \
    F(PFNGLVIEWPORTPROC            , Viewport) \
    /* Legacy fixed pipeline functions */ \
    F(PFNGLMATRIXMODEPROC          , MatrixMode ) \
    F(PFNGLPUSHMATRIXPROC          , PushMatrix) \
    F(PFNGLPOPMATRIXPROC           , PopMatrix) \
    F(PFNGLFRUSTUMPROC             , Frustum) \
    F(PFNGLTRANSLATEFPROC          , Translatef) \
    F(PFNGLSCALEFPROC              , Scalef) \
    F(PFNGLROTATEFPROC             , Rotatef) \
    F(PFNGLCOLORMATERIALPROC       , ColorMaterial) \
    F(PFNGLCOLOR4FPROC             , Color4f) \
    F(PFNGLLIGHTFVPROC             , Lightfv) \
    F(PFNGLSHADEMODELPROC          , ShadeModel) \
    F(PFNGLVERTEXPOINTERPROC       , VertexPointer) \
    F(PFNGLNORMALPOINTERPROC       , NormalPointer) \
    F(PFNGLENABLECLIENTSTATEPROC   , EnableClientState) \
    F(PFNGLDISABLECLIENTSTATEPROC  , DisableClientState)


struct glspectrum_opengl_vtable
{
#define DECLARE_VTABLE(type, symbol) \
    type symbol;
OPENGL_VTABLE_F(DECLARE_VTABLE)
};


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
typedef struct
{
    vlc_thread_t thread;

    /* Audio data */
    vlc_queue_t queue;
    bool dead;
    unsigned i_channels;
    unsigned i_prev_nb_samples;
    int16_t *p_prev_s16_buff;

    /* Opengl */
    vlc_gl_t *gl;
    struct glspectrum_opengl_vtable vt;

    float f_rotationAngle;
    float f_rotationIncrement;

    /* FFT window parameters */
    window_param wind_param;
} filter_sys_t;


static block_t *DoWork(filter_t *, block_t *);
static void *Thread(void *);

#define SPECTRUM_WIDTH 4.f
#define NB_BANDS 20
#define ROTATION_INCREMENT .1f
#define BAR_DECREMENT .075f
#define ROTATION_MAX 20

const GLfloat lightZeroColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
const GLfloat lightZeroPosition[] = {0.0f, 3.0f, 10.0f, 0.0f};

static const struct vlc_filter_operations filter_ops = {
    .filter_audio = DoWork, .close = Close,
};

/**
 * Open the module.
 * @param p_this: the filter object
 * @return VLC_SUCCESS or vlc error codes
 */
static int Open(vlc_object_t * p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = vlc_obj_malloc(p_this, sizeof (*p_sys));

    if (p_sys == NULL)
        return VLC_ENOMEM;

    p_filter->p_sys = p_sys;

    /* Create the object for the thread */
    p_sys->i_channels = aout_FormatNbChannels(&p_filter->fmt_in.audio);
    p_sys->i_prev_nb_samples = 0;
    p_sys->p_prev_s16_buff = NULL;

    p_sys->f_rotationAngle = 0;
    p_sys->f_rotationIncrement = ROTATION_INCREMENT;

    /* Fetch the FFT window parameters */
    window_get_param( VLC_OBJECT( p_filter ), &p_sys->wind_param );

    /* Create the FIFO for the audio data. */
    vlc_queue_Init(&p_sys->queue, offsetof (block_t, p_next));
    p_sys->dead = false;

    /* Create the openGL provider */
    vlc_window_cfg_t cfg = {
        .is_decorated = true,
        .width = var_InheritInteger(p_filter, "glspectrum-width"),
        .height = var_InheritInteger(p_filter, "glspectrum-height"),
    };

    p_sys->gl = vlc_gl_surface_Create(p_this, &cfg, NULL, NULL);
    if (p_sys->gl == NULL)
        return VLC_EGENERIC;

    if (vlc_gl_MakeCurrent(p_sys->gl) != VLC_SUCCESS)
    {
        msg_Err(p_filter, "Can't attach gl context");
        goto error;
    }

#define LOAD_SYMBOL(type, name) do { \
    p_sys->vt.name = vlc_gl_GetProcAddress(p_sys->gl, "gl" #name); \
    if (p_sys->vt.name == NULL) { \
        vlc_gl_ReleaseCurrent(p_sys->gl); \
        goto error; \
    } \
} while(0);

    OPENGL_VTABLE_F(LOAD_SYMBOL);

    vlc_gl_ReleaseCurrent(p_sys->gl);

    /* Create the thread */
    if (vlc_clone(&p_sys->thread, Thread, p_filter)) {
        vlc_gl_surface_Destroy(p_sys->gl);
        return VLC_ENOMEM;
    }

    p_filter->fmt_in.audio.i_format = VLC_CODEC_FL32;
    p_filter->fmt_out.audio = p_filter->fmt_in.audio;
    p_filter->ops = &filter_ops;

    return VLC_SUCCESS;

error:
    vlc_gl_surface_Destroy(p_sys->gl);
    return VLC_EGENERIC;
}


/**
 * Close the module.
 * @param p_this: the filter object
 */
static void Close(filter_t *p_filter)
{
    filter_sys_t *p_sys = p_filter->p_sys;

    /* Terminate the thread. */
    vlc_queue_Kill(&p_sys->queue, &p_sys->dead);
    vlc_join(p_sys->thread, NULL);

    /* Free the resources */
    vlc_gl_surface_Destroy(p_sys->gl);
    free(p_sys->p_prev_s16_buff);
}


/**
 * Do the actual work with the new sample.
 * @param p_filter: filter object
 * @param p_in_buf: input buffer
 */
static block_t *DoWork(filter_t *p_filter, block_t *p_in_buf)
{
    filter_sys_t *p_sys = p_filter->p_sys;

    vlc_queue_Enqueue(&p_sys->queue, block_Duplicate(p_in_buf));
    return p_in_buf;
}


/**
  * Init the OpenGL scene.
  **/
static void initOpenGLScene(filter_t *p_filter)
{
    filter_sys_t *p_sys = p_filter->p_sys;
    struct glspectrum_opengl_vtable *vt = &p_sys->vt;

    vt->Enable(GL_CULL_FACE);
    vt->Enable(GL_DEPTH_TEST);
    vt->DepthMask(GL_TRUE);

    vt->MatrixMode(GL_PROJECTION);
    vt->Frustum(-1.0f, 1.0f, -1.0f, 1.0f, 0.5f, 10.0f);

    vt->MatrixMode(GL_MODELVIEW);
    vt->Translatef(0.0, -2.0, -2.0);

    // Init the light.
    vt->Enable(GL_LIGHTING);

    vt->ColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    vt->Enable(GL_COLOR_MATERIAL);

    vt->Enable(GL_LIGHT0);
    vt->Lightfv(GL_LIGHT0, GL_DIFFUSE, lightZeroColor);
    vt->Lightfv(GL_LIGHT0, GL_POSITION, lightZeroPosition);

    vt->ShadeModel(GL_SMOOTH);

    vt->Enable(GL_BLEND);
    vt->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}


/**
 * Draw one bar of the Spectrum.
 */
static void drawBar(filter_t *p_filter)
{
    filter_sys_t *p_sys = p_filter->p_sys;
    struct glspectrum_opengl_vtable *vt = &p_sys->vt;

    const float w = SPECTRUM_WIDTH / NB_BANDS - 0.05f;

    const GLfloat vertexCoords[] = {
        0.f, 0.f, 0.f,     w, 0.f, 0.f,   0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,     w, 0.f, 0.f,     w, 1.f, 0.f,

        0.f, 0.f, -w,    0.f, 0.f, 0.f,   0.f, 1.f,  -w,
        0.f, 1.f, -w,    0.f, 0.f, 0.f,   0.f, 1.f, 0.f,

          w, 0.f, 0.f,     w, 0.f,  -w,     w, 1.f, 0.f,
          w, 1.f, 0.f,     w, 0.f,  -w,     w, 1.f,  -w,

          w, 0.f,  -w,   0.f, 0.f,  -w,   0.f, 1.f,  -w,
        0.f, 1.f,  -w,     w, 1.f,  -w,     w, 0.f,  -w,

        0.f, 1.f, 0.f,     w, 1.f, 0.f,     w, 1.f,  -w,
        0.f, 1.f, 0.f,     w, 1.f,  -w,   0.f, 1.f,  -w,
    };

    const GLfloat normals[] = {
        0.f, 0.f, 1.f,   0.f, 0.f, 1.f,   0.f, 0.f, 1.f,
        0.f, 0.f, 1.f,   0.f, 0.f, 1.f,   0.f, 0.f, 1.f,

        -1.f, 0.f, 0.f,   -1.f, 0.f, 0.f,   -1.f, 0.f, 0.f,
        -1.f, 0.f, 0.f,   -1.f, 0.f, 0.f,   -1.f, 0.f, 0.f,

        1.f, 0.f, 0.f,   1.f, 0.f, 0.f,   1.f, 0.f, 0.f,
        1.f, 0.f, 0.f,   1.f, 0.f, 0.f,   1.f, 0.f, 0.f,

        0.f, 0.f, -1.f,   0.f, 0.f, -1.f,   0.f, 0.f, -1.f,
        0.f, 0.f, -1.f,   0.f, 0.f, -1.f,   0.f, 0.f, -1.f,

        0.f, 1.f, 0.f,   0.f, 1.f, 0.f,   0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,   0.f, 1.f, 0.f,   0.f, 1.f, 0.f,
    };

    vt->VertexPointer(3, GL_FLOAT, 0, vertexCoords);
    vt->NormalPointer(GL_FLOAT, 0, normals);
    vt->DrawArrays(GL_TRIANGLES, 0, 6 * 5);
}


/**
 * Set the color of one bar of the spectrum.
 * @param f_height the height of the bar.
 */
static void setBarColor(filter_t *p_filter, float f_height)
{
    filter_sys_t *p_sys = p_filter->p_sys;
    struct glspectrum_opengl_vtable *vt = &p_sys->vt;

    float r, b;

#define BAR_MAX_HEIGHT 4.2f
    r = -1.f + 2 / BAR_MAX_HEIGHT * f_height;
    b = 2.f - 2 / BAR_MAX_HEIGHT * f_height;
#undef BAR_MAX_HEIGHT

    /* Test the ranges. */
    r = r > 1.f ? 1.f : r;
    b = b > 1.f ? 1.f : b;

    r = r < 0.f ? 0.f : r;
    b = b < 0.f ? 0.f : b;

    /* Set the bar color. */
    vt->Color4f(r, 0.f, b, 1.f);
}


/**
 * Draw all the bars of the spectrum.
 * @param heights the heights of all the bars.
 */
static void drawBars(filter_t *p_filter, float heights[])
{
    filter_sys_t *p_sys = p_filter->p_sys;
    struct glspectrum_opengl_vtable *vt = &p_sys->vt;

    vt->PushMatrix();
    vt->Translatef(-2.f, 0.f, 0.f);

    vt->EnableClientState(GL_VERTEX_ARRAY);
    vt->EnableClientState(GL_NORMAL_ARRAY);

    float w = SPECTRUM_WIDTH / NB_BANDS;
    for (unsigned i = 0; i < NB_BANDS; ++i)
    {
        vt->PushMatrix();
        vt->Scalef(1.f, heights[i], 1.f);
        setBarColor(p_filter, heights[i]);
        drawBar(p_filter);
        vt->PopMatrix();

        vt->Translatef(w, 0.f, 0.f);
    }

    vt->DisableClientState(GL_VERTEX_ARRAY);
    vt->DisableClientState(GL_NORMAL_ARRAY);

    vt->PopMatrix();
}


/**
 * Update thread which do the rendering
 * @param p_this: the p_thread object
 */
static void *Thread( void *p_data )
{
    vlc_thread_set_name("vlc-glspectrum");

    filter_t  *p_filter = (filter_t*)p_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_gl_t *gl = p_sys->gl;
    block_t *block;

    if (vlc_gl_MakeCurrent(gl) != VLC_SUCCESS)
    {
        msg_Err(p_filter, "Can't attach gl context");
        return NULL;
    }

    struct glspectrum_opengl_vtable *vt = &p_sys->vt;

    initOpenGLScene(p_filter);
    float height[NB_BANDS] = {0};

    while ((block = vlc_queue_DequeueKillable(&p_sys->queue, &p_sys->dead)))
    {
        unsigned win_width, win_height;

        if (vlc_gl_surface_CheckSize(gl, &win_width, &win_height))
            vt->Viewport(0, 0, win_width, win_height);

        /* Horizontal scale for 20-band equalizer */
        const unsigned xscale[] = {0,1,2,3,4,5,6,7,8,11,15,20,27,
                                   36,47,62,82,107,141,184,255};

        fft_state *p_state = NULL; /* internal FFT data */
        DEFINE_WIND_CONTEXT(wind_ctx); /* internal window data */

        unsigned i, j;
        float p_output[FFT_BUFFER_SIZE];           /* Raw FFT Result  */
        int16_t p_buffer1[FFT_BUFFER_SIZE];        /* Buffer on which we perform
                                                      the FFT (first channel) */
        uint16_t p_dest[FFT_BUFFER_SIZE];          /* Adapted FFT result */
        float *p_buffl = (float*)block->p_buffer;  /* Original buffer */

        int16_t  *p_buffs;                         /* int16_t converted buffer */
        int16_t  *p_s16_buff;                      /* int16_t converted buffer */

        if (!block->i_nb_samples) {
            msg_Err(p_filter, "no samples yet");
            goto release;
        }

        /* Allocate the buffer only if the number of samples change */
        if (block->i_nb_samples != p_sys->i_prev_nb_samples)
        {
            free(p_sys->p_prev_s16_buff);
            p_sys->p_prev_s16_buff = malloc(block->i_nb_samples *
                                            p_sys->i_channels *
                                            sizeof(int16_t));
            if (!p_sys->p_prev_s16_buff)
                goto release;
            p_sys->i_prev_nb_samples = block->i_nb_samples;
        }
        p_buffs = p_s16_buff = p_sys->p_prev_s16_buff;

        /* Convert the buffer to int16_t
           Pasted from float32tos16.c */
        for (i = block->i_nb_samples * p_sys->i_channels; i--;)
        {
            union {float f; int32_t i;} u;

            u.f = *p_buffl + 384.f;
            if (u.i > 0x43c07fff)
                *p_buffs = 32767;
            else if (u.i < 0x43bf8000)
                *p_buffs = -32768;
            else
                *p_buffs = u.i - 0x43c00000;

            p_buffl++; p_buffs++;
        }
        p_state = visual_fft_init();
        if (!p_state)
        {
            msg_Err(p_filter,"unable to initialize FFT transform");
            goto release;
        }
        if (!window_init(FFT_BUFFER_SIZE, &p_sys->wind_param, &wind_ctx))
        {
            msg_Err(p_filter,"unable to initialize FFT window");
            goto release;
        }
        p_buffs = p_s16_buff;
        for (i = 0 ; i < FFT_BUFFER_SIZE; i++)
        {
            p_output[i] = 0;
            p_buffer1[i] = *p_buffs;

            p_buffs += p_sys->i_channels;
            if (p_buffs >= &p_s16_buff[block->i_nb_samples * p_sys->i_channels])
                p_buffs = p_s16_buff;
        }
        window_scale_in_place (p_buffer1, &wind_ctx);
        fft_perform (p_buffer1, p_output, p_state);

        for( i = 0; i< FFT_BUFFER_SIZE ; i++ )
        {
            /* Scale the output between 0 and UINT16MAX */
            p_dest[i] = p_output[i] * UINT16_MAX / FFT_SCALING_VALUE;
        }

        for (i = 0 ; i < NB_BANDS; i++)
        {
            /* Decrease the previous size of the bar. */
            height[i] -= BAR_DECREMENT;
            if (height[i] < 0)
                height[i] = 0;

            int y = 0;
            /* We search the maximum on one scale
               to determine the current size of the bar. */
            for (j = xscale[i]; j < xscale[i + 1]; j++)
            {
                if (p_dest[j] > y)
                     y = p_dest[j];
            }
            /* Calculate the height of the bar
               This log_offset makes it possible to display low values */
            float new_height = y != 0 ? logf( y + LOG_OFFSET ) * 0.4f : 0;
            height[i] = new_height > height[i]
                        ? new_height : height[i];
        }

        /* Determine the camera rotation angle. */
        p_sys->f_rotationAngle += p_sys->f_rotationIncrement;
        if (p_sys->f_rotationAngle <= -ROTATION_MAX)
            p_sys->f_rotationIncrement = ROTATION_INCREMENT;
        else if (p_sys->f_rotationAngle >= ROTATION_MAX)
            p_sys->f_rotationIncrement = -ROTATION_INCREMENT;

        /* Render the frame. */
        vt->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        vt->PushMatrix();
            vt->Rotatef(p_sys->f_rotationAngle, 0, 1, 0);
            drawBars(p_filter, height);
        vt->PopMatrix();
        
        /* Wait to swapp the frame on time. */
        vlc_tick_wait(block->i_pts + (block->i_length / 2));
        vlc_gl_Swap(gl);

release:
        window_close(&wind_ctx);
        fft_close(p_state);
        block_Release(block);
    }
    vlc_gl_ReleaseCurrent(gl);

    return NULL;
}
