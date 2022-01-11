#include <X86TargetMachine.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cli-parser.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <vmemu_t.hpp>
#include <vmlocate.hpp>

#include "X86TargetMachine.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"

using namespace llvm;

int __cdecl main(int argc, const char* argv[]) {
  argparse::argument_parser_t parser("VMProtect 3 Static Devirtualization",
                                     "vmdevirt");
  parser.add_argument()
      .name("--vmentry")
      .description("relative virtual address to a vm entry...")
      .required(true);
  parser.add_argument()
      .name("--bin")
      .description("path to unpacked virtualized binary...")
      .required(true);
  parser.add_argument().name("--out").description("output file name...");

  vm::utils::init();
  parser.enable_help();
  auto result = parser.parse(argc, argv);

  if (result || parser.exists("help")) {
    parser.print_help();
    return -1;
  }

  std::vector<std::uint8_t> module_data, tmp, unpacked_bin;
  if (!vm::utils::open_binary_file(parser.get<std::string>("bin"),
                                   module_data)) {
    std::printf("[!] failed to open binary file...\n");
    return -1;
  }

  auto img = reinterpret_cast<win::image_t<>*>(module_data.data());
  auto image_size = img->get_nt_headers()->optional_header.size_image;
  const auto image_base = img->get_nt_headers()->optional_header.image_base;

  // page align the vector allocation so that unicorn-engine is happy girl...
  tmp.resize(image_size + PAGE_4KB);
  const std::uintptr_t module_base =
      reinterpret_cast<std::uintptr_t>(tmp.data()) +
      (PAGE_4KB - (reinterpret_cast<std::uintptr_t>(tmp.data()) & 0xFFFull));

  std::memcpy((void*)module_base, module_data.data(), 0x1000);
  std::for_each(img->get_nt_headers()->get_sections(),
                img->get_nt_headers()->get_sections() +
                    img->get_nt_headers()->file_header.num_sections,
                [&](const auto& section_header) {
                  std::memcpy(
                      (void*)(module_base + section_header.virtual_address),
                      module_data.data() + section_header.ptr_raw_data,
                      section_header.size_raw_data);
                });

  auto win_img = reinterpret_cast<win::image_t<>*>(module_base);

  auto basereloc_dir =
      win_img->get_directory(win::directory_id::directory_entry_basereloc);

  auto reloc_dir = reinterpret_cast<win::reloc_directory_t*>(
      basereloc_dir->rva + module_base);

  win::reloc_block_t* reloc_block = &reloc_dir->first_block;

  // apply relocations to all sections...
  while (reloc_block->base_rva && reloc_block->size_block) {
    std::for_each(reloc_block->begin(), reloc_block->end(),
                  [&](win::reloc_entry_t& entry) {
                    switch (entry.type) {
                      case win::reloc_type_id::rel_based_dir64: {
                        auto reloc_at = reinterpret_cast<std::uintptr_t*>(
                            entry.offset + reloc_block->base_rva + module_base);
                        *reloc_at = module_base + ((*reloc_at) - image_base);
                        break;
                      }
                      default:
                        break;
                    }
                  });

    reloc_block = reloc_block->next();
  }

  std::printf("> image base = %p, image size = %p, module base = %p\n",
              image_base, image_size, module_base);

  if (!image_base || !image_size || !module_base) {
    std::printf("[!] failed to open binary on disk...\n");
    return -1;
  }

  const auto vm_entry_rva =
      std::strtoull(parser.get<std::string>("vmentry").c_str(), nullptr, 16);

  vm::vmctx_t vmctx(module_base, image_base, image_size, vm_entry_rva);
  if (!vmctx.init()) {
    std::printf(
        "[!] failed to init vmctx... this can be for many reasons..."
        " try validating your vm entry rva... make sure the binary is "
        "unpacked and is"
        "protected with VMProtect 3...\n");
    return -1;
  }

  vm::emu_t emu(&vmctx);
  if (!emu.init()) {
    std::printf(
        "[!] failed to init vm::emu_t... read above in the console for the "
        "reason...\n");
    return -1;
  }

  vm::instrs::vrtn_t virt_rtn;
  if (!emu.emulate(vm_entry_rva, virt_rtn)) {
    std::printf(
        "[!] failed to emulate virtualized routine... read above in the "
        "console for the reason...\n");
  }

  std::printf("> traced %d virtual code blocks... \n", virt_rtn.m_blks.size());
  std::string module_name =
      parser.exists("out") ? parser.get<std::string>("out")
                           : parser.get<std::string>("bin")
                                 .append("-")
                                 .append(parser.get<std::string>("vmentry"))
                                 .append(".devirt");

  LLVMContext llvm_ctx;
  Module llvm_module(module_name, llvm_ctx);

  // lift virtual instructions to llvm-ir...

  // generate a bitcode file on disk...

  // run optimizations...

  // compile back to native X86...
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmParser();
  LLVMInitializeX86AsmPrinter();
}