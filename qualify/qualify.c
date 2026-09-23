#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <sys/sysmacros.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>

#define WIDTH 64
#define HEIGHT 64

static void die(const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", what, strerror(errno));
    exit(1);
}

static void fail(const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    exit(1);
}

static int sync_buf(int fd, __u64 flags) {
    struct dma_buf_sync sync = {.flags = flags};
    for (;;) {
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
            return 0;
        if (errno == EINTR || errno == EAGAIN)
            continue;
        return -1;
    }
}

/* Pick the VKMS primary node by device path, not by card number. */
static int find_vkms_primary(char *out, size_t out_len, dev_t *rdev) {
    drmDevicePtr devices[16];
    int count = drmGetDevices2(0, devices, 16);
    if (count < 0)
        fail("drmGetDevices2");
    for (int i = 0; i < count; i++) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
        const char *primary = devices[i]->nodes[DRM_NODE_PRIMARY];
        char sys_path[512];
        snprintf(sys_path, sizeof sys_path, "/sys/class/drm/%s", strrchr(primary, '/') + 1);
        char *resolved = realpath(sys_path, NULL);
        /* Linux 6.18 registers VKMS on the faux bus, so the driver symlink is
         * faux_driver. The device path still contains /vkms/. */
        int match = resolved && strstr(resolved, "/vkms/") != NULL;
        free(resolved);
        if (!match)
            continue;
        struct stat st;
        if (stat(primary, &st) != 0)
            die("stat primary");
        snprintf(out, out_len, "%s", primary);
        *rdev = st.st_rdev;
        drmFreeDevices(devices, count);
        return 0;
    }
    drmFreeDevices(devices, count);
    fail("no vkms primary node");
    return -1;
}

static int reopen_like_aquamarine(int fd) {
    char *name = drmGetDeviceNameFromFd2(fd);
    if (!name)
        fail("drmGetDeviceNameFromFd2");
    int neu = open(name, O_RDWR | O_CLOEXEC);
    fprintf(stderr, "reopen %s -> %d\n", name, neu);
    free(name);
    if (neu < 0)
        die("reopen");
    return neu;
}

static struct gbm_bo *alloc_bo(struct gbm_device *gbm, uint32_t format) {
    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, WIDTH, HEIGHT, format, &linear, 1, GBM_BO_USE_RENDERING);
    if (!bo) {
        fprintf(stderr, "modifier alloc failed for %u, falling back to linear flag\n", format);
        bo = gbm_bo_create(gbm, WIDTH, HEIGHT, format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!bo)
        fail("gbm_bo_create");
    return bo;
}

static void render_pattern(EGLImage image) {
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!target)
        fail("glEGLImageTargetTexture2DOES");
    target(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fail("framebuffer incomplete");

    const char *vs = "#version 300 es\n"
                     "void main(){vec2 p=vec2((gl_VertexID<<1)&2, gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
    const char *fs = "#version 300 es\n"
                     "precision mediump float;\n"
                     "out vec4 o;\n"
                     "void main(){\n"
                     "  float x=floor(gl_FragCoord.x);\n"
                     "  float y=floor(gl_FragCoord.y);\n"
                     "  o=vec4(mod(x,256.0)/255.0, mod(y,256.0)/255.0, mod(x*3.0+y,256.0)/255.0, mod(x+y*2.0,256.0)/255.0);\n"
                     "}\n";
    GLuint program = glCreateProgram();
    GLuint sv = glCreateShader(GL_VERTEX_SHADER);
    GLuint sf = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(sv, 1, &vs, NULL);
    glShaderSource(sf, 1, &fs, NULL);
    glCompileShader(sv);
    glCompileShader(sf);
    glAttachShader(program, sv);
    glAttachShader(program, sf);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
        fail("shader link");
    glUseProgram(program);
    glViewport(0, 0, WIDTH, HEIGHT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(program);
}

static int expected_byte(int format, int x, int y, int channel) {
    int r = x & 255;
    int g = y & 255;
    int b = (x * 3 + y) & 255;
    int a = (x + y * 2) & 255;
    /* Little-endian memory for XRGB8888 is B,G,R,X and ARGB8888 is B,G,R,A. */
    (void)format;
    if (channel == 0)
        return b;
    if (channel == 1)
        return g;
    if (channel == 2)
        return r;
    return format == DRM_FORMAT_ARGB8888 ? a : 0;
}

static int child_check(int fd, uint32_t format, uint32_t stride, uint32_t offset) {
    if (sync_buf(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) != 0)
        die("child sync start");
    size_t map_len = (size_t)offset + (size_t)stride * HEIGHT;
    void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        die("child mmap");
    int mismatches = 0;
    for (int y = 0; y < HEIGHT; y++) {
        unsigned char *row = (unsigned char *)map + offset + (size_t)y * stride;
        for (int x = 0; x < WIDTH; x++) {
            int channels = format == DRM_FORMAT_XRGB8888 ? 3 : 4;
            for (int c = 0; c < channels; c++) {
                int want = expected_byte(format, x, y, c);
                if (row[x * 4 + c] != (unsigned char)want) {
                    if (mismatches == 0)
                        fprintf(stderr, "first mismatch x=%d y=%d c=%d got=%u want=%d\n", x, y, c, row[x * 4 + c], want);
                    mismatches++;
                }
            }
        }
    }
    if (sync_buf(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) != 0)
        die("child sync end");
    munmap(map, map_len);
    if (mismatches) {
        fprintf(stderr, "FAIL pixel mismatches %d format %u\n", mismatches, format);
        return 1;
    }
    printf("PASS readback format %u stride %u\n", format, stride);
    return 0;
}

static void pass_fd_and_check(int fd, uint32_t format, uint32_t stride, uint32_t offset) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        die("socketpair");
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        close(sv[0]);
        close(fd);
        char c;
        struct iovec iov = {.iov_base = &c, .iov_len = 1};
        char control[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = {0};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        if (recvmsg(sv[1], &msg, 0) < 0)
            die("recvmsg");
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (!cmsg)
            fail("no cmsg");
        int got;
        memcpy(&got, CMSG_DATA(cmsg), sizeof got);
        int rc = child_check(got, format, stride, offset);
        close(got);
        _exit(rc);
    }
    close(sv[1]);
    char c = 'x';
    struct iovec iov = {.iov_base = &c, .iov_len = 1};
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof fd);
    if (sendmsg(sv[0], &msg, 0) < 0)
        die("sendmsg");
    close(sv[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        die("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("second process readback");
}

static void qualify_format(struct gbm_device *gbm, EGLDisplay dpy, EGLContext ctx, uint32_t format) {
    struct gbm_bo *bo = alloc_bo(gbm, format);
    int fd = gbm_bo_get_fd(bo);
    if (fd < 0)
        fail("gbm_bo_get_fd");
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t offset = gbm_bo_get_offset(bo, 0);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    fprintf(stderr, "format %u stride %u offset %u modifier 0x%llx fd %d\n", format, stride, offset, (unsigned long long)modifier, fd);

    EGLAttrib attrs[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)format,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLAttrib)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)stride,
        EGL_NONE,
    };
    EGLImage image = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE)
        fail("eglCreateImage");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    render_pattern(image);
    eglDestroyImage(dpy, image);
    pass_fd_and_check(fd, format, stride, offset);
    close(fd);
    gbm_bo_destroy(bo);
    printf("PASS format %u\n", format);
}

int main(void) {
    setenv("GBM_ALWAYS_SOFTWARE", "1", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    setenv("GALLIUM_DRIVER", "llvmpipe", 1);

    char primary[256];
    dev_t rdev = 0;
    find_vkms_primary(primary, sizeof primary, &rdev);
    printf("vkms primary %s rdev %lu:%lu\n", primary, (unsigned long)major(rdev), (unsigned long)minor(rdev));

    drmDevicePtr resolved = NULL;
    if (drmGetDeviceFromDevId(rdev, 0, &resolved) != 0)
        die("drmGetDeviceFromDevId");
    const char *chosen = NULL;
    if (resolved->available_nodes & (1 << DRM_NODE_RENDER))
        chosen = resolved->nodes[DRM_NODE_RENDER];
    else if (resolved->available_nodes & (1 << DRM_NODE_PRIMARY)) {
        chosen = resolved->nodes[DRM_NODE_PRIMARY];
        fprintf(stderr, "no render node, using primary\n");
    } else
        fail("resolved device has no node");
    printf("selected node %s\n", chosen);

    int master = open(chosen, O_RDWR | O_CLOEXEC);
    if (master < 0)
        die("open selected node");
    int fd = reopen_like_aquamarine(master);
    close(master);

    uint64_t prime = 0;
    if (drmGetCap(fd, DRM_CAP_PRIME, &prime) != 0)
        die("DRM_CAP_PRIME");
    if (!(prime & DRM_PRIME_CAP_EXPORT))
        fail("PRIME export missing");
    printf("PRIME caps 0x%llx\n", (unsigned long long)prime);

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm)
        fail("gbm_create_device");

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (dpy == EGL_NO_DISPLAY)
        fail("eglGetPlatformDisplay");
    if (!eglInitialize(dpy, NULL, NULL))
        fail("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        fail("eglBindAPI");
    EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint nconfig = 0;
    if (!eglChooseConfig(dpy, config_attribs, &config, 1, &nconfig) || nconfig < 1)
        fail("eglChooseConfig");
    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT)
        fail("eglCreateContext");

    qualify_format(gbm, dpy, ctx, DRM_FORMAT_XRGB8888);
    qualify_format(gbm, dpy, ctx, DRM_FORMAT_ARGB8888);

    printf("PASS qualification\n");
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    drmFreeDevice(&resolved);
    return 0;
}
