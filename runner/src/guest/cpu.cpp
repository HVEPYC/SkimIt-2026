#include "guest/cpu.h"

#include <sys/syscall.h>
#include <cstring>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <vector>

#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "hle/registry.h"

namespace skim {

using namespace Dynarmic;

namespace {
constexpr std::size_t kMaxThreads = 64;
constexpr std::uint32_t kDefaultStack = 1024 * 1024;
constexpr std::uint32_t kCpsrThumb = 1u << 5;

thread_local GuestThread* tls_current = nullptr;

struct Registry {
    std::mutex lock;
    std::set<GuestThread*> threads;
    std::vector<bool> ids = std::vector<bool>(kMaxThreads, false);
    ExclusiveMonitor monitor{kMaxThreads};
};

Registry& reg() {
    static Registry r;
    return r;
}

// cp15: TLS registers (TPIDRURO / TPIDRURW) and the ARMv6-style barrier operations.
class Cp15 final : public A32::Coprocessor {
public:
    Cp15(std::uint32_t* ro, std::uint32_t* rw) : ro_(ro), rw_(rw) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg crn, A32::CoprocReg crm, unsigned opc2) override {
        if (two || opc1 != 0) return std::monostate{};
        if (crn == A32::CoprocReg::C13 && crm == A32::CoprocReg::C0 && opc2 == 2) return rw_;
        if (crn == A32::CoprocReg::C7) return Callback{&Nop, std::nullopt};  // DSB / DMB / ISB (c7, c10/c5)
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg crn, A32::CoprocReg crm, unsigned opc2) override {
        if (two || opc1 != 0 || crn != A32::CoprocReg::C13 || crm != A32::CoprocReg::C0) return std::monostate{};
        if (opc2 == 3) return ro_;
        if (opc2 == 2) return rw_;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }

private:
    static std::uint64_t Nop(void*, std::uint32_t, std::uint32_t) { return 0; }
    std::uint32_t* ro_;
    std::uint32_t* rw_;
};
}  // namespace

GuestThread::GuestThread(std::uint32_t stack_size) {
    Memory& m = Memory::get();
    {
        Registry& r = reg();
        std::lock_guard g{r.lock};
        std::size_t id = 0;
        while (id < kMaxThreads && r.ids[id]) ++id;
        if (id == kMaxThreads) { std::fprintf(stderr, "[cpu] too many guest threads\n"); std::abort(); }
        r.ids[id] = true;
        processor_id_ = id;
    }
    tid_ = static_cast<std::uint32_t>(::syscall(SYS_gettid));

    stack_size_ = stack_size ? stack_size : kDefaultStack;
    stack_lo_ = m.map_anywhere(stack_size_);
    stack_hi_ = stack_lo_ + stack_size_;

    tls_ = heap::calloc(kTlsSlots, 4);
    m.write<gaddr>(tls_ + kTlsSlotSelf * 4, tls_);
    // bionic keeps the pthread_internal_t* (== pthread_self()) here; our pthread_t is the TLS block.
    m.write<std::uint32_t>(tls_ + kTlsSlotThreadId * 4, tls_);
    tpidruro_ = tls_;

    A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.processor_id = processor_id_;
    cfg.global_monitor = &reg().monitor;
    cfg.arch_version = A32::ArchVersion::v7;
    cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(m.base());
    cfg.enable_cycle_counting = false;
    cfg.coprocessors[15] = std::make_shared<Cp15>(&tpidruro_, &tpidrurw_);
    cfg.code_cache_size = 64 * 1024 * 1024;
    jit_ = std::make_unique<A32::Jit>(cfg);
    jit_->SetCpsr(0x10);  // user mode, ARM state
    jit_->SetFpscr(0);
    jit_->Regs()[13] = stack_hi_;
    tls_current = this;
    // Visible to invalidate_all / signals only once the JIT exists (a new thread has no stale code).
    std::lock_guard g{reg().lock};
    reg().threads.insert(this);
}

GuestThread::~GuestThread() {
    {
        Registry& r = reg();
        std::lock_guard g{r.lock};
        r.threads.erase(this);
        r.ids[processor_id_] = false;
        r.monitor.ClearProcessor(processor_id_);
    }
    heap::free(tls_);
    Memory::get().unmap(stack_lo_, stack_size_);
    if (tls_current == this) tls_current = nullptr;
}

GuestThread* GuestThread::current() { return tls_current; }

void GuestThread::set_pc(gaddr pc) {
    const std::uint32_t cpsr = jit_->Cpsr();
    jit_->SetCpsr((pc & 1) ? (cpsr | kCpsrThumb) : (cpsr & ~kCpsrThumb));
    jit_->Regs()[15] = pc & ~1u;
}

GuestContext GuestThread::save() const {
    return {jit_->Regs(), jit_->ExtRegs(), jit_->Cpsr(), jit_->Fpscr()};
}

void GuestThread::restore(const GuestContext& c) {
    jit_->Regs() = c.regs;
    jit_->ExtRegs() = c.ext;
    jit_->SetCpsr(c.cpsr);
    jit_->SetFpscr(c.fpscr);
}

std::uint64_t GuestThread::call(gaddr fn, std::span<const std::uint32_t> args) {
    const GuestContext saved = save();
    auto& R = jit_->Regs();
    std::uint32_t sp = depth_ == 0 ? stack_hi_ : R[13];
    const std::size_t on_stack = args.size() > 4 ? args.size() - 4 : 0;
    sp = (sp - static_cast<std::uint32_t>(on_stack * 4)) & ~7u;
    for (std::size_t i = 0; i < on_stack; ++i) Memory::get().write<std::uint32_t>(sp + 4 * i, args[4 + i]);
    for (std::size_t i = 0; i < 4; ++i) R[i] = i < args.size() ? args[i] : 0;
    R[13] = sp;
    R[14] = kReturnThunk;
    set_pc(fn);

    ++depth_;
    for (;;) {
        jit_->Run();
        jit_->ClearHalt(HaltReason::UserDefined1);
        jit_->ClearHalt(HaltReason::UserDefined2);
        if (svc_pending_) {
            svc_pending_ = false;
            if (pending_svc_ == kSvcReturn) break;
            dispatch_svc(pending_svc_);
        }
        // Otherwise: a signal or a cache invalidation requested by another thread.
        if (signal_deliverable()) deliver_pending();
    }
    --depth_;

    const std::uint64_t result = R[0] | static_cast<std::uint64_t>(R[1]) << 32;
    restore(saved);
    return result;
}

void GuestThread::dispatch_svc(std::uint32_t swi) {
    if (swi >= kSvcImport && swi < kSvcReturn) {
        hle::dispatch(*this, swi - kSvcImport);
    } else if (swi == 0) {
        guest_syscall(*this);
    } else if (swi == 0x9F0002) {  // OABI-style cacheflush
        invalidate_all(regs()[0], regs()[1] - regs()[0]);
        regs()[0] = 0;
    } else {
        fatal("unknown svc #0x%x", swi);
    }
}

void GuestThread::invalidate_all(gaddr start, std::uint32_t len) {
    if (!len) return;
    Registry& r = reg();
    std::lock_guard g{r.lock};
    for (GuestThread* t : r.threads) t->jit_->InvalidateCacheRange(start, len);
}

GuestThread* GuestThread::by_tid(std::uint32_t tid) {
    for (GuestThread* t : reg().threads)
        if (t->tid_ == tid) return t;
    return nullptr;
}

GuestThread* GuestThread::by_handle(gaddr pthread) {
    for (GuestThread* t : reg().threads)
        if (t->tls_ == pthread) return t;
    return nullptr;
}

std::unique_lock<std::mutex> GuestThread::signal_lock() { return std::unique_lock{reg().lock}; }

void GuestThread::send_signal(int sig) {
    pending_.fetch_or(1u << (sig - 1));
    jit_->HaltExecution(HaltReason::UserDefined2);
    if (auto* cv = waiting_cv_.load()) cv->notify_all();
}

namespace {
// Linux ARM signal frame: siginfo_t (128 bytes), then ucontext: uc_flags, uc_link, uc_stack (12),
// sigcontext at +20 (trap_no, error_code, oldmask, r0-r10, fp, ip, sp, lr, pc, cpsr, fault_address),
// uc_sigmask at +104.
constexpr std::uint32_t kSigInfo = 128, kUcontext = 744, kUcRegs = 20 + 12, kUcCpsr = kUcRegs + 16 * 4, kUcMask = 104;
constexpr std::uint32_t kSaSiginfo = 4, kSaNodefer = 0x40000000, kSiTkill = static_cast<std::uint32_t>(-6);
}  // namespace

void GuestThread::deliver_pending() {
    Memory& m = Memory::get();
    while (std::uint32_t ready = pending_.load() & ~blocked_) {
        const int sig = __builtin_ctz(ready) + 1;
        pending_.fetch_and(~(1u << (sig - 1)));
        const hle::SigAction act = hle::sigaction_for(sig);
        if (act.handler == 1) continue;  // SIG_IGN
        if (act.handler == 0) {          // SIG_DFL: the signals the runtime uses are all handled
            std::fprintf(stderr, "[sig] default action for signal %d on tid %u ignored\n", sig, tid_);
            continue;
        }
        static const bool trace = std::getenv("SKIM_TRACE_SIGNALS") != nullptr;
        if (trace) std::fprintf(stderr, "[sig] delivering %d on tid %u\n", sig, tid_);
        auto& R = jit_->Regs();
        const std::uint32_t old_blocked = blocked_;
        const gaddr frame = (R[13] - kSigInfo - kUcontext) & ~7u;
        const gaddr info = frame, uc = frame + kSigInfo;
        std::memset(m.ptr(frame), 0, kSigInfo + kUcontext);
        m.write<std::uint32_t>(info, static_cast<std::uint32_t>(sig));
        m.write<std::uint32_t>(info + 8, kSiTkill);
        m.write<std::uint32_t>(info + 12, static_cast<std::uint32_t>(getpid()));
        for (int i = 0; i < 16; ++i) m.write<std::uint32_t>(uc + kUcRegs + 4 * i, R[i]);
        m.write<std::uint32_t>(uc + kUcCpsr, jit_->Cpsr());
        m.write<std::uint32_t>(uc + kUcMask, old_blocked);

        blocked_ |= act.mask | ((act.flags & kSaNodefer) ? 0 : 1u << (sig - 1));
        const GuestContext saved = save();
        R[13] = frame;
        if (act.flags & kSaSiginfo) call(act.handler, {static_cast<std::uint32_t>(sig), info, uc});
        else call(act.handler, {static_cast<std::uint32_t>(sig)});

        // sigreturn: registers and mask from the (possibly modified) frame.
        restore(saved);
        for (int i = 0; i < 16; ++i) R[i] = m.read<std::uint32_t>(uc + kUcRegs + 4 * i);
        jit_->SetCpsr(m.read<std::uint32_t>(uc + kUcCpsr));
        blocked_ = m.read<std::uint32_t>(uc + kUcMask);
    }
}

void GuestThread::CallSVC(std::uint32_t swi) {
    pending_svc_ = swi;
    svc_pending_ = true;
    jit_->HaltExecution(HaltReason::UserDefined1);
}

void GuestThread::check_access(std::uint32_t a, const char* kind) {
    if (!Memory::get().is_mapped(a)) fatal("guest %s fault at 0x%08x", kind, a);
}

#define SKIM_READ(bits)                                                       \
    std::uint##bits##_t GuestThread::MemoryRead##bits(std::uint32_t a) {      \
        check_access(a, "read");                                              \
        return Memory::get().read<std::uint##bits##_t>(a);                    \
    }                                                                         \
    void GuestThread::MemoryWrite##bits(std::uint32_t a, std::uint##bits##_t v) { \
        check_access(a, "write");                                             \
        Memory::get().write<std::uint##bits##_t>(a, v);                       \
    }                                                                         \
    bool GuestThread::MemoryWriteExclusive##bits(std::uint32_t a, std::uint##bits##_t v, std::uint##bits##_t expected) { \
        check_access(a, "exclusive write");                                   \
        auto* p = Memory::get().ptr<std::uint##bits##_t>(a);                  \
        return __atomic_compare_exchange_n(p, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    }
SKIM_READ(8)
SKIM_READ(16)
SKIM_READ(32)
SKIM_READ(64)
#undef SKIM_READ

void GuestThread::InterpreterFallback(std::uint32_t pc, std::size_t n) {
    fatal("interpreter fallback requested at 0x%08x (%zu instructions)", pc, n);
}

void GuestThread::ExceptionRaised(std::uint32_t pc, A32::Exception e) {
    switch (e) {
    case A32::Exception::Yield:
    case A32::Exception::SendEvent:
    case A32::Exception::SendEventLocal:
    case A32::Exception::WaitForEvent:
    case A32::Exception::WaitForInterrupt:
    case A32::Exception::PreloadData:
    case A32::Exception::PreloadDataWithIntentToWrite:
    case A32::Exception::PreloadInstruction:
        return;
    default:
        fatal("guest exception %d at 0x%08x", static_cast<int>(e), pc);
    }
}

void GuestThread::fatal(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[cpu] fatal (tid %u): ", tid_);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    const auto& R = jit_->Regs();
    std::fprintf(stderr, "\n  pc=%08x lr=%08x sp=%08x cpsr=%08x\n", R[15], R[14], R[13], jit_->Cpsr());
    for (int i = 0; i < 13; ++i) std::fprintf(stderr, "  r%-2d=%08x%s", i, R[i], i % 4 == 3 ? "\n" : "");
    std::fprintf(stderr, "\n  %s / %s\n", hle::describe(R[15]).c_str(), hle::describe(R[14]).c_str());
    std::fflush(stderr);
    std::abort();
}

}  // namespace skim
