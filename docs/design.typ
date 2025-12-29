#import "@preview/ilm:1.4.2": *
#import "@preview/tiptoe:0.4.0": *

#set text(font: ("Libertinus Serif", "Noto Serif CJK SC"), lang: "zh", region: "cn", features: (calt: 0))

#let bib = (
  
);

#let bibliography = {

let bib = (
  (
    label: "defect2017boot",
    author: "The Defect",
    title: "Boot Process",
    from: "Slay The Spire",
    year: 2017,
    link: "https://sts.huijiwiki.com/wiki/%E5%90%AF%E5%8A%A8%E6%B5%81%E7%A8%8B",
  ),
  (
    label: "kinich2025price",
    author: "Kinich",
    title: "Character Voice: Something to Share",
    from: "Genshin Impact",
    year: 2025,
    link: "\"My people are strong believers in absolute freedom. They think the bond ... should be built on mutual trust and support. Guess I'm the odd one out.\"",
  )
)

for i in range(array.len(bib)) {
  let entry = bib.at(i);
  
  [
    // A special value, examined afterwards.
    // We use a hack here: the length of this string will be the index to look at in the array.
    #heading(depth: 5, numbering: "1" + "." * i, outlined: false)[
      \[#(i+1)\] #entry.author, #strong(entry.title). #emph(entry.from), #entry.year.
      #if ("link" in entry) {
        entry.link; [.]
      }
    ]; #label(entry.label);
  ]
}

};

#show: ilm.with(
  title: [操作系统设计文档],
  author: "黄越",
  date: datetime.today(),
  date-format: "[year].[month].[day]",
  bibliography: none,
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
  } else if (it.depth == 3) {
    set text(13pt)
    // Remove numbering.
    pad(it.body, bottom: -12pt)
  } else if (it.depth == 5) {
    // A special "heading" for reference.
    set text(12pt, weight: "regular");
    pad(par(it.body), bottom: -20pt); "\n";
  } else {
    panic("heading: unknown depth")
  }
}

// Generate a fake paragraph, so that the "first" paragraph isn't special.
#let fakepar = par(leading: 1.5em)[#text(size:0.0em)[#h(0.0em)]];

#show heading: it => {
  it
  if (it.depth <= 3) {
    fakepar
  }
}

#show raw.where(block: true): it => {
  pad(it, bottom: -12pt)
  fakepar
}

#show list: it => {
  pad(it, bottom: -12pt)
  fakepar
}

#let parindent = 2em;
#set par(first-line-indent: parindent)
#set list(indent: 1em)
#let indent = h(parindent);

#show ref: it => {
  let x = it.element;
  if (x == none or x.func() != heading) {
    it
    return;
  }
  let text = counter(heading).at(x.location()).map((x) => str(x)).join(".");
  if (x.depth == 1) {
    [第 #text 章]
  }
  if (x.depth >= 2 and x.depth != 5) {
    [#text 节]
  }
  // The special reference to bibliography.
  if (x.depth == 5) {
    [\[#(it.element.numbering.len())\]];
  }
}

= 前言

这是我单人从零开发的操作系统内核。在当前 OS 比赛中，大多数队伍会基于 Rust 的 ArceOS 或是 StarryMix，并利用社区中大量的 no_std crates 编写操作系统。不过我并没有这么选择，而是完全采用 C++ 编写，且不依赖任何第三方代码。

我没有选择 C 或是 Rust，其实是出于个人理由：
- *语言偏好*。C 过于简陋，在处理一些复杂结构时（例如各类容器和 inode 的继承——见@inode），有些力不从心。而 Rust 过于不自由：它不支持各类语法糖，例如自定义的隐式转换和函数重载，而且严格的借用检查会让我感觉在与编译器搏斗。

- *学习路径*。在项目开始前，我并没有开发 OS 的经验。对于初学者而言，现有的 OS 内部实在过于复杂。为了保证我能够真正理解内核的每一个行为，我必须亲手实现每一个模块。

由于 C++ 在没有 std 的情况下缺少可用的库，我必须从最底层开始构建一切：

+ *基础库* (`utils/`, @infra)。我实现了一套小型 STL，包括了内核所需的各种容器。它还包含了一部分模板元编程的工具。

+ *锁* (`lock/`)。它实现了常见的 spinlock, mutex 与 condition variable，同时用户态的 futex 也在这里。我在@infra 中一并介绍了它。

+ *调试工具* (`instr/`, @instr)。为了应对单人开发的调试压力，我实现了简单的内存越界检测、double-free 检测和泄漏检测。我还实现了调用栈的记录，任何一个函数都可以任意地复制、存储当前的调用栈，并在合适的时候打印出来。

+ *文件系统* (`fs/`, @fs)。由于单人开发的时间紧迫，不可能完全实现文件系统的每个细节；但 ext2, ext4, devfs, tmpfs, procfs 以及 initramfs 都处于可用状态。同时，特殊的管道、socket（和有关的网络通信协议）都属于“文件”，所以它们也在这个文件夹中。

+ *内存* (`mem/`)。我实现了页表和动态内存分配，同时还会通过 lazy-mapping支持 `mmap` 相关操作。

+ *进程* (`proc/`, @process)。ELF 读取、`ld.so` 加载，以及 TCB, PCB 的操作与调度都在这里。

+ *中断* (`interrupt/`)。它会处理中断向量与系统调用分发。

+ *硬件驱动* (`driver/`, `fdt/`)。它包含各类 VirtIO 设备的驱动以及 FDT 的读取。

从工作量的角度来看，#include("data/git_summary.tex");。

文档接下来的部分将详细解释各个模块的具体实现细节。如果没有特殊说明，这些细节默认是 RISC-V 的。

这篇文档也算是我的 OS 学习记录，所以并未直接使用 AI 编写（当然，让 Gemini 给了一些参考意见）。相信我，AI 的遣词造句水平是我望尘莫及的。况且你不能指望 AI 真的能写 Typst 代码：我试过，GPT 一直在胡言乱语。

此外，参考文献中被我放了一些#h(-0.3em)#[#set text(stroke: stroke(paint: luma(85%), thickness: 0.5pt)); #strike(stroke: luma(80%))[其实全部都是]]彩蛋。

= 启动流程

== 内存布局

这个 OS 的内存布局如下：
#{
let width = 150pt;
let height = 35pt;
set rect(stroke: 0.5pt, width: width, height: 35pt);
set align(center);

let frame(mark: "", framed: true, content: none) = {
  let rectangle = rect(stroke: if (not framed) { 0pt } else { 0.5pt })[#set align(center + horizon); #content];
  let moved = it => move(it, dx: width / 2 + 20pt, dy: -height);
  let line = line(toe: stealth, stroke: 0.5pt);
  
  (rectangle, moved(line), place([
    #mark
  ], dx: width + 40pt, dy: -height - 5pt));
}

grid(
..frame(mark: "0x8020'0000", content: [.text.low]),
..frame(mark: "0x8020'1000", content: [pt_root]),
..frame(mark: "0x8020'2000", content: [a0, a1]),
..frame(mark: "0x8020'3000", framed: false, content: [#rotate(90deg)[#set text(size: 2em); =]]),
..frame(mark: "0xffff'ffc0'8020'3000", content: [.text]),
..frame(mark: "0xffff'ffc0'802_'____", content: [.data, .bss]),
..frame(mark: ".", content: [栈]),
..frame(mark: ". + 131072", framed: false),
v(-height)
)
}

#indent 这里显示的地址是*虚拟地址*。实际加载的地址 (_load memory address_, LMA) 是减去 `0xffff'ffc0'0000‘0000` 偏移量后的值：这是为了让 OS 占据高位地址。这里有 38 位可用的地址，而 39-64 位需要全部为 1，这是 Sv39 页表的规定。

== 启动流程 <boot>

最初启动的时候，我还没有设置栈，但需要立即启动虚拟内存。这就是 .text.low 段的作用：它可以在不使用栈的情况下，以 pt_root 作为页表的根，将前 16 GB 内存映射到高位地址。同时，它还会将 0x8000'0000 所在的 1GB 映射到它本身，以免下一条指令直接 page fault。

这里的 a0, a1 是 OpenSBI 提供的 Hart ID 以及 FDT 的地址。我需要将它们先保存在内存中的确定位置，以便设置好栈之后再去阅读。

在映射完成之后，我会跳到 .text.high 执行，并初始化栈。在这之后，C++ 的基本特性就可以使用了。

接下来我会开启内存分配，使得 ```cpp operator new, delete```可以工作。同时，@memsafety 所提到的内存检查工具也开始工作，提供额外的格挡 @defect2017boot，方便后续启动。

接下来，我会初始化这几个模块：

- *FDT*。读取 MMIO 硬件的位置，以及可用物理内存的范围。可以管理较大内存的 bitmap allocator 在这时才被启动；前面的那个是个 free-list allocator，它管理的内存也是操作系统本身镜像的一部分，放在操作系统的栈的后面。

- *硬件驱动*。包括 VirtIO 的硬盘与网卡驱动。

- *文件系统*。首先是 initramfs，然后是 devfs 和 tmpfs。其他文件系统会在 `init` 进程中挂载。

- *启动进程*。启动 `idle` (pid 0),`init` (pid 1) 以及一个 DHCP 进程。实际上，如果在构建脚本中启用 `--unit-test`，那么不会启动进程，而是执行一部分单元测试，然后直接退出。这里的单元测试目前只包括 B-tree 的插入、删除、查找和迭代器操作。

如果没有指定 `--unit-test` 选项，这时，操作系统本体就启动完成了。

接下来，`init` 会挂载 ext2，然后将刚刚挂上的 devfs 和 tmpfs 移动到 ext2 下，再 chroot。这样就没有人能访问 initramfs 了。（启动 `init` 时需要 /dev/console，所以必须先挂载 devfs 才行。）

现在有了完整的 ext2 磁盘，就可以启动 shell 了。这需要先设置好 stdin/stdout/stderr，将它们重定向到 TTY。最后执行 execve，就启动完成了。

= 基础设施 <infra>

在编写操作系统时，C++ 的标准库是不可用的。因此，我需要自行编写一些基础设施。

它们大多依赖动态内存分配。在@boot[]和@memsafety[]两处简略地介绍了这个 OS 中内存分配的方式，所以就不再单独提及。

== 容器 <containers>

我实现了下列容器：

*动态数组* `os::vector` 和*字符串* `os::string`。前者可以自动管理长度并自动扩容，而后者可以让我免于关注字符串结尾的 `'\0'` 的位置。由于内核中的字符串通常较短，我也实现了 SSO 优化，使得小于 24 个字节的字符串被存储在栈上而不是堆上。

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

*B-tree* `os::btree`。它只支持偶数阶。我需要的其实是一个类似 `std::map` 的有序键值对容器。考虑到 `std::map` 使用的是红黑树，而这和 4 阶的 B-tree 是等价的，我选择直接实现一个 B-tree：它可以减少一点内存分配次数，而且缓存命中率更高一些。

B-tree 唯一的缺点就是在修改元素的时候，会导致其他元素的迭代器失效。这通常可以通过重新查找下一个 key 来避免。

我可以在每个用到它的地方指定合适的 `Order`，来优化访问效率：

```cpp
template<class K, class V, int Order> requires(Order % 2 == 0)
class btree;
```

在地址空间一节中，我在处理地址空间的时候使用了特化的 B-tree，具体请见 @addrspace。

*红黑树* 

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

== 同步工具

=== 自旋锁 <spinlock>

这是一个单核 OS。因此，自旋锁实际上只需要禁止中断即可。我们需要记录当前 `spinlock::acquire()` 与 `release()` 调用次数的差值，以防提前开启中断。

实际上，这个数值还可以方便调试：如果进程在差值不为零的时候进入睡眠，将会触发 panic。这个功能默认关闭，但可以通过构建脚本中的 `--detect-deadlock` 打开。

=== 等待队列 <wait-queue>

等待是内核中常见的操作：当线程进行磁盘 IO、进入睡眠、或者调用 wait(2) 的时候，它们都需要将自身加入某个等待队列中，以便后续唤醒。

我们需要避免这样的 lost-wakeup 问题：

#context {
set rect(stroke: none)
set align(center)

let line = line(stroke: stroke(dash: "dashed", paint: blue), length: 10%);
let underline(str) = pad(
  stack(dir: ltr, spacing: 3pt, text(str, baseline: -5pt, stroke: stroke(thickness: 0.5pt, paint: blue)), line),
  left: -100pt);

box(
stack(
  dir:ltr,
  stack(dir: ttb, spacing: 0pt,
    rect()[*进程 A*],
    rect()[加入等待队列],
    underline("preempt"),
    v(40pt),
    underline("resume"),
    rect(inset: 0pt, outset: 0pt)[入睡],
  ),
  h(60pt),
  stack(dir: ttb,
    rect()[*进程 B*],
    pad(rect()[唤醒 A], top: 30pt)
  ),
),
)
}

为了解决这个问题，需要让已经被唤醒过的进程在“入睡”阶段不真正入睡。

换言之，我们可以让“入睡” (`suspend()`) 仅仅只是切换线程的操作，而不会更改线程本身的状态。相对地，“加入等待队列”会让线程切换到 sleeping状态，而“唤醒”则会让线程切换到 ready 状态。这样就可以完全避免 lost-wakeup 了。

但这还不够。我们必须考虑这种情况：


#context {
set rect(stroke: none)
set align(center)

let line = line(stroke: stroke(dash: "dashed", paint: blue), length: 10%);
let underline(str) = pad(
  stack(dir: ltr, spacing: 3pt, text(str, baseline: -5pt, stroke: stroke(thickness: 0.5pt, paint: blue)), line),
  left: -100pt);

box(
stack(
  dir:ltr,
  stack(dir: ttb, spacing: 0pt,
    rect()[*进程 A*],
    rect()[加入等待队列],
    underline("preempt"),
    v(40pt),
    underline("resume"),
    rect(inset: 0pt)[入睡],
  ),
  h(60pt),
  stack(dir: ttb,
    rect()[*进程 B*],
    pad(rect()[唤醒 A], top: 20pt)
  ),
  h(60pt),
  stack(dir: ttb,
    rect()[*进程 C*],
    pad(rect()[唤醒 A], top: 40pt)
  )
),
)
}

这是合法的。例如，A 在 sigtimedwait(2) 处陷入沉睡，但是信号和计时器同时到来，这时 A 就会被唤醒两次。所以，我们必须保证 `wake()` 是*幂等*的，也就是重复操作与一次操作是等效的。

因此，我采用了这样的设计：

```cpp
struct wait_entry : intrusive_list_node<wait_entry> {
  tcb_t *tcb;
  bool queued = false;
};

struct wait_queue {
  spinlock lock;
  os::intrusive_list<wait_entry> q;

  void prepare(wait_entry &entry);
  void finish(wait_entry &entry);
  int wake(int n = 1);
  // More overloads of wake()
};
```

我额外存储了一个 `queue` 的值，来表示它当前是否在队列中。考虑到每个线程的 ksp 都是独立的，`wait_entry` 可以直接在栈上分配。

关于 `tcb_t` 的更多内容，请见@threads。

= 进程 <process>

== 线程与进程 <threads>

为了支持 clone(2)，我需要区分线程和进程。记载进程信息的是 PCB (_process control block_)，而记载线程信息的是 TCB (_thread control block_)。为了避免混淆，我并没有给 TCP 网络协议的控制块单独取名——否则这个也会叫 TCB，而是将字段全部放在了 `tcp_socket_inode` 中（见 @tcp）。

当 clone(2) 的 `SHARE_VM` 存在时，将会创建线程。它们有不同的 TCB，但共享同一个 PCB。当没有这个 flag 的时候，会复制整个 PCB。

线程独有的信息主要有：

- *栈*。`ksp`, `usp`记录内核与用户态的栈的位置。就算是内核进程，这两个栈依然是分开的。对于 `ksp` 记录 trapframe 的具体方式，请见@interrupt。

- *上下文*。进入睡眠时，记录当前 system call 的进度。

- *状态*。`Sleeping`, `Ready` 等用于调度的信息。它们的切换在@wait-queue[]中有所提及。

此外，还有如信号、睡眠时长等各个 system call 专用的信息。

剩下的几乎所有内容，包括页表、uid/gid、VFS、地址空间，都存储在 PCB 内。其中还有更多的和 system call 相关的琐碎内容。

== 线程的一生

=== 创造

第一个用户态的线程（和进程）是直接在启动时创造的。之后的线程都是通过 clone 产生的。

在 clone 时，我们几乎只需要复制所有东西就可以了，只有一个需要额外操作的地方：地址空间。为了避免复制所有已经分配的页，我采取了 copy-on-write: 复制页表，取消写权限，并增加一个 `PTE_COW` flag 即可。这时，还需要给物理页分配器中的引用计数加一，以防二次释放。

在 page fault handler 中，如果遇到 store fault 的同时发现 `PTE_COW` 存在，那么就复制当前页，并让原有页的引用计数减一。

这种机制实际上依赖 Sv39 给的软件可以自由使用的两个 bit。对于 Loongarch 而言，页表是纯软件的，所以其实无所谓；我直接重复使用了 RISC-V 的页表。

=== 执行新程序

对于 execve 而言，我们需要先读取 ELF（或是 shebang），更改地址空间的内容，再往栈上放 argc, argv, envp 和 auxv。

实际上，glibc 需要的 auxv 似乎还挺多的。使用 `LD_SHOW_AUXV=1` 可以显示所有提供给程序的 auxv：在 host OS 上使用可以看到需要哪些，在 guest OS 上使用可以方便调试。

有的程序需要 TLS (_thread local storage_)。TLS 的值实际上完全由用户空间决定，内核并不参与分配，也无需记录。只需要在 clone() 的时候设置好 tp 的值就可以了。

=== 睡眠与醒来

在睡眠时，我们需要记录“当前 system call 的进度”。换言之，当线程醒来时，它会认为“它刚从睡眠函数返回”。

这说明我们只需要保存 callee-saved registers，同时保存睡眠函数的 `ra` 就可以了：这时 `ra` 恰好是返回后的下一条指令。

在醒来时，我们加载这些寄存器，并跳转回 ra，就能继续执行了。

按照这个原理来说，`context_save` 其实是不会返回的，但它“看起来”返回了。这意味着我们不能给它加上 `[[noreturn]]` 属性，否则编译器会做出错误的假设。

=== 终止与死亡

当 exit/exit_group 被调用时，线程就终止了。这时不能直接 ```cpp delete tcb```，而是先释放绝大部分资源，留着 wait4() 调用的时候再 ```cpp delete```。

特别需要注意的是，不能直接释放内核态的栈 `ksp`：我们正在这个栈上。这个留给析构函数删除就好了。

接下来，唤醒 wait4() 中的线程，然后给父进程发个 `SIGCHLD` 信号，最后把所有的子进程都交给 `init` (pid 1)，就可以了。

== 地址空间 <addrspace>

每个进程具有独立的虚拟地址空间。

在加载 ELF 文件时，进程只有 ELF 文件的 PT_LOAD 段、一个堆和一个栈。我们需要知道这些空间的开始与结束地址、读写权限、是否需要从文件中读取，以及从哪读取、最大读取多长（为了避免错误地读入本属于 .bss 段的地方）。

换言之，我们需要的是一个 `vma_t` 结构：
```cpp
struct vma_t {
  uintptr_t begin, end;
  int prot, flags;
  file *backup;
  size_t offset, maxread;
};
```

它被看做是 `backup` 的 owner，因此在构造与析构时会对应地增减这个文件的引用计数。

我们需要支持 mmap/munmap/mprotect 这三个对地址空间的操作、以及 brk 这个专门针对堆的 system call。我们还需要在 page fault 时快速地查找到底应该从哪个 `vma_t` 来 map。因此，我们需要一种合适的数据结构来支持下面这些操作：

- 插入、删除；

- 查找包含地址 `addr` 的一个 `vma_t`；

- 给定一段地址 `[begin, end)`，查找所有重合的 `vma_t`；

- 查找一个长度至少为 `len` 的空隙。

我采取的数据结构是添加了一些内容的 B-tree。常见的 B-tree node 是长这样的：
```cpp
struct node {
  node *ch[Order];    // Children.
  K k[Order - 1];     // Keys.
  V v[Order - 1];     // Values.
  int count = 0;      // Key count; children count is always `count + 1`.
  bool leaf;
};
```

这里的 `Order` 指的是 B-tree 的阶，是一个由模板指定的常量（见@containers）。这存储的是键值对，不过我们只需要存储 `vma_t` 这些值。为了方便起见，我规定对于 ```cpp vma_t v```， 这里的 key 就等于 ```cpp v.begin```。

为了支持三种特殊操作，我们需要加入三个字段：
```cpp
struct node {
  ...
  size_t minstart;    // Min starting point in children.
  size_t maxend;      // Max ending point in children.
  size_t maxgap;      // Max gap in children.
  int count = 0;
  bool leaf;
};
```

在插入和删除过程中，我们可以顺手更新这些值。具体的更新方法是，每当一个节点本身的 `k`、`v` 或 `ch` 有任何变化时，就执行这样的更新：

对 `minstart` 而言，这显然就是 `ch[0]->minstart`，或者对叶子节点来说就是 `k[0]`。

对 `maxend` 来说，我们保证这个 B-tree 中没有重叠的 `vma_t`。这意味着和 `minstart` 类似，我们只需要检查 `ch[count]->maxend` 或者 `v[count-1].end` 就可以了。别忘了 `ch` 的个数总是比 `v` 多一个。

至于 `maxgap` 就有些麻烦了。我们无法轻易地判断到底哪个地方的空隙最大，所以只好每一个都算一遍。空隙也就是上一个 `vma_t` 的结束和下一个 `vma_t` 的开始之差，但*并不总是* `k[i]` 和 `v[i-1].end` 之差：实际上，只有叶子节点才是这样的。它应该是 `ch[i]` 这颗子树的最后一个 `vma_t`，换言之，就是 `ch[i]->maxend`。同理，我们需要比较 `ch[i+1]->minstart` 和 `v[i].end` 之差。

别忘了还有一个可能来源：子树内部的空隙。好在我们不需要递归进入子树，不然插入和删除的复杂度就不再是 O(log n) 了。我们已经有了子树的 `maxgap` 属性，所以直接与当前计算出来的值取最大就行。

这样我们就完成了更新。在 split, merge 等一次操作触及多个节点的时候，按照上面的解释，我们需要先更新靠近叶子的节点，再更新靠近根部的节点。

= 文件系统 <fs>

所谓 _virtual_ file system，正应该用 ```cpp virtual``` 函数实现。

在这个 OS 中，所有的文件系统对象（无论是磁盘上的文件、目录，还是设备文件、管道）都是继承自 `inode` 的对象，实现了它的虚函数。

== inode <inode>

inode 中的纯虚函数包含这些内容：

- *基础 IO*。`read` 与 `write` 从文件中读取/写入字节流。

- *目录*。`lookup` 用于路径解析，`create` 和 `unlink` 用来创建/删除文件。

- *metadata*。`get_meta` 和 `set_meta` 管理文件的时间戳。

它还包括一些普通的、具有默认实现的虚函数。

- *等待*。在 poll(2) 中，我们可能需要等待文件变得可读/可写。为此，inode 可以重写 `{prepare,finish}_{read,write}_wait` 这四个函数。它们主要和@wait-queue 所提到的等待队列挂钩。

- *触发器*。当执行某些操作时，可能需要更新硬盘上的数据，例如 `chmod` 与 `close` 等。这并不是必须实现的操作，因为很多文件系统只存在于内存中。因此，我在 inode 中添加了 `onclose`, `onchmod` 等操作：它们的默认实现是空的，但子类可以自由重写。

为了能够在运行时区分不同的 inode，我们需要为 inode 增加一个“种类”字段。实际上，这就是 C++ 的 RTTI。考虑到我们没有 libstdc++，我利用 CRTP 实现了一个简单的 RTTI：

```cpp
template<class T>
class inode_impl : public inode {
  static uint64_t class_id() {
    static int unique;
    return (uint64_t) &unique;
  }
public:
  inode_impl(/*metadata*/): inode(/*metadata*/, /*rtti=*/ (long) class_id()) {}
  static bool classof(inode *p) {
    return p->rtti == class_id();
  }
};
```

只要每个 inode 的子类 T 都继承 `inode_impl<T>`，它们就能自动获得各异的 RTTI。这和@containers 中所提到的侵入式链表是相似的。

除了上面提到的 metadata 和虚表之外，inode 还维护了引用计数和链接计数。对于硬盘上的文件系统，当引用计数归零的时候，就可以释放 inode；对于内存中的文件系统，释放 inode 就相当于删除，因此只有在引用计数和链接计数都为零时才可以释放。

== 网络

=== UDP <udp>

=== TCP <tcp>

= 调试工具 <instr>

== Shadow Stack <instrument>

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

在这种实现方式下，复制、存储调用栈也十分简便。

这种插桩的方式确实会产生巨大的性能影响：编译器损失了大量的内联机会，减少了叶子函数，而且增大了寄存器压力。不过，在构建脚本中传入 `--no-instrument`，就可以通过条件编译禁用这个功能。

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
  ...
kernel panicked: src/main/../mem/../utils/stl/list.h:66: assertion failed: !contains(node)
```

== 内存安全 <memsafety>

C++ 提供了绝对的自由，但为了让运行稳定，在使用内存时依旧需要一些“契约” @kinich2025price。

我通过染色、guard page、重载 ```cpp operator new```等方法来在运行期检测内存安全问题。自然，这会带来性能损失，但与上面的 shadow stack 类似，可以通过在构建脚本中使用 `--no-debug-memory` 取消这部分代码的编译。

=== 内存分配方式

我的内存分配分为三级：分配物理内存，分配虚拟内存页，以及可以分配任意大小虚拟内存的 `vmalloc()`。

正如@boot[]所说，在启动时，我首先初始化了一个 16MB 的 free-list allocator，然后读取 FDT 并初始化了 128 MB（以 QEMU 的默认设置为例）的 bitmap allocator。

对于虚拟内存页的分配，我也采用 bitmap allocator。

对于任意长度的虚拟内存分配，我采用的是 slab allocator。其中 slab 的大小设置是 8, 16, ..., 256, 以及`rounddown<16>((4096 - sizeof(slab_header)) / n)`, 其中 $1 < n < 7 or n = 12$。这样的大小能让一页恰好可以分配 $n$ 个对象。超出这个范围的对象将会按整页分配。这里的 `rounddown<16>` 是为了对齐要求：这个 OS 中除了按页对齐之外，最大的对齐要求就是 16 字节的。

至于 ```cpp operator new```，它会直接调用 `vmalloc()`。值得注意的是 C++ 会自动考虑 ```cpp operator new[]``` 所附带的数组长度信息，这会加在数组元素所需要的大小上。这无需来自 C++ runtime 的支持。

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

当@instrument 所述的 shadow stack 启用时，我会在每个虚拟页的 alloc/free 函数中记录它们的调用栈，并在合适的位置（手动调整源代码）打印所有未被释放的记录。

我给 ```cpp operator new``` 和```cpp operator new[]```分别添加了两个重载：

```cpp
void *operator new(size_t len, os::permanent_t);
void *operator new(size_t len, os::safe_t);
```

它们的效果是一样的，都会让分配的内存不计入内存泄漏检测中。第一个重载主要用于不会被释放的内容，例如 initramfs 的 inode。第二个重载主要用于容器中：容器会管理好自己的分配和释放，因此是安全的。这样可以减少一些误报。

=== 越界检测

考虑到虚拟内存分配一共有两种方式，我为它们分别添加了越界检测。

对于按页分配的大对象，我会在它的头尾各额外空出一页来，不将它们 map 到任何物理页上。值得注意的是，我并不能 map 但是将权限设为 0，否则这违反了 RISC-V Sv39 页表的要求（权限为 0 的页一定不是页表的叶子）。

在回收时，这两个 guard page 也会一并回收。

对于 slab allocator 中的小对象，在@instrument 中的 shadow stack 启用时，我会在 `__cyg_profile_func_exit` 中检查所有 slab 中每个空位的 `next` 指针是否完好（指向同一页内，并且对齐正确）。如果不正确，那么立即触发 panic，从而可以直接定位越界的函数。

这要求 panic 所依赖的函数全部都标注了 `[[gnu::no_instrument_function]]`，否则将会无限递归。同时，这极其消耗性能，所以只有在启用 `--debug-memory-expensive` 的时候，我才会进行这个昂贵的检查。

=== 二次释放检测

这实际上是免费送的检测。

在实现 copy-on-write 的时候，我们需要增加线程所引用的物理页的引用计数。为了检测二次释放，只需要在释放的时候检测引用计数是否已经为零就可以了。

= 中断处理 <interrupt>

= 后记



= 参考文献

#bibliography
