/*
 * MIT/X Consortium License
 *
 * © 2013 Jakub Klinkovský <kuba.klinkovsky at gmail dot com>
 * © 2010-2011 Ben Ruijl
 * © 2006-2008 Anselm R Garbe <garbeam at gmail dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdarg.h>     // variable arguments number
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>      // isprint()
#include <time.h>       // time()
#include <getopt.h>     // getopt_long()
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>   // mlock()
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xdbe.h>
#include <security/pam_appl.h>
#include <giblib/giblib.h>

#include "ziggurat_inline.h"

#ifdef __GNUC__
    #define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
    #define UNUSED(x) UNUSED_ ## x
#endif

typedef struct Dpms {
    BOOL state;
    CARD16 level;  // why?
    CARD16 standby, suspend, off;
} Dpms;

typedef struct WindowPositionInfo {
    int display_width, display_height;
    int output_x, output_y;
    int output_width, output_height;
} WindowPositionInfo;

static int conv_callback(int num_msgs, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr);

/* command-line arguments */
static char* opt_font;
static char* opt_username;
static char* opt_passchar;
static Bool  opt_hidelength;
static Bool  opt_primary;

/* need globals for signal handling */
Display *dpy;
Dpms dpms_original = { .state = True, .level = 0, .standby = 600, .suspend = 600, .off = 600 };  // holds original values
int dpms_timeout = 10;  // dpms timeout until program exits
Bool using_dpms;

XdbeBackBuffer bb;
static XImage *bd_img;
static int backdrop_width,
           backdrop_height,
           backdrop_x,
           backdrop_y;

pam_handle_t *pam_handle;
struct pam_conv conv = { conv_callback, NULL };

/* Holds the password you enter */
static char password[256];

static void
die(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    fprintf(stderr, "%s: ", PROGNAME);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

/*
 * Clears the memory which stored the password to be a bit safer against
 * cold-boot attacks.
 *
 */
static void
clear_password_memory(void) {
    /* A volatile pointer to the password buffer to prevent the compiler from
     * optimizing this out. */
    volatile char *vpassword = password;
    for (unsigned int c = 0; c < sizeof(password); c++)
        /* rewrite with random values */
        vpassword[c] = rand();
}

/*
 * Callback function for PAM. We only react on password request callbacks.
 *
 */
static int
conv_callback(int num_msgs, const struct pam_message **msg, struct pam_response **resp, void *UNUSED(appdata_ptr)) {
    if (num_msgs == 0)
        return PAM_BUF_ERR;

    // PAM expects an array of responses, one for each message
    if ((*resp = calloc(num_msgs, sizeof(struct pam_message))) == NULL)
        return PAM_BUF_ERR;

    for (int i = 0; i < num_msgs; i++) {
        if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF &&
            msg[i]->msg_style != PAM_PROMPT_ECHO_ON)
            continue;

        // return code is currently not used but should be set to zero
        resp[i]->resp_retcode = 0;
        if ((resp[i]->resp = strdup(password)) == NULL) {
            free(*resp);
            return PAM_BUF_ERR;
        }
    }

    return PAM_SUCCESS;
}

void
handle_signal(int sig) {
    /* restore dpms settings */
    if (using_dpms) {
        DPMSSetTimeouts(dpy, dpms_original.standby, dpms_original.suspend, dpms_original.off);
        if (!dpms_original.state)
            DPMSDisable(dpy);
    }

    die("Caught signal %d; dying\n", sig);
}

Imlib_Image image;

void
main_loop(Window w, GC gc, XFontStruct* font, WindowPositionInfo* info, char passdisp[256], char* username, XColor black, XColor white, XColor red, Bool hidelength) {
    XEvent event;
    KeySym ksym;

    unsigned int len = 0;
    Bool running = True;
    Bool sleepmode = False;
    Bool failed = False;

    XSync(dpy, False);

    /* define base coordinates - middle of screen */
    int base_x = info->output_x + info->output_width / 2;
    int base_y = info->output_y + info->output_height / 2;    /* y-position of the line */

    int line_width = info->output_width / 4;
    if (line_width > 800) {
        line_width = 800;
    }

    /* not changed in the loop */
    int line_x_left = base_x - line_width / 2;
    int line_x_right = base_x + line_width / 2;

    /* font properties */
    int ascent, descent;
    {
        int dir;
        XCharStruct overall;
        XTextExtents(font, passdisp, strlen(username), &dir, &ascent, &descent, &overall);
    }

    XdbeSwapInfo swapInfo;
    swapInfo.swap_window = w;
    swapInfo.swap_action = XdbeBackground;

    if (!XdbeSwapBuffers(dpy, &swapInfo, 1)) {
        fprintf(stderr, "swap buffers failed!\n");
        return;
    }

    XClearArea(dpy, w, info->output_x, info->output_y, info->output_width, info->output_height, False);

    XMapRaised(dpy, w);

    /* main event loop */
    while(running && !XNextEvent(dpy, &event)) {
        if (sleepmode && using_dpms)
            DPMSForceLevel(dpy, DPMSModeOff);

        /* update window if no events pending */
        if (!XPending(dpy)) {
            // draw backdrop
            XPutImage(dpy, bb, gc, bd_img, 0, 0, backdrop_x, backdrop_y, backdrop_width, backdrop_height);

            // draw username and separator
            XSetForeground(dpy, gc, white.pixel);
            int x = base_x - XTextWidth(font, username, strlen(username)) / 2;
            XDrawString(dpy, bb, gc, x, base_y - 10, username, strlen(username));
            XDrawLine(dpy, bb, gc, line_x_left, base_y, line_x_right, base_y);

            /* draw new passdisp or 'auth failed' */
            if (failed) {
                x = base_x - XTextWidth(font, "authentication failed", 21) / 2;
                XSetForeground(dpy, gc, red.pixel);
                XDrawString(dpy, bb, gc, x, base_y + ascent + 20, "authentication failed", 21);
                XSetForeground(dpy, gc, white.pixel);
            } else {
                int lendisp = len;
                if (hidelength && len > 0)
                    lendisp += (passdisp[len] * len) % 5;
                x = base_x - XTextWidth(font, passdisp, lendisp) / 2;
                XDrawString(dpy, bb, gc, x, base_y + ascent + 20, passdisp, lendisp % 256);
            }

            if (!XdbeSwapBuffers(dpy, &swapInfo, 1)) {
                fprintf(stderr, "swap buffers failed!\n");
                return;
            }
        }

        if (event.type == MotionNotify) {
            sleepmode = False;
            failed = False;
        }

        if (event.type == KeyPress) {
            sleepmode = False;
            failed = False;

            char inputChar = 0;
            XLookupString(&event.xkey, &inputChar, sizeof(inputChar), &ksym, 0);

            switch (ksym) {
                case XK_Return:
                case XK_KP_Enter:
                    password[len] = 0;
                    if (pam_authenticate(pam_handle, 0) == PAM_SUCCESS) {
                        clear_password_memory();
                        running = False;
                    } else {
                        failed = True;
                    }
                    len = 0;
                    break;
                case XK_Escape:
                    len = 0;
                    sleepmode = True;
                    break;
                case XK_BackSpace:
                    if (len)
                        --len;
                    break;
                default:
                    if (isprint(inputChar) && (len + sizeof(inputChar) < sizeof password)) {
                        memcpy(password + len, &inputChar, sizeof(inputChar));
                        len += sizeof(inputChar);
                    }
                    break;
            }
        }
    }
}

Bool
parse_options(int argc, char** argv)
{
    static struct option opts[] = {
        { "primary",        no_argument,       0, '1' },
        { "font",           required_argument, 0, 'f' },
        { "help",           no_argument,       0, 'h' },
        { "passchar",       required_argument, 0, 'p' },
        { "username",       required_argument, 0, 'u' },
        { "hidelength",     no_argument,       0, 'l' },
        { "version",        no_argument,       0, 'v' },
        { 0, 0, 0, 0 },
    };

    for (;;) {
        int opt = getopt_long(argc, argv, "1f:hp:u:vl", opts, NULL);
        if (opt == -1)
            break;

        switch (opt) {
            case '1':
                opt_primary = True;
                break;
            case 'f':
                opt_font = optarg;
                break;
            case 'h':
                die("usage: "PROGNAME" [-hvd] [-p passchars] [-f font] [-u username]\n"
                    "   -h: show this help page and exit\n"
                    "   -1: only show background on primary screen\n"
                    "   -v: show version info and exit\n"
                    "   -l: derange the password length indicator\n"
                    "   -p passchars: characters used to obfuscate the password\n"
                    "   -f font: X logical font description\n"
                    "   -u username: user name to show\n"
                );
                break;
            case 'p':
                opt_passchar = optarg;
                break;
            case 'u':
                opt_username = optarg;
                break;
            case 'l':
                opt_hidelength = True;
                break;
            case 'v':
                die(PROGNAME"-"VERSION", © 2013 Jakub Klinkovský\n");
                break;
            default:
                return False;
        }
    }

    return True;
}

// NOTE(ktravis): the following have been ported from https://github.com/r00tman/corrupter
// it's not 100% correct or the same yet, but it's close

// force x to stay in [0, b) range. x is assumed to be in [-b,2*b) range
int wrap(int x, int b) {
    if (x < 0) {
        return x + b;
    }
    if (x >= b) {
        return x - b;
    }
    return x;
}

#define NUM_RAND_FLOATS 15000000
float rnjesus[NUM_RAND_FLOATS];

static inline float nrandf() {
    static int start = 0;
    start = (start+1) % NUM_RAND_FLOATS;
    return rnjesus[start];
}

static void rand_init() {
    srand(0);
    r4_nor_setup();
    for (int i = 0; i < NUM_RAND_FLOATS; i++) {
        rnjesus[i] = r4_nor_value();
    }
}

// get normally distributed (rounded to int) value with the specified std. dev.
int offset(double stddev) {
    return (int)(nrandf() * stddev);
}

// brighten the color safely, i.e., by simultaneously reducing contrast
uint8_t brighten(uint8_t r, uint8_t add) {
    uint32_t r32 = (uint32_t)(r);
    uint32_t add32 = (uint32_t)(add);
    return (uint8_t)(r32 - r32*add32/255 + add32);
}

void corrupt_it(DATA32 *data, int w, int h) {
    rand_init();

    double mag = 7.0;
    int bheight = 10;
    double boffset = 30.0;
    double stride_mag = 0.1;
    double lag = 0.005;
    double lr = -7.0;
    double lg = 0.0;
    double lb = 3.0;
    double std_offset = 10.0;
    uint8_t add = 37;
    int meanabber = 10;
    double stdabber = 10.0;

    int line_off = 0;
    double stride = 0.0;
    int yset = 0;

    int m_raw_stride = 4*w;

    uint8_t *real_src = (uint8_t*)data;

    uint8_t *buf1 = malloc(4*w*h);
    uint8_t *buf2 = malloc(4*w*h);

    uint8_t *src = real_src;
    uint8_t *dst = buf1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
			// Every BHEIGHT lines in average a new distorted block begins
			if ((rand() % (bheight*w)) == 0) {
				line_off = offset(boffset);
				stride = stride_mag*nrandf();
				yset = y;
			}
			// at the line where the block has begun, we don't want to offset the image
			// so stride_off is 0 on the block's line
			int stride_off = (int)(stride * (double)(y-yset));

			// offset is composed of the blur, block offset, and skew offset (stride)
			int offx = offset(mag) + line_off + stride_off;
			int offy = offset(mag);

			// copy the corresponding pixel (4 bytes) to the new image
			int src_idx = m_raw_stride*wrap(y+offy, h) + 4*wrap(x+offx, w);
			int dst_idx = m_raw_stride*y + 4*x;

			memcpy(&dst[dst_idx], &src[src_idx], 4);
		}
	}

    src = dst;
    dst = buf2;

	// second stage is adding per-channel scan inconsistency and brightening
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            lr += lag * nrandf();
            lg += lag * nrandf();
            lb += lag * nrandf();
            int offx = offset(std_offset);

            // obtain source pixel base offsets. red/blue border is also smoothed by offx
            int ra_idx = m_raw_stride*y + 4*wrap(x+(int)(lr)-offx, w);
            int g_idx  = m_raw_stride*y + 4*wrap(x+(int)(lg), w);
            int b_idx  = m_raw_stride*y + 4*wrap(x+(int)(lb)+offx, w);

            // pixels are stored in (b, g, r, a) order in memory
            uint8_t b = src[b_idx+0];
            uint8_t g = src[g_idx+1];
            uint8_t r = src[ra_idx+2];
            uint8_t a = src[ra_idx+3];

            b = brighten(b, add);
            g = brighten(g, add);
            r = brighten(r, add);

            // copy the corresponding pixel (4 bytes) to the new image
			int dst_idx = m_raw_stride*y + 4*x;

            dst[dst_idx+0] = b;
            dst[dst_idx+1] = g;
            dst[dst_idx+2] = r;
            dst[dst_idx+3] = a;
        }
    }

    src = dst;
    dst = real_src;

	/*// third stage is to add chromatic abberation+chromatic trails*/
	/*// (trails happen because we're changing the same image we process)*/
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int offx = meanabber + offset(stdabber); // lower offset arg = longer trails

            // obtain source pixel base offsets. only red and blue are distorted
            int ra_idx = m_raw_stride*y + 4*wrap(x+offx, w);
            int g_idx  = m_raw_stride*y + 4*x;
            int b_idx  = m_raw_stride*y + 4*wrap(x-offx, w);

            // pixels are stored in (b, g, r, a) order in memory
            uint8_t b = src[b_idx+0];
            uint8_t g = src[g_idx+1];
            uint8_t r = src[ra_idx+2];
            uint8_t a = src[ra_idx+3];

            // copy the corresponding pixel (4 bytes) to the SAME image. this gets us nice colorful trails
            int dst_idx = m_raw_stride*y + 4*x;

            dst[dst_idx+0] = b;
            dst[dst_idx+1] = g;
            dst[dst_idx+2] = r;
            dst[dst_idx+3] = a;
        }
    }

}

// -- end ported section

int
main(int argc, char** argv) {
    char passdisp[256];
    int screen_num;
    WindowPositionInfo info;

    Cursor invisible;
    Window root, w;
    XColor black, red, white;
    XFontStruct* font;
    GC gc;

    /* get username (used for PAM authentication) */
    char* username;
    if ((username = getenv("USER")) == NULL)
        die("USER environment variable not set, please set it.\n");

    /* set default values for command-line arguments */
    opt_passchar = "*";
    opt_font = "-xos4-terminus-medium-r-normal--32-320-72-72-c-160-iso10646-1";
    opt_username = username;
    opt_hidelength = False;

    if (!parse_options(argc, argv))
        exit(EXIT_FAILURE);

    /* register signal handler function */
    if (signal (SIGINT, handle_signal) == SIG_IGN)
        signal (SIGINT, SIG_IGN);
    if (signal (SIGHUP, handle_signal) == SIG_IGN)
        signal (SIGHUP, SIG_IGN);
    if (signal (SIGTERM, handle_signal) == SIG_IGN)
        signal (SIGTERM, SIG_IGN);

    /* fill with password characters */
    for (unsigned int i = 0; i < sizeof(passdisp); i += strlen(opt_passchar))
        for (unsigned int j = 0; j < strlen(opt_passchar) && i + j < sizeof(passdisp); j++)
            passdisp[i + j] = opt_passchar[j];

    /* initialize random number generator */
    srand(time(NULL));

    if (!(dpy = XOpenDisplay(NULL)))
        die("cannot open dpy\n");

    if (!(font = XLoadQueryFont(dpy, opt_font)))
        die("error: could not find font. Try using a full description.\n");

    screen_num = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    Visual *vis = DefaultVisual(dpy, screen_num);
    /*int depth = DefaultDepth(dpy, XScreenNumberOfScreen(scr));*/
    Colormap cm = DefaultColormap(dpy, screen_num);

    /* get display/output size and position */
    {
        XRRScreenResources* screen = NULL;
        RROutput output;
        XRROutputInfo* output_info = NULL;
        XRRCrtcInfo* crtc_info = NULL;

        screen = XRRGetScreenResources (dpy, root);
        output = XRRGetOutputPrimary(dpy, root);

        /* When there is no primary output, the return value of XRRGetOutputPrimary
         * is undocumented, probably it is 0. Fall back to the first output in this
         * case, connected state will be checked later.
         */
        if (output == 0) {
            output = screen->outputs[0];
        }
        output_info = XRRGetOutputInfo(dpy, screen, output);

        /* Iterate through screen->outputs until connected output is found. */
        int i = 0;
        while (output_info->connection != RR_Connected || output_info->crtc == 0) {
            XRRFreeOutputInfo(output_info);
            output_info = XRRGetOutputInfo(dpy, screen, screen->outputs[i++]);
            fprintf(stderr, "Warning: no primary output detected, trying %s.\n", output_info->name);
            if (i == screen->noutput)
                die("error: no connected output detected.\n");
        }

        crtc_info = XRRGetCrtcInfo (dpy, screen, output_info->crtc);

        info.output_x = crtc_info->x;
        info.output_y = crtc_info->y;
        info.output_width = crtc_info->width;
        info.output_height = crtc_info->height;
        info.display_width = DisplayWidth(dpy, screen_num);
        info.display_height = DisplayHeight(dpy, screen_num);

        XRRFreeScreenResources(screen);
        XRRFreeOutputInfo(output_info);
        XRRFreeCrtcInfo(crtc_info);
    }

    /* allocate colors */
    {
        XColor dummy;
        Colormap cmap = DefaultColormap(dpy, screen_num);
        XAllocNamedColor(dpy, cmap, "orange red", &red, &dummy);
        XAllocNamedColor(dpy, cmap, "black", &black, &dummy);
        XAllocNamedColor(dpy, cmap, "white", &white, &dummy);
    }

    {
        int major, minor;
        if (!XdbeQueryExtension(dpy, &major, &minor)) {
            fprintf(stderr, "double buffering/xdbe not supported ...\n");
            return 1;
        }
        int numScreens = 1;
        Drawable screens[] = { root };
        XdbeScreenVisualInfo *info = XdbeGetVisualInfo(dpy, screens, &numScreens);
        if (!info || numScreens < 1 || info->count < 1) {
            fprintf(stderr, "created window does not support xdbe ...\n");
            return 1;
        }

        XVisualInfo xvisinfo_templ;
        xvisinfo_templ.visualid = info->visinfo[0].visual;
        xvisinfo_templ.screen = 0;
        xvisinfo_templ.depth = info->visinfo[0].depth;

        int matches;
        XVisualInfo *xvisinfo_match = XGetVisualInfo(dpy, VisualIDMask|VisualScreenMask|VisualDepthMask, &xvisinfo_templ, &matches);

        if (!xvisinfo_match || matches < 1) {
            fprintf(stderr, "no visual found with double buffering ...\n");
            return 1;
        }

        vis = xvisinfo_match->visual;
    }

    /* create window */
    {
        XSetWindowAttributes wa;
        wa.override_redirect = 1;
        wa.background_pixel = black.pixel;
        w = XCreateWindow(dpy, root, 0, 0, info.display_width, info.display_height,
                0, DefaultDepth(dpy, screen_num), CopyFromParent,
                vis, CWOverrideRedirect | CWBackPixel, &wa);

        bb = XdbeAllocateBackBufferName(dpy, w, XdbeBackground);
        XSelectInput(dpy, w, StructureNotifyMask);
    }

    /* define cursor */
    {
        char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
        Pixmap pmap = XCreateBitmapFromData(dpy, w, curs, 8, 8);
        invisible = XCreatePixmapCursor(dpy, pmap, pmap, &black, &black, 0, 0);
        XDefineCursor(dpy, w, invisible);
        XFreePixmap(dpy, pmap);
    }

    /*char *img_data = malloc(ww * hh * 4);*/
    /*imlib_context_set_image(image);*/
    /*[>imlib_image_set_has_alpha(1);<]*/
    /*DATA32 *imagedata = imlib_image_get_data();*/
    /*memcpy(imagedata, img_data, ww * hh * 4);*/

    /*int img_x, img_y, img_n;*/
    /*unsigned char *img_data = stbi_load(img_filename, &img_x, &img_y, &img_n, 4);*/
    /*if (!img_data) {*/
        /*fprintf(stderr, "Image failed to load\n");*/
        /*return;*/
    /*}*/
    /*// image loads as BGRA */
    /*for (int i = 0; i < img_x * img_y; i++) {*/
        /*unsigned char *p = &img_data[i*4];*/
        /*unsigned char x = p[0];*/
        /*p[0] = p[2];*/
        /*p[2] = x;*/
    /*}*/

    imlib_context_set_display(dpy);
    imlib_context_set_visual(vis);
    imlib_context_set_colormap(cm);
    imlib_context_set_color_modifier(NULL);
    imlib_context_set_operation(IMLIB_OP_COPY);

    int capture_x = opt_primary ? info.output_x : 0;
    int capture_y = opt_primary ? info.output_y : 0;
    int capture_width = opt_primary ? info.output_width : info.display_width;
    int capture_height = opt_primary ? info.output_height : info.display_height;

    image = gib_imlib_create_image_from_drawable(root, 0, capture_x, capture_y, capture_width, capture_height, 0);

    imlib_context_set_image(image);
    /*gib_imlib_image_blur(image, 5);*/
    DATA32 *data = imlib_image_get_data();
    corrupt_it(data, capture_width, capture_height);
    imlib_image_put_back_data(data);

    /* create Graphics Context */
    {
        XGCValues values;
        gc = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gc, font->fid);
        XSetForeground(dpy, gc, black.pixel);

        Pixmap gbpix = XCreatePixmap(dpy, w, info.display_width, info.display_height, DefaultDepth(dpy, screen_num));
        XFillRectangle(dpy, gbpix, gc, 0, 0, info.display_width, info.display_height);
        XSetForeground(dpy, gc, white.pixel);
        XImage *img = XCreateImage(dpy, vis, 24, ZPixmap, 0, (char*)data, capture_width, capture_height, 32, 0);
        XPutImage(dpy, gbpix, gc, img, 0, 0, capture_x, capture_y, capture_width, capture_height);
        XSetWindowBackgroundPixmap(dpy, w, gbpix);
        XFreePixmap(dpy, gbpix);

        XPutImage(dpy, bb, gc, img, 0, 0, capture_x, capture_y, capture_width, capture_height);
        XClearArea(dpy, w, info.output_x, info.output_y, info.output_width, info.output_height, False);
    }

    backdrop_width = info.output_width / 4;
    if (backdrop_width > 1000)
        backdrop_width = 1000;
    backdrop_height = 400;
    backdrop_x = info.output_x + info.output_width/2 - backdrop_width/2;
    backdrop_y = info.output_y + info.output_height/2 - backdrop_height/2;

    uint32_t *bd_data = malloc(sizeof(uint32_t) * backdrop_width * backdrop_height);
    for (int x = 0; x < backdrop_width; x++) {
        for (int y = 0; y < backdrop_height; y++) {
            DATA32 p = data[x+backdrop_x + (y+backdrop_y)*capture_width];
            bd_data[x + y*backdrop_width] = p & 0x00dbdbdb;
        }
    }
    bd_img = XCreateImage(dpy, vis, 24, ZPixmap, 0, (char*)bd_data, backdrop_width, backdrop_height, 32, 0);

    /* grab pointer and keyboard */
    int len = 1000;
    while (len-- > 0) {
        if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, invisible, CurrentTime) == GrabSuccess)
            break;
        usleep(50);
    }
    while (len-- > 0) {
        if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
            break;
        usleep(50);
    }
    if (len <= 0)
        die("Cannot grab pointer/keyboard\n");

    /* set up PAM */
    {
        int ret = pam_start("sxlock", username, &conv, &pam_handle);
        if (ret != PAM_SUCCESS)
            die("PAM: %s\n", pam_strerror(pam_handle, ret));
    }

    /* Lock the area where we store the password in memory, we don’t want it to
     * be swapped to disk. Since Linux 2.6.9, this does not require any
     * privileges, just enough bytes in the RLIMIT_MEMLOCK limit. */
    if (mlock(password, sizeof(password)) != 0)
        die("Could not lock page in memory, check RLIMIT_MEMLOCK\n");

    /* handle dpms */
    using_dpms = DPMSCapable(dpy);
    if (using_dpms) {
        /* save dpms timeouts to restore on exit */
        DPMSGetTimeouts(dpy, &dpms_original.standby, &dpms_original.suspend, &dpms_original.off);
        DPMSInfo(dpy, &dpms_original.level, &dpms_original.state);

        /* set program specific dpms timeouts */
        DPMSSetTimeouts(dpy, dpms_timeout, dpms_timeout, dpms_timeout);

        /* force dpms enabled until exit */
        DPMSEnable(dpy);
    }

    /* run main loop */
    main_loop(w, gc, font, &info, passdisp, opt_username, black, white, red, opt_hidelength);

    /* restore dpms settings */
    if (using_dpms) {
        DPMSSetTimeouts(dpy, dpms_original.standby, dpms_original.suspend, dpms_original.off);
        if (!dpms_original.state)
            DPMSDisable(dpy);
    }

    XUngrabPointer(dpy, CurrentTime);
    XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, w);
    XCloseDisplay(dpy);
    /*stbi_image_free(img_data);*/
    return 0;
}
