// Preloaded only by the Linux CLI regression. Fail before a native window can
// flash or steal focus; this does not replace GTK's display/runtime startup.
#include <cstdio>
#include <cstdlib>

extern "C" void* gtk_window_new() {
    std::fputs("CLI regression: attempted to create a native GTK window\n", stderr);
    std::_Exit(86);
}
