// ############################################################################
// # Linpmem userspace interface					      #
// ############################################################################

/* For you, the usermode program writer, this is the really important header
 * file. You will find here all needed for doing a proper invocation.
 * Therefore, most documentation and explanation is put here.
 * Include this header file into your usermode program.
 *
 * Contains:
 * > struct definitions
 * > ioctl definitions
 * > documentation on how to use them
 */

#ifndef _LINPMEM_SHARED_H_
#define _LINPMEM_SHARED_H_

#ifndef __KERNEL__
#include <stdint.h>
#include <sys/ioctl.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define LINPMEM_DEVICE_NAME "linpmem"

// ############################################################################
// # Structs								      #
// ############################################################################

/* The driver has one job: reading from whatever address you want.
 *
 * You can read from reserved space, even memory holes. Everything is up to
 * your responsibility; e.g., try not to hit the I/O space accidentally!
 * In addition to this, the driver offers a translation service to
 * translate virtual addresses into physical addresses.
 * And since the CR3 is important, you can also query CR3.
 *
 * === Please zero out your structs before using this driver. ===
 */

/* Access mode types used (in LINPMEM_DATA_TRANSFER struct) when reading from
 * a physical address. Tells the driver if it should read-access in bytes,
 * words, dwords, qwords, or buffer access.
 */
typedef enum _PHYS_ACCESS_MODE {
	PHYS_BYTE_READ = 1,
	PHYS_WORD_READ = 2,
	PHYS_DWORD_READ = 4,
	PHYS_QWORD_READ = 8,
	PHYS_BUFFER_READ = 9
} PHYS_ACCESS_MODE;

/* LINPMEM_DATA_TRANSFER (for physical reading, the main capability):
 * You must provide a physical address, and then choose whether you want
 * a true integer read (1/2/4/8 byte), or a buffer read.
 *
 * Use the access type to specify the access mode.
 * The 1/2/4/8 byte read is returned on out_value. You could try this
 * for accessing mapped io or dma space, if you know the semantics.
 * The buffer read is returned on your own provided buffer, which you
 * must allocate. Convenient reading.
 */
typedef struct _LINPMEM_DATA_TRANSFER {
	// (_IN_) The physical address you want to read from. Mandatory.
	uint64_t phys_address;

	// (_OUT_) The read value. On return, this will contain either the
	// read byte, word, dword or qword; or zero on error.
	uint64_t out_value;
	// If you hit the page boundary by your n-byte integer read, you will
	// get zero.
	// Example: you want to read from: 0x123ffe.
	//          You specify a 2 byte read.
	//          This will fail. With 0xffe, you are too near the page
	//          boundary.

	// (_INOUT_) For buffer access mode. The usermode program must provide
	// the buffer!
	void *readbuffer;

	// (_INOUT_)  The usermode buffer size, as told by the  usermode
	// program.
	uint64_t readbuffer_size;
	// On return --PHYS_BUFFER_READ only--, this contains the number of
	// bytes that could be read.
	// Ideally, this number is identical to original input size, but
	// it will be less when a page boundary is encountered.
	// Example: you want to read from: 0x123aaa.
	//          You want to read: 0xf00 bytes.
	//          Maximum the driver will (currently) read:
	//          	0x1000 - 0xaaa = 0x556 bytes.
	//
	// In future there might be an option to force-ignore page boundary.
	// (In other words, you can provide a buffer as large as you want.)
	// However, reading from a physical address you got from translating a
	// virtual address and *then* ignoring the page boundary is most
	// certainly not what you want!
	// On the other side, being able to force ignore page boundary for
	// reading from contiguous memory (such as acpi tables, for instance)
	// might really come in handy.
	// A reserved field might be used in future for a
	// "forceIgnorePageBoundary" flag.

	// (_IN_)  access mode types: byte, word, dword, qword, buffer
	uint8_t access_type;
	// List of possible access types: see PHYS_ACCESS_MODE enum (above).
	// Remarks:
	// > If 0, your request gets rejected, because you apparently forgot
	//   to set an access type. ;-)
	// > If access type doesn't match any known, your request gets
	//   rejected, too.

	// Unused. 
	uint8_t write_access;

	// Every good struct has minimum one!
	uint8_t reserved1;
	uint8_t reserved2;
} LINPMEM_DATA_TRANSFER, *PLINPMEM_DATA_TRANSFER;

/* LINPMEM_VTOP_INFO: Use this struct for an ioctl invocation of
 * type "IOCTL_LINPMEM_VTOP_TRANSLATION_SERVICE" to the driver.
 * vtop returns the physical address from a virtual address.
 */
typedef struct _LINPMEM_VTOP_INFO {
	// (_IN_) The virtual address in question.
	uint64_t virt_address;

	// (_IN_OPT_) Optional: specify a custom CR3 (of a foreign process) to
	// be used in the translation service.
	uint64_t associated_cr3;
	// Leave this to zero if you do not want a foreign CR3 to be used!
	// You can specify a foreign CR3 that belongs to another (valid)
	// process context.
	// E.g., you are Alice, and you want Bob's CR3 to be used, to read
	// something from Bob's userspace. As Alice, previously to using vtop,
	// you would have done a CR3 query invocation, so that you have Bob's
	// CR3 by now. Also, Bob must still live, of course. :-D
	// Beware, this value is used if nonzero!

	// (_OUT_) returns the physical address you wanted.
	uint64_t phys_address;

	// (_OUT_) returns the PTE virtual address, too.
	void *ppte;
} LINPMEM_VTOP_INFO, *PLINPMEM_VTOP_INFO;

/* LINPMEM_CR3_INFO: Use this struct for an ioctl invocation of type
 * "IOCTL_LINPMEM_QUERY_CR3" to the driver.
 */
typedef struct _LINPMEM_CR3_INFO {
	// (_IN_) A (foreign) process (pid_t) from which you want the CR3.
	uint64_t target_process;

	// (_OUT_) returned CR3 value.
	uint64_t result_cr3;
} LINPMEM_CR3_INFO, *PLINPMEM_CR3_INFO;

// ############################################################################
// # Possible Linpmem invocations					      #
// ############################################################################

// read bytes from physical address.
#define IOCTL_LINPMEM_READ_PHYSADDR _IOWR('a', 'a', LINPMEM_DATA_TRANSFER)

// The classical vtop operation: translates virtual address to physical
// address. Optionally, a foreign CR3 can be specified to translate a 
// virtual address from *another* usermode process to a physical page.
#define IOCTL_LINPMEM_VTOP_TRANSLATION_SERVICE \
	_IOWR('a', 'b', LINPMEM_VTOP_INFO)

// A service to return the CR3 of a foreign process (e.g., for use in vtop). 
#define IOCTL_LINPMEM_QUERY_CR3 _IOWR('a', 'c', LINPMEM_CR3_INFO)

#endif
