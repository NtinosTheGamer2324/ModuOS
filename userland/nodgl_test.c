// NodGL GPU Acceleration Test

#include "lib_NodGL_syscalls.c"

int main(void) {
    if (NodGL_Init() != 0) {
        return 1;
    }
    
    NodGL_Clear(0xFF000000);
    NodGL_Flush();
    
    NodGL_FillRect(100, 100, 200, 150, 0xFFFF0000); // Red
    NodGL_FillRect(350, 100, 200, 150, 0xFF00FF00); // Green
    NodGL_FillRect(600, 100, 200, 150, 0xFF0000FF); // Blue
    NodGL_Flush();
    
    for (int i = 0; i < 100; i++) {
        NodGL_DrawLine(50 + i * 2, 300, 150 + i * 2, 500, 0xFFFFFFFF);
    }
    NodGL_Flush();
    
    NodGL_DrawTriangle(400, 300, 500, 500, 300, 500, 0xFFFFFF00);
    NodGL_Flush();
    
    NodGL_Present();
    
    for (volatile int i = 0; i < 100000000; i++); // Using a cycle loop cause i am lazy
    
    NodGL_Shutdown();
    return 0;
}
