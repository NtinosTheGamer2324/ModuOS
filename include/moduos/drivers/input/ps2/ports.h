// PS/2 Controller Ports
#define PS2_DATA_PORT       0x60
#define PS2_COMMAND_PORT    0x64

// PS/2 Controller Commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT1   0xAD
#define PS2_CMD_ENABLE_PORT1    0xAE

// PS/2 Status Register Bits (read from 0x64)
#define PS2_STATUS_OUTPUT_BUFFER 0x01
#define PS2_STATUS_INPUT_BUFFER  0x02

