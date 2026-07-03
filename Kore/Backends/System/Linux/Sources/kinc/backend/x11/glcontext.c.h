#include "x11.h"

#include <kinc/log.h>

#include <GL/glx.h>

#include <stdlib.h>

// GLX-glue for the OpenGL-backend, the counterpart of the EGL-code in OpenGL.c.h.
// GLX is used instead of EGL because the Steam overlay and RenderDoc hook glXSwapBuffers,
// desktop-GL over EGL is invisible to both of them.

typedef GLXContext (*glXCreateContextAttribsARBProc)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
typedef void (*glXSwapIntervalEXTProc)(Display *, GLXDrawable, int);

static GLXFBConfig glx_fbconfig = NULL;
static XVisualInfo *glx_visual_info = NULL;
static GLXContext glx_context = NULL;

static int glx_silent_error_handler(Display *display, XErrorEvent *error_event) {
	return 0;
}

// also used by kinc_x11_window_create so the window is created with a GLX-compatible visual
XVisualInfo *kinc_glx_choose_visual(void) {
	if (glx_visual_info != NULL) {
		return glx_visual_info;
	}

	int dummy;
	if (!glXQueryExtension(x11_ctx.display, &dummy, &dummy)) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "X server has no OpenGL GLX extension");
		exit(1);
	}

	const int depth_sizes[] = {24, 16};
	for (int i = 0; i < 2 && glx_fbconfig == NULL; ++i) {
		const int attribs[] = {GLX_X_RENDERABLE,
		                       True,
		                       GLX_DRAWABLE_TYPE,
		                       GLX_WINDOW_BIT,
		                       GLX_RENDER_TYPE,
		                       GLX_RGBA_BIT,
		                       GLX_X_VISUAL_TYPE,
		                       GLX_TRUE_COLOR,
		                       GLX_RED_SIZE,
		                       8,
		                       GLX_GREEN_SIZE,
		                       8,
		                       GLX_BLUE_SIZE,
		                       8,
		                       GLX_DEPTH_SIZE,
		                       depth_sizes[i],
		                       GLX_STENCIL_SIZE,
		                       8,
		                       GLX_DOUBLEBUFFER,
		                       True,
		                       None};

		int fbconfig_count = 0;
		GLXFBConfig *fbconfigs = glXChooseFBConfig(x11_ctx.display, DefaultScreen(x11_ctx.display), attribs, &fbconfig_count);
		if (fbconfigs != NULL) {
			if (fbconfig_count > 0) {
				glx_fbconfig = fbconfigs[0];
			}
			xlib.XFree(fbconfigs);
		}
	}

	if (glx_fbconfig == NULL) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "Unable to choose a GLX framebuffer-config");
		exit(1);
	}

	glx_visual_info = glXGetVisualFromFBConfig(x11_ctx.display, glx_fbconfig);
	if (glx_visual_info == NULL) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "Unable to get a visual from the GLX framebuffer-config");
		exit(1);
	}

	return glx_visual_info;
}

void kinc_glx_init(void) {
	kinc_glx_choose_visual();

	glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
	    (glXCreateContextAttribsARBProc)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");

	if (glXCreateContextAttribsARB != NULL) {
		// probing for unsupported versions provokes X errors, silence them - the failed attempts return NULL
		XErrorHandler previous_handler = xlib.XSetErrorHandler(glx_silent_error_handler);

		const int gl_versions[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}, {3, 1}, {3, 0}, {2, 1}};
		for (int i = 0; i < sizeof(gl_versions) / sizeof(gl_versions[0]); ++i) {
			const int context_attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, gl_versions[i][0], GLX_CONTEXT_MINOR_VERSION_ARB, gl_versions[i][1], None};
			glx_context = glXCreateContextAttribsARB(x11_ctx.display, glx_fbconfig, None, True, context_attribs);
			if (glx_context != NULL) {
				kinc_log(KINC_LOG_LEVEL_INFO, "Using OpenGL version %i.%i over GLX.", gl_versions[i][0], gl_versions[i][1]);
				break;
			}
		}

		xlib.XSync(x11_ctx.display, False);
		xlib.XSetErrorHandler(previous_handler);
	}

	if (glx_context == NULL) {
		glx_context = glXCreateContext(x11_ctx.display, glx_visual_info, None, True);
		if (glx_context != NULL) {
			kinc_log(KINC_LOG_LEVEL_INFO, "Using a legacy OpenGL-context over GLX.");
		}
	}

	if (glx_context == NULL) {
		kinc_log(KINC_LOG_LEVEL_ERROR, "Could not create a GLX context");
		exit(1);
	}
}

void kinc_glx_make_current(int window) {
	glXMakeCurrent(x11_ctx.display, x11_ctx.windows[window].window, glx_context);
}

void kinc_glx_init_window(int window, bool vsync) {
	kinc_glx_make_current(window);

	glXSwapIntervalEXTProc glXSwapIntervalEXT = (glXSwapIntervalEXTProc)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
	if (glXSwapIntervalEXT != NULL) {
		glXSwapIntervalEXT(x11_ctx.display, x11_ctx.windows[window].window, vsync ? 1 : 0);
	}
}

void kinc_glx_destroy_window(int window) {
	if (glXGetCurrentDrawable() == x11_ctx.windows[window].window) {
		glXMakeCurrent(x11_ctx.display, None, NULL);
	}
}

void kinc_glx_destroy(void) {
	if (glx_context != NULL) {
		glXMakeCurrent(x11_ctx.display, None, NULL);
		glXDestroyContext(x11_ctx.display, glx_context);
		glx_context = NULL;
	}
}

bool kinc_glx_swap_buffers(void) {
	// window 0 last so its context stays current, like the EGL-code does
	for (int window = MAXIMUM_WINDOWS - 1; window >= 0; --window) {
		if (x11_ctx.windows[window].window != None) {
			glXSwapBuffers(x11_ctx.display, x11_ctx.windows[window].window);
		}
	}
	return true;
}
