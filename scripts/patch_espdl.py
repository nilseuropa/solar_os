#!/usr/bin/env python3
"""Apply reviewed ownership/allocation fixes to the pinned ESP-DL release."""
import argparse
import hashlib
import re
from pathlib import Path

OLD = "    Model() {}"
NEW = "    Model() { m_model_context = new ModelContext(); }"

ALLOC_OLD = "    void *operator new(size_t size) { return tool::malloc_aligned(size, MALLOC_CAP_DEFAULT); }"
ALLOC_NEW = """    void *operator new(size_t size)
    {
        void *p = tool::malloc_aligned(size, MALLOC_CAP_DEFAULT);
        if (!p) throw std::bad_alloc();
        return p;
    }"""

WORKER_INCLUDE = '#include "solar_os_dl_workers.inc"'
WORKER_HASH = "a0bd905f5da1c501f7f01e2a6c8277c6cdf05f20e3b8348967a8e7b11004a026"
DISPATCH_HASH = "d58f72d62daf7bd91f6f6e842175a51b444b499c1efa566d092f768a3251bcff"


def patch_workers(component: Path) -> None:
    source = component / "dl/module/src/dl_module_base.cpp"
    replace(source, "#include <string.h>\n",
        '#include <string.h>\n#include "esp_heap_caps.h"\n#include <new>\n')
    text = source.read_text()
    if WORKER_INCLUDE not in text:
        pool = re.search(r"namespace \{\n.*?\n\} // namespace\n", text, re.S)
        dispatch = re.search(r"void module_forward_dual_core\(.*?\n\}\n", text, re.S)
        for match, expected in ((pool, WORKER_HASH), (dispatch, DISPATCH_HASH)):
            if not match or hashlib.sha256(match[0].encode()).hexdigest() != expected:
                raise ValueError("ESP-DL source changed; review overlay: worker runtime")
        text = text.replace(dispatch[0], "").replace(pool[0], WORKER_INCLUDE + "\n")
        source.write_text(text)
    include = Path(__file__).parent / "espdl/worker_runtime.inc"
    destination = source.with_name("solar_os_dl_workers.inc")
    if not destination.exists() or destination.read_bytes() != include.read_bytes():
        destination.write_bytes(include.read_bytes())
    header = component / "dl/module/include/dl_module_base.hpp"
    declaration = "void module_forward_dual_core(Module *op, void *args1, void *args2);"
    replace(header, declaration, declaration + "\nvoid module_workers_release();")


def patch_flash_kernels(component: Path, placement: str = "flash") -> None:
    # Only explicit S3 code sections change; SIMD instructions remain intact.
    # Already-flash kernels and other target ISAs are untouched.
    for folder in ("dl/base/isa/tie728", "dl/tool/isa/tie728"):
        for path in (component / folder).glob("*.S"):
            source = path.read_text()
            if placement == "flash":
                changed = re.sub(r"(?m)^([ \t]*)\.section \.iram1[ \t]*$",
                    r"\1.text # SolarOS: cached flash SIMD kernel", source)
            elif placement == "iram":
                changed = re.sub(r"(?m)^([ \t]*)\.text # SolarOS: cached flash SIMD kernel[ \t]*$",
                    r"\1.section .iram1", source)
            else:
                raise ValueError("kernel placement must be flash or iram")
            if changed != source:
                path.write_text(changed)


def replace(path: Path, old: str, new: str) -> None:
    source = path.read_text()
    if new in source:
        return
    if source.count(old) != 1:
        raise ValueError(f"ESP-DL source changed; review overlay: {path.name}")
    path.write_text(source.replace(old, new))


def patch(component: Path, kernel_placement: str = "flash") -> None:
    manifest = (component / "idf_component.yml").read_text()
    if not re.search(r'''(?m)^version:\s*["']?3\.3\.13["']?\s*$''', manifest):
        raise ValueError("SolarOS ESP-DL overlay requires pinned version 3.3.13")
    replace(component / "dl/model/include/dl_model_base.hpp", OLD, NEW)
    for header in ("dl/module/include/dl_module_base.hpp", "dl/tensor/include/dl_tensor_base.hpp"):
        path = component / header
        replace(path, "#pragma once\n", "#pragma once\n#include <new>\n")
        replace(path, ALLOC_OLD, ALLOC_NEW)
    model = component / "dl/model/src/dl_model_base.cpp"
    replace(model, "#include <algorithm>\n", "#include <algorithm>\n#include <memory>\n")
    replace(model, "    MemoryManagerBase *memory_manager = nullptr;",
        "    std::unique_ptr<MemoryManagerBase> memory_manager;")
    replace(model, "        memory_manager = new MemoryManagerGreedy(max_internal_size);\n    } else",
        "        memory_manager.reset(new MemoryManagerGreedy(max_internal_size));\n    } else")
    replace(model, "        memory_manager = new MemoryManagerGreedy(max_internal_size);\n    }\n    memory_manager->alloc",
        "        memory_manager.reset(new MemoryManagerGreedy(max_internal_size));\n    }\n    memory_manager->alloc")
    replace(model, "    delete memory_manager;", "    memory_manager.reset();")
    replace(model, "    std::vector<std::string> sorted_nodes = m_fbs_model->topological_sort();\n    for",
        "    std::vector<std::string> sorted_nodes = m_fbs_model->topological_sort();\n"
        "    m_execution_plan.reserve(sorted_nodes.size());\n    for")
    greedy = component / "dl/model/src/dl_memory_manager_greedy.cpp"
    replace(greedy, "    std::vector<TensorInfo *> tensor_info;", """    std::vector<TensorInfo *> tensor_info;
    struct TensorInfoGuard {
        std::vector<TensorInfo *> &items;
        ~TensorInfoGuard() { for (auto *item : items) delete item; }
    } tensor_info_guard{tensor_info};""")
    replace(greedy, "        delete tensor_info[i];", "        delete tensor_info[i];\n        tensor_info[i] = nullptr;")
    patch_workers(component)
    patch_flash_kernels(component, kernel_placement)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("component", type=Path)
    parser.add_argument("--kernel-placement", choices=("flash", "iram"), default="flash")
    args = parser.parse_args()
    patch(args.component, args.kernel_placement)
