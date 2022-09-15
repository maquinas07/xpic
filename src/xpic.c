#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>

#include <png.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

typedef struct _Region _Region;
struct _Region {
    int x;          /* offset from left of screen */
    int y;          /* offset from top of screen */
    int X;          /* offset from right of screen */
    int Y;          /* offset from bottom of screen */
    unsigned int w; /* width */
    unsigned int h; /* height */
    unsigned int b; /* border_width */
    unsigned int d; /* depth */
};

static void verror(const char *errstr, va_list argp) {
    fprintf(stderr, "%s: ", "xpic");
    vfprintf(stderr, errstr, argp);
}

static void error(const char *errstr, ...) {
    va_list argp;
    va_start(argp, errstr);
    verror(errstr, argp);
    va_end(argp);
}

static int save_as_png(XImage *img, char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        error("couldn't open file\n");
        return -1;
    }

    png_image pi = {0};
    pi.version = PNG_IMAGE_VERSION;
    pi.width = img->width;
    pi.height = img->height;
    pi.format = PNG_FORMAT_BGRA;

    unsigned int scanline = pi.width * img->bits_per_pixel / 8;
    if (!png_image_write_to_stdio(&pi, f, 0, img->data, scanline, NULL)) {
        fclose(f) ;
        error("could not save the png image\n");
        return -1;
    }

    fclose(f);
    return 0;
}

static void get_default_file_name(char *default_file_name, const char *id, const char *extension) {
    time_t rawtime = time(NULL);
    struct tm *tm = localtime(&rawtime);
    strftime(default_file_name, FILENAME_MAX, "xpic-%Y%m%d%H%M%S-", tm);
    strcat(default_file_name, id);
    strcat(default_file_name, extension);
}

static int init_shm(Display *dpy, XShmSegmentInfo *shm_ctx, XImage **img, _Region sr) {
    int screen = DefaultScreen(dpy);
    *img       = XShmCreateImage(dpy, DefaultVisual(dpy, screen),
            sr.d, ZPixmap, NULL,
            shm_ctx, sr.w, sr.h);
    if (!img) {
        error("couldn't allocate XImage structure\n");
        return -1;
    }

    shm_ctx->shmid = shmget(IPC_PRIVATE, (*img)->bytes_per_line * (*img)->height, IPC_CREAT | 0777);
    if (shm_ctx->shmid == -1) {
        error("couldn't get shared memory\n");
        return -1;
    }
    shm_ctx->shmaddr = (*img)->data = shmat(shm_ctx->shmid, 0, 0);
    if (shm_ctx->shmaddr == (void *) -1) {
        error("couldn't map shared memory address space\n");
        return -1;
    }
    shm_ctx->readOnly = False;
    shmctl(shm_ctx->shmid, IPC_RMID, 0);

    if (!XShmAttach(dpy, shm_ctx)) {
        error("couldn't attach to shared memory\n");
        return -1;
    }
    XSync(dpy, False);
    return 0;
}

static int uninit_shm(Display *dpy, XShmSegmentInfo *shm_ctx, XImage *img) {
    XShmDetach(dpy, shm_ctx);
    shmdt(shm_ctx->shmaddr);

    XDestroyImage(img);
    return 0;
}

struct ScreenshotContext {
    Display *dpy;
    unsigned int window;
    char *output_file;
};
typedef struct ScreenshotContext ScreenshotContext;

typedef int (*TakeScreenshotFunction)(ScreenshotContext *);

static int take_window_screenshot_xshm(ScreenshotContext *ctx) {
    _Region sr = {0};
    {
        Window root;
        XGetGeometry(ctx->dpy, ctx->window, &root, &sr.x, &sr.y, &sr.w, &sr.h,
                &sr.b, &sr.d);
    }

    XShmSegmentInfo shm_ctx = {0};
    XImage *img;
    if (init_shm(ctx->dpy, &shm_ctx, &img, sr) == -1) {
        return -1;
    };

    XShmGetImage(ctx->dpy, ctx->window, img, 0, 0, AllPlanes);
    unsigned int * p = (unsigned int *) img->data;
    for (int y = 0 ; y < img->height; ++y) {
        for (int x = 0; x < img->width; ++x) {
            *p++ |= 0xff000000;
        }
    }

    save_as_png(img, ctx->output_file);
    uninit_shm(ctx->dpy, &shm_ctx, img);

    return 0;
}

static int take_window_screenshot_composite(ScreenshotContext *ctx) {
    if (ctx->window != DefaultRootWindow(ctx->dpy)) { 
        XCompositeRedirectWindow(ctx->dpy, ctx->window, CompositeRedirectAutomatic);
        Pixmap p = XCompositeNameWindowPixmap(ctx->dpy, ctx->window);
        ctx->window = p;
    }
    return take_window_screenshot_xshm(ctx);
}

static int check_xcomposite(Display *dpy) {
    int c_events;
    int c_errors;
    if (!XCompositeQueryExtension(dpy, &c_events, &c_errors)) {
        error("X Composite Extension is not available\n");
        return -1;
    }

    int c_major;
    int c_minor; 
    if (!XCompositeQueryVersion(dpy, &c_major, &c_minor) || (c_minor < 2 && c_major == 0)) {
        error("X Composite Extension has a non compatible version\n");
        return -1;
    }

    return 0;
}


int main(int argc, char **argv) {
    // Force to use x11
    setenv("GDK_BACKEND", "x11", 1);

    char output_file[FILENAME_MAX] = {0};

    ScreenshotContext ctx = {0};
    ctx.output_file = output_file;

    int use_default_filename = 1;
    {
        int opt;
        while ((opt = getopt(argc, argv, "o:")) != -1) {
            switch (opt) {
                case ('o'): {
                                use_default_filename = 0;
                                strcpy(output_file, optarg);
                                break;
                            }
            }
        }
    }

    unsigned int windows[argc];
    memset(windows, 0, argc * sizeof(unsigned int));

    for (unsigned int i = optind; i < argc; ++i) {
        errno = 0;
        unsigned int window = strtoul(argv[i], NULL, 0);
        if (errno != 0) {
            error("bad argument %s: %s\n", argv[i], strerror(errno));
            return -1;
        }
        windows[i-optind] = window;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        error("failed to open display %s\n");
        return -1;
    }
    ctx.dpy = dpy;

    if (!XShmQueryExtension(dpy)) {
        error("X Shared Memory Extension is not available\n");
        return -1;
    }

    TakeScreenshotFunction take_screenshot = 0;
    if (!check_xcomposite(dpy)) {
        take_screenshot = &take_window_screenshot_composite;
    } else {
        fprintf(stdout, "falling back to XShm\n");
        take_screenshot = &take_window_screenshot_xshm;
    }

    int i = 0;
    ctx.window = windows[i];
    while (ctx.window != 0) {
        if (use_default_filename) {
            get_default_file_name(output_file, argv[i+optind], ".png");
        }
        take_screenshot(&ctx);
        ctx.window = windows[++i];
    }

    XFlush(dpy);
    XCloseDisplay(dpy);
}
