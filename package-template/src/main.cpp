#include <stdint.h>
#include <neutrino.h>

int main(uint64_t arg_ptr, uint64_t) {
    const char* args = reinterpret_cast<const char*>(arg_ptr);
    long console = neutrino_open_stdout();

    neutrino_write(console, "hello from a Neutrino package");
    if (args != nullptr && args[0] != '\0') {
        neutrino_write(console, ": ");
        neutrino_write(console, args);
    }
    neutrino_write(console, "\n");
    return 0;
}
