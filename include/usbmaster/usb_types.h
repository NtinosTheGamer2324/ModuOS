#ifndef USB_TYPES_H
#define USB_TYPES_H

/* Request */
#define GET_STATUS          0
#define CLEAR_FEATURE       1
#define SET_FEATURE         3
#define SET_ADDRESS         5
#define GET_DESCRIPTOR      6
#define SET_DESCRIPTOR      7
#define GET_CONFIGURATION   8
#define SET_CONFIGURATION   9
#define GET_INTERFACE       10
#define SET_INTERFACE       11
#define SYNCH_FRAME         12

/* Data Directions */
#define USB_DIR_OUT             0x00 
#define USB_DIR_IN              0x80

// --- USB Descriptor Types (used in the high byte of GET_DESCRIPTOR's wValue) ---
#define USB_DESC_TYPE_DEVICE        1
#define USB_DESC_TYPE_CONFIGURATION 2
#define USB_DESC_TYPE_STRING        3
#define USB_DESC_TYPE_INTERFACE     4
#define USB_DESC_TYPE_ENDPOINT      5

#endif