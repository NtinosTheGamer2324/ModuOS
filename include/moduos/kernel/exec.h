#ifndef EXEC_H
#define EXEC_H

void exec(const char *args);

/* Spawn a user app and return immediately. Returns PID on success, <0 on error. */
int exec_async(const char *args);

/* Internal: shared implementation. */
int exec_run(const char *args, int wait_for_exit);

#endif