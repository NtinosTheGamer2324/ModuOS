Network SQRM Skeleton (Template)

This folder contains a starter template for a ModuOS SQRM network *device* module (netdev/NIC).

IMPORTANT LAYERING RULE:
  - This module should only implement link-layer (L2) networking: raw Ethernet frames + link status.
  - Do NOT implement IP/TCP/UDP/DNS/HTTP here.
  - Higher-level protocols belong in userland /ModuOS/System64/services/*.ctl built on top of the netdev service.

Files:
  - net_skeleton_sqrm.c : skeleton module source
  - build.sh            : builds net_skel.sqrm using x86_64-elf-gcc

By default, the module returns -1 in sqrm_module_init() so that it does NOT
claim the network slot during autoload.

To turn this into a real NIC driver:
  1) Detect your NIC hardware (typically PCI class 0x02)
  2) Map MMIO BARs via api->ioremap
  3) Set up DMA rings and interrupts
  4) Implement tx_frame() and rx_poll() for raw Ethernet frames
  5) (Optional) Register device nodes via api->devfs_register_path
  6) Return 0 from sqrm_module_init() once you successfully claimed hardware
