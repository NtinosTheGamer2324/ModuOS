// PS/2 Controller Ports
#define PS2_DATA_PORT       0x60
#define PS2_COMMAND_PORT    0x64

// PS/2 Controller Commands
#define PS2_CMD_READ_CONFIG      0x20
#define PS2_CMD_WRITE_CONFIG     0x60
#define PS2_CMD_DISABLE_PORT1    0xAD
#define PS2_CMD_ENABLE_PORT1     0xAE
#define PS2_CMD_DISABLE_PORT2    0xA7
#define PS2_CMD_ENABLE_PORT2     0xA8
#define PS2_CMD_WRITE_TO_PORT2   0xD4

// PS/2 Status Register Bits (read from 0x64)
#define PS2_STATUS_OUTPUT_BUFFER 0x01
#define PS2_STATUS_INPUT_BUFFER  0x02
/*
#define PS2_CMD_SELF_TEST         0xAA
#define PS2_CMD_TEST_PORT1        0xAB
#define PS2_CMD_TEST_PORT2        0xA9

#define PS2_CONFIG_PORT1_IRQ_EN     (1 << 0)
#define PS2_CONFIG_PORT2_IRQ_EN     (1 << 1)
#define PS2_CONFIG_PORT1_TRANSLATE  (1 << 6)

#define PS2_SELF_TEST_PASS   0x55
#define PS2_PORT_TEST_PASS   0x00

#define KB_CMD_SET_SCANCODE_SET  0xF0
#define KB_CMD_ENABLE_SCANNING   0xF4
#define KB_CMD_RESET             0xFF

#define KB_RESP_ACK          0xFA
#define KB_RESP_SELF_TEST_OK 0xAA
*/
