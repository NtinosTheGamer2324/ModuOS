Generic SQRM Template

This folder contains a starter template for a generic ModuOS SQRM module.

Why SQRM_TYPE_GENERIC?
  - For modules that aren't strictly a filesystem/audio/usb/gpu/net driver.
  - For helper modules, debug services, compatibility shims, etc.

Files:
  - generic_sqrm.c : skeleton module source
  - build.sh       : builds generic.sqrm using x86_64-elf-gcc

Notes:
  - Default is ABI v1 (SQRM_DEFINE_MODULE).
  - If you need dependencies, use SQRM_DEFINE_MODULE_V2 and list dependency module names.
