// A mock OpenXR runtime for testing a Vulkan OpenXR app (the frame loop,
// swapchains, layers, pacing) without a headset.  Loaded by the OpenXR loader
// through XR_RUNTIME_JSON (runtime.json from build.sh), in the app's process.
// The Vulkan instance and device are made through it with the app's own
// loader (XR_KHR_vulkan_enable2, up to Vulkan 1.3 by default), so validation
// layers and Turnip apply as on the Frame.  Never touches SteamVR.
//
// What it models: a session lifecycle (IDLE, READY, SYNCHRONIZED, VISIBLE,
// FOCUSED, STOPPING, EXITING), LOCAL / LOCAL_FLOOR / VIEW / STAGE reference
// spaces and action spaces, two views from one scripted head pose, a frame
// clock (xrWaitFrame paced at MOCK_XR_HZ), colour and depth swapchains made
// of VkImages on the app's device, projection and quad layers, depth info.
// It checks the call discipline where an app can get it wrong and keeps a
// deterministic call trace plus a separate timing file.  It can dump the
// images the app submitted to PNG.
//
// Environment (all optional):
//   MOCK_XR_TRACE=<file>     call trace (no times, so two runs can be diffed;
//                            tools/xrmock/trace-compare.sh)
//   MOCK_XR_TIMING=<file>    one CSV line per frame (header lists the columns)
//   MOCK_XR_SECONDS=<s>      session length before STOPPING is sent (20; 0 = until the app quits)
//   MOCK_XR_FRAMES=<n>       or stop after n frames (0 = off)
//   MOCK_XR_HZ=<hz>          display refresh, 120 default (90, 72 ...); xrEnumerateDisplayRefreshRatesFB offers 72/90/120
//   MOCK_XR_PIPELINE=<n>     refreshes from xrWaitFrame's return to the predicted display time (2)
//   MOCK_XR_DEADLINE_MS=<ms> an xrEndFrame later than this before the display time counts as late (period / 2)
//   MOCK_XR_SKIP=0           a frame loop that falls behind loses refreshes (the wait grid jumps), as on a headset; 0 makes it catch up instead (on by default)
//   MOCK_XR_EYE_SIZE=WxH     recommended per-eye size (1680x1760, the Frame's default at 120 Hz); max is 4096x4096
//   MOCK_XR_IPD=<m>          eye separation (0.064)
//   MOCK_XR_FOV=l,r,u,d      per-eye fov in degrees (-48,48,48,-48)
//   MOCK_XR_POSE=<spec>      head pose in STAGE: sway (default: yaw +-17 deg at 0.5 Hz, 1.6 m up) |
//                            static[:x,y,z,yaw[,pitch[,roll]]] (metres, degrees) | file:<path>
//                            (lines "t x y z yaw pitch roll", t in seconds from xrBeginSession, linear, held at the ends)
//   MOCK_XR_SHOT=<spec>      write the submitted images to PNG for these frames (1-based, as in the trace):
//                            "30", "10,20", "5-8", "every:60", "all"
//                            (a dumped frame stalls the loop for ~0.5 s per image: copy and PNG on the CPU)
//   MOCK_XR_SHOT_DIR=<dir>   where (default .): f<frame>_proj<layer>_eye<0|1>.png, f<frame>_quad<layer>.png
//   MOCK_XR_VK_MAX=1.3       highest Vulkan version offered (1.2 is what SteamVR offers; the instance is refused above it)
//   MOCK_XR_BUSY_MS=<ms>     at each release wait for the app's queue to go idle; longer than this is an error
//                            (the app released an image whose GPU work was unfinished).  Off unless set.
//   MOCK_XR_EXTENSIONS=a,b   replace the extension list offered
//   MOCK_XR_NO_LOCAL_FLOOR=1 refuse LOCAL_FLOOR spaces
//
// Checks (ERROR lines in the trace and stderr, counted in the summary printed
// at xrEndSession): call order of xrWaitFrame / xrBeginFrame / xrEndFrame and
// of swapchain acquire / wait / release; swapchain mips (SteamVR on the Frame
// allocates level 0 only); layers: the swapchain exists, its image was
// released (not still acquired) and not never rendered, the rect fits, fov and
// pose are sane, both eyes come from one xrLocateViews; Vulkan handles made by
// the runtime; the Vulkan version.
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include "xrmock_vk.h"

static FILE *gTrace, *gTiming;
static int gFrame = 0;  // xrWaitFrame calls so far (the frame number in the trace)
static int gErrors = 0, gWarns = 0;

void xrmock_trace(const char* fmt, ...) {
    if (!gTrace) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(gTrace, "f%d ", gFrame);
    vfprintf(gTrace, fmt, ap);
    fputc('\n', gTrace);
    va_end(ap);
    fflush(gTrace);
}
void xrmock_error(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gErrors++;
    xrmock_trace("ERROR %s", buf);
    fprintf(stderr, "xrmock: ERROR f%d %s\n", gFrame, buf);
}
static void warnf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gWarns++;
    xrmock_trace("WARN %s", buf);
}
#define trace xrmock_trace

static int64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
static int64_t gT0 = 0, gPeriod = 8333333, gEndAt = 0;
static double msSince(int64_t t) { return (double)(t - gT0) / 1e6; }

// ---- pose maths -----------------------------------------------------------
typedef struct { float x, y, z, w; } Q;
typedef struct { Q q; XrVector3f p; } Pose;
static Q qmul(Q a, Q b) {
    return (Q){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
               a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Q qconj(Q a) { return (Q){-a.x, -a.y, -a.z, a.w}; }
static XrVector3f qrot(Q q, XrVector3f v) {
    Q r = qmul(qmul(q, (Q){v.x, v.y, v.z, 0}), qconj(q));
    return (XrVector3f){r.x, r.y, r.z};
}
static Pose pcompose(Pose a, Pose b) {
    XrVector3f t = qrot(a.q, b.p);
    return (Pose){qmul(a.q, b.q), {a.p.x + t.x, a.p.y + t.y, a.p.z + t.z}};
}
static Pose pinv(Pose a) {
    Q c = qconj(a.q);
    XrVector3f t = qrot(c, a.p);
    return (Pose){c, {-t.x, -t.y, -t.z}};
}
static Q qeuler(float yaw, float pitch, float roll) {  // yaw about Y, pitch about X, roll about Z
    Q y = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)}, x = {sinf(pitch * 0.5f), 0, 0, cosf(pitch * 0.5f)}, z = {0, 0, sinf(roll * 0.5f), cosf(roll * 0.5f)};
    return qmul(qmul(y, x), z);
}
static XrPosef toXr(Pose p) { return (XrPosef){{p.q.x, p.q.y, p.q.z, p.q.w}, p.p}; }
static Pose fromXr(XrPosef p) { return (Pose){{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, p.position}; }

// ---- head pose script -----------------------------------------------------
typedef struct { double t; float v[6]; } Key;  // x y z yaw pitch roll (radians)
static Key gKeys[4096];
static int gKeyCount = 0, gPoseMode = 0;  // 0 sway, 1 static/script
#define D2R 0.017453292519943295f

static Pose headPoseAt(double t) {  // in STAGE
    float v[6] = {0, 1.6f, 0, 0, 0, 0};
    if (gPoseMode == 0) {
        v[3] = (float)(0.3 * sin(t * 2.0 * 3.14159265358979 * 0.5));
    } else if (gKeyCount == 1 || t <= gKeys[0].t) {
        memcpy(v, gKeys[0].v, sizeof(v));
    } else if (t >= gKeys[gKeyCount - 1].t) {
        memcpy(v, gKeys[gKeyCount - 1].v, sizeof(v));
    } else {
        int k = 0;
        while (k + 2 < gKeyCount && gKeys[k + 1].t < t) k++;
        double f = (t - gKeys[k].t) / (gKeys[k + 1].t - gKeys[k].t);
        for (int i = 0; i < 6; i++) v[i] = (float)(gKeys[k].v[i] + f * (gKeys[k + 1].v[i] - gKeys[k].v[i]));
    }
    return (Pose){qeuler(v[3], v[4], v[5]), {v[0], v[1], v[2]}};
}
static void loadPose(const char* spec) {
    if (!spec || !*spec || !strcmp(spec, "sway")) return;
    gPoseMode = 1;
    gKeys[0] = (Key){0, {0, 1.6f, 0, 0, 0, 0}};
    gKeyCount = 1;
    if (!strncmp(spec, "static", 6)) {
        if (spec[6] == ':') {
            float a[6] = {0, 1.6f, 0, 0, 0, 0};
            sscanf(spec + 7, "%f,%f,%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
            for (int i = 3; i < 6; i++) a[i] *= D2R;
            memcpy(gKeys[0].v, a, sizeof(a));
        }
    } else if (!strncmp(spec, "file:", 5)) {
        FILE* f = fopen(spec + 5, "r");
        if (!f) {
            fprintf(stderr, "xrmock: cannot read pose script %s\n", spec + 5);
            return;
        }
        char line[256];
        gKeyCount = 0;
        while (fgets(line, sizeof(line), f) && gKeyCount < 4096) {
            if (line[0] == '#') continue;
            Key k;
            if (sscanf(line, "%lf %f %f %f %f %f %f", &k.t, &k.v[0], &k.v[1], &k.v[2], &k.v[3], &k.v[4], &k.v[5]) == 7) {
                for (int i = 3; i < 6; i++) k.v[i] *= D2R;
                gKeys[gKeyCount++] = k;
            }
        }
        fclose(f);
        if (!gKeyCount) gKeyCount = 1;
    }
}

// ---- configuration --------------------------------------------------------
static int gEyeW = 1680, gEyeH = 1760;
static float gIpd = 0.064f;
static XrFovf gFov = {-0.8377f, 0.8377f, 0.8377f, -0.8377f};
static int gPipeline = 2;
static double gDeadlineMs = -1;
static int gMaxFrames = 0;
static const char* gShotDir = ".";
static char gShotSpec[256] = "";

static int shotWanted(int frame) {
    if (!gShotSpec[0]) return 0;
    if (!strcmp(gShotSpec, "all")) return 1;
    if (!strncmp(gShotSpec, "every:", 6)) {
        int k = atoi(gShotSpec + 6);
        return k > 0 && frame % k == 0;
    }
    char copy[256];
    strcpy(copy, gShotSpec);
    for (char* tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        int a, b;
        if (sscanf(tok, "%d-%d", &a, &b) == 2) {
            if (frame >= a && frame <= b) return 1;
        } else if (atoi(tok) == frame) {
            return 1;
        }
    }
    return 0;
}

// ---- instance and handles -------------------------------------------------
static char gExtNames[64][XR_MAX_EXTENSION_NAME_SIZE];
static int gExtCount = 0;
static int gInstanceMade = 0, gSessionMade = 0, gSessionRunning = 0, gSessionState = 0;
static XrPath gPaths[512];
static char gPathStr[512][128];
static int gPathCount = 0;
static int gEventQueue[16], gEventCount = 0;
static int gQuitSent = 0, gSummaryDone = 0;

static void pushState(int s) {
    if (gEventCount < 16) gEventQueue[gEventCount++] = s;
}

static XrResult XRAPI_CALL mEnumExt(const char* layer, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
    (void)layer;
    *count = (uint32_t)gExtCount;
    if (cap == 0) return XR_SUCCESS;
    if (cap < (uint32_t)gExtCount) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < gExtCount; i++) {
        memset(props[i].extensionName, 0, sizeof(props[i].extensionName));
        strcpy(props[i].extensionName, gExtNames[i]);
        props[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateInstance(const XrInstanceCreateInfo* ci, XrInstance* out) {
    char line[2048] = "";
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (strlen(line) + 80 < sizeof(line)) {
            strcat(line, " ");
            strcat(line, ci->enabledExtensionNames[i]);
        }
        int found = 0;
        for (int k = 0; k < gExtCount; k++) found |= !strcmp(gExtNames[k], ci->enabledExtensionNames[i]);
        if (!found) {
            xrmock_error("extension %s not offered by the runtime", ci->enabledExtensionNames[i]);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    trace("createInstance extensions:%s", line);
    gInstanceMade = 1;
    *out = (XrInstance)(uintptr_t)1;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroyInstance(XrInstance i) { (void)i; return XR_SUCCESS; }
static XrResult XRAPI_CALL mResultToString(XrInstance i, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) {
    (void)i;
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XrResult_%d", (int)r);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetInstanceProps(XrInstance i, XrInstanceProperties* p) {
    (void)i;
    p->runtimeVersion = XR_MAKE_VERSION(0, 2, 0);
    strcpy(p->runtimeName, "xrmock");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetSystem(XrInstance i, const XrSystemGetInfo* gi, XrSystemId* id) {
    (void)i;
    if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *id = 1;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetSystemProps(XrInstance i, XrSystemId s, XrSystemProperties* p) {
    (void)i;
    (void)s;
    p->vendorId = 0x1234;
    strcpy(p->systemName, "xrmock headset");
    p->systemId = 1;
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = 16;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    for (XrBaseOutStructure* n = (XrBaseOutStructure*)p->next; n; n = n->next) {
        if (n->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) ((XrSystemEyeGazeInteractionPropertiesEXT*)n)->supportsEyeGazeInteraction = XR_TRUE;
    }
    return XR_SUCCESS;
}

// ---- spaces ---------------------------------------------------------------
typedef struct { int used, kind; Pose offset; int hand; } Space;  // kind: reference type, or -1 action
static Space gSpaces[256];
static int gSpaceCount = 0;
static Pose gLocalOrigin, gLocalFloorOrigin;

static XrResult XRAPI_CALL mEnumRefSpaces(XrSession s, uint32_t cap, uint32_t* count, XrReferenceSpaceType* t) {
    (void)s;
    XrReferenceSpaceType all[4] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR};
    uint32_t n = getenv("MOCK_XR_NO_LOCAL_FLOOR") ? 3 : 4;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    for (uint32_t i = 0; i < n && i < cap; i++) t[i] = all[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateRefSpace(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    if (ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && getenv("MOCK_XR_NO_LOCAL_FLOOR")) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    if (ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE && ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    if (gSpaceCount >= 256) return XR_ERROR_LIMIT_REACHED;
    gSpaces[gSpaceCount] = (Space){1, (int)ci->referenceSpaceType, fromXr(ci->poseInReferenceSpace), 0};
    trace("createReferenceSpace type=%d", (int)ci->referenceSpaceType);
    *out = (XrSpace)(uintptr_t)(100 + gSpaceCount++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    if (gSpaceCount >= 256) return XR_ERROR_LIMIT_REACHED;
    int hand = 1;
    for (int k = 0; k < gPathCount; k++)
        if (gPaths[k] == ci->subactionPath && strstr(gPathStr[k], "left")) hand = 0;
    gSpaces[gSpaceCount] = (Space){1, -1, fromXr(ci->poseInActionSpace), hand};
    *out = (XrSpace)(uintptr_t)(100 + gSpaceCount++);
    return XR_SUCCESS;
}
static Space* spaceOf(XrSpace sp) {
    uintptr_t k = (uintptr_t)sp;
    if (k < 100 || k >= 100 + (uintptr_t)gSpaceCount || !gSpaces[k - 100].used) return NULL;
    return &gSpaces[k - 100];
}
static XrResult XRAPI_CALL mDestroySpace(XrSpace sp) {
    Space* s = spaceOf(sp);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    s->used = 0;
    return XR_SUCCESS;
}
// Pose of a space in STAGE at time t (seconds from the session start).
static Pose spaceInStage(const Space* s, double t) {
    Pose base;
    switch (s->kind) {
    case XR_REFERENCE_SPACE_TYPE_VIEW: base = headPoseAt(t); break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL: base = gLocalOrigin; break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: base = gLocalFloorOrigin; break;
    case -1: base = (Pose){{0, 0, 0, 1}, {s->hand ? 0.25f : -0.25f, 1.15f, -0.35f}}; break;
    default: base = (Pose){{0, 0, 0, 1}, {0, 0, 0}}; break;
    }
    return pcompose(base, s->offset);
}
static void initOrigins(void) {
    Pose h = headPoseAt(0);
    // LOCAL: at the head at the start, yaw only, gravity aligned; LOCAL_FLOOR: the same on the floor.
    float yaw = atan2f(2 * (h.q.w * h.q.y + h.q.x * h.q.z), 1 - 2 * (h.q.y * h.q.y + h.q.x * h.q.x));
    Q q = qeuler(yaw, 0, 0);
    gLocalOrigin = (Pose){q, h.p};
    gLocalFloorOrigin = (Pose){q, {h.p.x, 0, h.p.z}};
}
static XrTime relTime(XrTime t) { return t; }
static XrResult XRAPI_CALL mLocateSpace(XrSpace sp, XrSpace base, XrTime t, XrSpaceLocation* loc) {
    Space *a = spaceOf(sp), *b = spaceOf(base);
    if (!a || !b) return XR_ERROR_HANDLE_INVALID;
    double ts = (double)(relTime(t) - gT0) / 1e9;
    Pose rel = pcompose(pinv(spaceInStage(b, ts)), spaceInStage(a, ts));
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    loc->pose = toXr(rel);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetBounds(XrSession s, XrReferenceSpaceType t, XrExtent2Df* b) {
    (void)s;
    if (t != XR_REFERENCE_SPACE_TYPE_STAGE) { b->width = b->height = 0; return XR_SPACE_BOUNDS_UNAVAILABLE; }
    b->width = b->height = 2.0f;
    return XR_SUCCESS;
}

// ---- views ----------------------------------------------------------------
static XrResult XRAPI_CALL mEnumViewConfigs(XrInstance i, XrSystemId s, uint32_t cap, uint32_t* count, XrViewConfigurationType* t) {
    (void)i;
    (void)s;
    *count = 1;
    if (cap) t[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mViewConfigProps(XrInstance i, XrSystemId s, XrViewConfigurationType t, XrViewConfigurationProperties* p) {
    (void)i;
    (void)s;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    p->viewConfigurationType = t;
    p->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumViewConfigViews(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap, uint32_t* count, XrViewConfigurationView* v) {
    (void)i;
    (void)s;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    for (uint32_t e = 0; e < 2 && e < cap; e++) {
        v[e].recommendedImageRectWidth = (uint32_t)gEyeW;
        v[e].recommendedImageRectHeight = (uint32_t)gEyeH;
        v[e].maxImageRectWidth = 4096;
        v[e].maxImageRectHeight = 4096;
        v[e].recommendedSwapchainSampleCount = 1;
        v[e].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumBlend(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* m) {
    (void)i;
    (void)s;
    (void)t;
    *count = 1;
    if (cap) m[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

#define LOCATES 32
static struct { XrTime time; XrPosef pose[2]; } gLocates[LOCATES];
static int gLocateNext = 0;
static XrResult XRAPI_CALL mLocateViews(XrSession s, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* v) {
    (void)s;
    if (li->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    Space* base = spaceOf(li->space);
    if (!base) return XR_ERROR_HANDLE_INVALID;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    double t = (double)(relTime(li->displayTime) - gT0) / 1e9;
    Pose head = headPoseAt(t);
    Pose inBase = pinv(spaceInStage(base, t));
    for (int e = 0; e < 2; e++) {
        Pose eye = pcompose(head, (Pose){{0, 0, 0, 1}, {e ? gIpd * 0.5f : -gIpd * 0.5f, 0, 0}});
        v[e].pose = toXr(pcompose(inBase, eye));
        v[e].fov = gFov;
        gLocates[gLocateNext].pose[e] = v[e].pose;
    }
    gLocates[gLocateNext].time = li->displayTime;
    gLocateNext = (gLocateNext + 1) % LOCATES;
    return XR_SUCCESS;
}

// ---- swapchains -----------------------------------------------------------
#define MAXSC 64
typedef struct {
    int used, w, h, mips, count, arraySize;
    int64_t format;
    uint64_t usage;
    void* images;
    int acquired, next, waited;
    int lastReleased;       // image index, -1 none yet
    int everReleased;
} Chain;
static Chain gChains[MAXSC];
static int gChainCount = 0;

static XrResult XRAPI_CALL mEnumFormats(XrSession s, uint32_t cap, uint32_t* count, int64_t* f) {
    (void)s;
    const int64_t* list;
    int n = mockvk_formats(&list);
    *count = (uint32_t)n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < (uint32_t)n) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < n; i++) f[i] = list[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateSwapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    (void)s;
    if (gChainCount >= MAXSC) return XR_ERROR_LIMIT_REACHED;
    const int64_t* list;
    int n = mockvk_formats(&list), ok = 0;
    for (int i = 0; i < n; i++) ok |= list[i] == ci->format;
    if (!ok) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (ci->faceCount != 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (ci->sampleCount > 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (ci->width == 0 || ci->height == 0 || ci->width > 4096 || ci->height > 4096) return XR_ERROR_SIZE_INSUFFICIENT;
    Chain* c = &gChains[gChainCount];
    memset(c, 0, sizeof(*c));
    c->w = (int)ci->width;
    c->h = (int)ci->height;
    c->mips = (int)ci->mipCount;
    c->arraySize = ci->arraySize ? (int)ci->arraySize : 1;
    c->format = ci->format;
    c->usage = ci->usageFlags;
    c->count = 3;
    c->lastReleased = -1;
    XrResult err;
    c->images = mockvk_create_images(ci, c->count, &err);
    if (!c->images) return err;
    trace("createSwapchain sc%d %dx%d mips=%d array=%d format=0x%llx usage=0x%llx", gChainCount, c->w, c->h, c->mips, c->arraySize, (long long)c->format,
          (unsigned long long)ci->usageFlags);
    if (ci->mipCount > 1) xrmock_error("createSwapchain sc%d with %u mip levels: SteamVR on the Frame allocates level 0 only", gChainCount, ci->mipCount);
    c->used = 1;
    *out = (XrSwapchain)(uintptr_t)(1000 + gChainCount);
    gChainCount++;
    return XR_SUCCESS;
}
static Chain* chainOf(XrSwapchain sc) {
    uintptr_t k = (uintptr_t)sc;
    if (k < 1000 || k >= 1000 + (uintptr_t)gChainCount || !gChains[k - 1000].used) return NULL;
    return &gChains[k - 1000];
}
static int chainId(XrSwapchain sc) { return (int)((uintptr_t)sc - 1000); }
static XrResult XRAPI_CALL mDestroySwapchain(XrSwapchain sc) {
    Chain* c = chainOf(sc);
    if (!c) return XR_ERROR_HANDLE_INVALID;
    mockvk_destroy_images(c->images);
    c->images = NULL;
    c->used = 0;
    trace("destroySwapchain sc%d", chainId(sc));
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumSwapImages(XrSwapchain sc, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* im) {
    Chain* c = chainOf(sc);
    if (!c) return XR_ERROR_HANDLE_INVALID;
    *count = (uint32_t)c->count;
    if (cap == 0) return XR_SUCCESS;
    if (cap < (uint32_t)c->count) return XR_ERROR_SIZE_INSUFFICIENT;
    return mockvk_fill_images(c->images, c->count, im);
}
static XrResult XRAPI_CALL mAcquire(XrSwapchain sc, const XrSwapchainImageAcquireInfo* ai, uint32_t* idx) {
    (void)ai;
    Chain* c = chainOf(sc);
    if (!c) return XR_ERROR_HANDLE_INVALID;
    if (c->acquired >= c->count) {
        xrmock_error("acquire sc%d with all images acquired", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *idx = (uint32_t)c->next;
    c->next = (c->next + 1) % c->count;
    c->acquired++;
    trace("acquire sc%d idx=%u", chainId(sc), *idx);
    return XR_SUCCESS;
}
static int gReleases = 0, gBusyReleases = 0;
static double gBusyMaxMs = 0.0;
static int gWaitHist[5];
static XrResult XRAPI_CALL mWaitImage(XrSwapchain sc, const XrSwapchainImageWaitInfo* wi) {
    (void)wi;
    Chain* c = chainOf(sc);
    if (!c) return XR_ERROR_HANDLE_INVALID;
    if (c->waited >= c->acquired) {
        xrmock_error("wait sc%d without an image acquired", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // One image of a swapchain waited for at a time (SteamVR returns XR_ERROR_CALL_ORDER_INVALID otherwise).
    if (c->waited > 0) {
        xrmock_error("wait sc%d with an image already waited for and not released", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    c->waited++;
    trace("wait sc%d", chainId(sc));
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mRelease(XrSwapchain sc, const XrSwapchainImageReleaseInfo* ri) {
    (void)ri;
    Chain* c = chainOf(sc);
    if (!c) return XR_ERROR_HANDLE_INVALID;
    if (c->acquired <= 0) {
        xrmock_error("release sc%d without acquire", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (c->waited <= 0) {
        xrmock_error("release sc%d of an image not waited for", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    static int busy = -1;
    static double limit;
    if (busy < 0) {
        busy = getenv("MOCK_XR_BUSY_MS") != NULL;
        limit = busy ? atof(getenv("MOCK_XR_BUSY_MS")) : 0.0;
    }
    if (busy) {
        double ms = mockvk_release_wait_ms();
        gReleases++;
        gWaitHist[ms < 0.5 ? 0 : ms < 1.0 ? 1 : ms < 2.0 ? 2 : ms < 4.0 ? 3 : 4]++;
        if (ms > limit) {
            gBusyReleases++;
            if (ms > gBusyMaxMs) gBusyMaxMs = ms;
            xrmock_error("release sc%d before the GPU finished (queue drained in %.1f ms)", chainId(sc), ms);
        }
    }
    // The image released is the one acquired first: images are acquired in rotation.
    c->lastReleased = ((c->next - c->acquired) % c->count + c->count) % c->count;
    c->everReleased = 1;
    c->acquired--;
    c->waited--;
    trace("release sc%d", chainId(sc));
    return XR_SUCCESS;
}

// ---- session --------------------------------------------------------------
static XrResult XRAPI_CALL mCreateSession(XrInstance i, const XrSessionCreateInfo* ci, XrSession* out) {
    (void)i;
    if (gSessionMade) return XR_ERROR_LIMIT_REACHED;
    int bound = 0;
    for (const XrBaseInStructure* n = (const XrBaseInStructure*)ci->next; n && !bound; n = n->next) {
        if (mockvk_owns_binding(n->type)) {
            char desc[256];
            mockvk_describe_binding(n, desc, sizeof(desc));
            trace("createSession %s", desc);
            bound = 1;
        }
    }
    if (!bound) {
        trace("createSession: no Vulkan graphics binding");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    gSessionMade = 1;
    *out = (XrSession)(uintptr_t)2;
    gSessionState = XR_SESSION_STATE_IDLE;
    pushState(XR_SESSION_STATE_IDLE);
    pushState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroySession(XrSession s) {
    (void)s;
    trace("destroySession");
    gSessionMade = 0;
    return XR_SUCCESS;
}

// ---- timing ---------------------------------------------------------------
typedef struct {
    int id;
    int64_t waitCall, waitRet, predicted, begin;
    int skipped;
} FrameRec;
static FrameRec gWaitRec, gBeginRec;
static int gWaitPending = 0, gBeginPending = 0;
static int64_t gLastWaitRet = 0;
#define MAXSAMPLES 400000
static float *gIntervals, *gBeginEnd;
static int gNIntervals = 0, gNEnd = 0, gLate = 0, gSkippedRefreshes = 0, gEnded = 0;
static double gMargins[2] = {1e9, -1e9};
static int gSetCount = 0, gActionCount = 0, gSyncs = 0, gHaptics[2] = {0, 0}, gHapticStops = 0;
static int gPoseSets = 0, gPoseSameTime = 0, gPoseEarlier = 0, gPoseLater = 0, gPoseSplit = 0, gPoseUnknown = 0, gPoseAge[6];

static int cmpf(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }

static void summary(void) {
    if (gSummaryDone) return;
    gSummaryDone = 1;
    fprintf(stderr, "xrmock: summary: %d errors, %d warnings; %d frames ended\n", gErrors, gWarns, gEnded);
    if (gNIntervals > 1) {
        qsort(gIntervals, (size_t)gNIntervals, sizeof(float), cmpf);
        double sum = 0;
        for (int i = 0; i < gNIntervals; i++) sum += gIntervals[i];
        double avg = sum / gNIntervals;
        fprintf(stderr, "xrmock: timing: xrWaitFrame interval avg %.3f ms (%.1f Hz, period %.3f) min %.3f p99 %.3f max %.3f over %d frames\n", avg, 1000.0 / avg,
                (double)gPeriod / 1e6, gIntervals[0], gIntervals[(int)((gNIntervals - 1) * 0.99)], gIntervals[gNIntervals - 1], gNIntervals);
    }
    if (gNEnd) {
        double sum = 0, mx = 0;
        for (int i = 0; i < gNEnd; i++) { sum += gBeginEnd[i]; if (gBeginEnd[i] > mx) mx = gBeginEnd[i]; }
        fprintf(stderr, "xrmock: timing: xrBeginFrame to xrEndFrame avg %.3f ms max %.3f; end margin before the display time min %.2f max %.2f ms; %d late of %d (%d refreshes lost)\n",
                sum / gNEnd, mx, gMargins[0], gMargins[1], gLate, gNEnd, gSkippedRefreshes);
    }
    fprintf(stderr, "xrmock: projection layers %d: both eyes from one locate for the display time submitted %d, an earlier one %d, a later one %d; different locates %d, not from a locate %d\n",
            gPoseSets, gPoseSameTime, gPoseEarlier, gPoseLater, gPoseSplit, gPoseUnknown);
    fprintf(stderr, "xrmock: projection layers by refreshes from the locate to the display time: later %d, 0: %d, 1: %d, 2: %d, 3: %d, more %d\n", gPoseAge[0], gPoseAge[1],
            gPoseAge[2], gPoseAge[3], gPoseAge[4], gPoseAge[5]);
    fprintf(stderr, "xrmock: input: %d action sets, %d actions, %d syncs, haptic pulses left %d right %d, %d stops\n", gSetCount, gActionCount, gSyncs, gHaptics[0], gHaptics[1], gHapticStops);
    if (gReleases)
        fprintf(stderr, "xrmock: releases checked for finished GPU work %d, %d before it finished (worst %.1f ms); queue drain <0.5 ms %d, 0.5-1 %d, 1-2 %d, 2-4 %d, >4 %d\n",
                gReleases, gBusyReleases, gBusyMaxMs, gWaitHist[0], gWaitHist[1], gWaitHist[2], gWaitHist[3], gWaitHist[4]);
}
__attribute__((destructor)) static void atExit(void) {
    if (gInstanceMade && !gSummaryDone) {
        fprintf(stderr, "xrmock: the app exited without xrEndSession\n");
        summary();
    }
}

static XrResult XRAPI_CALL mBeginSession(XrSession s, const XrSessionBeginInfo* bi) {
    (void)s;
    if (gSessionRunning) return XR_ERROR_SESSION_RUNNING;
    if (gSessionState != XR_SESSION_STATE_READY) {
        xrmock_error("xrBeginSession in state %d, not READY", gSessionState);
        return XR_ERROR_SESSION_NOT_READY;
    }
    if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    gT0 = nowNs();
    initOrigins();
    const char* secs = getenv("MOCK_XR_SECONDS");
    double sec = secs ? atof(secs) : 20.0;
    gEndAt = sec > 0 ? gT0 + (int64_t)(sec * 1e9) : 0;
    gSessionRunning = 1;
    pushState(XR_SESSION_STATE_SYNCHRONIZED);
    pushState(XR_SESSION_STATE_VISIBLE);
    pushState(XR_SESSION_STATE_FOCUSED);
    trace("beginSession");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEndSession(XrSession s) {
    (void)s;
    if (!gSessionRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    if (gSessionState != XR_SESSION_STATE_STOPPING) xrmock_error("xrEndSession in state %d, not STOPPING", gSessionState);
    gSessionRunning = 0;
    summary();
    pushState(XR_SESSION_STATE_IDLE);
    pushState(XR_SESSION_STATE_EXITING);
    trace("endSession");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mRequestExit(XrSession s) {
    (void)s;
    if (!gSessionRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    gQuitSent = 1;
    pushState(XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPollEvent(XrInstance i, XrEventDataBuffer* ev) {
    (void)i;
    if (gEventCount == 0) return XR_EVENT_UNAVAILABLE;
    int st = gEventQueue[0];
    memmove(gEventQueue, gEventQueue + 1, (size_t)(--gEventCount) * sizeof(int));
    XrEventDataSessionStateChanged* e = (XrEventDataSessionStateChanged*)ev;
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->next = NULL;
    e->session = (XrSession)(uintptr_t)2;
    e->state = (XrSessionState)st;
    e->time = nowNs();
    gSessionState = st;
    trace("event sessionState=%d", st);
    return XR_SUCCESS;
}

// ---- frame loop -----------------------------------------------------------
static int gWaits = 0;
static XrResult XRAPI_CALL mWaitFrame(XrSession s, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    (void)s;
    (void)wi;
    if (!gSessionRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    int64_t call = nowNs();
    static int skip = -1;
    if (skip < 0) skip = getenv("MOCK_XR_SKIP") ? atoi(getenv("MOCK_XR_SKIP")) != 0 : 1;
    int skipped = 0;
    if (skip && call > gT0 + (int64_t)gWaits * gPeriod + gPeriod / 2) {
        int to = (int)((call - gT0) / gPeriod) + 1;
        skipped = to - gWaits;
        gWaits = to;
    }
    int64_t wake = gT0 + (int64_t)gWaits * gPeriod;
    if (wake > call) {
        struct timespec ts = {(time_t)((wake - call) / 1000000000ll), (long)((wake - call) % 1000000000ll)};
        nanosleep(&ts, NULL);
    }
    int64_t ret = nowNs();
    fs->predictedDisplayTime = wake + (int64_t)gPipeline * gPeriod;
    fs->predictedDisplayPeriod = gPeriod;
    fs->shouldRender = XR_TRUE;
    gWaits++;
    gFrame++;
    if (gWaitPending) warnf("xrWaitFrame called again before xrBeginFrame");
    gWaitRec = (FrameRec){gFrame, call, ret, fs->predictedDisplayTime, 0, skipped};
    gWaitPending = 1;
    if (gLastWaitRet && gNIntervals < MAXSAMPLES) gIntervals[gNIntervals++] = (float)((double)(ret - gLastWaitRet) / 1e6);
    gLastWaitRet = ret;
    gSkippedRefreshes += skipped;
    if (((gEndAt && ret > gEndAt) || (gMaxFrames && gFrame >= gMaxFrames)) && !gQuitSent) {
        gQuitSent = 1;
        pushState(XR_SESSION_STATE_STOPPING);
    }
    trace("waitFrame");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mBeginFrame(XrSession s, const XrFrameBeginInfo* bi) {
    (void)s;
    (void)bi;
    if (!gSessionRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    if (!gWaitPending) {
        xrmock_error("xrBeginFrame without xrWaitFrame");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    XrResult r = XR_SUCCESS;
    if (gBeginPending) {
        warnf("xrBeginFrame before the previous xrEndFrame: that frame is discarded");
        r = XR_FRAME_DISCARDED;
    }
    gBeginRec = gWaitRec;
    gBeginRec.begin = nowNs();
    gBeginPending = 1;
    gWaitPending = 0;
    trace("beginFrame");
    return r;
}

static int samePose(const XrPosef* a, const XrPosef* b) { return !memcmp(a, b, sizeof(XrPosef)); }
static void checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime) {
    if (p->viewCount != 2) {
        xrmock_error("projection layer with %u views", p->viewCount);
        return;
    }
    gPoseSets++;
    int left = -1, both = -1;
    for (int i = 0; i < LOCATES; i++) {
        if (!gLocates[i].time) continue;
        if (samePose(&gLocates[i].pose[0], &p->views[0].pose)) {
            left = i;
            if (samePose(&gLocates[i].pose[1], &p->views[1].pose)) both = i;
        }
    }
    if (both < 0) {
        if (left >= 0) {
            gPoseSplit++;
            xrmock_error("projection layer eyes from different locates");
        } else {
            gPoseUnknown++;
        }
        return;
    }
    XrTime t = gLocates[both].time;
    if (t == displayTime) gPoseSameTime++;
    else if (t < displayTime) gPoseEarlier++;
    else gPoseLater++;
    long k = lround((double)(displayTime - t) / (double)gPeriod);
    gPoseAge[k < 0 ? 0 : k > 3 ? 5 : k + 1]++;
}

// Checks a sub-image against its swapchain; returns the chain or NULL.
static Chain* checkSub(const XrSwapchainSubImage* si, const char* what) {
    Chain* c = chainOf(si->swapchain);
    if (!c) {
        xrmock_error("%s: swapchain handle invalid", what);
        return NULL;
    }
    int id = chainId(si->swapchain);
    if (c->acquired > 0) xrmock_error("%s: sc%d still has an acquired image (not released before xrEndFrame)", what, id);
    if (!c->everReleased) xrmock_error("%s: sc%d never had an image released", what, id);
    const XrRect2Di* r = &si->imageRect;
    if (r->offset.x < 0 || r->offset.y < 0 || r->extent.width <= 0 || r->extent.height <= 0 || r->offset.x + r->extent.width > c->w || r->offset.y + r->extent.height > c->h)
        xrmock_error("%s: image rect %d,%d %dx%d outside sc%d (%dx%d)", what, r->offset.x, r->offset.y, r->extent.width, r->extent.height, id, c->w, c->h);
    if ((int)si->imageArrayIndex >= c->arraySize) xrmock_error("%s: array index %u of sc%d with %d layers", what, si->imageArrayIndex, id, c->arraySize);
    return c;
}
static void shot(int frame, Chain* c, const XrSwapchainSubImage* si, const char* name) {
    if (!c || c->lastReleased < 0) return;
    char path[512], stats[160] = "";
    snprintf(path, sizeof(path), "%s/f%04d_%s.png", gShotDir, frame, name);
    int rc = mockvk_dump_png(c->images, c->lastReleased, si->imageArrayIndex, si->imageRect.offset.x, si->imageRect.offset.y, si->imageRect.extent.width,
                             si->imageRect.extent.height, path, stats, sizeof(stats));
    trace("shot %s sc%d idx=%d %s %s", name, chainId(si->swapchain), c->lastReleased, rc ? "FAILED" : "ok", stats);
    if (rc) warnf("could not dump %s", path);
}
static int sane(const XrPosef* p) {
    float n = p->orientation.x * p->orientation.x + p->orientation.y * p->orientation.y + p->orientation.z * p->orientation.z + p->orientation.w * p->orientation.w;
    return fabsf(n - 1.0f) < 0.01f;
}
static int fovOk(const XrFovf* f) {
    return f->angleLeft < f->angleRight && f->angleDown < f->angleUp && f->angleLeft > -1.5707f && f->angleRight < 1.5707f && f->angleDown > -1.5707f && f->angleUp < 1.5707f;
}

static XrResult XRAPI_CALL mEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    (void)s;
    if (!gSessionRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    if (!gBeginPending) {
        xrmock_error("xrEndFrame without xrBeginFrame");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    int64_t end = nowNs();
    int frame = gBeginRec.id, want = shotWanted(frame);
    if (ei->displayTime != gBeginRec.predicted) warnf("xrEndFrame displayTime differs from the predicted time of its xrWaitFrame (%+.3f ms)", (double)(ei->displayTime - gBeginRec.predicted) / 1e6);
    if (ei->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) xrmock_error("xrEndFrame blend mode %d not offered", (int)ei->environmentBlendMode);
    char buf[1024] = "";
    XrResult result = XR_SUCCESS;
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader* l = ei->layers[i];
        char t[256], nm[64];
        if (!spaceOf(l->space)) {
            xrmock_error("layer %u: space handle invalid", i);
            result = XR_ERROR_HANDLE_INVALID;
            continue;
        }
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection* p = (const XrCompositionLayerProjection*)l;
            checkPoses(p, ei->displayTime);
            snprintf(t, sizeof(t), " proj[flags=%llx", (unsigned long long)p->layerFlags);
            strcat(buf, t);
            for (uint32_t v = 0; v < p->viewCount; v++) {
                const XrCompositionLayerProjectionView* pv = &p->views[v];
                if (!sane(&pv->pose)) xrmock_error("layer %u view %u: orientation not normalised", i, v);
                if (!fovOk(&pv->fov)) xrmock_error("layer %u view %u: fov out of range", i, v);
                snprintf(nm, sizeof(nm), "layer %u view %u", i, v);
                Chain* c = checkSub(&pv->subImage, nm);
                int depth = 0;
                for (const XrBaseInStructure* n = (const XrBaseInStructure*)pv->next; n; n = n->next) {
                    if (n->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                        const XrCompositionLayerDepthInfoKHR* d = (const XrCompositionLayerDepthInfoKHR*)n;
                        if (d->minDepth > d->maxDepth || d->nearZ == d->farZ) xrmock_error("layer %u view %u: bad depth info", i, v);
                        Chain* dc = checkSub(&d->subImage, nm);
                        depth = dc ? 1 : 0;
                    }
                }
                snprintf(t, sizeof(t), " sc%d:%dx%d%s", c ? chainId(pv->subImage.swapchain) : -1, pv->subImage.imageRect.extent.width, pv->subImage.imageRect.extent.height,
                         depth ? "+depth" : "");
                strcat(buf, t);
                if (want) {
                    snprintf(nm, sizeof(nm), "proj%u_eye%u", i, v);
                    shot(frame, c, &pv->subImage, nm);
                }
            }
            strcat(buf, "]");
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)l;
            if (!sane(&q->pose)) xrmock_error("layer %u quad: orientation not normalised", i);
            if (!(q->size.width > 0 && q->size.height > 0)) xrmock_error("layer %u quad: size %gx%g", i, q->size.width, q->size.height);
            snprintf(nm, sizeof(nm), "layer %u quad", i);
            Chain* c = checkSub(&q->subImage, nm);
            snprintf(t, sizeof(t), " quad[sc%d:%dx%d eye=%d]", c ? chainId(q->subImage.swapchain) : -1, q->subImage.imageRect.extent.width, q->subImage.imageRect.extent.height,
                     (int)q->eyeVisibility);
            strcat(buf, t);
            if (want) {
                snprintf(nm, sizeof(nm), "quad%u", i);
                shot(frame, c, &q->subImage, nm);
            }
        } else {
            snprintf(t, sizeof(t), " layer%d", (int)l->type);
            strcat(buf, t);
        }
    }
    double margin = (double)(gBeginRec.predicted - end) / 1e6;
    double deadline = gDeadlineMs >= 0 ? gDeadlineMs : (double)gPeriod / 2e6;
    int late = margin < deadline;
    if (late) gLate++;
    if (margin < gMargins[0]) gMargins[0] = margin;
    if (margin > gMargins[1]) gMargins[1] = margin;
    if (gNEnd < MAXSAMPLES) gBeginEnd[gNEnd++] = (float)((double)(end - gBeginRec.begin) / 1e6);
    gEnded++;
    if (gTiming)
        fprintf(gTiming, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%d,%d\n", frame, msSince(gBeginRec.waitCall), msSince(gBeginRec.waitRet),
                (double)(gBeginRec.waitRet - gBeginRec.waitCall) / 1e6, msSince(gBeginRec.predicted), (double)gPeriod / 1e6, msSince(gBeginRec.begin), msSince(end),
                (double)(end - gBeginRec.begin) / 1e6, margin, ei->layerCount, gBeginRec.skipped, late);
    gBeginPending = 0;
    trace("endFrame layers=%u%s", ei->layerCount, buf);
    return result;
}

// ---- paths, actions, input (inert) ----------------------------------------
static XrResult XRAPI_CALL mStringToPath(XrInstance i, const char* s, XrPath* p) {
    (void)i;
    for (int k = 0; k < gPathCount; k++)
        if (!strcmp(gPathStr[k], s)) { *p = gPaths[k]; return XR_SUCCESS; }
    if (gPathCount >= 512) return XR_ERROR_LIMIT_REACHED;
    strncpy(gPathStr[gPathCount], s, 127);
    gPaths[gPathCount] = (XrPath)(5000 + gPathCount);
    *p = gPaths[gPathCount++];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPathToString(XrInstance i, XrPath p, uint32_t cap, uint32_t* len, char* buf) {
    (void)i;
    for (int k = 0; k < gPathCount; k++)
        if (gPaths[k] == p) {
            *len = (uint32_t)strlen(gPathStr[k]) + 1;
            if (cap == 0) return XR_SUCCESS;
            if (cap < *len) return XR_ERROR_SIZE_INSUFFICIENT;
            memcpy(buf, gPathStr[k], *len);
            return XR_SUCCESS;
        }
    return XR_ERROR_PATH_INVALID;
}
// Actions: created, bound and attached with the OpenXR rules (nothing changes after the attach, a bound path has to be
// an input of the profile), synced, and scripted: MOCK_XR_INPUT="t0-t1:path[=v[,v]];..." holds that input path from t0
// to t1 seconds after xrBeginSession (a button: no value or 1; a float: =0.8; a thumbstick: =x,y). An action reports
// the scripted input on any path it is bound to, as of the last xrSyncActions. Haptic pulses go to the trace.
typedef struct { char name[64]; XrActionType type; int set; XrPath bind[24]; int nbind; XrBool32 b; float f, x, y; } MAction;
typedef struct { char name[64]; int attached; } MSet;
typedef struct { double t0, t1; char path[128]; float v[2]; int nv; } MScript;
static MAction gAct[96];
static MSet gSets[8];
static int gAttachedAll = 0;
static MScript gScript[64];
static int gScriptCount = 0, gScriptLoaded = 0;
static const char* const kFrameInputs[] = {
    "a/click", "b/click", "x/click", "y/click", "menu/click", "view/click", "dpad_up/click", "dpad_down/click", "dpad_left/click", "dpad_right/click",
    "thumbstick", "thumbstick/click", "trigger/value", "squeeze/value", "grip/value", "bumper/click", "aim/pose", "grip/pose", "haptic"};

static const char* pathStr(XrPath p) {
    for (int k = 0; k < gPathCount; k++)
        if (gPaths[k] == p) return gPathStr[k];
    return NULL;
}
static void loadScript(void) {
    if (gScriptLoaded) return;
    gScriptLoaded = 1;
    const char* e = getenv("MOCK_XR_INPUT");
    if (!e) return;
    char* copy = strdup(e);
    char* save = NULL;
    for (char* tok = strtok_r(copy, ";", &save); tok && gScriptCount < 64; tok = strtok_r(NULL, ";", &save)) {
        MScript* m = &gScript[gScriptCount];
        char path[128] = "";
        char vals[64] = "";
        double t0, t1;
        char* eq = strchr(tok, '=');
        if (eq) { snprintf(vals, sizeof vals, "%s", eq + 1); *eq = 0; }
        if (sscanf(tok, "%lf-%lf:%127s", &t0, &t1, path) != 3) { fprintf(stderr, "xrmock: MOCK_XR_INPUT: cannot read '%s'\n", tok); continue; }
        m->t0 = t0, m->t1 = t1, m->nv = 0;
        snprintf(m->path, sizeof m->path, "%s", path);
        if (vals[0]) m->nv = sscanf(vals, "%f,%f", &m->v[0], &m->v[1]);
        else m->v[0] = 1, m->nv = 1;
        gScriptCount++;
    }
    free(copy);
}
static XrResult XRAPI_CALL mCreateActionSet(XrInstance i, const XrActionSetCreateInfo* ci, XrActionSet* o) {
    (void)i;
    if (!ci->actionSetName[0]) return XR_ERROR_NAME_INVALID;
    for (int k = 0; k < gSetCount; k++) if (!strcmp(gSets[k].name, ci->actionSetName)) return XR_ERROR_NAME_DUPLICATED;
    if (gSetCount >= 8) return XR_ERROR_LIMIT_REACHED;
    snprintf(gSets[gSetCount].name, sizeof gSets[0].name, "%s", ci->actionSetName);
    *o = (XrActionSet)(uintptr_t)(3 + gSetCount);
    trace("createActionSet %s", ci->actionSetName);
    gSetCount++;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateAction(XrActionSet s, const XrActionCreateInfo* ci, XrAction* o) {
    int set = (int)(uintptr_t)s - 3;
    if (set < 0 || set >= gSetCount) return XR_ERROR_HANDLE_INVALID;
    if (gSets[set].attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    if (gActionCount >= 96) return XR_ERROR_LIMIT_REACHED;
    for (int k = 0; k < gActionCount; k++)
        if (gAct[k].set == set && !strcmp(gAct[k].name, ci->actionName)) return XR_ERROR_NAME_DUPLICATED;
    MAction* a = &gAct[gActionCount];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", ci->actionName);
    a->type = ci->actionType, a->set = set;
    *o = (XrAction)(uintptr_t)(200 + gActionCount++);
    return XR_SUCCESS;
}
static MAction* actionOf(XrAction h) {
    int k = (int)((uintptr_t)h - 200);
    return k >= 0 && k < gActionCount ? &gAct[k] : NULL;
}
static XrResult XRAPI_CALL mSuggest(XrInstance i, const XrInteractionProfileSuggestedBinding* b) {
    (void)i;
    const char* prof = pathStr(b->interactionProfile);
    if (!prof) return XR_ERROR_PATH_INVALID;
    int checked = !strcmp(prof, "/interaction_profiles/valve/frame_controller");
    for (uint32_t k = 0; k < b->countSuggestedBindings; k++) {
        const char* ps = pathStr(b->suggestedBindings[k].binding);
        MAction* a = actionOf(b->suggestedBindings[k].action);
        if (!a || !ps) { xrmock_error("suggestBindings %s: unknown action or path", prof); return XR_ERROR_HANDLE_INVALID; }
        if (gSets[a->set].attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        if (checked) {  // (the Frame profile: only the inputs SteamVR's profile has)
            int ok = 0;
            const char* in = !strncmp(ps, "/user/hand/left/input/", 22) ? ps + 22 : !strncmp(ps, "/user/hand/right/input/", 23) ? ps + 23 : NULL;
            if (!in && (!strcmp(ps, "/user/hand/left/output/haptic") || !strcmp(ps, "/user/hand/right/output/haptic"))) in = "haptic";
            if (in) for (unsigned c = 0; c < sizeof kFrameInputs / sizeof *kFrameInputs; c++) if (!strcmp(in, kFrameInputs[c])) ok = 1;
            if (!ok) { xrmock_error("suggestBindings %s: %s is not an input of the profile", prof, ps); return XR_ERROR_PATH_UNSUPPORTED; }
        }
        if (a->nbind < 24) a->bind[a->nbind++] = b->suggestedBindings[k].binding;
    }
    trace("suggestBindings %s n=%u", prof, b->countSuggestedBindings);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mAttach(XrSession s, const XrSessionActionSetsAttachInfo* a) {
    (void)s;
    if (gAttachedAll) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t k = 0; k < a->countActionSets; k++) {
        int set = (int)(uintptr_t)a->actionSets[k] - 3;
        if (set < 0 || set >= gSetCount) return XR_ERROR_HANDLE_INVALID;
        gSets[set].attached = 1;
    }
    gAttachedAll = 1;
    trace("attachActionSets sets=%u actions=%d", a->countActionSets, gActionCount);
    return XR_SUCCESS;
}
static int scriptState(const MAction* a, double t, float* v, int* nv) {  // a scripted input on one of its paths held at t
    for (int k = 0; k < gScriptCount; k++) {
        const MScript* m = &gScript[k];
        if (t < m->t0 || t >= m->t1) continue;
        for (int j = 0; j < a->nbind; j++) {
            const char* ps = pathStr(a->bind[j]);
            if (ps && !strcmp(ps, m->path)) { v[0] = m->v[0], v[1] = m->nv > 1 ? m->v[1] : 0, *nv = m->nv; return 1; }
        }
    }
    return 0;
}
static XrResult XRAPI_CALL mSync(XrSession s, const XrActionsSyncInfo* in) {
    (void)s;
    if (!gAttachedAll) { xrmock_error("xrSyncActions before xrAttachSessionActionSets"); return XR_ERROR_ACTIONSET_NOT_ATTACHED; }
    if (gSessionState != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
    loadScript();
    double t = (double)(nowNs() - gT0) / 1e9;
    for (uint32_t k = 0; k < in->countActiveActionSets; k++) {
        int set = (int)(uintptr_t)in->activeActionSets[k].actionSet - 3;
        if (set < 0 || set >= gSetCount || !gSets[set].attached) { xrmock_error("xrSyncActions: action set not attached"); return XR_ERROR_ACTIONSET_NOT_ATTACHED; }
        for (int i = 0; i < gActionCount; i++) {
            MAction* a = &gAct[i];
            if (a->set != set) continue;
            float v[2] = {0, 0};
            int nv = 0, on = scriptState(a, t, v, &nv);
            XrBool32 b = on && v[0] > 0.5f;
            float f = on ? v[0] : 0, x = on ? v[0] : 0, y = on && nv > 1 ? v[1] : 0;
            if (a->type == XR_ACTION_TYPE_VECTOR2F_INPUT && on && nv < 2) x = v[0], y = 0;
            if ((b != a->b || f != a->f || x != a->x || y != a->y) && (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT || a->type == XR_ACTION_TYPE_FLOAT_INPUT || a->type == XR_ACTION_TYPE_VECTOR2F_INPUT))
                trace("input %s = %.2f %.2f,%.2f", a->name, a->type == XR_ACTION_TYPE_BOOLEAN_INPUT ? (float)b : f, x, y);
            a->b = b, a->f = f, a->x = x, a->y = y;
        }
    }
    gSyncs++;
    return XR_SUCCESS;
}
static MAction* stateAction(const XrActionStateGetInfo* g, XrActionType want) {
    MAction* a = actionOf(g->action);
    if (!a) return NULL;
    if (!gSets[a->set].attached) { xrmock_error("xrGetActionState for an action of a set that is not attached"); return NULL; }
    if (a->type != want) { xrmock_error("xrGetActionState of %s with the wrong type", a->name); return NULL; }
    if (!gSyncs) warnf("xrGetActionState before the first xrSyncActions");
    return a;
}
static XrResult XRAPI_CALL mGetBool(XrSession s, const XrActionStateGetInfo* g, XrActionStateBoolean* o) {
    (void)s;
    MAction* a = stateAction(g, XR_ACTION_TYPE_BOOLEAN_INPUT);
    if (!a) return XR_ERROR_ACTION_TYPE_MISMATCH;
    o->isActive = a->nbind > 0 && gSyncs > 0;
    o->currentState = o->isActive ? a->b : XR_FALSE;
    o->changedSinceLastSync = XR_FALSE, o->lastChangeTime = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetFloat(XrSession s, const XrActionStateGetInfo* g, XrActionStateFloat* o) {
    (void)s;
    MAction* a = stateAction(g, XR_ACTION_TYPE_FLOAT_INPUT);
    if (!a) return XR_ERROR_ACTION_TYPE_MISMATCH;
    o->isActive = a->nbind > 0 && gSyncs > 0;
    o->currentState = o->isActive ? a->f : 0;
    o->changedSinceLastSync = XR_FALSE, o->lastChangeTime = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetVec2(XrSession s, const XrActionStateGetInfo* g, XrActionStateVector2f* o) {
    (void)s;
    MAction* a = stateAction(g, XR_ACTION_TYPE_VECTOR2F_INPUT);
    if (!a) return XR_ERROR_ACTION_TYPE_MISMATCH;
    o->isActive = a->nbind > 0 && gSyncs > 0;
    o->currentState.x = o->isActive ? a->x : 0, o->currentState.y = o->isActive ? a->y : 0;
    o->changedSinceLastSync = XR_FALSE, o->lastChangeTime = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetPose(XrSession s, const XrActionStateGetInfo* g, XrActionStatePose* o) { (void)s; (void)g; o->isActive = XR_TRUE; return XR_SUCCESS; }
static int handOf(XrPath p) {
    const char* ps = pathStr(p);
    return ps && strstr(ps, "left") ? 0 : ps && strstr(ps, "right") ? 1 : -1;
}
static XrResult XRAPI_CALL mHaptic(XrSession s, const XrHapticActionInfo* h, const XrHapticBaseHeader* b) {
    (void)s;
    MAction* a = actionOf(h->action);
    if (!a || a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT) { xrmock_error("xrApplyHapticFeedback with an action that is not a vibration output"); return XR_ERROR_ACTION_TYPE_MISMATCH; }
    if (!gAttachedAll) { xrmock_error("xrApplyHapticFeedback before the action sets are attached"); return XR_ERROR_ACTIONSET_NOT_ATTACHED; }
    if (b->type != XR_TYPE_HAPTIC_VIBRATION) return XR_ERROR_VALIDATION_FAILURE;
    const XrHapticVibration* v = (const XrHapticVibration*)b;
    int hand = handOf(h->subactionPath);
    if (v->amplitude < 0 || v->amplitude > 1) xrmock_error("haptic amplitude %.2f outside 0..1", v->amplitude);
    if (hand >= 0) gHaptics[hand]++;
    trace("haptic hand=%s amplitude=%.2f duration_ms=%.0f frequency=%.0f", hand == 0 ? "left" : hand == 1 ? "right" : "?", v->amplitude, (double)v->duration / 1e6, v->frequency);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mHapticStop(XrSession s, const XrHapticActionInfo* h) {
    (void)s;
    int hand = handOf(h->subactionPath);
    gHapticStops++;
    trace("hapticStop hand=%s", hand == 0 ? "left" : hand == 1 ? "right" : "?");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroyActionSet(XrActionSet s) { (void)s; return XR_SUCCESS; }
static XrResult XRAPI_CALL mGetProfile(XrSession s, XrPath p, XrInteractionProfileState* o) { (void)s; (void)p; o->interactionProfile = XR_NULL_PATH; return XR_SUCCESS; }

// ---- refresh rate, perf, time conversion ----------------------------------
static XrResult XRAPI_CALL mReqRefresh(XrSession s, float hz) { (void)s; trace("requestRefreshRate %.0f", hz); return XR_SUCCESS; }
static XrResult XRAPI_CALL mEnumRefresh(XrSession s, uint32_t cap, uint32_t* count, float* r) {
    (void)s;
    *count = 3;
    if (cap) { r[0] = 72; r[1] = 90; r[2] = 120; }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetRefresh(XrSession s, float* hz) { (void)s; *hz = (float)(1e9 / (double)gPeriod); return XR_SUCCESS; }
static XrResult XRAPI_CALL mPerfLevel(XrSession s, XrPerfSettingsDomainEXT d, XrPerfSettingsLevelEXT l) { (void)s; (void)d; (void)l; return XR_SUCCESS; }
static XrResult XRAPI_CALL mTimespecToTime(XrInstance i, const struct timespec* ts, XrTime* t) { (void)i; *t = (int64_t)ts->tv_sec * 1000000000ll + ts->tv_nsec; return XR_SUCCESS; }
static XrResult XRAPI_CALL mTimeToTimespec(XrInstance i, XrTime t, struct timespec* ts) { (void)i; ts->tv_sec = (time_t)(t / 1000000000ll); ts->tv_nsec = (long)(t % 1000000000ll); return XR_SUCCESS; }

typedef struct { const char* name; PFN_xrVoidFunction fn; } Entry;
#define E(n, f) {n, (PFN_xrVoidFunction)f}
static const Entry kTable[] = {
    E("xrEnumerateInstanceExtensionProperties", mEnumExt), E("xrCreateInstance", mCreateInstance), E("xrDestroyInstance", mDestroyInstance),
    E("xrGetInstanceProperties", mGetInstanceProps), E("xrResultToString", mResultToString), E("xrGetSystem", mGetSystem), E("xrGetSystemProperties", mGetSystemProps),
    E("xrCreateSession", mCreateSession), E("xrDestroySession", mDestroySession), E("xrEnumerateReferenceSpaces", mEnumRefSpaces),
    E("xrCreateReferenceSpace", mCreateRefSpace), E("xrCreateActionSpace", mCreateActionSpace), E("xrDestroySpace", mDestroySpace), E("xrLocateSpace", mLocateSpace),
    E("xrGetReferenceSpaceBoundsRect", mGetBounds), E("xrEnumerateViewConfigurations", mEnumViewConfigs), E("xrGetViewConfigurationProperties", mViewConfigProps),
    E("xrEnumerateViewConfigurationViews", mEnumViewConfigViews), E("xrEnumerateEnvironmentBlendModes", mEnumBlend), E("xrEnumerateSwapchainFormats", mEnumFormats),
    E("xrCreateSwapchain", mCreateSwapchain), E("xrDestroySwapchain", mDestroySwapchain), E("xrEnumerateSwapchainImages", mEnumSwapImages),
    E("xrAcquireSwapchainImage", mAcquire), E("xrWaitSwapchainImage", mWaitImage), E("xrReleaseSwapchainImage", mRelease), E("xrBeginSession", mBeginSession),
    E("xrEndSession", mEndSession), E("xrRequestExitSession", mRequestExit), E("xrPollEvent", mPollEvent), E("xrWaitFrame", mWaitFrame), E("xrBeginFrame", mBeginFrame),
    E("xrEndFrame", mEndFrame), E("xrLocateViews", mLocateViews), E("xrStringToPath", mStringToPath), E("xrPathToString", mPathToString),
    E("xrCreateActionSet", mCreateActionSet), E("xrCreateAction", mCreateAction), E("xrSuggestInteractionProfileBindings", mSuggest),
    E("xrAttachSessionActionSets", mAttach), E("xrSyncActions", mSync), E("xrGetActionStateBoolean", mGetBool), E("xrGetActionStateFloat", mGetFloat),
    E("xrGetActionStateVector2f", mGetVec2), E("xrGetActionStatePose", mGetPose), E("xrApplyHapticFeedback", mHaptic), E("xrStopHapticFeedback", mHapticStop),
    E("xrGetCurrentInteractionProfile", mGetProfile), E("xrDestroyActionSet", mDestroyActionSet), E("xrRequestDisplayRefreshRateFB", mReqRefresh), E("xrEnumerateDisplayRefreshRatesFB", mEnumRefresh),
    E("xrGetDisplayRefreshRateFB", mGetRefresh), E("xrPerfSettingsSetPerformanceLevelEXT", mPerfLevel), E("xrConvertTimespecTimeToTimeKHR", mTimespecToTime),
    E("xrConvertTimeToTimespecTimeKHR", mTimeToTimespec),
};

static void init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char* v;
    if ((v = getenv("MOCK_XR_TRACE"))) gTrace = fopen(v, "w");
    if ((v = getenv("MOCK_XR_TIMING"))) {
        gTiming = fopen(v, "w");
        if (gTiming)
            fprintf(gTiming, "# frame,wait_call_ms,wait_return_ms,wait_block_ms,predicted_display_ms,period_ms,begin_ms,end_ms,begin_to_end_ms,end_margin_ms,layers,refreshes_lost,late"
                             " (times in ms from xrBeginSession; end_margin = predicted display time minus the xrEndFrame call)\n");
    }
    if ((v = getenv("MOCK_XR_HZ")) && atof(v) > 0) gPeriod = (int64_t)(1e9 / atof(v));
    if ((v = getenv("MOCK_XR_PIPELINE")) && atoi(v) > 0) gPipeline = atoi(v);
    if ((v = getenv("MOCK_XR_DEADLINE_MS"))) gDeadlineMs = atof(v);
    if ((v = getenv("MOCK_XR_FRAMES"))) gMaxFrames = atoi(v);
    if ((v = getenv("MOCK_XR_EYE_SIZE"))) sscanf(v, "%dx%d", &gEyeW, &gEyeH);
    if ((v = getenv("MOCK_XR_IPD"))) gIpd = (float)atof(v);
    if ((v = getenv("MOCK_XR_FOV"))) {
        float l, r, u, d;
        if (sscanf(v, "%f,%f,%f,%f", &l, &r, &u, &d) == 4) gFov = (XrFovf){l * D2R, r * D2R, u * D2R, d * D2R};
    }
    if ((v = getenv("MOCK_XR_SHOT"))) snprintf(gShotSpec, sizeof(gShotSpec), "%s", v);
    if ((v = getenv("MOCK_XR_SHOT_DIR"))) gShotDir = v;
    if ((v = getenv("MOCK_XR_VK_MAX"))) {
        unsigned a = 1, b = 3;
        if (sscanf(v, "%u.%u", &a, &b) == 2) mockvk_set_max_api(a, b);
    }
    loadPose(getenv("MOCK_XR_POSE"));
    gIntervals = (float*)malloc(sizeof(float) * MAXSAMPLES);
    gBeginEnd = (float*)malloc(sizeof(float) * MAXSAMPLES);
    const char* list = getenv("MOCK_XR_EXTENSIONS");
    if (list) {
        char* copy = strdup(list);
        for (char* tok = strtok(copy, ","); tok && gExtCount < 64; tok = strtok(NULL, ",")) strcpy(gExtNames[gExtCount++], tok);
        free(copy);
    } else {
        // What SteamVR offers on the Frame, to the best of our knowledge (widen with MOCK_XR_EXTENSIONS).
        const char* exts[] = {"XR_KHR_vulkan_enable2", "XR_KHR_convert_timespec_time", "XR_FB_display_refresh_rate", "XR_EXT_performance_settings",
                              "XR_VALVE_frame_controller_interaction", "XR_KHR_composition_layer_depth", "XR_EXT_eye_gaze_interaction"};
        for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) strcpy(gExtNames[gExtCount++], exts[i]);
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL mockGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
    (void)instance;
    init();
    for (unsigned i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
        if (!strcmp(kTable[i].name, name)) {
            *fn = kTable[i].fn;
            return XR_SUCCESS;
        }
    }
    PFN_xrVoidFunction f = mockvk_get_proc(name);
    if (f) {
        *fn = f;
        return XR_SUCCESS;
    }
    *fn = NULL;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

__attribute__((visibility("default"))) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* li, XrNegotiateRuntimeRequest* rr) {
    (void)li;
    init();
    rr->runtimeInterfaceVersion = 1;
    rr->runtimeApiVersion = XR_CURRENT_API_VERSION;
    rr->getInstanceProcAddr = mockGetInstanceProcAddr;
    return XR_SUCCESS;
}
