# Updates and changes

11. May 2024

There was a serious issue in Linpmem where in the middle of PTE remapping, Linpmem could get scheduled off the CPU processor and later being re-scheduled on another CPU core along with another CPU cache. This definitely made trouble on Linux in the sense of reading wrong data. This has been fixed. (see commit log).

1. Sept 2023:

* Many safety enhancements. Most important, this driver is now thread-safe. It can be called simultaneously from multiple processes, while returning the correct results to each. Also, better deadlock prevention and generally safety and sanity checks.
* Now not only can a foreign CR3 be specified in VTOP, but you can also rely on the CR3 info service from Linpmem to acquire said CR3 from any running foreign process.
* There is a basic Linpmem shell (Linpmem-cli) you can use now. It implements all basic functionalities from Linpmem and can also be used as library. 


25. July 2023:

* Enabled using a foreign CR3 in the VTOP translation service. Leave it to zero for default CR3. (See linpmem_shared.h, struct `LINPMEM_VTOP_INFO`, field `associated_cr3`). Use at your own risk!
* Foreign CR3's can currently be acquired by inserting a thread into another process in gentlemen agreement, and calling Linpmem for CR3 query from there. There is no extra service.
* Made precompiler switches consistent. Commenting a precompiler switch always disables it.

25. July 2023:

* Initial commit
