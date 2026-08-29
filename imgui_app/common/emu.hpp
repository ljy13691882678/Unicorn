// emu.hpp - Unicorn 模拟封装（可选）。未编译进 Unicorn 时退化为桩。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef USE_UNICORN
  #include <unicorn/unicorn.h>
#endif

namespace emu {

// ARM64 裸机代码段模拟：把 blob 加载到 0x100000 执行，
// 结果写向 0x10000。返回寄存器 x0..x3 与结果内存 hex。
// 未启用 Unicorn 时返回 "not built"（桩），便于两端口径统一。
#ifdef USE_UNICORN
struct Result {
    bool ok = false;
    std::string error;
    unsigned long long x0 = 0, x1 = 0, x2 = 0, x3 = 0;
    std::vector<uint8_t> outbox;
};
Result runArm64(const std::vector<uint8_t>& blob);
#else
inline std::string emuStatus() { return "Unicorn 未编译，请以 -DUSE_UNICORN=ON 重新构建"; }
#endif

} // namespace emu