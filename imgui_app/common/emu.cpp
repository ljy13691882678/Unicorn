// emu.cpp - 见 emu.hpp。
#include "emu.hpp"

namespace emu {

#ifdef USE_UNICORN

// 与 Python 版一致的内存布局
static constexpr uint64_t kCode   = 0x00100000ULL;
static constexpr uint64_t kData   = 0x00010000ULL;
static constexpr uint64_t kStack  = 0x03000000ULL;
static constexpr uint64_t kMagic  = 0x00110000ULL;
static constexpr size_t   kPage   = 0x1000;

// 代码 ret 后 LR=kMagic，在此处停机（模拟正常返回）。
static void hook_stop(uc_engine* uc, uint64_t address, uint32_t size, void* data) {
    if (address == kMagic) uc_emu_stop(uc);
}

Result runArm64(const std::vector<uint8_t>& blob) {
    Result r;
    uc_engine* uc = nullptr;
    auto clear = [&]() { if (uc) uc_close(uc); };

    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        r.error = "uc_open failed"; return r;
    }
    // 代码区 + 停机页
    if (uc_mem_map(uc, kCode - kPage, 2 * kPage, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_map(uc, kMagic & ~(kPage - 1), kPage, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_map(uc, kData & ~(kPage - 1), kPage, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_map(uc, (kStack - kPage) & ~(kPage - 1), 2 * kPage, UC_PROT_ALL) != UC_ERR_OK) {
        r.error = "uc_mem_map failed"; clear(); return r;
    }
    if (!blob.empty())
        uc_mem_write(uc, kCode, blob.data(), blob.size());

    uc_reg_write(uc, UC_ARM64_REG_SP, &kStack);
    uint64_t lr = kMagic;
    uc_reg_write(uc, UC_ARM64_REG_LR, &lr);

    // 在 kMagic 处停机，代表代码正常返回。
    uc_hook hook = 0;
    uc_err err = uc_hook_add(uc, &hook, UC_HOOK_CODE,
                             (void*)&hook_stop, nullptr, kMagic, kMagic);
    if (err != UC_ERR_OK) {
        r.error = "uc_hook_add failed"; clear(); return r;
    }
    err = uc_emu_start(uc, kCode, 0, 0, 0);
    if (err != UC_ERR_OK) {
        r.error = std::string("uc_emu_start: ") + uc_strerror(err);
        clear(); return r;
    }

    auto rd = [&](int reg, unsigned long long& v) {
        uint64_t x = 0; uc_reg_read(uc, reg, &x); v = x;
    };
    rd(UC_ARM64_REG_X0, r.x0); rd(UC_ARM64_REG_X1, r.x1);
    rd(UC_ARM64_REG_X2, r.x2); rd(UC_ARM64_REG_X3, r.x3);
    r.outbox.resize(32);
    uc_mem_read(uc, kData, r.outbox.data(), r.outbox.size());
    r.ok = true;
    clear();
    return r;
}

#endif // USE_UNICORN

} // namespace emu