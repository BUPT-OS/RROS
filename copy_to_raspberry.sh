#! /bin/bash
make LLVM=1 -j80 Image.gz modules dtbs&> /dev/null
if [ $? -ne 0 ]; then
    echo "make kernel failed"
    exit 1
else
    echo "make kernel success"
fi
cp arch/arm64/boot/Image ../Image
mkdir ../boot_dtb
cp arch/arm64/boot/dts/broadcom/*.dtb ../boot_dtb

make LLVM=1 -j80 INSTALL_MOD_PATH=../modules modules_install &> /dev/null
if [ $? -ne 0 ]; then
    echo "make module install failed"
    exit 1
else
    echo "make module install success"
fi
cd ..
tar jcvf modules.tar.bz2 modules/
tar jcvf dtb.tar.bz2 boot_dtb/
sudo rm -r boot_dtb/
sudo rm -r modules/

# sudo cp Image /boot/rros.img
# sudo du -h /boot/rros.img
# scp Image rtos@169.254.117.1:/home/rtos/kernel/Imag
# scp dtb.tar.bz2 rtos@169.254.117.1:/home/rtos/kernel/dtb.tar.bz2
# scp modules.tar.bz2 rtos@169.254.117.1:/home/rtos/kernel/modules.tar.bz2
