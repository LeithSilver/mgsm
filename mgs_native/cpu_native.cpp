// Native CPU backend (README.md §6.26): on a 32-bit ARM host the game's ARMv5 / Thumb code runs on the host CPU.
//
// Guest addresses are host addresses (the image is relocated to wherever Reserve puts it, §6.25). The runtime
// still sees a register file: the guest runs until it enters a stub, native_arm.S saves its registers into
// g_native_ctx and returns here, the handler reads and writes them (RegRead / RegWrite, PC included), and the
// guest resumes from the result:
//  - import trampolines (WriteTrampoline): `mov r12, #index; b common` -> the SVC hook with PC = trampoline + 4;
//  - code hooks: the instructions at the site are replaced by a branch to a stub (ARM: `b stub`; Thumb:
//    [nop] `bx pc; nop` + ARM `b stub`). Where the patch goes comes from native_hooks.inc (tools/native_hooks.py
//    checks that nothing else enters it). On entry the displaced instructions are run by the small interpreter
//    below, with the hooks called at their addresses, and the guest resumes after them;
//  - pointer hooks: a function pointer in the game's data is pointed at a stub; the hooks run, then the function.
#include "cpu.h"

#if defined(__arm__) && !defined(__aarch64__)
#include "loader.h"
#include "guest_interp.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <csetjmp>
#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#elif defined(__vita__)
#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

extern "C" {
struct NativeCtx {
    uint32_t r[16];
    uint32_t cpsr, reason, fpscr, host_sp;
    uint64_t d[16];
    uint32_t host_fpscr, pad;
};
extern NativeCtx g_native_ctx;
void native_enter(uint32_t* frame, uint32_t flags);
void native_exit();
// Every return from the game's code to ours (stub entries), the last stub reason and the PC it left at: a
// watchdog thread can tell a busy guest (the count rising) from one stuck in a loop of its own (vita/main_vita.cpp).
volatile uint32_t g_native_exits = 0, g_native_last_reason = 0, g_native_last_pc = 0;
#ifdef __3DS__
int32_t native_flush_dcache_range(uint32_t addr, uint32_t size);        // native_arm.S: Luma3DS svc 0x91
int32_t native_invalidate_icache_range(uint32_t addr, uint32_t size);   // native_arm.S: Luma3DS svc 0x93
bool g_profile = false;                 // MGS_PROFILE=1: time every syscall (svcGetSystemTick is a kernel call: costly)
unsigned long long g_host_ticks = 0;    // time spent outside the game's code (main_3ds.cpp's [Perf] line)
unsigned g_code_flushes = 0;            // instruction cache flushes (code written), ditto
unsigned long long g_svc_ticks[512];    // per import: time in its handler and calls (the [Perf] profile)
unsigned g_svc_calls[512];
#endif
}

namespace {
struct HookPlan { uint32_t addr, start, length; bool arm; uint32_t pointer; };
const HookPlan kPlans[] = {
#include "native_hooks.inc"
};

#ifdef __3DS__
// 3DS (README.md §6.28): no signals. libctru's per-thread exception handler gets the guest's registers at a data
// abort, prefetch abort or undefined instruction while the game's code runs, and jumps back into Start as on
// Android. Other faults are reported and the app stops there (Luma3DS would otherwise show its own crash screen).
extern "C" void ShowLogOnScreen();
jmp_buf g_fault_jmp;
volatile int g_in_guest = 0;
struct { int sig; uint32_t addr, r[16], cpsr; } g_fault;
alignas(8) uint8_t g_exc_stack[16 * 1024];
const char* FaultName(int type) {
    switch (type) {
        case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch abort";
        case ERRF_EXCEPTION_DATA_ABORT: return "data abort";
        case ERRF_EXCEPTION_UNDEFINED: return "undefined instruction";
        default: return "VFP exception";
    }
}

void OnException(ERRF_ExceptionInfo* e, CpuRegisters* regs) {
    for (int k = 0; k < 13; ++k) g_fault.r[k] = regs->r[k];
    g_fault.r[13] = regs->sp; g_fault.r[14] = regs->lr; g_fault.r[15] = regs->pc;
    g_fault.cpsr = regs->cpsr;
    g_fault.sig = e->type;
    g_fault.addr = e->type == ERRF_EXCEPTION_DATA_ABORT ? e->far : regs->pc;
    if (!g_in_guest) {
        ShowLogOnScreen();   // (main_3ds.cpp: the bottom screen is blank otherwise)
        fprintf(stderr, "\n[Native] Host crash: %s at pc 0x%08lx, address 0x%08lx, lr 0x%08lx\n", FaultName(e->type),
                (unsigned long)regs->pc, (unsigned long)g_fault.addr, (unsigned long)regs->lr);
        fflush(nullptr);   // (the log file is buffered)
        for (;;) svcSleepThread(1000000000LL);
    }
    g_in_guest = 0;
    longjmp(g_fault_jmp, 1);
}

void InstallFaultHandler() {
    threadOnException(OnException, g_exc_stack + sizeof(g_exc_stack), WRITE_DATA_TO_HANDLER_STACK);
}

Handle ProcessHandle() {
    static Handle h = 0;
    if (!h) svcDuplicateHandle(&h, CUR_PROCESS_HANDLE);
    return h;
}
#elif defined(__vita__)
// Vita (README.md §6.29): no signals. kubridge's exception handlers get the registers at a data abort, prefetch
// abort or undefined instruction; one in the game's code jumps back into Start as on Android. For a fault in our
// own code the handlers are released and the instruction runs again, so the system's crash dump takes it.
jmp_buf g_fault_jmp;
volatile int g_in_guest = 0;
struct { int sig; uint32_t addr, r[16], cpsr; } g_fault;
constexpr uint32_t kExceptionTypes[] = { KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
                                         KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION };
const char* FaultName(int type) {
    switch (type) {
        case KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT: return "data abort";
        case KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT: return "prefetch abort";
        default: return "undefined instruction";
    }
}

// The registers and the words on the stack that point into our code (the callers). The module isn't loaded at its
// link address: FaultName's address here, against its address in the ELF (nm), converts them.
__attribute__((noinline)) void DumpHostCrash(const uint32_t* regs) {
    for (int k = 0; k < 13; ++k) fprintf(stderr, "r%d %08lx%s", k, (unsigned long)regs[k], k % 7 == 6 ? "\n" : "  ");
    const uint32_t ref = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&FaultName));
    fprintf(stderr, "sp %08lx, FaultName at %08lx\ncode addresses on the stack:", (unsigned long)regs[13], (unsigned long)ref);
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(regs[13]);
    for (int k = 0, n = 0; k < 1024 && n < 24; ++k)
        if (sp[k] + 0x200000 >= ref && sp[k] < ref + 0x200000) { fprintf(stderr, " %08lx", (unsigned long)sp[k]); ++n; }
    fprintf(stderr, "\n");
}

void OnException(KuKernelExceptionContext* e) {
    const uint32_t regs[16] = { e->r0, e->r1, e->r2, e->r3, e->r4, e->r5, e->r6, e->r7, e->r8, e->r9, e->r10, e->r11,
                                e->r12, e->sp, e->lr, e->pc };
    for (int k = 0; k < 16; ++k) g_fault.r[k] = regs[k];
    g_fault.cpsr = e->SPSR;
    g_fault.sig = static_cast<int>(e->exceptionType);
    g_fault.addr = e->exceptionType == KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT ? e->FAR : e->pc;
    if (!g_in_guest) {
        fprintf(stderr, "\n[Native] Host crash: %s at pc 0x%08lx, address 0x%08lx, lr 0x%08lx\n", FaultName(g_fault.sig),
                (unsigned long)e->pc, (unsigned long)g_fault.addr, (unsigned long)e->lr);
        DumpHostCrash(regs);
        fflush(nullptr);
        for (uint32_t t : kExceptionTypes) kuKernelReleaseExceptionHandler(t);
        return;
    }
    g_in_guest = 0;
    longjmp(g_fault_jmp, 1);
}

void InstallFaultHandler() {
    static bool done = false;
    if (done) return;
    done = true;
    if (getenv("MGS_NO_FAULT_HANDLER")) { std::cout << "[Native] No fault handler (MGS_NO_FAULT_HANDLER)" << std::endl; return; }
    for (uint32_t t : kExceptionTypes)
        if (kuKernelRegisterExceptionHandler(t, OnException, nullptr, nullptr) < 0)
            std::cerr << "[Native] No exception handler (kubridge.skprx not loaded?): guest faults will crash the app" << std::endl;
}
#else
// Guest faults: a signal while the game's code runs (g_in_guest) jumps back into Start, which returns
// ERR_GUEST_FAULT with the guest's registers at the fault, as Unicorn returns an error. Other faults go to the
// handler that was there before.
sigjmp_buf g_fault_jmp;
volatile sig_atomic_t g_in_guest = 0;
struct { int sig; uint32_t addr, r[16], cpsr; } g_fault;
struct sigaction g_old_segv, g_old_bus, g_old_ill;

void OnFault(int sig, siginfo_t* si, void* uctx) {
    if (!g_in_guest) {
        const struct sigaction& old = sig == SIGSEGV ? g_old_segv : sig == SIGBUS ? g_old_bus : g_old_ill;
        sigaction(sig, &old, nullptr);   // the previous handler (or the default: crash) takes it from here
        raise(sig);
        return;
    }
    g_in_guest = 0;
    const mcontext_t& m = static_cast<ucontext_t*>(uctx)->uc_mcontext;
    const unsigned long regs[16] = { m.arm_r0, m.arm_r1, m.arm_r2, m.arm_r3, m.arm_r4, m.arm_r5, m.arm_r6, m.arm_r7,
                                     m.arm_r8, m.arm_r9, m.arm_r10, m.arm_fp, m.arm_ip, m.arm_sp, m.arm_lr, m.arm_pc };
    for (int k = 0; k < 16; ++k) g_fault.r[k] = static_cast<uint32_t>(regs[k]);
    g_fault.cpsr = static_cast<uint32_t>(m.arm_cpsr);
    g_fault.sig = sig;
    g_fault.addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(si->si_addr));
    siglongjmp(g_fault_jmp, 1);
}

void InstallFaultHandler() {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction sa {};
    sa.sa_sigaction = OnFault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    sigaction(SIGILL, &sa, &g_old_ill);
}

const char* FaultName(int sig) { return sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGILL"; }
#endif

enum : uint32_t { KIND_SVC = 1, KIND_SITE = 2, KIND_POINTER = 3, KIND_EXIT = 4 };
constexpr uint32_t kStubArea = 0x8000;   // trampoline block offset where the stubs go (the imports use < 2 KB)
constexpr uint32_t T_BIT = 0x20;

template <typename T> T& Mem(uint32_t a) { return *reinterpret_cast<T*>(static_cast<uintptr_t>(a)); }

#ifdef __3DS__
uint32_t g_next_region = 0;   // where Reserve maps next (0: just above libctru's heap)
#endif
#if defined(__3DS__) || defined(__vita__)
#define MGS_FLUSH_CODE_RANGES 1   // code writes collected and flushed before the guest runs (one system call)
#endif

class NativeCpu : public Cpu {
public:
    // The guest memory goes back to the system. On the 3DS the next instance then reserves from where this one
    // started: the game's sandbox reuses the installer's addresses (each instance is destroyed before the next).
    ~NativeCpu() override {
        for (const Region& r : regions_) {
#ifdef __3DS__
            u32 out = 0;
            svcControlMemory(&out, r.addr, 0, r.size, MEMOP_FREE, MEMPERM_DONTCARE);
#elif defined(__vita__)
            if (r.block < 0) continue;   // (the guest stack: part of the thread's own stack)
            kuKernelMemDecommit(reinterpret_cast<void*>(static_cast<uintptr_t>(r.addr)), r.size);
            sceKernelFreeMemBlock(r.block);
#else
            munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(r.addr)), r.size);
#endif
        }
#ifdef __3DS__
        if (!regions_.empty()) g_next_region = regions_.front().addr;
#endif
    }
    bool IsNative() const override { return true; }
    uint32_t* RegisterFile() override { return g_native_ctx.r; }

    int MemMap(uint64_t, size_t) override { return ERR_WRITE_UNMAPPED; }   // fixed addresses: Reserve instead
    uint64_t Reserve(uint64_t preferred, size_t size) override {
        // Ask for the address Unicorn would use (a hint; the kernel may pick another). Addresses below 2 GB
        // keep the game's pointers positive as signed numbers, as on the phone.
#ifdef __3DS__
        // 3DS: pages of their own above libctru's heap (main_3ds.cpp caps it), with 64 KB unmapped between regions
        // so that an overrun faults (and is reported) instead of corrupting the neighbour, as under Unicorn. The
        // first region (trampolines + image) is made executable: Luma3DS lets homebrew change its own pages.
        uint32_t& next = g_next_region;
        constexpr uint32_t kGap = 0x10000;   // 64 KB unmapped between regions (the heap address window is 96 MB)
        if (!next) next = ((OS_HEAP_AREA_BEGIN + envGetHeapSize() + kGap - 1) & ~(kGap - 1)) + kGap;
        size = (size + 0xFFF) & ~static_cast<size_t>(0xFFF);
        u32 out = 0;
        Result rc = svcControlMemory(&out, next, 0, size, MEMOP_ALLOC, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
        if (R_FAILED(rc)) {
            std::cerr << "[Native] Could not map " << size / 1024 << " KB at 0x" << std::hex << next << " (0x" << rc << std::dec
                      << "), " << osGetMemRegionFree(MEMREGION_APPLICATION) / 1024 << " KB free" << std::endl;
            return 0;
        }
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(next));
        next = ((next + size + kGap - 1) & ~(kGap - 1)) + kGap;
        memset(p, 0, size);
        if (!block_) {
            const uint32_t pa = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
            rc = svcControlProcessMemory(ProcessHandle(), pa, pa, size, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
            if (R_FAILED(rc)) {
                std::cerr << "[Native] Could not make the code memory executable (0x" << std::hex << rc << std::dec
                          << "): run from the Homebrew Launcher under Luma3DS" << std::endl;
                return 0;
            }
        }
        (void)preferred;
#elif defined(__vita__)
        // Vita: a kubridge block wherever the system puts it (above 0x80000000: fine for the game, README.md
        // §6.29); the first (trampolines + image) readable, writable and executable at once. The guest stack is
        // the exception: the system stops the whole process when a thread runs with its stack pointer outside its
        // own stack, so it is the lowest part of the calling thread's stack (main_vita.cpp makes it 4 MB), below
        // what our own code uses. (The loader asks for the stack at STACK_BASE; nothing else comes from there.)
        if (preferred == STACK_BASE) {
            SceKernelThreadInfo info{};
            info.size = sizeof(info);
            uint32_t sp = 0;
            asm volatile("mov %0, sp" : "=r"(sp));
            const uint32_t low = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0
                                 ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.stack)) : 0;
            if (!low || sp < low || sp - low < size + 512 * 1024) {   // (room for the guest's and ours)
                std::cerr << "[Native] The thread's stack is too small for the guest's " << size / 1024 << " KB (stack at 0x"
                          << std::hex << low << ", sp 0x" << sp << std::dec << ")" << std::endl;
                return 0;
            }
            memset(reinterpret_cast<void*>(static_cast<uintptr_t>(low)), 0, size);
            regions_.push_back({low, static_cast<uint32_t>(size), -1});
            return low;
        }
        size = (size + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
        void* p = nullptr;
        const SceUID block = kuKernelMemReserve(&p, size, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        if (block < 0) {
            std::cerr << "[Native] kuKernelMemReserve(" << size / 1024 << " KB) failed: 0x" << std::hex << block << std::dec
                      << " (kubridge.skprx not loaded, or out of memory)" << std::endl;
            return 0;
        }
        const uint32_t prot = KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | (block_ ? 0 : KU_KERNEL_PROT_EXEC);
        const int rc = kuKernelMemCommit(p, size, prot, nullptr);
        if (rc < 0) {
            std::cerr << "[Native] kuKernelMemCommit(" << size / 1024 << " KB) failed: 0x" << std::hex << rc << std::dec << std::endl;
            sceKernelFreeMemBlock(block);
            return 0;
        }
        memset(p, 0, size);
#else
        void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(preferred)), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            std::cerr << "[Native] mmap(" << size << ", rwx) failed" << std::endl;
            return 0;
        }
#endif
        const uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
#ifdef __vita__
        regions_.push_back({a, static_cast<uint32_t>(size), block});
#else
        regions_.push_back({a, static_cast<uint32_t>(size)});
#endif
        if (!block_) block_ = a;   // the first: trampolines + image (loader.cpp)
        return a;
    }
    int MemRead(uint64_t addr, void* out, size_t size) override {
        if (!Mapped(addr, size)) return ERR_READ_UNMAPPED;
        memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), size);
        return OK;
    }
    int MemWrite(uint64_t addr, const void* data, size_t size) override {
        if (!Mapped(addr, size)) return ERR_WRITE_UNMAPPED;
        char* p = reinterpret_cast<char*>(static_cast<uintptr_t>(addr));
        memcpy(p, data, size);
#ifdef MGS_FLUSH_CODE_RANGES
        // Only code (trampolines, stubs and the code section; not the image's data, which the runtime writes all
        // the time) needs the caches flushed, before the guest runs again.
        if (addr < block_ + kCodeEnd && addr + size > block_) {
            dirty_lo_ = std::min<uint32_t>(dirty_lo_, static_cast<uint32_t>(addr));
            dirty_hi_ = std::max<uint32_t>(dirty_hi_, static_cast<uint32_t>(addr + size));
        }
#else
        if (addr >= block_ && addr < block_ + regions_[0].size) __builtin___clear_cache(p, p + size);
#endif
        return OK;
    }
    int RegRead(int reg, void* out) override {
        uint32_t v;
        if (reg >= REG_R0 && reg <= REG_PC) v = ctx().r[reg];
        else if (reg == REG_CPSR) v = ctx().cpsr;
        else return 1;
        memcpy(out, &v, 4);
        return OK;
    }
    int RegWrite(int reg, const void* value) override {
        uint32_t v;
        memcpy(&v, value, 4);
        if (reg == REG_PC) {   // like Unicorn: bit 0 of a PC write selects Thumb
            ctx().r[15] = v & ~1u;
            if (v & 1) ctx().cpsr |= T_BIT;
        } else if (reg >= REG_R0 && reg < REG_PC) ctx().r[reg] = v;
        else if (reg == REG_CPSR) ctx().cpsr = v;
        else return 1;
        return OK;
    }

    void WriteTrampoline(uint64_t addr, uint32_t index) override {
        // b stub (which keeps the guest's r12, as SVC does under Unicorn); the second word is never reached
        const uint32_t stub = NewStub(KIND_SVC, index);
        const uint32_t code[2] = { Branch(static_cast<uint32_t>(addr), stub), 0xE1A00000 };
        MemWrite(addr, code, sizeof(code));
    }

    int AddCodeHook(CodeHook fn, void* user, uint64_t begin, uint64_t end) override {
        const uint32_t a = static_cast<uint32_t>(begin);
        if (end != begin) { std::cerr << "[Native] Code hook ranges aren't supported" << std::endl; return 1; }
        const uint32_t stock = a - ImageBase() + STOCK_BASE;
        if (const char* skip = getenv("MGS_NATIVE_SKIP")) {   // debug: comma list of stock hook addresses left out
            char buf[16];
            snprintf(buf, sizeof(buf), "%x", stock);
            if (strstr(skip, buf)) { std::cout << "[Native] Skipping the hook at 0x" << buf << std::endl; return OK; }
        }
        bool found = false;
        for (const HookPlan& p : kPlans) {
            if (p.addr != stock) continue;
            found = true;
            if (p.pointer) InstallPointer(p); else InstallSite(p);
        }
        if (!found) { std::cerr << "[Native] No patch planned for a hook at 0x" << std::hex << stock << std::dec << " (tools/native_hooks.py)" << std::endl; return 1; }
        hooks_[a].push_back({fn, user});
        return OK;
    }
    int AddSvcHook(SvcHook fn, void* user) override { svc_ = fn; svc_user_ = user; return OK; }
    int AddWriteHook(WriteHook, void*, uint64_t, uint64_t) override { return 1; }   // debug only (MGS_WATCH)

    int Start(uint64_t pc, uint64_t until, size_t) override {
        NativeCtx& c = ctx();
        InstallFaultHandler();
#if defined(__3DS__) || defined(__vita__)
        if (setjmp(g_fault_jmp)) {
#else
        if (sigsetjmp(g_fault_jmp, 1)) {
#endif   // the game's code faulted (OnFault)
            const uint32_t host_fpscr = c.host_fpscr;
            asm volatile("vmsr fpscr, %0" : : "r"(host_fpscr));
            for (int k = 0; k < 16; ++k) c.r[k] = g_fault.r[k];
            c.cpsr = g_fault.cpsr;
            const uint32_t pc_now = g_fault.r[15], base = ImageBase();
            std::cerr << "[Native] Guest fault: " << g_fault.sig << " (" << FaultName(g_fault.sig) << ") at 0x" << std::hex << pc_now;
            if (pc_now >= base && pc_now < base + 0x11B700) std::cerr << " (stock 0x" << pc_now - base + STOCK_BASE << ")";
            std::cerr << ", address 0x" << g_fault.addr << ", lr 0x" << g_fault.r[14] << std::dec << std::endl;
            return ERR_GUEST_FAULT;
        }
        if (c.r[14] == static_cast<uint32_t>(until)) c.r[14] = Common(KIND_EXIT);   // returning ends the run
        c.r[15] = static_cast<uint32_t>(pc) & ~1u;
        c.cpsr = 0x10 | ((pc & 1) ? T_BIT : 0);
        while (!stop_) {
            Run();
            ++g_native_exits; g_native_last_reason = c.reason; g_native_last_pc = c.r[15];
#ifdef __3DS__
            const u64 t_host = g_profile ? svcGetSystemTick() : 0;
#endif
            const uint32_t kind = c.reason >> 16, index = c.reason & 0xFFFF;
            switch (kind) {
                case KIND_SVC:
                    c.r[15] = TRAMPOLINE_BASE + index * 8 + 4;   // as after `SVC #index` (the runtime reads it)
#ifdef __3DS__
                    if (g_profile) {
                        const u64 t = svcGetSystemTick();
                        if (svc_) svc_(this, index, svc_user_);
                        if (index < 512) { g_svc_ticks[index] += svcGetSystemTick() - t; ++g_svc_calls[index]; }
                    } else if (svc_) svc_(this, index, svc_user_);
#else
                    if (svc_) svc_(this, index, svc_user_);
#endif
                    break;
                case KIND_SITE: RunSite(sites_[index]); break;
                case KIND_POINTER: {
                    const Pointer& p = pointers_[index];
                    c.r[15] = p.target & ~1u;
                    if (p.target & 1) c.cpsr |= T_BIT;
                    CallHooks(c.r[15]);
                    break;
                }
                case KIND_EXIT: return OK;
                default:
                    std::cerr << "[Native] Unknown stub reason 0x" << std::hex << c.reason << std::dec << std::endl;
                    return 1;
            }
#ifdef __3DS__
            if (g_profile) g_host_ticks += svcGetSystemTick() - t_host;
#endif
            if (error_) return error_;
        }
        return OK;
    }
    int Stop() override { stop_ = true; return OK; }
    const char* ErrorText(int err) const override {
        switch (err) {
            case OK: return "OK";
            case ERR_READ_UNMAPPED: return "read from unmapped memory";
            case ERR_WRITE_UNMAPPED: return "write to unmapped memory";
            case ERR_GUEST_FAULT: return "the game's code crashed (a signal in it; see [Native] above)";
            default: return "native backend error (see the log)";
        }
    }

private:
#ifdef __vita__
    struct Region { uint32_t addr, size; SceUID block; };
#else
    struct Region { uint32_t addr, size; };
#endif
    struct Hook { CodeHook fn; void* user; };
    struct Site { uint32_t start, end; bool arm; };
    struct Pointer { uint32_t target; };

    static NativeCtx& ctx() { return g_native_ctx; }

    bool Mapped(uint64_t addr, size_t size) const {
        for (const Region& r : regions_)
            if (addr >= r.addr && addr + size <= static_cast<uint64_t>(r.addr) + r.size) return true;
        return false;
    }

    // ARM `b to` at `at`.
    static uint32_t Branch(uint32_t at, uint32_t to) { return 0xEA000000 | (((to - (at + 8)) >> 2) & 0x00FFFFFF); }

    // A stub in the trampoline block's stub area. Code: str r12, [sp, #-4]; mov r12, #index; orr r12, #kind << 16;
    // ldr pc, [pc, #-4]; .word native_exit.
    uint32_t NewStub(uint32_t kind, uint32_t index) {
        if (!stub_next_) stub_next_ = TRAMPOLINE_BASE + kStubArea;
        const uint32_t a = stub_next_;
        stub_next_ += 20;
        const uint32_t code[5] = { 0xE50DC004, 0xE3A0C000 | (index & 0xFF), 0xE38CC800 | kind, 0xE51FF004,
                                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&native_exit)) };
        MemWrite(a, code, sizeof(code));
        return a;
    }
    uint32_t Common(uint32_t kind) {   // one stub per kind for the trampolines and the exit (r12 = index already)
        auto it = common_.find(kind);
        if (it != common_.end()) return it->second;
        if (!stub_next_) stub_next_ = TRAMPOLINE_BASE + kStubArea;
        const uint32_t a = stub_next_;
        stub_next_ += 12;
        // SVC: orr r12, r12, #kind << 16 (the trampoline put the index in r12); exit: mov r12, #kind << 16
        const uint32_t set = kind == KIND_SVC ? 0xE38CC800 : 0xE3A0C800;
        const uint32_t code[3] = { set | kind, 0xE51FF004, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&native_exit)) };
        MemWrite(a, code, sizeof(code));
        return common_[kind] = a;
    }

    void InstallSite(const HookPlan& p) {
        const uint32_t start = G(p.start);
        for (const Site& s : sites_) if (s.start == start) return;   // already patched (several hooks, one site)
        const uint32_t stub = NewStub(KIND_SITE, static_cast<uint32_t>(sites_.size()));
        sites_.push_back({start, start + p.length, p.arm});
        // The original instructions go to a copy the interpreter reads (the patch overwrites them).
        for (uint32_t a = start; a < start + p.length; a += 2) original_[a] = Mem<uint16_t>(a);
        if (p.arm) {
            const uint32_t b = Branch(start, stub);
            MemWrite(start, &b, 4);
            return;
        }
        uint32_t at = start;
        std::vector<uint16_t> half;
        if (at % 4 == 2) half.push_back(0x46C0);   // nop
        half.push_back(0x4778);                    // bx pc -> ARM at the next word
        half.push_back(0x46C0);
        MemWrite(at, half.data(), half.size() * 2);
        at += static_cast<uint32_t>(half.size()) * 2;
        const uint32_t b = Branch(at, stub);
        MemWrite(at, &b, 4);
    }
    void InstallPointer(const HookPlan& p) {
        const uint32_t word = G(p.pointer), target = G(p.addr) | (p.arm ? 0 : 1);
        if (Mem<uint32_t>(word) != target) { std::cerr << "[Native] Pointer hook: 0x" << std::hex << word << " doesn't hold 0x" << target << std::dec << std::endl; return; }
        const uint32_t stub = NewStub(KIND_POINTER, static_cast<uint32_t>(pointers_.size()));
        pointers_.push_back({target});
        MemWrite(word, &stub, 4);
    }

    void CallHooks(uint32_t addr) {
        auto it = hooks_.find(addr);
        if (it == hooks_.end()) return;
        for (const Hook& h : it->second) h.fn(this, addr, 2, h.user);
    }

    // Runs the displaced instructions of a patched site, with the hooks at their addresses. Leaves PC where the
    // guest continues: after the site, or wherever a branch or a hook sent it.
    void RunSite(const Site& s) {
        NativeCtx& c = ctx();
        c.r[15] = s.start;
        if (!s.arm) c.cpsr |= T_BIT;
        while (c.r[15] >= s.start && c.r[15] < s.end && !error_) {
            const uint32_t pc = c.r[15];
            const bool thumb = (c.cpsr & T_BIT) != 0;
            CallHooks(pc);
            if (c.r[15] != pc || ((c.cpsr & T_BIT) != 0) != thumb) continue;   // a hook moved the PC
            Interp interp(this);
            if (!interp.Step()) error_ = 1;
        }
    }

    uint16_t Half(uint32_t a) { auto it = original_.find(a); return it != original_.end() ? it->second : Mem<uint16_t>(a); }

    // The shared interpreter (guest_interp.cpp) on g_native_ctx; instructions from the unpatched copy.
    struct Interp : GuestInterp {
        explicit Interp(NativeCpu* cpu) : GuestInterp(g_native_ctx.r, &g_native_ctx.cpsr), cpu_(cpu) {}
        uint16_t Fetch16(uint32_t a) override { return cpu_->Half(a); }
        uint32_t Read32(uint32_t a) override { return Mem<uint32_t>(a); }
        uint16_t Read16(uint32_t a) override { return Mem<uint16_t>(a); }
        uint8_t Read8(uint32_t a) override { return Mem<uint8_t>(a); }
        void Write32(uint32_t a, uint32_t v) override { Mem<uint32_t>(a) = v; }
        void Write16(uint32_t a, uint16_t v) override { Mem<uint16_t>(a) = v; }
        void Write8(uint32_t a, uint8_t v) override { Mem<uint8_t>(a) = v; }
        NativeCpu* cpu_;
    };

    void Run() {
        NativeCtx& c = ctx();
        uint32_t* frame = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(c.r[13] - 15 * 4));
        for (int k = 0; k < 13; ++k) frame[k] = c.r[k];
        frame[13] = c.r[14];
        frame[14] = c.r[15] | ((c.cpsr & T_BIT) ? 1 : 0);
#ifdef __3DS__
        if (dirty_hi_) {   // clean the data cache, then invalidate the instruction cache (Luma3DS system calls)
            const uint32_t lo = dirty_lo_ & ~31u, hi = (dirty_hi_ + 31) & ~31u;
            dirty_lo_ = 0xFFFFFFFF; dirty_hi_ = 0;
            native_flush_dcache_range(lo, hi - lo);
            native_invalidate_icache_range(lo, hi - lo);
            ++g_code_flushes;
        }
#elif defined(__vita__)
        if (dirty_hi_) {   // clean the data cache and invalidate the instruction cache over the range (kubridge)
            const uint32_t lo = dirty_lo_ & ~31u, hi = (dirty_hi_ + 31) & ~31u;
            dirty_lo_ = 0xFFFFFFFF; dirty_hi_ = 0;
            kuKernelFlushCaches(reinterpret_cast<const void*>(static_cast<uintptr_t>(lo)), hi - lo);
        }
#endif
        g_in_guest = 1;
        native_enter(frame, c.cpsr & 0xF8000000);
        g_in_guest = 0;
        c.cpsr = (c.cpsr & 0xF8000000) | 0x10;   // native_exit stored the APSR: keep its flags, ARM user mode
    }

    std::vector<Region> regions_;
    uint32_t block_ = 0, stub_next_ = 0;
    std::map<uint32_t, uint32_t> common_;
    std::map<uint32_t, std::vector<Hook>> hooks_;
    std::vector<Site> sites_;
    std::vector<Pointer> pointers_;
    std::map<uint32_t, uint16_t> original_;
    SvcHook svc_ = nullptr;
    void* svc_user_ = nullptr;
    std::atomic<bool> stop_{false};
    int error_ = 0;
    // 3DS / Vita: code bytes written since the last cache flush. Code = the trampoline block (64 KB) and the image up to
    // stock 0x4A0E4000: function pointers end at 0x4A0E2FC4, and the rest of the code section is the game's
    // writable globals (0x4A0F025C, ...), which the runtime writes all the time (README.md section 6.28).
    static constexpr uint32_t kCodeEnd = TRAMPOLINE_SIZE + 0xE4000;
    uint32_t dirty_lo_ = 0xFFFFFFFF, dirty_hi_ = 0;
};
}

std::unique_ptr<Cpu> Cpu::CreateNative(std::string*) { return std::make_unique<NativeCpu>(); }

#else
std::unique_ptr<Cpu> Cpu::CreateNative(std::string* error) {
    if (error) *error = "the native CPU backend needs a 32-bit ARM host";
    return nullptr;
}
#endif
