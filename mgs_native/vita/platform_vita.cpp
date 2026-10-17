// Platform helpers on the Vita (README.md §6.29): everything lives in ux0:data/mgs_mobile/.
#include "platform.h"

std::filesystem::path Platform::ExeDir() { return U8Path("ux0:data/mgs_mobile/"); }

void Platform::StartLog() {}   // main_vita.cpp sets up the log file and the log port itself

// No Unicorn on the Vita: the PC-only users of a second CPU (the asset viewer's sandbox) report it as unavailable.
#include "cpu.h"
std::unique_ptr<Cpu> Cpu::CreateUnicorn(std::string* error) {
    if (error) *error = "Unicorn isn't available on the Vita";
    return nullptr;
}
