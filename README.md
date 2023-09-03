# Linpmem -- a physical memory acquisition tool for Linux

![alt text](figures/linpmem_with_eye.png "LinPmem -- a physical memory acquisition tool (For Linux)")

##### Linpmem is a Linux x64-only tool for reading physical memory.

Like its Windows counterpart, [Winpmem](https://github.com/Velocidex/WinPmem), this is not a traditional memory dumper. Linpmem offers an API for reading from *any* physical address, including *reserved memory* and *memory holes*, but it can also be used for normal memory dumping. Furthermore, the driver offers a variety of access modes to read physical memory, such as byte, word, dword, qword, and buffer access mode, where buffer access mode is appropriate in most standard cases. If reading requires an aligned byte/word/dword/qword read, Linpmem will do precisely that. 

Currently, the Linpmem features:

1. Read from physical address (access mode byte, word, dword, qword, or buffer)
2. CR3 info service (specify target process by pid)
3. Virtual to physical address translation service

Cache Control is to be added in future for support of the specialized read access modes.

## Summary table

* [Building The Kernel Driver](#building-the-kernel-driver)
* [Loading The Driver](#loading-the-driver)
* [Usage](#usage)
    * [Demo Code](#demo-code)
    * [CLI tool](#command-line-interface-tool)
    * [Library](#libraries)
    * [Memdumping tool](#memdumping-tool)
* [Library](#libraries)
* [Tested Linux Distributions](#tested-linux-distributions)
* [Handling Secure Boot](#handling-secure-boot)
* [Known Issues](#known-issues)
* [Under Work](#under-work)
* [Future Work](#future-work)
* [Acknowledgements](#acknowledgements)


## Building the kernel driver

*At least for now*, you must compile the Linpmem driver yourself. A method to load a precompiled Linpmem driver on other Linux systems is currently under work, but not finished yet. That said, compiling the Linpmem driver is not difficult, basically it's executing 'make'.

### Step 1 - getting the right headers

You need `make` and a C compiler. (We recommend gcc, but clang should work as well).

Make sure that you have the `linux-headers` installed (using whatever package manager your target linux distro has). The exact package name may vary on your distribution. 
A quick (distro-independent) way to check if you have the package installed:

```
ls -l /usr/lib/modules/`uname -r`/
```

That's it, you can proceed to step 2.

**Foreign system:** *Currently*, if you want to compile the driver for _another_ system, e.g., because you want to create a memory dump but can't compile on the target, you have to download the header package directly from the package repositories of that system's Linux distribution. Double-check that the package version *exactly* matches the release and kernel version running on the foreign system. In case the other system is using a self-compiled kernel you have to obtain a copy of that kernel's build directory. Then, place the location of either directory in the `KDIR` environment variable.

```
export KDIR=path/to/extracted/header/package/or/kernel/root
```

### Step 2 - make

Compiling the driver is simple, just type:

```
make
```

This should produce `linpmem.ko` in the current working directory.

You might want to check `precompiler.h` before and chose whether to compile for release or debug (e.g., with debug printing). There aren't much other precompiler settings right now.

## Loading The Driver

The linpmem.ko module can be loaded by using `insmod path-to-linpmem.ko`, and unloaded with `rmmod path-to-linpmem.ko`. (This will load the driver only for this uptime.) If you compiled for debug, also take a look at dmesg.

After loading, for talking to the driver, you need to create the device:

``` 
mknod /dev/linpmem c 42 0
``` 

If you can't talk to the driver, potentially check in dmesg log to verify that '42' was indeed the registered major:

``` 
[12827.900168] linpmem: registered chrdev with major 42
``` 

Though usually the kernel would try to really assign this number.

You can use `chown` on the device to give it to your user, if you do not want to have a root console open all the time. (Or just keep using it in a root console.)

* Watch dmesg output. Please report errors if you see any!
* Warning: if there is a dmesg error print from Linpmem telling to reboot, better do it immediately.
* Warning: this is an early version.

## Usage

### Demo Code

There is an example code demonstrating and explaining (in detail) how to interact with the driver. The user-space API reference can furthermore be found in `./userspace_interface/linpmem_shared.h`.

1. cd demo
2. gcc -o test test.c
3. (sudo) ./test  // <= you need sudo if you did not use chown on the device.

This code is important, if you want to understand how to directly interact with the driver instead of using a [library](#libraries). It can also be used as a short function test. 

### Command Line Interface Tool

There is an (optional) basic command line interface tool to Linpmem, the *pmem CLI tool*. It can be found here: [https://github.com/vobst/linpmem-cli](https://github.com/vobst/linpmem-cli). Aside from the source code, there is also a precompiled CLI tool as well as the precompiled static library and headers that can be found [here](https://github.com/vobst/linpmem-cli/releases/) (signed). Note: this is a preliminary version, be sure to check for updates, as many additions and enhancements will follow soon. 

The pmem CLI tool can be used for testing the various functions of Linpmem in a (relatively) safe and convenient manner. Linpmem can also be loaded by this tool instead of using insmod/rmmod, with some extra options in future. This also has the advantage that pmem auto-creates the right device for you for immediate use. It is extremely portable and runs on any Linux system (and, in fact, has been tested even on a Linux 2.6).

```
$ ./pmem -h
Command-line client for the linpmem driver

Usage: pmem [OPTIONS] [COMMAND]

Commands:
  insmod  Load the linpmem driver
  help    Print this message or the help of the given subcommand(s)

Options:
  -a, --address <ADDRESS>            Address for physical read operations
  -v, --virt-address <VIRT_ADDRESS>  Translate address in target process' address space (default: current process)
  -s, --size <SIZE>                  Size of buffer read operations
  -m, --mode <MODE>                  Access mode for read operations [possible values: byte, word, dword, qword, buffer]
  -p, --pid <PID>                    Target process for cr3 info and virtual-to-physical translations
      --cr3                          Query cr3 value of target process (default: current process)
      --verbose                      Display debug output
  -h, --help                         Print help (see more with '--help')
  -V, --version                      Print version
```

If you want to compile the cli tool yourself, change to its directory and follow the instructions in the (cli) Readme to build it. Otherwise, just download the prebuilt program, it should work on *any* Linux. To load the kernel driver with the cli tool:

```
# pmem insmod path/to/linpmem.ko
```

The advantage of using the pmem tool to load the driver is that you do not have to create the device file yourself, and it will offer (on next releases) to choose who owns the linpmem device.

### Libraries

The [pmem command line interface](#command-line-interface-tool) is only a thin wrapper around a small Rust library that exposes an API for interfacing with the driver. More advanced users can also use this library. The library is automatically compiled (as static portable library) along with the pmem cli tool when compiling from [https://github.com/vobst/linpmem-cli](https://github.com/vobst/linpmem-cli), but also included (precompiled) [here](https://github.com/vobst/linpmem-cli/releases/) (signed). Note: this is a preliminary version, more to follow soon.

If you do not want to use the usermode library and prefer to interface with the driver directly on your own, you can find its user-space API/interface and documentation in `./userspace_interface/linpmem_shared.h`. We also provide example code in `demo/test.c` that explains how to use the driver directly. 

### Memdumping tool

Not implemented yet.


## Tested Linux Distributions

* Debian, self-compiled 6.4.X, Qemu/KVM, not paravirtualized.
    * PTI: off/on
* Debian 12, Qemu/KVM, fully paravirtualized.
    * PTI: on
* Ubuntu server, Qemu/KVM, not paravirtualized.
    * PTI: on
* Fedora 38, Qemu/KVM, fully paravirtualized.
    * PTI: on
* Baremetal Linux test, AMI BIOS: Linux 6.4.4
    * PTI: on
* Baremetal Linux test, HP: Linux 6.4.4
    * PTI: on
* Baremetal, Arch[-hardened], Dell BIOS, Linux 6.4.X
* Baremetal, Debian, 6.1.X
* Baremetal, Ubuntu 20.04 with Secure Boot on. Works, but [sign](#handling-secure-boot) driver first.
* Baremetal, Ubuntu 22.04, Linux 6.2.X

## Handling Secure Boot

If the system reports the following error message when loading the module, it might be because of secure boot:
```
$ sudo insmod linpmem.ko
insmod: ERROR: could not insert module linpmem.ko: Operation not permitted
```
There are different ways to still load the module. The obvious one is to disable secure boot in your UEFI settings.

If your distribution supports it, a more elegant solution would be to sign the module before using it.
This can be done using the following steps (tested on Ubuntu 20.04).
1. Install mokutil:
   ```
   $ sudo apt install mokutil
   ```
2. Create the singing key material:
   ```
   $ openssl req -new -newkey rsa:4096 -keyout mok-signing.key -out mok-signing.crt -outform DER -days 365 -nodes -subj "/CN=Some descriptive name/"
   ```
   Make sure to adjust the options to your needs. Especially, consider the key length (-newkey), the validity (-days), the option to set a key pass phrase (-nodes; leave it out, if you want to set a pass phrase), and the common name to include into the certificate (-subj).
3. Register the new MOK:
   ```
   $ sudo mokutil --import mok-signing.crt
   ```
   You will be asked for a password, which is required in the following step. Consider using a password, which you can type on a US keyboard layout.
4. Reboot the system.
  It will enter a MOK enrollment menu. Follow the instructions to enroll your new key.
5. Sign the module
   Once the MOK is enrolled, you can sign your module.
   ```
   $ /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 path/to/mok-singing/MOK.key path/to//MOK.cert path/to/linpmem.ko
   ```
After that, you should be able to load the module.

Note that from a forensic-readiness perspective, you should prepare a signed module **before** you need it, as the system will reboot twice during the process described above, destroying most of your volatile data in memory.

## Known Issues

* Huge page read is not implemented. Linpmem recognizes a huge page and rejects the read, for now.
* Reading from mapped io and DMA space will be done with CPU caching enabled.
* No locks are taken during the page table walk. This might lead to funny results when concurrent modifications are going on. This is a general and (mostly unsolvable) problem of live RAM reading, without halting the entire OS to full stop.
* Secure Boot (Ubuntu): please [sign](#handling-secure-boot) your driver prior to using.
* Any CPU-powered memory encryption, e.g., AMD SME, Intel SGX/TDX, ...
* Pluton chips?

(Please report potential issues if you encounter anything.)

## Under work

* Loading precompiled driver on any Linux.
* Processor cache control. Example: for uncached reading of mapped I/O and DMA space.

## Future work

* Arm/Mips support. (far future work)
* Legacy kernels (such as 2.6), unix-based kernels

## Acknowledgements

[Linpmem](https://github.com/Velocidex/Linpmem), as well as [Winpmem](https://github.com/Velocidex/WinPmem), would not exist without the work of our predecessors of the (now retired) REKALL project: https://github.com/google/rekall. 

* We would like to thank Mike Cohen and Johannes Stüttgen for their pioneer work and open source contribution on PTE remapping, a technique which is still in use 10 years later.

Our open source contributors:

* Viviane Zwanger
* Valentin Obst
