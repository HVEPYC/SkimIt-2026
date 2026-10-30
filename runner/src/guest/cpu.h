// A guest thread: one dynarmic A32 JIT, a guest stack and a TLS block.
//
// Imports are thunks of the form `svc #(kSvcImport + index)`. CallSVC only records the number
// and halts the JIT; the import runs after Run() returns. dynarmic is not re-entrant, so this is
// what lets an HLE function call back into guest code (call()) safely.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <initializer_list>
#include <memory>
#include <span>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "guest/memory.h"

namespace skim {

constexpr std::uint32_t kSvcImport = 0x00A0'0000;  // + import index
constexpr std::uint32_t kSvcReturn = 0x00BF'FFFF;  // end of a host->guest call
constexpr std::uint32_t kReturnThunk = layout::kThunks;  // `svc #kSvcReturn`

// bionic's TLS slots (arm): TPIDRURO points at an array of these.
constexpr int kTlsSlots = 64;
constexpr int kTlsSlotSelf = 0, kTlsSlotThreadId = 1, kTlsSlotErrno = 2;

struct GuestContext {
    std::array<std::uint32_t, 16> regs;
    std::array<std::uint32_t, 64> ext;
    std::uint32_t cpsr, fpscr;
};

class GuestThread final : public Dynarmic::A32::UserCallbacks {
public:
    // stack_size 0 = default (1 MiB). Must be constructed on the host thread that will run it.
    explicit GuestThread(std::uint32_t stack_size = 0);
    ~GuestThread() override;

    static GuestThread* current();

    // Calls a guest function (Thumb if bit 0 is set) with AAPCS arguments; returns r0 | r1 << 32.
    std::uint64_t call(gaddr fn, std::span<const std::uint32_t> args);
    std::uint64_t call(gaddr fn, std::initializer_list<std::uint32_t> args) {
        return call(fn, std::span<const std::uint32_t>(args.begin(), args.size()));
    }

    Dynarmic::A32::Jit& jit() { return *jit_; }
    std::array<std::uint32_t, 16>& regs() { return jit_->Regs(); }
    void set_pc(gaddr pc);  // handles the Thumb bit
    GuestContext save() const;
    void restore(const GuestContext& c);

    std::uint32_t tid() const { return tid_; }
    gaddr tls() const { return tls_; }
    gaddr errno_addr() const { return tls_ + kTlsSlotErrno * 4; }
    void set_errno(int e) { Memory::get().write<std::int32_t>(errno_addr(), e); }
    gaddr stack_lo() const { return stack_lo_; }
    gaddr stack_hi() const { return stack_hi_; }

    // Invalidates translated code in every guest thread (Mono's JIT writes code, then cacheflush).
    static void invalidate_all(gaddr start, std::uint32_t len);

    // ---- signals (1-32; bionic's sigset_t is one word) ----
    static GuestThread* by_tid(std::uint32_t tid);   // registry lookups; the caller must
    static GuestThread* by_handle(gaddr pthread);    // hold signal_lock() while using the result
    static std::unique_lock<std::mutex> signal_lock();
    // Marks a signal pending and interrupts the thread (JIT halt, or wakes an HLE wait).
    void send_signal(int sig);
    bool signal_deliverable() const { return (pending_.load() & ~blocked_) != 0; }
    // Runs the handlers of deliverable signals on this thread, kernel style: a siginfo + ucontext
    // frame on the guest stack, the handler called on it, the registers restored from the frame.
    void deliver_pending();
    std::uint32_t blocked() const { return blocked_; }
    void set_blocked(std::uint32_t m) { blocked_ = m; }

    // HLE waits: waits until pred() or the deadline, running signal handlers that arrive meanwhile
    // (with the lock released). Returns pred().
    using WaitClock = std::chrono::system_clock;
    template<typename Pred>
    bool wait(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, Pred pred,
              const WaitClock::time_point* deadline = nullptr) {
        for (;;) {
            if (pred()) return true;
            if (signal_deliverable()) {
                lk.unlock();
                deliver_pending();
                lk.lock();
                continue;
            }
            auto until = WaitClock::now() + std::chrono::milliseconds(10);  // fallback poll for signals
            if (deadline && *deadline < until) until = *deadline;
            waiting_cv_.store(&cv);
            cv.wait_until(lk, until);
            waiting_cv_.store(nullptr);
            if (deadline && WaitClock::now() >= *deadline) return pred();
        }
    }

    // UserCallbacks: the slow path behind fastmem (faults and exclusive stores).
    std::uint8_t MemoryRead8(std::uint32_t a) override;
    std::uint16_t MemoryRead16(std::uint32_t a) override;
    std::uint32_t MemoryRead32(std::uint32_t a) override;
    std::uint64_t MemoryRead64(std::uint32_t a) override;
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override;
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override;
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override;
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override;
    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t expected) override;
    void InterpreterFallback(std::uint32_t pc, std::size_t n) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1'000'000'000; }

    [[noreturn]] void fatal(const char* fmt, ...);

private:
    void check_access(std::uint32_t a, const char* kind);
    void dispatch_svc(std::uint32_t swi);

    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    std::uint32_t tpidruro_ = 0, tpidrurw_ = 0;
    std::size_t processor_id_ = 0;
    std::uint32_t tid_ = 0;
    gaddr tls_ = 0, stack_lo_ = 0, stack_hi_ = 0;
    std::uint32_t stack_size_ = 0;
    std::uint32_t pending_svc_ = 0;
    bool svc_pending_ = false;
    int depth_ = 0;
    std::atomic<std::uint32_t> pending_{0};
    std::uint32_t blocked_ = 0;
    std::atomic<std::condition_variable*> waiting_cv_{nullptr};
};

// Linux syscalls made with `svc #0` (r7 = number), e.g. Mono's cacheflush.
void guest_syscall(GuestThread& t);

}  // namespace skim
