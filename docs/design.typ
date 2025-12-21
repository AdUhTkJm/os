#import "@preview/ilm:1.4.2": *

#set text(font: ("Libertinus Serif", "Noto Serif CJK SC"), lang: "zh", region: "cn")

#show: ilm.with(
  title: [操作系统设计文档],
  author: "黄越",
  date: datetime.today(),
  date-format: "[year].[month].[day]",
  bibliography: bibliography("bibtex.bib"),
  figure-index: (enabled: true),
  table-index: (enabled: true),
  listing-index: (enabled: true)
)

#let chapter(id, title) = {
  set align(right);
  // Compensate for the inner -48pt, so the next paragraph starts at the correct place.
  stack(dir: ttb, spacing: 48pt, 
    stack(dir: ttb, spacing: -48pt, [
      #set text(120pt, fill: color.gray, weight: "regular");
      #id
    ], [
      #set text(32pt, weight: "bold"); #title
    ]),
  [])
}

#set heading(numbering: "1.")
#show heading: it => {
  pagebreak();
  // This is a string like "1."
  let number = counter(heading).display(it.numbering);
  chapter(number.slice(0, -1), it.body)
}

// Generate a fake paragraph, so that the "first" paragraph isn't special.
#show heading: it =>  {
  it
  par(leading: 1.5em)[#text(size:0.0em)[#h(0.0em)]]
}

#set par(first-line-indent: 2em)
#set list(indent: 1em)

= 前言

这是一个由单人、从零开始开发的操作系统内核。在当前 OS 比赛中，大多数队伍会基于 Rust 的 ArceOS 或是 StarryMix，并利用社区中大量的 no_std crates 编写操作系统。不过我并没有这么选择，而是完全采用 C++ 编写，且不依赖任何第三方代码。

我没有选择 C 或是 Rust，其实是出于个人理由：
- *语言偏好*。C 过于简陋，在处理一些复杂结构时（例如各类容器和 inode 的继承——见@inode），有些力不从心。而 Rust 过于不自由：它不支持各类语法糖，例如自定义的隐式转换和函数重载，而且严格的借用检查会让我感觉在与编译器搏斗。

- *学习路径*。在项目开始前，我并没有开发 OS 的经验。对于初学者而言，现有的 OS 内部实在过于复杂。为了保证我能够真正理解内核的每一个行为，我必须禽兽实现每一个模块。

#h(2em) 由于 C++ 在没有 std 的情况下缺少可用的库，且我放弃了基于现成操作系统开发，我必须从最底层开始构建一切：

+ *基础库* (`utils/`)。我实现了一套小型 STL，包括了内核所需的各种容器。它还包含了一部分模板元编程的工具。

+ *锁* (`lock/`)。它实现了常见的 spinlock, mutex 与 condition variable，同时用户态的 futex 也在这里。

+ *调试工具* (`instr/`)。为了应对单人开发的调试压力，我实现了简单的内存越界检测、double-free 检测和泄漏检测。我还实现了调用栈的记录，任何一个函数都可以任意地复制、存储当前的调用栈，并在合适的时候打印出来。

+ *文件系统* (`fs/`)。由于单人开发的时间紧迫，不可能完全实现文件系统的每个细节；但 ext2, ext4, devfs, tmpfs, procfs 以及 initramfs 都处于可用状态。同时，特殊的管道、socket（和有关的网络通信协议）都属于“文件”，所以它们也在这个文件夹中。

+ *内存* (`mem/`)。我实现了页表和动态内存分配，同时还会处理 lazy-mapping 导致的 page fault。

+ *进程* (`proc/`)。进程有关的 TCB, PCB 与调度都在这里。

+ *中断* (`interrupt/`)。它会处理中断向量与系统调用分发。

+ *硬件驱动* (`driver/`, `fdt/`)。它包含各类 VirtIO 设备的驱动以及 FDT 的读取。

文档接下来的部分将深入探讨各个模块的具体实现细节。

= 启动流程

= 文件系统

== inode <inode>
