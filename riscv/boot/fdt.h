/*
 * Minimal read-only flattened-device-tree reader.
 *
 * Written because the two DTB consumers that existed could not read a property:
 * irq.c did a raw substring scan over the whole blob (so it could tell that the
 * string "riscv,plic0" appeared somewhere, but not which node it belonged to or
 * what its `reg` was), and runtime.c had a private one-shot walker hardcoded to
 * find `timebase-frequency`. Every MMIO base, interrupt context number and hart
 * id in the tree was therefore a hardcoded QEMU-virt convention.
 *
 * Node handles are byte offsets from the start of the struct block; -1 is the
 * invalid handle. Nothing here allocates.
 */
#ifndef DHEWM3_RV_FDT_H
#define DHEWM3_RV_FDT_H

#include <stdint.h>

#define FDT_INVALID (-1)

/* True once boot_dtb points at a blob with a valid header. */
int         fdt_ok(void);

/* Root node, or FDT_INVALID. */
int         fdt_root(void);

/* Depth-first successor of `node`. Pass FDT_INVALID to start at the root.
 * Returns FDT_INVALID at the end of the tree. */
int         fdt_next_node(int node);

/* Unit name of `node` ("cpu@3", "plic@c000000"), or "" for the root. */
const char *fdt_node_name(int node);

/* Property lookup on one node. Returns a pointer into the blob (big-endian
 * payload, NOT byte-swapped) and writes its length, or NULL. */
const void *fdt_getprop(int node, const char *name, uint32_t *len);

/* 1 if `node`'s `compatible` string list contains `compat`. */
int         fdt_node_is_compatible(int node, const char *compat);

/* First node at or after `from` whose compatible list contains `compat`.
 * Pass FDT_INVALID for `from` to search the whole tree. */
int         fdt_find_compatible(int from, const char *compat);

/* `node`'s parent, or FDT_INVALID for the root. O(tree); called rarely. */
int         fdt_parent(int node);

/* reg[index] decoded with the parent's #address-cells/#size-cells.
 * Returns 0 on success. `size` may be NULL. */
int         fdt_reg(int node, unsigned index, uint64_t *addr, uint64_t *size);

/* Big-endian cell accessors for property payloads. */
uint32_t    fdt_be32(const void *p);
uint64_t    fdt_be64(const void *p);

/* Convenience: read a u32 property, returning `dflt` when absent/short. */
uint32_t    fdt_getprop_u32(int node, const char *name, uint32_t dflt);

/* 1 if this cpu node's `riscv,isa-extensions` list contains `ext` (exact match
 * on one comma-separated entry), else 0. Falls back to a substring check of the
 * legacy `riscv,isa` string when the modern list is absent. */
int         fdt_cpu_has_extension(int cpu_node, const char *ext);

#endif /* DHEWM3_RV_FDT_H */
