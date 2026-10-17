// Platform helpers on the 3DS (README.md §6.28): everything lives in sdmc:/3ds/mgs_mobile/.
#include "platform.h"

std::filesystem::path Platform::ExeDir() { return U8Path("sdmc:/3ds/mgs_mobile/"); }

void Platform::StartLog() {}   // main_3ds.cpp sets up the console, 3dslink and the log file itself

// No Unicorn on the 3DS: the PC-only users of a second CPU (the installer's archive extractor, the asset
// viewer's sandbox) report it as unavailable.
#include "cpu.h"
std::unique_ptr<Cpu> Cpu::CreateUnicorn(std::string* error) {
    if (error) *error = "Unicorn isn't available on the 3DS";
    return nullptr;
}
