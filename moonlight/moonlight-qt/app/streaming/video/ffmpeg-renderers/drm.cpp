#include "drm.h"

extern "C" {
    #include <libavutil/hwcontext_drm.h>
}

#include <libdrm/drm_fourcc.h>

// Special Rockchip type
#ifndef DRM_FORMAT_NV12_10
#define DRM_FORMAT_NV12_10 fourcc_code('N', 'A', '1', '2')
#endif

// Special Raspberry Pi type (upstreamed)
#ifndef DRM_FORMAT_P030
#define DRM_FORMAT_P030	fourcc_code('P', '0', '3', '0')
#endif

// Regular P010 (not present in some old libdrm headers)
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010	fourcc_code('P', '0', '1', '0')
#endif

#include <unistd.h>
#include <fcntl.h>

#include "streaming/streamutils.h"
#include "streaming/session.h"

#include <Limelight.h>

// HACK: Avoid including X11 headers which conflict with QDir
#ifdef SDL_VIDEO_DRIVER_X11
#undef SDL_VIDEO_DRIVER_X11
#endif

#include <SDL_syswm.h>

#include <QDir>

DrmRenderer::DrmRenderer(IFFmpegRenderer *backendRenderer)
    : m_BackendRenderer(backendRenderer),
      m_HwContext(nullptr),
      m_DrmFd(-1),
      m_SdlOwnsDrmFd(false),
      m_SupportsDirectRendering(false),
      m_Main10Hdr(false),
      m_ConnectorId(0),
      m_EncoderId(0),
      m_CrtcId(0),
      m_PlaneId(0),
      m_CurrentFbId(0),
      m_LastFullRange(false),
      m_LastColorSpace(-1),
      m_ColorEncodingProp(nullptr),
      m_ColorRangeProp(nullptr),
      m_HdrOutputMetadataProp(nullptr),
      m_HdrOutputMetadataBlobId(0)
{
#ifdef HAVE_EGL
    m_EGLExtDmaBuf = false;
    m_eglCreateImage = nullptr;
    m_eglCreateImageKHR = nullptr;
    m_eglDestroyImage = nullptr;
    m_eglDestroyImageKHR = nullptr;
#endif
}

DrmRenderer::~DrmRenderer()
{
    // Ensure we're out of HDR mode
    setHdrMode(false);

    if (m_CurrentFbId != 0) {
        drmModeRmFB(m_DrmFd, m_CurrentFbId);
    }

    if (m_HdrOutputMetadataBlobId != 0) {
        drmModeDestroyPropertyBlob(m_DrmFd, m_HdrOutputMetadataBlobId);
    }

    if (m_ColorEncodingProp != nullptr) {
        drmModeFreeProperty(m_ColorEncodingProp);
    }

    if (m_ColorRangeProp != nullptr) {
        drmModeFreeProperty(m_ColorRangeProp);
    }

    if (m_HdrOutputMetadataProp != nullptr) {
        drmModeFreeProperty(m_HdrOutputMetadataProp);
    }

    if (m_HwContext != nullptr) {
        av_buffer_unref(&m_HwContext);
    }

    if (!m_SdlOwnsDrmFd && m_DrmFd != -1) {
        close(m_DrmFd);
    }
}

bool DrmRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** options)
{
    // The out-of-tree LibreELEC patches use this option to control the type of the V4L2
    // buffers that we get back. We only support NV12 buffers now.
    av_dict_set_int(options, "pixel_format", AV_PIX_FMT_NV12, 0);

    context->hw_device_ctx = av_buffer_ref(m_HwContext);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using DRM renderer");

    return true;
}

bool DrmRenderer::initialize(PDECODER_PARAMETERS params)
{
    return false;

}

enum AVPixelFormat DrmRenderer::getPreferredPixelFormat(int videoFormat)
{
    // DRM PRIME buffers, or whatever the backend renderer wants
    if (m_BackendRenderer != nullptr) {
        return m_BackendRenderer->getPreferredPixelFormat(videoFormat);
    }
    else {
        return AV_PIX_FMT_DRM_PRIME;
    }
}

bool DrmRenderer::isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) {
    // Pass through the backend renderer if we have one. Otherwise we use
    // the default behavior which only supports the preferred format.
    if (m_BackendRenderer != nullptr) {
        return m_BackendRenderer->isPixelFormatSupported(videoFormat, pixelFormat);
    }
    else {
        return getPreferredPixelFormat(videoFormat);
    }
}

int DrmRenderer::getRendererAttributes()
{
    int attributes = 0;

    // This renderer can only draw in full-screen
    attributes |= RENDERER_ATTRIBUTE_FULLSCREEN_ONLY;

    // This renderer supports HDR
    attributes |= RENDERER_ATTRIBUTE_HDR_SUPPORT;

    // This renderer does not buffer any frames in the graphics pipeline
    attributes |= RENDERER_ATTRIBUTE_NO_BUFFERING;

    return attributes;
}

void DrmRenderer::setHdrMode(bool enabled)
{    
    if (m_HdrOutputMetadataProp != nullptr) {
        if (m_HdrOutputMetadataBlobId != 0) {
            drmModeDestroyPropertyBlob(m_DrmFd, m_HdrOutputMetadataBlobId);
            m_HdrOutputMetadataBlobId = 0;
        }

        if (enabled) {
            DrmDefs::hdr_output_metadata outputMetadata;
            SS_HDR_METADATA sunshineHdrMetadata;

            // Sunshine will have HDR metadata but GFE will not
            if (!LiGetHdrMetadata(&sunshineHdrMetadata)) {
                memset(&sunshineHdrMetadata, 0, sizeof(sunshineHdrMetadata));
            }

            outputMetadata.metadata_type = 0; // HDMI_STATIC_METADATA_TYPE1
            outputMetadata.hdmi_metadata_type1.eotf = 2; // SMPTE ST 2084
            outputMetadata.hdmi_metadata_type1.metadata_type = 0; // Static Metadata Type 1
            for (int i = 0; i < 3; i++) {
                outputMetadata.hdmi_metadata_type1.display_primaries[i].x = sunshineHdrMetadata.displayPrimaries[i].x;
                outputMetadata.hdmi_metadata_type1.display_primaries[i].y = sunshineHdrMetadata.displayPrimaries[i].y;
            }
            outputMetadata.hdmi_metadata_type1.white_point.x = sunshineHdrMetadata.whitePoint.x;
            outputMetadata.hdmi_metadata_type1.white_point.y = sunshineHdrMetadata.whitePoint.y;
            outputMetadata.hdmi_metadata_type1.max_display_mastering_luminance = sunshineHdrMetadata.maxDisplayLuminance;
            outputMetadata.hdmi_metadata_type1.min_display_mastering_luminance = sunshineHdrMetadata.minDisplayLuminance;
            outputMetadata.hdmi_metadata_type1.max_cll = sunshineHdrMetadata.maxContentLightLevel;
            outputMetadata.hdmi_metadata_type1.max_fall = sunshineHdrMetadata.maxFrameAverageLightLevel;

            int err = drmModeCreatePropertyBlob(m_DrmFd, &outputMetadata, sizeof(outputMetadata), &m_HdrOutputMetadataBlobId);
            if (err < 0) {
                m_HdrOutputMetadataBlobId = 0;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "drmModeCreatePropertyBlob() failed: %d",
                             errno);
                // Non-fatal
            }
        }

        int err = drmModeObjectSetProperty(m_DrmFd, m_ConnectorId, DRM_MODE_OBJECT_CONNECTOR,
                                           m_HdrOutputMetadataProp->prop_id,
                                           enabled ? m_HdrOutputMetadataBlobId : 0);
        if (err == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Set display HDR mode: %s", enabled ? "enabled" : "disabled");
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "drmModeObjectSetProperty(%s) failed: %d",
                         m_HdrOutputMetadataProp->name,
                         errno);
            // Non-fatal
        }
    }
    else if (enabled) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HDR_OUTPUT_METADATA is unavailable on this display. Unable to enter HDR mode!");
    }
}
#include <QDebug>
void DrmRenderer::renderFrame(AVFrame* frame)
{
    AVDRMFrameDescriptor mappedFrame;
    AVDRMFrameDescriptor* drmFrame;
    qDebug()<<"1";
    // If we are acting as the frontend renderer, we'll need to have the backend
    // map this frame into a DRM PRIME descriptor that we can render.
    if (m_BackendRenderer != nullptr) {
        if (!m_BackendRenderer->mapDrmPrimeFrame(frame, &mappedFrame)) {
            return;
        }

        drmFrame = &mappedFrame;
    }
    else {
        // If we're the backend renderer, the frame should already have it.
        SDL_assert(frame->format == AV_PIX_FMT_DRM_PRIME);
        drmFrame = (AVDRMFrameDescriptor*)frame->data[0];
    }

    int err;
    uint32_t handles[4] = {};
    uint32_t pitches[4] = {};
    uint32_t offsets[4] = {};
    uint64_t modifiers[4] = {};
    uint32_t flags = 0;

    SDL_Rect src, dst;

    src.x = src.y = 0;
    src.w = frame->width;
    src.h = frame->height;
    dst = m_OutputRect;

    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    // DRM requires composed layers rather than separate layers per plane
    SDL_assert(drmFrame->nb_layers == 1);

    const auto &layer = drmFrame->layers[0];
    for (int i = 0; i < layer.nb_planes; i++) {
        const auto &object = drmFrame->objects[layer.planes[i].object_index];

        err = drmPrimeFDToHandle(m_DrmFd, object.fd, &handles[i]);
        if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "drmPrimeFDToHandle() failed: %d",
                         errno);
            if (m_BackendRenderer != nullptr) {
                SDL_assert(drmFrame == &mappedFrame);
                m_BackendRenderer->unmapDrmPrimeFrame(drmFrame);
            }
            return;
        }

        pitches[i] = layer.planes[i].pitch;
        offsets[i] = layer.planes[i].offset;
        modifiers[i] = object.format_modifier;

        // Pass along the modifiers to DRM if there are some in the descriptor
        if (modifiers[i] != DRM_FORMAT_MOD_INVALID) {
            flags |= DRM_MODE_FB_MODIFIERS;
        }
    }

    // Remember the last FB object we created so we can free it
    // when we are finished rendering this one (if successful).
    uint32_t lastFbId = m_CurrentFbId;

    // Create a frame buffer object from the PRIME buffer
    // NB: It is an error to pass modifiers without DRM_MODE_FB_MODIFIERS set.
    err = drmModeAddFB2WithModifiers(m_DrmFd, frame->width, frame->height,
                                     drmFrame->layers[0].format,
                                     handles, pitches, offsets,
                                     (flags & DRM_MODE_FB_MODIFIERS) ? modifiers : NULL,
                                     &m_CurrentFbId, flags);

    if (m_BackendRenderer != nullptr) {
        SDL_assert(drmFrame == &mappedFrame);
        m_BackendRenderer->unmapDrmPrimeFrame(drmFrame);
    }

    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "drmModeAddFB2WithModifiers() failed: %d",
                     errno);
        m_CurrentFbId = lastFbId;
        return;
    }

    int colorspace = getFrameColorspace(frame);
    bool fullRange = isFrameFullRange(frame);

    // We also update the color range when the colorspace changes in order to handle initialization
    // where the last color range value may not actual be applied to the plane.
    if (fullRange != m_LastFullRange || colorspace != m_LastColorSpace) {
        const char* desiredValue = getDrmColorRangeValue(frame);

        if (m_ColorRangeProp != nullptr && desiredValue != nullptr) {
            int i;

            for (i = 0; i < m_ColorRangeProp->count_enums; i++) {
                if (!strcmp(desiredValue, m_ColorRangeProp->enums[i].name)) {
                    err = drmModeObjectSetProperty(m_DrmFd, m_PlaneId, DRM_MODE_OBJECT_PLANE,
                                                   m_ColorRangeProp->prop_id, m_ColorRangeProp->enums[i].value);
                    if (err == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "%s: %s",
                                    m_ColorRangeProp->name,
                                    desiredValue);
                    }
                    else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                     "drmModeObjectSetProperty(%s) failed: %d",
                                     m_ColorRangeProp->name,
                                     errno);
                        // Non-fatal
                    }

                    break;
                }
            }

            if (i == m_ColorRangeProp->count_enums) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to find matching COLOR_RANGE value for '%s'. Colors may be inaccurate!",
                            desiredValue);
            }
        }
        else if (desiredValue != nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "COLOR_RANGE property does not exist on output plane. Colors may be inaccurate!");
        }

        m_LastFullRange = fullRange;
    }

    if (colorspace != m_LastColorSpace) {
        const char* desiredValue = getDrmColorEncodingValue(frame);

        if (m_ColorEncodingProp != nullptr && desiredValue != nullptr) {
            int i;

            for (i = 0; i < m_ColorEncodingProp->count_enums; i++) {
                if (!strcmp(desiredValue, m_ColorEncodingProp->enums[i].name)) {
                    err = drmModeObjectSetProperty(m_DrmFd, m_PlaneId, DRM_MODE_OBJECT_PLANE,
                                                   m_ColorEncodingProp->prop_id, m_ColorEncodingProp->enums[i].value);
                    if (err == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "%s: %s",
                                    m_ColorEncodingProp->name,
                                    desiredValue);
                    }
                    else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                     "drmModeObjectSetProperty(%s) failed: %d",
                                     m_ColorEncodingProp->name,
                                     errno);
                        // Non-fatal
                    }

                    break;
                }
            }

            if (i == m_ColorEncodingProp->count_enums) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to find matching COLOR_ENCODING value for '%s'. Colors may be inaccurate!",
                            desiredValue);
            }
        }
        else if (desiredValue != nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "COLOR_ENCODING property does not exist on output plane. Colors may be inaccurate!");
        }

        m_LastColorSpace = colorspace;
    }

    // Update the overlay
    err = drmModeSetPlane(m_DrmFd, m_PlaneId, m_CrtcId, m_CurrentFbId, 0,
                          dst.x, dst.y,
                          dst.w, dst.h,
                          0, 0,
                          frame->width << 16,
                          frame->height << 16);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "drmModeSetPlane() failed: %d",
                     errno);
        drmModeRmFB(m_DrmFd, m_CurrentFbId);
        m_CurrentFbId = lastFbId;
        return;
    }

    // Free the previous FB object which has now been superseded
    drmModeRmFB(m_DrmFd, lastFbId);
}

bool DrmRenderer::needsTestFrame()
{
    return true;
}

bool DrmRenderer::testRenderFrame(AVFrame* frame) {
    // If we have a backend renderer, we must make sure it can
    // successfully export DRM PRIME frames.
    if (m_BackendRenderer != nullptr) {
        AVDRMFrameDescriptor drmDescriptor;

        // We shouldn't get here unless the backend at least claims
        // it can export DRM PRIME frames.
        SDL_assert(m_BackendRenderer->canExportDrmPrime());

        if (!m_BackendRenderer->mapDrmPrimeFrame(frame, &drmDescriptor)) {
            // It can't, so we can't use this renderer.
            return false;
        }

        m_BackendRenderer->unmapDrmPrimeFrame(&drmDescriptor);
    }

    return true;
}

bool DrmRenderer::isDirectRenderingSupported()
{
    return m_SupportsDirectRendering;
}

const char* DrmRenderer::getDrmColorEncodingValue(AVFrame* frame)
{
    switch (getFrameColorspace(frame)) {
    case COLORSPACE_REC_601:
        return "ITU-R BT.601 YCbCr";
    case COLORSPACE_REC_709:
        return "ITU-R BT.709 YCbCr";
    case COLORSPACE_REC_2020:
        return "ITU-R BT.2020 YCbCr";
    default:
        return NULL;
    }
}

const char* DrmRenderer::getDrmColorRangeValue(AVFrame* frame)
{
    return isFrameFullRange(frame) ? "YCbCr full range" : "YCbCr limited range";
}

#ifdef HAVE_EGL

bool DrmRenderer::canExportEGL() {
    if (qgetenv("DRM_FORCE_DIRECT") == "1") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using direct rendering due to environment variable");
        return false;
    }
    else if (qgetenv("DRM_FORCE_EGL") == "1") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using EGL rendering due to environment variable");
        return true;
    }
    else if (m_SupportsDirectRendering && m_Main10Hdr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using direct rendering for HDR support");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DRM backend supports exporting EGLImage");
    return true;
}

AVPixelFormat DrmRenderer::getEGLImagePixelFormat() {
    // This tells EGLRenderer to treat the EGLImage as a single opaque texture
    return AV_PIX_FMT_DRM_PRIME;
}

bool DrmRenderer::initializeEGL(EGLDisplay,
                                const EGLExtensions &ext) {
    if (!ext.isSupported("EGL_EXT_image_dma_buf_import")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "DRM-EGL: DMABUF unsupported");
        return false;
    }

    m_EGLExtDmaBuf = ext.isSupported("EGL_EXT_image_dma_buf_import_modifiers");

    // NB: eglCreateImage() and eglCreateImageKHR() have slightly different definitions
    m_eglCreateImage = (typeof(m_eglCreateImage))eglGetProcAddress("eglCreateImage");
    m_eglCreateImageKHR = (typeof(m_eglCreateImageKHR))eglGetProcAddress("eglCreateImageKHR");
    m_eglDestroyImage = (typeof(m_eglDestroyImage))eglGetProcAddress("eglDestroyImage");
    m_eglDestroyImageKHR = (typeof(m_eglDestroyImageKHR))eglGetProcAddress("eglDestroyImageKHR");

    if (!(m_eglCreateImage && m_eglDestroyImage) &&
            !(m_eglCreateImageKHR && m_eglDestroyImageKHR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Missing eglCreateImage()/eglDestroyImage() in EGL driver");
        return false;
    }

    return true;
}

ssize_t DrmRenderer::exportEGLImages(AVFrame *frame, EGLDisplay dpy,
                                     EGLImage images[EGL_MAX_PLANES]) {
    AVDRMFrameDescriptor* drmFrame = (AVDRMFrameDescriptor*)frame->data[0];

    memset(images, 0, sizeof(EGLImage) * EGL_MAX_PLANES);

    // DRM requires composed layers rather than separate layers per plane
    SDL_assert(drmFrame->nb_layers == 1);

    // Max 30 attributes (1 key + 1 value for each)
    const int MAX_ATTRIB_COUNT = 30 * 2;
    EGLAttrib attribs[MAX_ATTRIB_COUNT] = {
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)drmFrame->layers[0].format,
        EGL_WIDTH, frame->width,
        EGL_HEIGHT, frame->height,
    };
    int attribIndex = 6;

    for (int i = 0; i < drmFrame->layers[0].nb_planes; ++i) {
        const auto &plane = drmFrame->layers[0].planes[i];
        const auto &object = drmFrame->objects[plane.object_index];

        switch (i) {
        case 0:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        case 1:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        case 2:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        case 3:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        default:
            Q_UNREACHABLE();
        }
    }

    // Add colorspace metadata
    switch (getFrameColorspace(frame)) {
    case COLORSPACE_REC_601:
        attribs[attribIndex++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
        attribs[attribIndex++] = EGL_ITU_REC601_EXT;
        break;
    case COLORSPACE_REC_709:
        attribs[attribIndex++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
        attribs[attribIndex++] = EGL_ITU_REC709_EXT;
        break;
    case COLORSPACE_REC_2020:
        attribs[attribIndex++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
        attribs[attribIndex++] = EGL_ITU_REC2020_EXT;
        break;
    }

    // Add color range metadata
    attribs[attribIndex++] = EGL_SAMPLE_RANGE_HINT_EXT;
    attribs[attribIndex++] = isFrameFullRange(frame) ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT;

    // Terminate the attribute list
    attribs[attribIndex++] = EGL_NONE;
    SDL_assert(attribIndex <= MAX_ATTRIB_COUNT);

    // Our EGLImages are non-planar, so we only populate the first entry
    if (m_eglCreateImage) {
        images[0] = m_eglCreateImage(dpy, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT,
                                     nullptr, attribs);
        if (!images[0]) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "eglCreateImage() Failed: %d", eglGetError());
            goto fail;
        }
    }
    else {
        // Cast the EGLAttrib array elements to EGLint for the KHR extension
        EGLint intAttribs[MAX_ATTRIB_COUNT];
        for (int i = 0; i < MAX_ATTRIB_COUNT; i++) {
            intAttribs[i] = (EGLint)attribs[i];
        }

        images[0] = m_eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT,
                                        nullptr, intAttribs);
        if (!images[0]) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "eglCreateImageKHR() Failed: %d", eglGetError());
            goto fail;
        }
    }

    return 1;

fail:
    freeEGLImages(dpy, images);
    return -1;
}

void DrmRenderer::freeEGLImages(EGLDisplay dpy, EGLImage images[EGL_MAX_PLANES]) {
    if (m_eglDestroyImage) {
        m_eglDestroyImage(dpy, images[0]);
    }
    else {
        m_eglDestroyImageKHR(dpy, images[0]);
    }

    // Our EGLImages are non-planar
    SDL_assert(images[1] == 0);
    SDL_assert(images[2] == 0);
}

#endif
