#pragma once
/*
 * sqrm_sdk.h — ModuOS SQRM Third-Party Module SDK (compatibility umbrella)
 *
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  COMPATIBILITY HEADER — FOR EXISTING MODULES ONLY               ║
 * ║                                                                  ║
 * ║  This header exists solely to avoid breaking modules written    ║
 * ║  against the original single-file SDK.                          ║
 * ║                                                                  ║
 * ║  NEW MODULES MUST NOT INCLUDE THIS FILE.                        ║
 * ║  Include only the interface headers you actually need:          ║
 * ║                                                                  ║
 * ║    interfaces/sqrm_core.h        — ABI version, type, macros   ║
 * ║    interfaces/sqrm_kernel_api.h  — sqrm_kernel_api_t (+ core,  ║
 * ║                                    blockdev, fs, audio, gpu)    ║
 * ║    interfaces/sqrm_blockdev.h    — blockdev_handle_t / ops      ║
 * ║    interfaces/sqrm_fs.h          — fs_mount_t / ext driver ops  ║
 * ║    interfaces/sqrm_audio.h       — audio_pcm_ops_t / config     ║
 * ║    interfaces/sqrm_gpu.h         — framebuffer_t / gpu device   ║
 * ║    interfaces/sqrm_net.h         — sqrm_net_api_v1_t            ║
 * ║    interfaces/sqrm_usb.h         — USB transfers / UHCI API     ║
 * ║    interfaces/sqrm_hid.h         — sqrm_hid_api_v1_t            ║
 * ║                                                                  ║
 * ║  This file may be removed in a future SDK major version.        ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include "interfaces/sqrm_core.h"
#include "interfaces/sqrm_blockdev.h"
#include "interfaces/sqrm_fs.h"
#include "interfaces/sqrm_audio.h"
#include "interfaces/sqrm_gpu.h"
#include "interfaces/sqrm_net.h"
#include "interfaces/sqrm_usb.h"
#include "interfaces/sqrm_hid.h"
#include "interfaces/sqrm_kernel_api.h"