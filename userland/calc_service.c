// calc_service.c
#include "libc.h"

// Simple request/response protocol
typedef struct {
    int cmd;      // 1 = add, 2 = multiply, 3 = power
    int a;
    int b;
} calc_request_t;

typedef struct {
    int success;
    int result;
    char message[64];
} calc_response_t;

static ssize_t calc_invoke(void* ctx, 
                          const void* in_buf, size_t in_size,
                          void* out_buf, size_t out_size)
{
    (void)ctx;

    if (in_size != sizeof(calc_request_t) || out_size != sizeof(calc_response_t)) {
        return -1;
    }

    const calc_request_t* req = (const calc_request_t*)in_buf;
    calc_response_t* resp = (calc_response_t*)out_buf;

    resp->success = 1;
    resp->result = 0;
    resp->message[0] = '\0';

    switch (req->cmd) {
        case 1:  // ADD
            resp->result = req->a + req->b;
            break;
        case 2:  // MULTIPLY
            resp->result = req->a * req->b;
            break;
        case 3:  // POWER (simple)
            resp->result = 1;
            for (int i = 0; i < req->b; i++) {
                resp->result *= req->a;
            }
            break;
        default:
            resp->success = 0;
            snprintf(resp->message, sizeof(resp->message), "Unknown command %d", req->cmd);
            return -1;
    }

    return sizeof(calc_response_t);
}

int md_main(long argc, char** argv)
{
    printf("Calc Service Starting...\n");

    userfs_user_ops_t ops = {0};
    ops.invoke = calc_invoke;

    userfs_user_node_t node = {0};
    node.path     = "calc";
    node.owner_id = "calc_service";
    node.perms    = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
    node.ops      = ops;
    node.ctx      = NULL;

    int ret = userfs_register(&node);
    if (ret != 0) {
        printf("Failed to register service: %d\n", ret);
        return 1;
    }

    printf("Calc service registered at $/user/calc\n");
    printf("Waiting for invoke calls...\n");

    // Keep service alive
    while (1) {
        sleep(10);
    }

    return 0;
}