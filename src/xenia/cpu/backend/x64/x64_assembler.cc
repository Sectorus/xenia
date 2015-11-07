/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_assembler.h"

#include <climits>

#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/capstone/include/capstone/x86.h"
#include "xenia/base/profiling.h"
#include "xenia/base/reset_scope.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_function.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

using xe::cpu::hir::HIRBuilder;

X64Assembler::X64Assembler(X64Backend* backend)
    : Assembler(backend), x64_backend_(backend), capstone_handle_(0) {
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &capstone_handle_) != CS_ERR_OK) {
    assert_always("Failed to initialize capstone");
  }
  cs_option(capstone_handle_, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
  cs_option(capstone_handle_, CS_OPT_DETAIL, CS_OPT_OFF);
}

X64Assembler::~X64Assembler() {
  // Emitter must be freed before the allocator.
  emitter_.reset();
  allocator_.reset();

  if (capstone_handle_) {
    cs_close(&capstone_handle_);
  }
}

bool X64Assembler::Initialize() {
  if (!Assembler::Initialize()) {
    return false;
  }

  allocator_.reset(new XbyakAllocator());
  emitter_.reset(new X64Emitter(x64_backend_, allocator_.get()));

  return true;
}

void X64Assembler::Reset() {
  string_buffer_.Reset();
  Assembler::Reset();
}

bool X64Assembler::Assemble(GuestFunction* function, HIRBuilder* builder,
                            uint32_t debug_info_flags,
                            std::unique_ptr<DebugInfo> debug_info) {
  SCOPE_profile_cpu_f("cpu");

  // Reset when we leave.
  xe::make_reset_scope(this);

  // Lower HIR -> x64.
  void* machine_code = nullptr;
  size_t code_size = 0;
  if (!emitter_->Emit(function, builder, debug_info_flags, debug_info.get(),
                      &machine_code, &code_size, &function->source_map())) {
    return false;
  }

  // Stash generated machine code.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmMachineCode) {
    DumpMachineCode(machine_code, code_size, function->source_map(),
                    &string_buffer_);
    debug_info->set_machine_code_disasm(string_buffer_.ToString());
    string_buffer_.Reset();
  }

  // Dump debug data.
  if (FLAGS_disassemble_functions) {
    if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmSource) {
      // auto fn_data = backend_->processor()->debugger()->AllocateFunctionData(
      //    xe::debug::FunctionDisasmData::SizeOfHeader());
    }
  }

  function->set_debug_info(std::move(debug_info));
  static_cast<X64Function*>(function)
      ->Setup(reinterpret_cast<uint8_t*>(machine_code), code_size);

  return true;
}

void X64Assembler::DumpMachineCode(
    void* machine_code, size_t code_size,
    const std::vector<SourceMapEntry>& source_map, StringBuffer* str) {
  if (source_map.empty()) {
    return;
  }
  auto source_map_index = 0;
  uint32_t next_code_offset = source_map[0].code_offset;

  const uint8_t* code_ptr = reinterpret_cast<uint8_t*>(machine_code);
  size_t remaining_code_size = code_size;
  uint64_t address = uint64_t(machine_code);
  cs_insn insn = {0};
  while (remaining_code_size &&
         cs_disasm_iter(capstone_handle_, &code_ptr, &remaining_code_size,
                        &address, &insn)) {
    // Look up source offset.
    auto code_offset =
        uint32_t(code_ptr - reinterpret_cast<uint8_t*>(machine_code));
    if (code_offset >= next_code_offset &&
        source_map_index < source_map.size()) {
      auto& source_map_entry = source_map[source_map_index];
      str->AppendFormat("%.8X ", source_map_entry.guest_address);
      ++source_map_index;
      next_code_offset = source_map_index < source_map.size()
                             ? source_map[source_map_index].code_offset
                             : UINT_MAX;
    } else {
      str->Append("         ");
    }

    str->AppendFormat("%.8X      %-6s %s\n", uint32_t(insn.address),
                      insn.mnemonic, insn.op_str);
  }
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
