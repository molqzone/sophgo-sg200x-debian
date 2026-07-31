#include "sg2002_rtos_mailbox.h"

#include <stddef.h>
#include <stdint.h>

#include "cvi_mailbox.h"
#include "cvi_spinlock.h"
#include "memmap.h"
#include "top_reg.h"

#ifdef SG2002_RTOS_TRACE_MAILBOX
#include "boot_trace.h"
#define MAILBOX_TRACE(event, argument) \
	cvitek_boot_trace_event((event), (argument))
#else
#define MAILBOX_TRACE(event, argument) do { } while (0)
#endif

typedef char sg2002_cmdqu_size_must_be_8[(sizeof(cmdqu_t) == 8) ? 1 : -1];

static volatile struct mailbox_set_register *mailbox_registers;
static volatile unsigned long *mailbox_context;

DEFINE_CVI_SPINLOCK(mailbox_lock, SPIN_MBOX);

static uint32_t command_header(const cmdqu_t *command)
{
	return ((uint32_t)command->ip_id << 0) |
	       ((uint32_t)command->cmd_id << 8) |
	       ((uint32_t)command->block << 15) |
	       ((uint32_t)command->resv.valid.linux_valid << 16) |
	       ((uint32_t)command->resv.valid.rtos_valid << 24);
}

#ifdef SG2002_RTOS_TRACE_MAILBOX
static uint32_t reply_trace_argument(const cmdqu_t *command, int send_to_cpu,
				     unsigned int slot)
{
	return (((uint32_t)send_to_cpu & 0xfU) << 28) |
	       ((slot & 0xfU) << 24) |
	       (command_header(command) & 0x00ffffffU);
}
#endif

void sg2002_rtos_mailbox_init(void)
{
	mailbox_registers =
		(volatile struct mailbox_set_register *)MAILBOX_REG_BASE;
	mailbox_context = (volatile unsigned long *)MAILBOX_REG_BUFF;
	cvi_spinlock_init();
}

void sg2002_rtos_mailbox_enable_receiver(void)
{
	/* Quiesce stale state before unmasking all eight C906L channels. */
	mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_mask.mbox_int_mask = 0xffU;
	mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_clr.mbox_int_clr = 0xffU;
	mailbox_registers->cpu_mbox_en[RECEIVE_CPU].mbox_info = 0U;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_mask.mbox_int_mask = 0U;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
}

void sg2002_rtos_mailbox_get_diagnostics(
	struct sg2002_rtos_mailbox_diagnostics *diagnostics)
{
	if (diagnostics == NULL)
		return;

	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	diagnostics->enabled = mailbox_registers->
		cpu_mbox_en[RECEIVE_CPU].mbox_info;
	diagnostics->raw = mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_raw.mbox_int_raw;
	diagnostics->mask = mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_mask.mbox_int_mask;
	diagnostics->pending = mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_int.mbox_int;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
}

static unsigned int receive_commands(cmdqu_t *commands,
				      unsigned int capacity,
				      unsigned char interrupt_mask)
{
	unsigned char consumed_mask = 0;
	unsigned int count = 0;
	unsigned int slot;

	if (commands == NULL || capacity == 0)
		return 0;

	for (slot = 0; slot < MAILBOX_MAX_NUM; slot++) {
		unsigned char slot_mask = interrupt_mask & (1U << slot);

		if (!slot_mask)
			continue;
		mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
			cpu_mbox_int_clr.mbox_int_clr = slot_mask;
		mailbox_registers->cpu_mbox_en[RECEIVE_CPU].mbox_info &=
			~slot_mask;
	}
	__asm__ volatile ("fence iorw, iorw" ::: "memory");

	for (slot = 0; slot < MAILBOX_MAX_NUM && count < capacity; slot++) {
		volatile cmdqu_t *mailbox_command =
			(volatile cmdqu_t *)mailbox_context + slot;

		if (mailbox_command->resv.valid.linux_valid != 1 ||
		    mailbox_command->resv.valid.rtos_valid != 0)
			continue;

		*((unsigned long *)&commands[count]) =
			*((volatile unsigned long *)mailbox_command);
		*((volatile unsigned long *)mailbox_command) = 0;
		consumed_mask |= 1U << slot;
		MAILBOX_TRACE(CVITEK_BOOT_TRACE_EVENT_LINUX_VALID,
			      command_header(&commands[count]));
		count++;
	}

	if (interrupt_mask || consumed_mask)
		MAILBOX_TRACE(CVITEK_BOOT_TRACE_EVENT_MAILBOX_POLL,
			      ((uint32_t)consumed_mask << 8) | interrupt_mask);
	return count;
}

unsigned int sg2002_rtos_mailbox_receive(cmdqu_t *commands,
					 unsigned int capacity)
{
	unsigned char interrupt_mask;
	unsigned int count;
	int flags;

	if (commands == NULL || capacity == 0)
		return 0;

	/* Linux publishes slots while holding the same hardware spinlock. */
	drv_spin_lock_irqsave(&mailbox_lock, flags);
	if (flags == MAILBOX_LOCK_FAILED)
		return 0;

	interrupt_mask = mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_int.mbox_int;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	count = receive_commands(commands, capacity, interrupt_mask);
	drv_spin_unlock_irqrestore(&mailbox_lock, flags);
	return count;
}

unsigned int sg2002_rtos_mailbox_receive_from_isr(cmdqu_t *commands,
						  unsigned int capacity)
{
	unsigned char interrupt_mask;

	if (commands == NULL || capacity == 0)
		return 0;

	interrupt_mask = mailbox_registers->cpu_mbox_set[RECEIVE_CPU].
		cpu_mbox_int_int.mbox_int;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	return receive_commands(commands, capacity, interrupt_mask);
}

int sg2002_rtos_mailbox_send(const cmdqu_t *command, int send_to_cpu)
{
	cmdqu_t reply;
	unsigned int slot;
	int flags;

	if (command == NULL)
		return -1;
	reply = *command;
	reply.resv.valid.linux_valid = 0;
	reply.resv.valid.rtos_valid = 1;

	drv_spin_lock_irqsave(&mailbox_lock, flags);
	if (flags == MAILBOX_LOCK_FAILED) {
		MAILBOX_TRACE(CVITEK_BOOT_TRACE_EVENT_SPINLOCK_FAIL,
			      command_header(&reply));
		return -1;
	}

	for (slot = 0; slot < MAILBOX_MAX_NUM; slot++) {
		volatile cmdqu_t *mailbox_command =
			(volatile cmdqu_t *)mailbox_context + slot;

		if (mailbox_command->resv.valid.linux_valid != 0 ||
		    mailbox_command->resv.valid.rtos_valid != 0)
			continue;

		/* Publish the valid header last, then ring the mailbox doorbell. */
		mailbox_command->param_ptr = reply.param_ptr;
		__asm__ volatile ("fence iorw, iorw" ::: "memory");
		*((volatile uint32_t *)mailbox_command) = command_header(&reply);
		__asm__ volatile ("fence iorw, iorw" ::: "memory");
		MAILBOX_TRACE(CVITEK_BOOT_TRACE_EVENT_REPLY_SLOT,
			      reply_trace_argument(&reply, send_to_cpu, slot));

		mailbox_registers->cpu_mbox_set[send_to_cpu].
			cpu_mbox_int_clr.mbox_int_clr = 1U << slot;
		mailbox_registers->cpu_mbox_en[send_to_cpu].mbox_info |=
			1U << slot;
		mailbox_registers->mbox_set.mbox_set = 1U << slot;
		MAILBOX_TRACE(CVITEK_BOOT_TRACE_EVENT_REPLY_POSTED,
			      reply_trace_argument(&reply, send_to_cpu, slot));
		break;
	}
	drv_spin_unlock_irqrestore(&mailbox_lock, flags);

	if (slot >= MAILBOX_MAX_NUM) {
		MAILBOX_TRACE(CVITEK_BOOT_TRACE_EVENT_REPLY_NO_SLOT,
			      command_header(&reply));
		return -1;
	}
	return 0;
}
