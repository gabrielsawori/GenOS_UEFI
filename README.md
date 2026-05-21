GenOS v3 (64-Bit)

Welcome to the official repository for GenOS, a custom-built, low-level 64-bit operating system developed from scratch for educational purposes and OS development exploration. This project utilizes the modern Limine bootloader (UEFI/BIOS compatible) and focuses on modularity, code readability, and the implementation of low-level hardware architecture.

🌟 Current Features

GenOS v3 already includes the foundational implementations necessary to run a basic operating system. Here are the features currently implemented:

1. CPU & Interrupt Management

GDT (Global Descriptor Table): Memory segment configuration (Ring 0 / Kernel Mode).

IDT (Interrupt Descriptor Table): System interrupt mapping table.

PIC (Programmable Interrupt Controller): Legacy PIC (8259) remapping to prevent conflicts with CPU exceptions.

ISR (Interrupt Service Routine): Handling for hardware interrupts and CPU exceptions.

2. Memory Management

PMM (Physical Memory Manager): Allocation and management of physical RAM blocks/pages (Bitmap/Stack based).

VMM (Virtual Memory Manager): Paging implementation (4-level paging in x86_64 architecture) for secure virtual memory.

Heap Allocator (kmalloc, kfree): Dynamic memory allocation management within the kernel.

3. External Hardware Drivers

Framebuffer & Visual Text: Custom font rendering library (based on PSF/Bitmap font8x8) capable of printing graphics and text independently to the screen.

Keyboard Driver: PS/2 driver to detect keystrokes (scancode to ASCII character conversion).

Timer (PIT - Programmable Interval Timer): Periodic timer interrupts for scheduling and delays.

Serial Debugging (COM1): Interactive logging via serial port (monitorable via QEMU stdio or PuTTY).

4. Multitasking & User Interface

Basic Tasking System: Structures to run functions concurrently/separately (early multitasking implementation).

Mandor Shell: A graphical terminal shell (Mandor@GenOS:~$) where users can type interactive commands directly on the OS screen!

🚀 Build Instructions & Usage

GenOS is designed to be easily built on Linux-based operating systems (e.g., Ubuntu/Debian/Arch) or WSL (Windows Subsystem for Linux).

Dependencies

Ensure you have the following compilation tools installed in your terminal:

gcc (Compiler)

ld (GNU Linker)

make (Build System)

xorriso (Mandatory utility for assembling and creating a bootable ISO file)

qemu-system-x86_64 (Emulator to run the OS without restarting your physical PC)

git

(For Ubuntu/Debian users, run: sudo apt install build-essential xorriso qemu-system-x86 mtools)

Compilation Steps

Clone This Repository
Open your terminal and run the following commands:

git clone <Your-GenOS-Repository-URL>
cd GenOS_UEFI-main


Prepare the Limine Bootloader
Because GenOS uses the Limine Bootloader, ensure you have downloaded the limine folder into the root directory of this project if it wasn't cloned along with the repository. (Use the command git clone https://github.com/limine-bootloader/limine.git --branch v7.x-branch-latest --depth=1 if the limine folder is empty, then build Limine using make -C limine).

Compile & Build the ISO
To assemble all C and Assembly source code into a bootable OS (GenOS.iso), simply run:

make


(The make system will automatically process all folders like kernel, CPU, memory, and drivers, outputting a ready-to-use file named GenOS.iso)

Run GenOS in QEMU
To immediately test the OS in a virtual machine and see the active shell, use:

make run


(QEMU will open displaying the GenOS interface, while debug output will be printed to your computer's terminal via the Serial StdIo connection)

Clean Build Artifacts
If you modify any code, clean the old compilation files before rebuilding using:

make clean


📜 License & Contribution

This project is licensed under the MIT License. You are free to study, duplicate, modify, and distribute this code. However, please note that the software is provided "AS IS"; the developer is not liable for any unforeseen losses or damages resulting from the use or experimental modification of the code (e.g., data corruption if the OS is installed on a bare-metal SSD/HDD containing important data).
Always use QEMU/VirtualBox for experimentation!