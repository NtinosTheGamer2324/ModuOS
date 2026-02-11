USB Stack Skeleton (Template)

This is a starter template showing how to split a USB implementation into multiple SQRM modules
using ABI v2 dependencies.

Model:
  - usb depends on controller modules:
      uhci (start here; add ohci/ehci later if needed)
  - hid depends on usb

Files:
  - usb_sqrm.c
  - ehci_sqrm.c
  - uhci_sqrm.c
  - ohci_sqrm.c
  - xhci_sqrm.c
  - hid_sqrm.c
  - build.sh (builds all into *.sqrm)

By default these are skeletons (no real USB).
