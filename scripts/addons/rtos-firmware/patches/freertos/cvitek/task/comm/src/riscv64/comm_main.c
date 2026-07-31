/* Standard includes. */
#include <stdio.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "mmio.h"
#include "delay.h"

/* cvitek includes. */
#include "printf.h"
#include "rtos_cmdqu.h"
#include "fast_image.h"
#include "cvi_mailbox.h"
#include "intr_conf.h"
#include "top_reg.h"
#include "memmap.h"
#include "gpio.h"
#include "boot_trace.h"

#include "comm.h"
#include "cvi_spinlock.h"

//#define __DEBUG__

#ifdef __DEBUG__
#define debug_printf printf
#else
#define debug_printf(...)
#endif

extern struct transfer_config_t transfer_config;
struct trace_snapshot_t snapshot;

typedef struct _TASK_CTX_S {
	char        name[32];
	u16         stack_size;
	UBaseType_t priority;
	void (*runTask)(void *pvParameters);
	u8            queLength;
	QueueHandle_t queHandle;
} TASK_CTX_S;

/****************************************************************************
 * Function prototypes
 ****************************************************************************/
int prvQueueISR(int irq, void *dev_id);
void prvCmdQuRunTask(void *pvParameters);
static void mailbox_hw_init(void);
static BaseType_t prvQueueCommandFromTask(cmdqu_t *rtos_cmdq);
static void prvPollMailbox(void);
static int prvHandleUserCmd(cmdqu_t *rtos_cmdq);
static uint32_t prvCmdHeader(const cmdqu_t *cmdq);
static uint32_t prvReplyTraceArg(const cmdqu_t *cmdq, int send_to_cpu, int slot);

/****************************************************************************
 * Global parameters
 ****************************************************************************/
TASK_CTX_S gTaskCtx[E_QUEUE_MAX] = {
	{
		.name = "ISP",
		.stack_size = configMINIMAL_STACK_SIZE * 8,
		.priority = tskIDLE_PRIORITY + 3,
		.runTask = NULL,
		.queLength = 1,
		.queHandle = NULL,
	},
	{
		.name = "VCODEC",
		.stack_size = configMINIMAL_STACK_SIZE,
		.priority = tskIDLE_PRIORITY + 3,
		.runTask = NULL,
		.queLength = 1,
		.queHandle = NULL,
	},
	{
		.name = "VI",
		.stack_size = configMINIMAL_STACK_SIZE,
		.priority = tskIDLE_PRIORITY + 3,
		.runTask = NULL,
		.queLength = 1,
		.queHandle = NULL,
	},
	{
		.name = "CAMERA",
		.stack_size = configMINIMAL_STACK_SIZE,
		.priority = tskIDLE_PRIORITY + 3,
		.runTask = NULL,
		.queLength = 1,
		.queHandle = NULL,
	},
	{
		.name = "RGN",
		.stack_size = configMINIMAL_STACK_SIZE,
		.priority = tskIDLE_PRIORITY + 3,
		.runTask = prvRGNRunTask,
		.queLength = 10,
		.queHandle = NULL,
	},
	{
		.name = "CMDQU",
		.stack_size = configMINIMAL_STACK_SIZE,
		.priority = tskIDLE_PRIORITY + 1,
		.runTask = prvCmdQuRunTask,
		.queLength = 30,
		.queHandle = NULL,
	},
	{
		.name = "AUDIO",
		.stack_size = configMINIMAL_STACK_SIZE*15,
		.priority = tskIDLE_PRIORITY + 3,
		.runTask = prvAudioRunTask,
		.queLength = 10,
		.queHandle = NULL,
	},
};

volatile struct mailbox_set_register *mbox_reg;
volatile struct mailbox_done_register *mbox_done_reg;
volatile unsigned long *mailbox_context; // mailbox buffer context is 64 Bytess

/****************************************************************************
 * Function definitions
 ****************************************************************************/
QueueHandle_t main_GetMODHandle(QUEUE_HANDLE_E handle_idx)
{
	if (handle_idx >= E_QUEUE_MAX)
		return NULL;

	return gTaskCtx[handle_idx].queHandle;
}

void main_create_tasks(void)
{
	u8 i = 0;

#define TASK_INIT(_idx) \
do { \
	gTaskCtx[_idx].queHandle = xQueueCreate(gTaskCtx[_idx].queLength, sizeof(cmdqu_t)); \
	if (gTaskCtx[_idx].queHandle != NULL && gTaskCtx[_idx].runTask != NULL) { \
		xTaskCreate(gTaskCtx[_idx].runTask, gTaskCtx[_idx].name, gTaskCtx[_idx].stack_size, \
			    NULL, gTaskCtx[_idx].priority, NULL); \
	} \
} while(0)

	for (; i < ARRAY_SIZE(gTaskCtx); i++) {
		TASK_INIT(i);
	}
}

DEFINE_CVI_SPINLOCK(mailbox_lock, SPIN_MBOX);

static void mailbox_hw_init(void)
{
	unsigned int reg_base = MAILBOX_REG_BASE;

	mbox_reg = (struct mailbox_set_register *)reg_base;
	mbox_done_reg = (struct mailbox_done_register *)(reg_base + 2);
	mailbox_context = (unsigned long *)MAILBOX_REG_BUFF;
	cvi_spinlock_init();
}

static uint32_t prvCmdHeader(const cmdqu_t *cmdq)
{
	return ((uint32_t)cmdq->ip_id << 0) |
	       ((uint32_t)cmdq->cmd_id << 8) |
	       ((uint32_t)cmdq->block << 15) |
	       ((uint32_t)cmdq->resv.valid.linux_valid << 16) |
	       ((uint32_t)cmdq->resv.valid.rtos_valid << 24);
}

static uint32_t prvReplyTraceArg(const cmdqu_t *cmdq, int send_to_cpu, int slot)
{
	return (((uint32_t)send_to_cpu & 0xfU) << 28) |
	       (((uint32_t)slot & 0xfU) << 24) |
	       (prvCmdHeader(cmdq) & 0x00ffffffU);
}

static int prvHandleUserCmd(cmdqu_t *rtos_cmdq)
{
	switch (rtos_cmdq->cmd_id) {
	case RTOS_USER_CMD_PING:
		rtos_cmdq->param_ptr ^= RTOS_USER_PING_XOR;
		cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_PING_HANDLED,
					rtos_cmdq->param_ptr);
		return 0;
	case RTOS_USER_CMD_GPIO_SET:
	{
		unsigned int pin = (rtos_cmdq->param_ptr >> 16) & 0xFFFF;
		unsigned int value = rtos_cmdq->param_ptr & 0x1;

		gpio_direction_output((int)pin, value ? 1 : 0);
		rtos_cmdq->param_ptr = value;
		return 0;
	}
	case RTOS_USER_CMD_GPIO_GET:
	{
		unsigned int pin = (rtos_cmdq->param_ptr >> 16) & 0xFFFF;

		rtos_cmdq->param_ptr = gpio_get_value((int)pin) & 0x1;
		return 0;
	}
	default:
		return -1;
	}
}

void main_cvirtos(void)
{
	int irq_ret;

	printf("create cvi task\n");

	mailbox_hw_init();
	/* IRQ61 is not delivered on SG2002 C906L; the CMDQU task owns RX polling. */
	irq_ret = 0;
	cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_IRQ_RESULT,
				(((uint32_t)MBOX_INT_C906_2ND & 0xffffU) << 16) |
				((uint32_t)irq_ret & 0xffffU));
	cvitek_boot_trace_mark(CVITEK_BOOT_TRACE_STAGE_IRQ_READY);

#ifdef FAST_IMAGE_ENABLE
	start_camera(0);
#endif

	main_create_tasks();

	/* Start the tasks and timer running. */
	vTaskStartScheduler();

    /* If all is well, the scheduler will now be running, and the following
    line will never be reached.  If the following line does execute, then
    there was either insufficient FreeRTOS heap memory available for the idle
    and/or timer tasks to be created, or vTaskStartScheduler() was called from
    User mode.  See the memory management section on the FreeRTOS web site for
    more details on the FreeRTOS heap http://www.freertos.org/a00111.html.  The
    mode from which main() is called is set in the C start up code and must be
    a privileged mode (not user mode). */
    for (;;)
        ;
}

void prvCmdQuRunTask(void *pvParameters)
{
	/* Remove compiler warning about unused parameter. */
	(void)pvParameters;
	cvitek_boot_trace_mark(CVITEK_BOOT_TRACE_STAGE_CMDQU_TASK);

	cmdqu_t rtos_cmdq;
	cmdqu_t *cmdq;
	cmdqu_t *rtos_cmdqu_t;
	static int stop_ip = 0;
	int ret = 0;
	int flags;
	int valid;
	int send_to_cpu = SEND_TO_CPU1;

	/* set mcu_status to type1 running*/
	transfer_config.mcu_status = MCU_STATUS_RTOS_T1_RUNNING;

	if (transfer_config.conf_magic == C906_MAGIC_HEADER)
		send_to_cpu = SEND_TO_CPU1;
	else if (transfer_config.conf_magic == CA53_MAGIC_HEADER)
		send_to_cpu = SEND_TO_CPU0;
	/* to compatible code with linux side */
	cmdq = &rtos_cmdq;
	cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_TASK_READY,
				(((uint32_t)RECEIVE_CPU & 0xffffU) << 16) |
				((uint32_t)send_to_cpu & 0xffffU));
	printf("prvCmdQuRunTask run; c906l_mailbox_direction_v5\n");

	for (;;) {
			if (xQueueReceive(gTaskCtx[E_QUEUE_CMDQU].queHandle,
					  &rtos_cmdq, 0U) != pdPASS) {
				prvPollMailbox();
				udelay(1000);
				taskYIELD();
				continue;
			}
			if (rtos_cmdq.cmd_id == RTOS_USER_CMD_PING)
				cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_DEQUEUE,
							prvCmdHeader(&rtos_cmdq));

			if (prvHandleUserCmd(&rtos_cmdq) == 0) {
				rtos_cmdq.ip_id = IP_SYSTEM;
				goto send_label;
			}

			switch (rtos_cmdq.cmd_id) {
#if ( configUSE_TRACE_FACILITY == 1 )
			case SYS_CMD_INFO_TRACE_SNAPSHOT_START:
				debug_printf("SYS_CMD_INFO_TRACE_SNAPSHOT_START\n");
				vTraceEnable(TRC_START);
				break;
			case SYS_CMD_INFO_TRACE_SNAPSHOT_STOP:
				snapshot.ptr = xTraceGetTraceBuffer();
				snapshot.size = uiTraceGetTraceBufferSize();
				snapshot.type = 0;
				rtos_cmdq.param_ptr = &snapshot;
				vTraceStop();
				flush_dcache_range(&snapshot, sizeof (struct trace_snapshot_t));
				flush_dcache_range(snapshot.ptr, snapshot.size);
				debug_printf("SYS_CMD_INFO_TRACE_SNAPSHOT_STOP PA =%lx\n", &snapshot);
				debug_printf("SYS_CMD_INFO_TRACE_SNAPSHOT_STOP ptr =%lx\n", snapshot.ptr);
				debug_printf("SYS_CMD_INFO_TRACE_SNAPSHOT_STOP size =%lx\n", snapshot.size);
				goto send_label;
				break;
#endif
			case SYS_CMD_INFO_DUMP_JPG:
				break;
			case SYS_CMD_INFO_DUMP_EN:
				dump_uart_enable();
				break;
			case SYS_CMD_INFO_DUMP_DIS:
				dump_uart_disable();
				break;
			case SYS_CMD_INFO_DUMP_MSG:
				/* SYS_CMD_INFO_DUMP_MSG is block mode*/
				rtos_cmdq.cmd_id = SYS_CMD_INFO_DUMP_MSG;
				rtos_cmdq.param_ptr = (unsigned int) dump_uart_msg();
				goto send_label;
				break;
			case SYS_CMD_INFO_LINUX_INIT_DONE:
				rtos_cmdq.cmd_id = SYS_CMD_INFO_RTOS_INIT_DONE;
				rtos_cmdq.param_ptr = &transfer_config;
				goto send_label;
				break;
			case SYS_CMD_INFO_STOP_ISR:
				stop_ip = 0;
				rtos_cmdq.cmd_id = SYS_CMD_INFO_STOP_ISR;
				rtos_cmdq.ip_id = IP_VI;
				xQueueSend(gTaskCtx[E_QUEUE_VI].queHandle, &rtos_cmdq, 0U);
				break;
			case SYS_CMD_INFO_STOP_ISR_DONE:
				// stop interrupt in order to avoid losing frame
				if (rtos_cmdq.ip_id == IP_VI) {
					stop_ip |= STOP_CMD_DONE_VI;
					rtos_cmdq.ip_id = IP_VCODEC;
					rtos_cmdq.cmd_id = SYS_CMD_INFO_STOP_ISR;
					xQueueSend(gTaskCtx[E_QUEUE_VCODEC].queHandle, &rtos_cmdq, 0U);
					break;
				}
				if (rtos_cmdq.ip_id == IP_VCODEC)
					stop_ip |= STOP_CMD_DONE_VCODE;
				if (stop_ip != STOP_CMD_DONE_ALL)
					break;
				else {
					// all isr of ip is disabled, and send msg back to linux
					rtos_cmdq.ip_id = IP_SYSTEM;
				}
			case SYS_CMD_INFO_LINUX:
			default:
send_label:
				/* used to send command to linux*/
				rtos_cmdqu_t = (cmdqu_t *) mailbox_context;

				debug_printf("RTOS_CMDQU_SEND\n");
				debug_printf("ip_id=%d cmd_id=%d param_ptr=%x\n", cmdq->ip_id, cmdq->cmd_id, (unsigned int)cmdq->param_ptr);
				debug_printf("mailbox_context = %x\n", mailbox_context);
				debug_printf("linux_cmdqu_t = %x\n", rtos_cmdqu_t);
				debug_printf("cmdq->ip_id = %d\n", cmdq->ip_id);
				debug_printf("cmdq->cmd_id = %d\n", cmdq->cmd_id);
				debug_printf("cmdq->block = %d\n", cmdq->block);
				debug_printf("cmdq->para_ptr = %x\n", cmdq->param_ptr);

				drv_spin_lock_irqsave(&mailbox_lock, flags);
				if (flags == MAILBOX_LOCK_FAILED) {
					if (cmdq->cmd_id == RTOS_USER_CMD_PING)
						cvitek_boot_trace_event(
							CVITEK_BOOT_TRACE_EVENT_SPINLOCK_FAIL,
							prvCmdHeader(cmdq));
					printf("[%s][%d] drv_spin_lock_irqsave failed! ip_id = %d , cmd_id = %d\n" , cmdq->ip_id , cmdq->cmd_id);
					break;
				}

				for (valid = 0; valid < MAILBOX_MAX_NUM; valid++) {
					if (rtos_cmdqu_t->resv.valid.linux_valid == 0 && rtos_cmdqu_t->resv.valid.rtos_valid == 0) {
						// mailbox buffer context is 4 bytes write access
						int *ptr = (int *)rtos_cmdqu_t;

						cmdq->resv.valid.linux_valid = 0;
						cmdq->resv.valid.rtos_valid = 1;
						*ptr = ((cmdq->ip_id << 0) | (cmdq->cmd_id << 8) | (cmdq->block << 15) |
								(cmdq->resv.valid.linux_valid << 16) |
								(cmdq->resv.valid.rtos_valid << 24));
						rtos_cmdqu_t->param_ptr = cmdq->param_ptr;
						if (cmdq->cmd_id == RTOS_USER_CMD_PING)
							cvitek_boot_trace_event(
								CVITEK_BOOT_TRACE_EVENT_REPLY_SLOT,
								prvReplyTraceArg(cmdq, send_to_cpu, valid));
						debug_printf("rtos_cmdqu_t->linux_valid = %d\n", rtos_cmdqu_t->resv.valid.linux_valid);
						debug_printf("rtos_cmdqu_t->rtos_valid = %d\n", rtos_cmdqu_t->resv.valid.rtos_valid);
						debug_printf("rtos_cmdqu_t->ip_id =%x %d\n", &rtos_cmdqu_t->ip_id, rtos_cmdqu_t->ip_id);
						debug_printf("rtos_cmdqu_t->cmd_id = %d\n", rtos_cmdqu_t->cmd_id);
						debug_printf("rtos_cmdqu_t->block = %d\n", rtos_cmdqu_t->block);
						debug_printf("rtos_cmdqu_t->param_ptr addr=%x %x\n", &rtos_cmdqu_t->param_ptr, rtos_cmdqu_t->param_ptr);
						debug_printf("*ptr = %x\n", *ptr);
						// clear mailbox
						mbox_reg->cpu_mbox_set[send_to_cpu].cpu_mbox_int_clr.mbox_int_clr = (1 << valid);
						// trigger mailbox valid to rtos
						mbox_reg->cpu_mbox_en[send_to_cpu].mbox_info |= (1 << valid);
						mbox_reg->mbox_set.mbox_set = (1 << valid);
						if (cmdq->cmd_id == RTOS_USER_CMD_PING)
							cvitek_boot_trace_event(
								CVITEK_BOOT_TRACE_EVENT_REPLY_POSTED,
								prvReplyTraceArg(cmdq, send_to_cpu, valid));
						break;
					}
					rtos_cmdqu_t++;
				}
				drv_spin_unlock_irqrestore(&mailbox_lock, flags);
				if (valid >= MAILBOX_MAX_NUM) {
					if (cmdq->cmd_id == RTOS_USER_CMD_PING)
						cvitek_boot_trace_event(
							CVITEK_BOOT_TRACE_EVENT_REPLY_NO_SLOT,
							prvCmdHeader(cmdq));
				    printf("No valid mailbox is available\n");
				    return -1;
				}
				break;
		}
	}
}

static BaseType_t prvQueueCommandFromTask(cmdqu_t *rtos_cmdq)
{
	switch (rtos_cmdq->ip_id) {
	case IP_ISP:
		return xQueueSend(gTaskCtx[E_QUEUE_ISP].queHandle, rtos_cmdq, 0U);
	case IP_VCODEC:
		return xQueueSend(gTaskCtx[E_QUEUE_VCODEC].queHandle, rtos_cmdq, 0U);
	case IP_VI:
		return xQueueSend(gTaskCtx[E_QUEUE_VI].queHandle, rtos_cmdq, 0U);
	case IP_RGN:
		return xQueueSend(gTaskCtx[E_QUEUE_RGN].queHandle, rtos_cmdq, 0U);
	case IP_AUDIO:
		return xQueueSend(gTaskCtx[E_QUEUE_AUDIO].queHandle, rtos_cmdq, 0U);
	case IP_SYSTEM:
		return xQueueSend(gTaskCtx[E_QUEUE_CMDQU].queHandle, rtos_cmdq, 0U);
	case IP_CAMERA:
		return xQueueSend(gTaskCtx[E_QUEUE_CAMERA].queHandle, rtos_cmdq, 0U);
	default:
		printf("unknown ip_id =%d cmd_id=%d\n", rtos_cmdq->ip_id,
		       rtos_cmdq->cmd_id);
		return pdFAIL;
	}
}

static void prvPollMailbox(void)
{
	cmdqu_t pending[MAILBOX_MAX_NUM];
	unsigned char set_val;
	unsigned char valid_val;
	unsigned char pending_mask = 0;
	int flags;
	int pending_count = 0;
	int i;

	/* Linux publishes each slot while holding the same hardware spinlock. */
	drv_spin_lock_irqsave(&mailbox_lock, flags);
	if (flags == MAILBOX_LOCK_FAILED)
		return;

	set_val = mbox_reg->cpu_mbox_set[RECEIVE_CPU].cpu_mbox_int_int.mbox_int;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");

	for (i = 0; i < MAILBOX_MAX_NUM; i++) {
		valid_val = set_val & (1U << i);
		if (!valid_val)
			continue;

		mbox_reg->cpu_mbox_set[RECEIVE_CPU].cpu_mbox_int_clr.mbox_int_clr = valid_val;
		mbox_reg->cpu_mbox_en[RECEIVE_CPU].mbox_info &= ~valid_val;
	}
	__asm__ volatile ("fence iorw, iorw" ::: "memory");

	for (i = 0; i < MAILBOX_MAX_NUM; i++) {
		cmdqu_t *cmdq = (cmdqu_t *)mailbox_context + i;

		if (cmdq->resv.valid.linux_valid != 1 ||
		    cmdq->resv.valid.rtos_valid != 0)
			continue;
		*((unsigned long *)&pending[pending_count]) = *((unsigned long *)cmdq);
		*((unsigned long *)cmdq) = 0;
		pending_mask |= 1U << i;
		pending_count++;
	}
	drv_spin_unlock_irqrestore(&mailbox_lock, flags);

	if (set_val || pending_mask)
		cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_MAILBOX_POLL,
					((uint32_t)pending_mask << 8) | set_val);
	for (i = 0; i < pending_count; i++) {
		BaseType_t queue_result;

		if (pending[i].resv.valid.linux_valid != 1)
			continue;
		if (pending[i].cmd_id == RTOS_USER_CMD_PING)
			cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_LINUX_VALID,
						prvCmdHeader(&pending[i]));
		queue_result = prvQueueCommandFromTask(&pending[i]);
		if (pending[i].cmd_id == RTOS_USER_CMD_PING)
			cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_QUEUE_SENT,
						(uint32_t)queue_result);
	}
}

int prvQueueISR(int irq, void *dev_id)
{
	//printf("prvQueueISR\n");

	unsigned char set_val;
//	unsigned char done_val;
	unsigned char valid_val;
	int i;
	cmdqu_t *cmdq;
	BaseType_t YieldRequired = pdFALSE;
	BaseType_t queue_result;

	(void)irq;
	(void)dev_id;

	cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_ISR_ENTRY, 0);
	set_val = mbox_reg->cpu_mbox_set[RECEIVE_CPU].cpu_mbox_int_int.mbox_int;
	cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_ISR_SET, set_val);
	/* Now, we do not implement info back feature */
	// done_val = mbox_done_reg->cpu_mbox_done[RECEIVE_CPU].cpu_mbox_int_int.mbox_int;

	if (set_val) {
		for(i = 0; i < MAILBOX_MAX_NUM; i++) {
			valid_val = set_val  & (1 << i);

			if (valid_val) {
				cmdqu_t rtos_cmdq;
				cmdq = (cmdqu_t *)(mailbox_context) + i;

				debug_printf("mailbox_context =%x\n", mailbox_context);
				debug_printf("sizeof mailbox_context =%x\n", sizeof(cmdqu_t));
				/* mailbox buffer context is send from linux, clear mailbox interrupt */
				mbox_reg->cpu_mbox_set[RECEIVE_CPU].cpu_mbox_int_clr.mbox_int_clr = valid_val;
				// need to disable enable bit
				mbox_reg->cpu_mbox_en[RECEIVE_CPU].mbox_info &= ~valid_val;

				// copy cmdq context (8 bytes) to buffer ASAP
				*((unsigned long *) &rtos_cmdq) = *((unsigned long *)cmdq);
				/* need to clear mailbox interrupt before clear mailbox buffer */
				*((unsigned long*) cmdq) = 0;

				/* mailbox buffer context is send from linux*/
				if (rtos_cmdq.resv.valid.linux_valid == 1 &&
				    rtos_cmdq.resv.valid.rtos_valid == 0) {
					if (rtos_cmdq.cmd_id == RTOS_USER_CMD_PING)
						cvitek_boot_trace_event(
							CVITEK_BOOT_TRACE_EVENT_LINUX_VALID,
							prvCmdHeader(&rtos_cmdq));
					debug_printf("cmdq=%x\n", cmdq);
					debug_printf("cmdq->ip_id =%d\n", rtos_cmdq.ip_id);
					debug_printf("cmdq->cmd_id =%d\n", rtos_cmdq.cmd_id);
					debug_printf("cmdq->param_ptr =%x\n", rtos_cmdq.param_ptr);
					debug_printf("cmdq->block =%x\n", rtos_cmdq.block);
					debug_printf("cmdq->linux_valid =%d\n", rtos_cmdq.resv.valid.linux_valid);
					debug_printf("cmdq->rtos_valid =%x\n", rtos_cmdq.resv.valid.rtos_valid);
					switch (rtos_cmdq.ip_id) {
					case IP_ISP:
						xQueueSendFromISR(gTaskCtx[E_QUEUE_ISP].queHandle, &rtos_cmdq, &YieldRequired);
						break;
					case IP_VCODEC:
						xQueueSendFromISR(gTaskCtx[E_QUEUE_VCODEC].queHandle, &rtos_cmdq, &YieldRequired);
						break;
					case IP_VI:
						xQueueSendFromISR(gTaskCtx[E_QUEUE_VI].queHandle, &rtos_cmdq, &YieldRequired);
						break;
					case IP_RGN:
						xQueueSendFromISR(gTaskCtx[E_QUEUE_RGN].queHandle, &rtos_cmdq, &YieldRequired);
						break;
					case IP_AUDIO:
						xQueueSendFromISR(gTaskCtx[E_QUEUE_AUDIO].queHandle, &rtos_cmdq, &YieldRequired);
						break;
					case IP_SYSTEM:
						queue_result = xQueueSendFromISR(
							gTaskCtx[E_QUEUE_CMDQU].queHandle,
							&rtos_cmdq, &YieldRequired);
						if (rtos_cmdq.cmd_id == RTOS_USER_CMD_PING)
							cvitek_boot_trace_event(
								CVITEK_BOOT_TRACE_EVENT_QUEUE_SENT,
								(uint32_t)queue_result);
						break;
					case IP_CAMERA:
						xQueueSendFromISR(gTaskCtx[E_QUEUE_CAMERA].queHandle, &rtos_cmdq, &YieldRequired);
						break;
					default:
						printf("unknown ip_id =%d cmd_id=%d\n", rtos_cmdq.ip_id, rtos_cmdq.cmd_id);
						break;
					}
					portYIELD_FROM_ISR(YieldRequired);
				} else
					printf("rtos cmdq is not valid %d, ip=%d , cmd=%d\n",
						rtos_cmdq.resv.valid.rtos_valid, rtos_cmdq.ip_id, rtos_cmdq.cmd_id);
			}
		}
	}

	return 0;
}
