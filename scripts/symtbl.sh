#!/bin/zsh
out=build/instr/symtbl.inc

if [[ ! -f build/kernel ]]; then
  # Create an empty file for the compilation to proceed.
  echo "" > $out
  exit 0
fi

riscv64-unknown-elf-nm --demangle -n build/kernel | python -c '
import fileinput
for line in fileinput.input():
  if line.strip() == "":
    continue
  parts = line.split(" ")
  if parts[1] not in ("T", "t", "W", "w"):
    continue
  addr = parts[0]
  name = " ".join(parts[2:]).strip()
  print("  { " + f"0x{addr}, \"{name}\"" + " },")
  ' > $out
