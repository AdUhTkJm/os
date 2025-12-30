#!/bin/python3
import argparse;
import subprocess as proc;
import os;
import hashlib;
import pickle;
import tempfile;
import multiprocessing as mp;
import time;
import shutil;
import json;
from pathlib import Path;

mp.set_start_method("fork")

parser = argparse.ArgumentParser()
parser.add_argument("-r", "--run", action="store_true")
parser.add_argument("--la", action="store_true")
parser.add_argument("--rebuild", action="store_true")
parser.add_argument("-d", "--objdump", action="store_true")
parser.add_argument("-a", "--assembly", action="store_true")
parser.add_argument("-m", "--mount", action="store_true")
parser.add_argument("--docs", action="store_true")
parser.add_argument("--log-refcnt", action="store_true")
parser.add_argument("--log-syscall", action="store_true")
parser.add_argument("--detect-deadlock", action="store_true")
parser.add_argument("--no-instrument", action="store_true")
parser.add_argument("--no-debug-memory", action="store_true")
parser.add_argument("--debug-memory-expensive", action="store_true")
parser.add_argument("--no-debug", action="store_true")
parser.add_argument("--gdb", action="store_true")
parser.add_argument("--smp", action="store_true")
parser.add_argument("--test", action="store_true")
parser.add_argument("--unit-test", action="store_true")
parser.add_argument("--dot", action="store_true")
parser.add_argument("--release", action="store_true")

args = parser.parse_args()

SRC_DIR = Path("src")
BUILD_DIR = Path("build")
FINAL_BINARY = BUILD_DIR / "kernel"
COMPILER = "riscv64-unknown-elf-g++"
QEMU = "qemu-system-riscv64"
AR = "riscv64-unknown-elf-ar"
CFLAGS = [
  "-x", "c", "-c", "-std=c11", "-O2",
  "-Wall", "-Wextra", "-Wuninitialized", "-fno-strict-aliasing",
  "-ffreestanding", "-nostdlib",
]
CXXFLAGS = [
  "-x", "c++", "-c", "-std=c++20", "-O2",
  "-Wall", "-Wextra", "-Wuninitialized", "-fno-strict-aliasing",
  "-ffreestanding", "-nostdlib", "-fno-rtti", "-fno-exceptions",
  "-fno-threadsafe-statics", "-Wno-invalid-offsetof", "-fno-stack-protector"
]
# Note this is included in https://gcc.gnu.org/onlinedocs/gcc/Overall-Options.html.
SFLAGS = [
  "-x", "assembler-with-cpp", "-c", "-ffreestanding", "-nostdlib",
]
LINK_SCRIPT = "link_la.ld" if args.la else "link.ld"
LDFLAGS = ["-T", LINK_SCRIPT, "-nostdlib"]
CACHE_FILE = BUILD_DIR / ".build_cache.pkl"
INCLUDE_CACHE_FILE = BUILD_DIR / ".include_cache.pkl"
INITRAMFS_PATH = SRC_DIR / "fs/init"
SPECIAL_FLAGS = {
  "src/interrupt/interrupt.cpp": ["-Wno-unused-variable"]
}

flags = []

if not args.no_instrument and not args.release:
  flags += ["-DFUNC_INSTRUMENT", "-finstrument-functions"]

if not args.no_debug_memory and not args.release:
  flags += ["-DDEBUG_MEMORY"]

if args.debug_memory_expensive:
  flags += ["-DDEBUG_MEMORY_EXPENSIVE"]

if args.log_refcnt:
  flags += ["-DLOG_REFCNT"]

if not args.log_syscall:
  flags += ["-DNO_SYSCALL_LOG"]

if args.detect_deadlock:
  flags += ["-DDEADLOCK"]
  
if args.no_debug or args.release:
  flags += ["-DNDEBUG"]
else:
  flags += ["-g"]

if not args.smp:
  flags += ["-DUNIPROCESSOR"]

if args.unit_test:
  flags += ["-DUNIT_TEST"]

if args.release:
  flags += ["-flto"]
  LDFLAGS += ["-flto"]

if args.la:
  # Loongarch.
  # For loongarch documentation, see:
  #   https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html
  COMPILER = "loongarch64-unknown-linux-gnu-g++"
  QEMU = "qemu-system-loongarch64"
  MACHINESPEC = ["-march=loongarch64", "-mabi=lp64d"]
else:
  # RISC-V.
  MACHINESPEC = ["-mcmodel=medany", "-march=rv64gc_zifencei", "-mabi=lp64"]
  flags += MACHINESPEC
  LDFLAGS += MACHINESPEC

CFLAGS.extend(flags)
CXXFLAGS.extend(flags)
SFLAGS.extend(flags)

def hash_file(path):
  h = hashlib.sha256()
  with open(path, 'rb') as f:
    h.update(f.read())
  return h.hexdigest()

def find_files() -> tuple[list[Path], list[Path]]:
  cpp_files = []
  h_files = []
  for path in SRC_DIR.rglob("*"):
    if path.is_relative_to(INITRAMFS_PATH):
      continue
    if path.suffix in [".cpp", ".c", ".s"]:
      cpp_files.append(path)
    elif path.suffix == ".h":
      h_files.append(path)
  return cpp_files, h_files

def load_cache():
  if CACHE_FILE.exists():
    with open(CACHE_FILE, "rb") as f:
      return pickle.load(f)
  return {}

def save_cache(cache):
  BUILD_DIR.mkdir(parents=True, exist_ok=True)
  with open(CACHE_FILE, "wb") as f:
    pickle.dump(cache, f)

def needs_recompile(src_path, obj_path, cache, dependencies):
  src_hash = hash_file(src_path)
  dep_hashes = { str(dep): hash_file(dep) for dep in dependencies }

  prev = cache.get(str(src_path))
  if not prev:
      return True
  if prev['src_hash'] != src_hash:
      return True
  if prev['dep_hashes'] != dep_hashes:
      return True
  if not obj_path.exists():
      return True
  return False

include_cache = {}
include_hashes = {}

def load_include_cache():
  if INCLUDE_CACHE_FILE.exists():
    with open(INCLUDE_CACHE_FILE, "rb") as f:
      return pickle.load(f)
  return {}

def save_include_cache(cache):
  BUILD_DIR.mkdir(parents=True, exist_ok=True)
  with open(INCLUDE_CACHE_FILE, "wb") as f:
    pickle.dump(cache, f)

def get_all_includes(src_path: Path, visited=None) -> set[Path]:
  if visited is None:
    visited = set()

  resolved_path = src_path.resolve()
  if resolved_path in visited:
    return set()

  visited.add(resolved_path)
  file_hash = hash_file(resolved_path)

  cached_entry = include_cache.get(resolved_path)
  if cached_entry and include_hashes.get(resolved_path) == file_hash:
    return cached_entry

  includes = set()

  with open(resolved_path, "r") as f:
    for line in f:
      line = line.strip()
      if line.startswith("#include \""):
        header = line.split("\"")[1]
        include_path = resolved_path.parent / header
        if include_path.exists():
          includes.add(include_path.resolve())
          includes.update(get_all_includes(include_path, visited))

  include_cache[resolved_path] = includes
  include_hashes[resolved_path] = file_hash
  return includes

flagmap = {
  ".cpp": CXXFLAGS,
  ".c" : CFLAGS,
  ".s" : SFLAGS
}

def get_flags(path: Path):
  flags = flagmap[path.suffix]
  if str(path) in SPECIAL_FLAGS:
    flags.extend(SPECIAL_FLAGS[str(path)])
  if path.name == "libc.cpp":
    try: flags.remove("-flto")
    except: pass
    flags.append("-fno-lto")
  return flags

def compile_file(src_path: Path, obj_path: Path):
  obj_path.parent.mkdir(parents=True, exist_ok=True)
  src = str(src_path)
  proc.check_call([COMPILER, *get_flags(src_path), "-o", str(obj_path), src])

def compile_initramfs(src_path: Path, obj_path: Path):
  obj_path.parent.mkdir(parents=True, exist_ok=True)
  flags = ["-ffreestanding", "-nostdlib", "-O2", *MACHINESPEC]
  if args.test:
    flags += ["-DTEST"]
  proc.check_call([COMPILER, *flags, "-o", str(obj_path),  str(src_path)])

def archive_objects(obj_files, lib_path: Path):
  if lib_path.exists():
    lib_path.unlink()
  print(f"Creating archive {lib_path}")
  proc.check_call([AR, "rcs", str(lib_path)] + [str(obj) for obj in obj_files])

def link_libraries(lib_files, output_binary):
  result = proc.run([COMPILER] + LDFLAGS + ["-o", str(output_binary)] + ["-Wl,--start-group"] + [str(lib) for lib in lib_files] + ["-Wl,--end-group"], stderr=proc.PIPE)
  # Manually ignore a warning.
  for line in result.stderr.decode("utf-8").split("\n"):
    if line.find("warning: build/kernel has a LOAD segment with RWX permissions") != -1:
      continue
    print(line)

  if result.returncode != 0:
    os._exit(1)
  

def build_initramfs():
  tasks: list[tuple[Path, Path]] = []
  cache = load_cache()
  for file in INITRAMFS_PATH.rglob("*"):
    if file.is_dir():
      continue
    obj_dir = BUILD_DIR / "initramfs"
    obj_path = obj_dir / (file.stem)
    if needs_recompile(file, obj_path, cache, dependencies={}):
      tasks.append((file, obj_path))
  total = len(tasks)
  file_prompt = "file" if total == 1 else "files"
  if total > 0:
    print(f"Bundling {total} {file_prompt}")
  with mp.Pool() as pool:
    pool.starmap(compile_initramfs, tasks)
  proc.check_call(f"cd {obj_dir} && find . -print | cpio -oH newc > ../initramfs.cpio 2> /dev/null", shell=True)
  (obj_dir / "dev").mkdir(exist_ok=True)
  (obj_dir / "tmp").mkdir(exist_ok=True)
  (obj_dir / "mnt").mkdir(exist_ok=True)

def build():
  # Create a symbol table.
  proc.check_call(["scripts/symtbl.sh"])

  build_initramfs()

  global include_cache, include_hashes
  include_cache_data = load_include_cache()
  include_cache = include_cache_data.get("cache", {})
  include_hashes = include_cache_data.get("hashes", {})

  cpp_files, _ = find_files()
  cache = load_cache()

  # Generate compile_commands.json.
  commands = []
  for file in cpp_files:
    absolute = str(file.absolute())
    flags = [x for x in get_flags(file) if x not in ["-mcmodel=medany", "-march=rv64gc_zifencei", "-mabi=lp64"]]
    commands.append({
      "directory": os.path.abspath("."),
      "file": absolute,
      "command": f"clang++ {' '.join(flags)} -DIN_VSCODE -c {absolute}"
    })
    
  with open("compile_commands.json", "w") as conf:
    json.dump(commands, conf, indent=2)

  # Step 1: Compile .cpp to .o
  obj_files: list[tuple[Path, Path]] = []
  folder_changed: dict[Path, bool] = {}
  tasks: list[tuple[Path, Path]] = []
  for cpp in cpp_files:
    rel_dir = cpp.relative_to(SRC_DIR).parent
    obj_dir = BUILD_DIR / rel_dir
    obj_path = obj_dir / (cpp.stem + ".o")

    dependencies = get_all_includes(cpp)
    if needs_recompile(cpp, obj_path, cache, dependencies):
      tasks.append((cpp, obj_path))
      cache[str(cpp)] = {
        'src_hash': hash_file(cpp),
        'dep_hashes': {str(dep): hash_file(dep) for dep in dependencies},
      }
      folder_changed[rel_dir] = True
    obj_files.append((rel_dir, obj_path))

  total = len(tasks)
  file_prompt = "file" if total == 1 else "files"
  if total > 0:
    print(f"Compiling {total} {file_prompt}")
  with mp.Pool() as pool:
    pool.starmap(compile_file, tasks)
  
  save_cache(cache)

  # Step 2: Archive .o's in same folder into .a
  folder_objs = {}
  for rel_dir, obj in obj_files:
    folder_objs.setdefault(rel_dir, []).append(obj)

  lib_files = []
  for folder, objs in folder_objs.items():
    lib_path = BUILD_DIR / folder / (folder.name + ".a")
    need_archive = folder_changed.get(folder, False) or not lib_path.exists()
    if need_archive:
      archive_objects(objs, lib_path)
    lib_files.append(lib_path)

  # Step 3: Link all .a's into final binary
  link_libraries(lib_files, FINAL_BINARY)

  save_include_cache({
    "cache": include_cache,
    "hashes": include_hashes
  })

if __name__ == "__main__":
  if args.rebuild or args.release:
    try: os.remove(BUILD_DIR / ".build_cache.pkl")
    except: pass
    try: os.remove(BUILD_DIR / ".include_cache.pkl")
    except: pass

  if args.docs:
    proc.check_call("cd docs && xelatex design.tex", shell=True)
    print("Docs built.")

  if args.dot:
    proc.check_call("dot -Tpng temp/graph.dot -o temp/graph.png", shell=True)
    print("Dot converted.")
    exit(0)

  if args.mount:
    proc.run(f"sudo mount -o loop build/rootfs/rootfs.ext2 mnt", shell=True)
    exit(0)

  build()
  if args.objdump:
    proc.run(f"riscv64-unknown-elf-objdump -d build/kernel > temp/kernel.s", shell=True)
    exit(0)

  if args.run:
    # -d in_asm -D qemu.log
    asm = "-d in_asm" if args.assembly else ""
    gdb = "-S -s" if args.gdb else ""
    proc.run(
f"""
~/.local/qemu/build/{QEMU} -nographic \
-machine virt -bios default -kernel {BUILD_DIR}/kernel \
-initrd {BUILD_DIR}/initramfs.cpio \
\
-drive file=scripts/rootfs.ext2,if=none,format=raw,id=x0 \
-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
\
-drive file=testsuite/sdcard-rv.img,if=none,format=raw,id=x1 \
-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
\
-device virtio-net-device,netdev=net -netdev user,id=net \
-d guest_errors -D qemu.log \
-rtc base=utc \
{asm} {gdb}
"""
  ,shell=True)
