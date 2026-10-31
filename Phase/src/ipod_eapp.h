#pragma once
#include "runtime/cpu.h"
#include "runtime/arm_runtime.h"
namespace phase::eapp {
void framework_call(Cpu& cpu, const char* framework, unsigned ordinal, void (*handler)(Cpu&));
}
