/*
 * Minimal dOS virtio-gpu 2D scanout.
 *
 * Presents an existing RGBA8 software framebuffer to QEMU's virtio-gpu device.
 * This is not a GL/GPU acceleration path; softgles2 still renders in software.
 */
#ifndef DHEWM3_DOS_VIRTIO_GPU_H
#define DHEWM3_DOS_VIRTIO_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int dvgpu_present_rgba(const uint32_t *pixels, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* DHEWM3_DOS_VIRTIO_GPU_H */
