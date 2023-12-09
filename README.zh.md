# RROS

[![Documentation](https://img.shields.io/badge/view-docs-blue)](https://bupt-os.github.io/website/docs/guides/example-guide/)
<!-- [![ci tests](https://github.com/rust-real-time-os/RTOS/actions/ci.yml/badge.svg)](TODO: add a new CI in this repo) -->
[![Zulip chat](https://img.shields.io/badge/chat-on%20zulip-brightgreen)](https://rros.zulipchat.com/)
![RROS](https://img.shields.io/badge/RROS-0.0.1-orange)
[![zh](https://img.shields.io/badge/lang-zh-yellow.svg)](https://github.com/BUPT-OS/RROS/blob/master/README.zh.md)
[![en](https://img.shields.io/badge/lang-en-yellow.svg)](https://github.com/BUPT-OS/RROS/blob/master/README.md)

RROS（Rust实时操作系统）是一个双内核操作系统，由实时内核（使用Rust编写）和通用内核（Linux）组成。 RROS几乎可以兼容所有的Linux程序，并提供比RT Linux更好的实时性能。RROS目前正在作为在轨卫星载荷的操作系统进行实验[“天算星座”项目](http://www.tiansuan.org.cn/)。


你可以在这里找到我们的[架构图](https://bupt-os.github.io/website/architecture.png)

## 为什么选择RROS：

RROS主要用于卫星（星务计算机、卫星载荷等）。其主要动机是现在的卫星不仅用于传统的主要实时任务（如激光通信和姿态定位），还需要成熟、复杂的软件支持的通用任务，如大数据和机器/深度学习任务。这促使了RROS双内核架构的诞生。RROS的实时内核完全使用Rust实现，以提供更好的安全性和鲁棒性。 当然，RROS也可以在更广泛的场景下使用，如自动驾驶、物联网、工业控制等。

RROS的优势包括：

* **硬实时**：
相较RT-Linux等软实时操作系统，RROS提供了硬实时能力，能够满足大多数场景的实时需求。通过其高效的任务调度程序，可以快速响应外部事件，减少任务切换和处理的延迟。
* **兼容性**：
RROS几乎与所有Linux程序兼容，允许Linux应用程序平滑迁移到RROS，如TensorFlow、Kubernetes等。 另外，您可以修改通用Linux程序以提供一定程度的实时能力。
* **易于使用**：
在RROS上便于编写和调试实时程序。在用户态，RROS使用libevl作为实时API系统库，允许您使用gdb等工具进行debug，就像处理标准Linux程序一样。 在内核态，您可以使用qemu，和kgdb的工具检查实时内核中的操作，也可以利用kgdb和jtag进行实板调试。
* **鲁棒性**：
RROS的实时内核是用Rust精心编写的，安全性好和鲁棒性强，在内存和并发问题上更安全。
* **运行时灵活性**：
RROS允许在多核处理器上灵活地调度任务。 根据需求和工作量，将不同的核分派给实时任务或通用任务。

## 快速开始

在Linux的类Debian的发行版上（如ubuntu），可以执行以下操作：

1. 克隆RROS代码:
    ```bash
    git clone https://github.com/rust-real-time-os/RTOS.git
    ```
2. 安装Rust工具链：

    ```bash
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
    ```
    切换到beta-2021-06-23-x86_64-unknown-linux-gnu版本。目前，我们只支持这个编译器版本。

    ```bash
    rustup toolchain install beta-2021-06-23-x86_64-unknown-linux-gnu
    ```

    为此项目设置Rust工具链：

    ```bash
    cd RTOS
    rustup override set beta-2021-06-23-x86_64-unknown-linux-gnu
    ```

    添加Rust的`no-std`组件。

    ```bash
    rustup component add rust-src
    ```
  
3. 选择编译选项

    创建默认配置：

    ```bash
    export CROSS_COMPILE=aarch64-linux-gnu-
    export ARCH=arm64

    make LLVM=1 defconfig
    make LLVM=1 menuconfig
    ```

    选择以下选项：

    ```
    General Setup --->  Rust Support
    Kernel Features ---> Bupt real-time core
    ```

    您可能需要取消选项`Module versioning support`以启用`Rust support`：

    ```
    Enable loadable module support ---> Module versioning support
    ```

4. 编译内核

    ```bash
    make LLVM=1 -j
    ```

    如果您想在Raspiberry PI 4上启动，请额外生成dtbs和modules。

    ```bash
    export INSTALL_MOD_PATH=/path/to/mod
    export INSTALL_DTBS_PATH=/path/to/dtbs
    make modules_install dtbs_install -j
    ```

    将`broadcom`，`lib`，`overlays`和`Image`移动到SD卡的引导分区。

5. 在模拟器上运行

    您需要一个文件系统来在QEMU上引导内核。

    这是一个在qemu上运行的示例：

    ```bash
    qemu-system-aarch64 -nographic  \
          -kernel Image \
          -drive file=ubuntu-20.04-server-cloudimg-arm64.img \
          -drive file=cloud_init.img,format=raw \
          -initrd ubuntu-20.04-server-cloudimg-arm64-initrd-generic \
          -machine virt-2.12,accel=kvm \
          -cpu host  -enable-kvm \
          -append "root=/dev/vda1 console=ttyAMA0"  \
          -device virtio-scsi-device \
          -smp 4 \
          -m 4096
    ```

## 文档

详细信息可以参考我们的[文档](https://bupt-os.github.io/website/docs/)。

## 贡献

如果想要贡献代码，可以参考[这里](https://bupt-os.github.io/website/docs/contributing/contributing/)。

## 路线图

查看[这里](https://bupt-os.github.io/website/docs/roadmap/roadmap/)了解我们未来的路线图

## 联系我们

- 论坛：您还可以在[Zulip](https://rros.zulipchat.com/)上与我们联系。
- 电子邮件：我们的电子邮箱是`buptrros AT gmail.com`。

## 发布

RROS依赖于dovetail和RFL，但是它们目前不再给出补丁。而将它们中的一个高频率下地回合到另一个是非常困难的。 因此，RROS目前固定在5.13版本的Linux内核上，它是在linux-dovetail-v5.13的基础上构建的，并且很容易打RFL patch v1的补丁。 幸运的是，RFL正在快速合并到Linux内核主线中。我们计划在我们所依赖的大多数RFL API被合并到linux-dovetail的主线后开始以固定频率发布版本。 届时，我们将一并选择LTS版本。

## 团队

查看[这里](https://bupt-os.github.io/website/docs/team/team/)了解我们的团队

## 致谢

在开发过程中，我们从以下项目/资源中收获颇丰，此处予以感谢。
- [linux-evl/xenomai 4](https://evlproject.org/overview/)：我们从evl内核学到了如何实现双内核，并使用dovetail进行中断虚拟化的工作并对接[libevl](https://source.denx.de/Xenomai/xenomai4/libevl)作为用户库。此外感谢[Philippe](https://source.denx.de/PhilippeGerum)的杰出工作和在riot中的耐心答疑！
- [Rust For Linux](https://rust-for-linux.com/)：我们使用RFL在Linux中编写RROS，为构建安全抽象层提出了很多问题。感谢RFL社区的ojeda、Wedson、Alex、boqun、Gary、Björn等人耐心地帮助我们。希望我们的工作能为RFL贡献更多的安全抽象层！
- [王顺刚Muduo](https://www.cnblogs.com/wsg1100/)老师：他的博客为xenomai/evl项目提供了详细的讲解。感谢他的无私分享。

## 许可证

RROS的源代码采用的许可证是GPL-2.0。
