#pragma once

void trigger_panic_unknown();
void trigger_no_shell_panic();
void trigger_panic_dodev();
void trigger_panic_dops2();
void trigger_panic_dofs();
void trigger_panic_doata();
void panic(const char* title, const char* message, const char* tips, const char* err_cat, const char* err_code, int reboot_delay);