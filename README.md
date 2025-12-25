# 操作系统内核

## 内核本身的编译

可以通过 `test.py` 来编译这个内核，它生成的最终文件是 `build/kernel.elf`。可以通过 `-r` 来编译并运行。

这个脚本默认运行 RISC-V 的版本，对于 Loongarch，可以使用 `--la`。此外，还可以通过一些命令行参数启动/禁用一些与调试有关的功能，具体可以参考文档。

## 文档编译

可以通过这个命令来生成这个 OS 的完整文档。

```bash
cd docs && ./mkdocs.sh
```

编译文档需要 Typst。你可能需要安装 Noto Serif CJK SC 以及 Fira Mono 这两款字体。
