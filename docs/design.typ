#import "@preview/ilm:1.4.2": *

#set text(font: ("Libertinus Serif", "Noto Serif CJK SC"), lang: "zh", region: "cn")

#show: ilm.with(
  title: [操作系统设计文档],
  author: "黄越",
  date: datetime.today(),
  date-format: "[year].[month].[day]",
  bibliography: bibliography("bibtex.bib"),
  raw-text: (
    custom-font: ("Fira Code")
  ),
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
  if (it.depth == 1) {
    pagebreak();
    // This is a string like "1."
    let number = counter(heading).display(it.numbering);
    chapter(number.slice(0, -1), it.body)
  } else if (it.depth == 2) {
    set text(18pt)
    pad(it, bottom: -12pt)
  } else {
    set text(13pt)
    // Remove numbering.
    pad(it.body, bottom: -12pt)
  }
}

// Generate a fake paragraph, so that the "first" paragraph isn't special.
#show heading: it => {
  it
  par(leading: 1.5em)[#text(size:0.0em)[#h(0.0em)]]
}

#show raw.where(block: true): it => {
  pad(it, bottom: -12pt)
  par(leading: 1.5em)[#text(size:0.0em)[#h(0.0em)]]
}

#show list: it => {
  pad(it, bottom: -12pt)
  par(leading: 1.5em)[#text(size:0.0em)[#h(0.0em)]]
}

#let parindent = 2em;
#set par(first-line-indent: parindent)
#set list(indent: 1em)
#let indent = h(parindent);

= 前言

这是我单人从零开发的操作系统内核。在当前 OS 比赛中，大多数队伍会基于 Rust 的 ArceOS 或是 StarryMix，并利用社区中大量的 no_std crates 编写操作系统。不过我并没有这么选择，而是完全采用 C++ 编写，且不依赖任何第三方代码。

我没有选择 C 或是 Rust，其实是出于个人理由：
- *语言偏好*。C 过于简陋，在处理一些复杂结构时（例如各类容器和 inode 的继承——见@inode），有些力不从心。而 Rust 过于不自由：它不支持各类语法糖，例如自定义的隐式转换和函数重载，而且严格的借用检查会让我感觉在与编译器搏斗。

- *学习路径*。在项目开始前，我并没有开发 OS 的经验。对于初学者而言，现有的 OS 内部实在过于复杂。为了保证我能够真正理解内核的每一个行为，我必须亲手实现每一个模块。

由于 C++ 在没有 std 的情况下缺少可用的库，而且我放弃了基于现成操作系统开发，我必须从最底层开始构建一切：

+ *基础库* (`utils/`)。我实现了一套小型 STL，包括了内核所需的各种容器。它还包含了一部分模板元编程的工具。

+ *锁* (`lock/`)。它实现了常见的 spinlock, mutex 与 condition variable，同时用户态的 futex 也在这里。

+ *调试工具* (`instr/`, @instr)。为了应对单人开发的调试压力，我实现了简单的内存越界检测、double-free 检测和泄漏检测。我还实现了调用栈的记录，任何一个函数都可以任意地复制、存储当前的调用栈，并在合适的时候打印出来。

+ *文件系统* (`fs/`)。由于单人开发的时间紧迫，不可能完全实现文件系统的每个细节；但 ext2, ext4, devfs, tmpfs, procfs 以及 initramfs 都处于可用状态。同时，特殊的管道、socket（和有关的网络通信协议）都属于“文件”，所以它们也在这个文件夹中。

+ *内存* (`mem/`)。我实现了页表和动态内存分配，同时还会处理 lazy-mapping 导致的 page fault。

+ *进程* (`proc/`)。进程有关的 TCB, PCB 与调度都在这里。

+ *中断* (`interrupt/`)。它会处理中断向量与系统调用分发。

+ *硬件驱动* (`driver/`, `fdt/`)。它包含各类 VirtIO 设备的驱动以及 FDT 的读取。

文档接下来的部分将详细解释各个模块的具体实现细节。

= 启动流程

= 基础设施

在编写操作系统时，C++ 的标准库是不可用的。因此，我需要自行编写一些基础设施。

== 容器

我实现了下列容器：

*动态数组* `Oos::vector` 和*字符串* `os::string`。前者可以自动管理长度并自动扩容，而后者可以让我免于关注字符串结尾的 `'\0'` 的位置。由于内核中的字符串通常较短，我也实现了 SSO 优化，使得小于 24 个字节的字符串被存储在栈上而不是堆上。

*链表* `os::list` 和*侵入式链表* `os::intrusive_list`。前者的节点在堆上分配，后者的节点是对象本身。为了使 `intrusive_list` 类型支持不同的对象，我使用了 CRTP，要求所有这种侵入式链表节点 `T` 都继承 `intrusive_list_node<T>`：

```cpp
template<class T>
struct intrusive_list_node {
  T *prev, *next;
};
```

我还使用了 `concept` 来确保这一点，以提早发现模板类型错误（例如不小心将 `T` 写作 `T*`），以及优化编译器的报错信息。

```cpp
template<class T>
concept intrusive_capable = is_base_of<intrusive_list_node<T>, T>::value;

template<intrusive_capable T>
class intrusive_list;
```

从而，在容器内部只需要将 `T` 转化为这个类型，就可以统一地操作了。

*哈希表* `os::hashmap`。它可以自动对任何 POD 类型生成哈希值。对于 `os::string`，它的哈希是单独特化的，基于字符串的内容，而不是这个类的内部数据。

```cpp
template<typename K, typename V, hasher<K> Hash = detail::fnv_1a<K>, comparator<K> Eq = detail::equal<K>>
class hashmap;
```

这些容器内部的内存分配都被 `safe` 标记了，不计入@leak-detect 所提到的内存泄漏检测中。

== 错误处理工具

在内核中，大量的操作都会返回错误。在 C 中，我们可能会这么写：

```c
int ret = vfs->lookup(path, &dentry);
```

而在 C++ 中，我们可以使用 `expected<T, E>`，使得在正确时返回 T，错误时返回 E。这类似于许多语言中的 `Result<T, E>`。

我给它添加了一个到 E 的隐式转换与检测是否错误的 `operator !`，这样就可以方便地向上传递错误了。

```cpp
expected<dentry *, int> ret = vfs->lookup(path);
if (!ret)             // If error happens...
  return ret;         // return error code

dentry *entry = *ret; // Extract value
```

同时，这里的 `expected<T, E>` 支持移动语义以及不可默认构造的类型，比如可以在其中放入 `unique_ptr`。这在内核的很多地方都有应用，比如@interrupt 所提到的 `copy_to_user` 与 `copy_from_user`。

类似地，当不需要 E 时，我也实现了 `optional<T>`。这是因为 C++ 没有零长度的类型，所以无法直接复用 `expected<T, unit>`。

== 锁

=== 自旋锁 <spinlock>

这是一个单核 OS。因此，自旋锁实际上只需要禁止中断即可。我们需要记录当前 `spinlock::acquire()` 与 `release()` 调用次数的差值，以防提前开启中断。

实际上，这个数值还可以方便调试：如果进程在差值不为零的时候进入睡眠，将会触发 panic。这个功能默认关闭，但可以通过构建脚本中的 `--detect-deadlock` 打开。

=== 等待队列 <wait-queue>

= 文件系统

所谓 _virtual_ file system，正应该用 ```cpp virtual``` 函数实现。

在这个 OS 中，所有的文件系统对象（无论是磁盘上的文件、目录，还是设备文件、管道）都是继承自 `inode` 的对象，实现了它的虚函数。

== inode <inode>

inode 中的纯虚函数包含这些内容：

- *基础 IO*。`read` 与 `write` 从文件中读取/写入字节流。

- *目录*。`lookup` 用于路径解析，`create` 和 `unlink` 用来创建/删除文件。

- *metadata*。`get_meta` 和 `set_meta` 管理文件的时间戳。

它还包括一些普通的、具有默认实现的虚函数。

- *等待*。在 poll(2) 中，我们可能需要等待文件变得可读/可写。为此，inode 可以重写 `{prepare,finish}_{read,write}_wait` 这四个函数。它们主要和@wait-queue 所提到的等待队列挂钩。

- *触发器*。

= 调试工具 <instr>

== Shadow Stack

我能够在这个 OS 的任何一个地方获取、存储或是打印当前调用栈。

在 `-O2` 编译选项下，g++ 通常会将栈的基址寄存器 `fp` (也就是 `s0`) 当做一个通用寄存器使用，因此直接通过栈上的信息获取调用栈一般是不可能的。通过 DWARF 的调试信息追踪调用栈是最“正统”的做法，但太过复杂。

我采取了比较简单的实现方式，使用了 g++ 的 `-finstrument-functions` 编译选项。

这时，g++ 会在每个函数入口自动调用 `__cyg_profile_func_enter`，在出口调用 `__cyg_profile_func_exit`，并传入当前函数的起始地址和调用者的地址。那么，只需要在这两个函数中操作下面这个 shadow stack 即可。

这是一个全局变量；这个 OS 是单核的，因此不必担心。当 shadow stack 溢出时，后来的栈帧将会被丢弃。

```cpp
struct shadow_stack {
  void *frames[SHADOW_DEPTH];
  int top;
} extern stack;
```

#indent 在这种实现方式下，复制、存储调用栈也十分简便。

这种插桩的方式确实会产生巨大的性能影响：编译器损失了大量的内联机会，而且增大了寄存器压力。不过，在构建脚本中传入 `--no-instrument`，就可以通过条件编译禁用这个功能。

我们有时会希望在调用栈中剔除一些无关的函数，例如打印这个 shadow stack 的函数。这时，使用 `[[gnu::no_instrument_function]]` 即可阻止 g++ 插桩。我对所有操作这个栈的函数，spinlock，以及硬件中断的处理器都添加了这个属性。

这个 shadow stack 只存储了函数地址，而我们更希望在打印时得知函数的名称。我采用了预处理的方式：构建脚本将会自动生成一个排好序的常量数组。

```cpp
struct symbol {
  uintptr_t addr;
  const char *name;
};
const symbol symbols[] = {
#include "../../build/instr/symtbl.inc"
};
```

这里的 `.inc` 文件是依靠 `nm --demangle` 生成的。`nm` 会自动将地址排序，同时 `--demangle` 可以获取 C++ 标识符，而非 `_ZN2os...` 这种难以阅读的形态。我同时还使用了 Python 简单过滤 `nm` 提供的符号表。

在 assert 失败时，kernel 将会自动打印当前的调用栈。作为一个例子，一个可能的 stack dump 如下：

```
Stack dump:
  #8: 0xffffffc080206af8 (os::intrusive_list<os::tcb_t>::push_back(os::tcb_t*))
  #7: 0xffffffc080226424 (os::scheduler_t::prepare_to_sleep())
  #6: 0xffffffc0802158f8 (os::wait_queue::prepare(os::wait_entry&))
  #5: 0xffffffc080230c1a (os::console_inode::prepare_read_wait(os::wait_entry&))
  #4: 0xffffffc080230f8e (os::tty_inode::prepare_read_wait(os::wait_entry&))
  #3: 0xffffffc0802070e8 ((anonymous namespace)::syshandle(os::trapframe*))
  #2: 0xffffffc08021a49e (os::vma::map_single(void*, unsigned long*))
  #1: 0xffffffc08021aa66 (os::vma::map_current(void*))
  #0: 0xffffffc0802070e8 ((anonymous namespace)::syshandle(os::trapframe*))
kernel panicked: src/main/../mem/../utils/stl/list.h:66: assertion failed: !contains(node)
```

== 内存安全

或许有人会问，在 Rust 盛行的今天，依赖这种简陋的 ASan 是否是一种倒退？我的回答是，我选择了 C++ 无限制的自由，也做好了为每一字节内存负责的准备。C++ 并不代表放弃内存安全，它是一个系统的性质，而不是一个语言的保证。

我通过染色、guard page、重载 ```cpp operator new```等方法来在运行期检测内存安全问题。自然，这会带来性能损失，但与上面的 shadow stack 类似，可以通过在构建脚本中使用 `--no-debug-memory` 取消这部分代码的编译。

=== 染色

在分配物理内存时，我会将这一页置为 0xAA; 在回收物理内存时，我会将这一页置为 0xCC。

C++ 并不默认初始化变量，而这种染色很适合排查未初始化或 use-after-free 所导致的 bug。与上面的调用栈信息结合，当出现这些问题时，我通常看到的信息是：

```
exception: load access fault at 0xaaaaaaaaaaaaafc when executing ...
Stack dump:
  #8: ... (Faulting function)
kernel panicked: exception ocurred in kernel
```

这等同于直接告诉我这个函数中有未初始化的变量。

=== 泄漏检测 <leak-detect>

=== 越界检测

=== 二次释放检测

= 中断处理 <interrupt>
