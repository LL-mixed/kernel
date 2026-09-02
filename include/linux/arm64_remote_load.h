/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ARM64_REMOTE_LOAD_H
#define _LINUX_ARM64_REMOTE_LOAD_H

struct module;
struct pt_regs;

/*
 * A handler returns zero after the remote completion is ready for instruction
 * replay. Any non-zero value leaves the abort unhandled and delivers SIGBUS to
 * the EL0 task through the normal arm64 data-abort path.
 */
struct arm64_remote_load_fault_ops {
	int (*handle)(unsigned long far, unsigned long esr,
		      struct pt_regs *regs);
	int (*handle_svc)(unsigned int imm, struct pt_regs *regs);
	struct module *owner;
};

int arm64_register_remote_load_fault_handler(
	const struct arm64_remote_load_fault_ops *ops);
void arm64_unregister_remote_load_fault_handler(
	const struct arm64_remote_load_fault_ops *ops);
int arm64_handle_remote_load_svc(unsigned int imm, struct pt_regs *regs);

#endif /* _LINUX_ARM64_REMOTE_LOAD_H */
