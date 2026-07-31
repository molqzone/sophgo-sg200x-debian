/* Standard includes. */
#include <stdio.h>
#include <stdint.h>

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
#include "dump_uart.h"
#include "intr_conf.h"
#include "irq.h"
#include "csr.h"
#include "top_reg.h"
#include "memmap.h"
#include "boot_trace.h"

#include "comm.h"
#include "sg2002_rtos_app.h"
#include "sg2002_rtos_mailbox.h"

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
void prvCmdQuRunTask(void *pvParameters);
static int prvMailboxISR(int irqn, void *priv);
static QueueHandle_t prvQueueForIp(unsigned int ip_id);
static BaseType_t prvQueueCommandFromTask(cmdqu_t *rtos_cmdq);
static BaseType_t prvQueueCommandFromISR(
	cmdqu_t *rtos_cmdq, BaseType_t *higher_priority_task_woken);
static void prvRecoverMailbox(void);
static uint32_t prvPackMailboxDiagnostics(
	const struct sg2002_rtos_mailbox_diagnostics *diagnostics);
static uint32_t prvPlicDiagnostics(void);
static uint32_t prvCsrDiagnostics(void);
static void prvTraceMailboxDiagnostics(
	uint32_t reason,
	const struct sg2002_rtos_mailbox_diagnostics *diagnostics,
	uint32_t plic_state, uint32_t csr_state);

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

static struct sg2002_rtos_mailbox_diagnostics initial_mailbox_diagnostics;
static volatile uint32_t mailbox_irq_count;
static uint32_t mailbox_recovery_count;

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

void main_cvirtos(void)
{
	struct sg2002_rtos_mailbox_diagnostics ready_diagnostics;
	int irq_ret;

	printf("create cvi task\n");

	sg2002_rtos_mailbox_init();
	sg2002_rtos_mailbox_get_diagnostics(&initial_mailbox_diagnostics);
	sg2002_rtos_mailbox_enable_receiver();

#ifdef FAST_IMAGE_ENABLE
	start_camera(0);
#endif

	main_create_tasks();
	if (gTaskCtx[E_QUEUE_CMDQU].queHandle == NULL)
		irq_ret = -1;
	else
		irq_ret = request_irq(MBOX_INT_C906_2ND, prvMailboxISR, 0,
				      "mailbox", NULL);
	sg2002_rtos_mailbox_get_diagnostics(&ready_diagnostics);
	prvTraceMailboxDiagnostics(CVITEK_BOOT_TRACE_DIAG_REASON_INIT,
				   &ready_diagnostics, prvPlicDiagnostics(),
				   prvCsrDiagnostics());
	cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_IRQ_RESULT,
				(((uint32_t)MBOX_INT_C906_2ND & 0xffffU) << 16) |
				((uint32_t)irq_ret & 0xffffU));
	cvitek_boot_trace_mark(CVITEK_BOOT_TRACE_STAGE_IRQ_READY);

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
	cmdqu_t rtos_cmdq;
	static int stop_ip = 0;
	int send_to_cpu = SEND_TO_CPU1;

	(void)pvParameters;
	cvitek_boot_trace_mark(CVITEK_BOOT_TRACE_STAGE_CMDQU_TASK);
	transfer_config.mcu_status = MCU_STATUS_RTOS_T1_RUNNING;

	if (transfer_config.conf_magic == C906_MAGIC_HEADER)
		send_to_cpu = SEND_TO_CPU1;
	else if (transfer_config.conf_magic == CA53_MAGIC_HEADER)
		send_to_cpu = SEND_TO_CPU0;
	cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_TASK_READY,
				(((uint32_t)RECEIVE_CPU & 0xffffU) << 16) |
				((uint32_t)send_to_cpu & 0xffffU));
	printf("prvCmdQuRunTask run; c906l_mailbox_irq_v2\n");

	for (;;) {
		if (xQueueReceive(gTaskCtx[E_QUEUE_CMDQU].queHandle,
				  &rtos_cmdq,
				  pdMS_TO_TICKS(
					  SG2002_RTOS_MAILBOX_RECOVERY_INTERVAL_MS)) !=
		    pdPASS) {
			prvRecoverMailbox();
			continue;
		}

#ifdef SG2002_RTOS_TRACE_MAILBOX
		cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_DEQUEUE,
					((uint32_t)rtos_cmdq.ip_id << 8) |
					rtos_cmdq.cmd_id);
#endif
		if (rtos_cmdq.ip_id == IP_SYSTEM &&
		    sg2002_rtos_app_handle(rtos_cmdq.cmd_id,
					   &rtos_cmdq.param_ptr) ==
		    SG2002_RTOS_APP_HANDLED) {
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
			rtos_cmdq.param_ptr = (uint32_t)(uintptr_t)&snapshot;
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
			rtos_cmdq.cmd_id = SYS_CMD_INFO_DUMP_MSG;
			rtos_cmdq.param_ptr = (unsigned int)dump_uart_msg();
			goto send_label;
			break;
		case SYS_CMD_INFO_LINUX_INIT_DONE:
			rtos_cmdq.cmd_id = SYS_CMD_INFO_RTOS_INIT_DONE;
			rtos_cmdq.param_ptr =
				(uint32_t)(uintptr_t)&transfer_config;
			goto send_label;
			break;
		case SYS_CMD_INFO_STOP_ISR:
			stop_ip = 0;
			rtos_cmdq.cmd_id = SYS_CMD_INFO_STOP_ISR;
			rtos_cmdq.ip_id = IP_VI;
			xQueueSend(gTaskCtx[E_QUEUE_VI].queHandle, &rtos_cmdq, 0U);
			break;
		case SYS_CMD_INFO_STOP_ISR_DONE:
			if (rtos_cmdq.ip_id == IP_VI) {
				stop_ip |= STOP_CMD_DONE_VI;
				rtos_cmdq.ip_id = IP_VCODEC;
				rtos_cmdq.cmd_id = SYS_CMD_INFO_STOP_ISR;
				xQueueSend(gTaskCtx[E_QUEUE_VCODEC].queHandle,
					   &rtos_cmdq, 0U);
				break;
			}
			if (rtos_cmdq.ip_id == IP_VCODEC)
				stop_ip |= STOP_CMD_DONE_VCODE;
			if (stop_ip != STOP_CMD_DONE_ALL)
				break;
			rtos_cmdq.ip_id = IP_SYSTEM;
			/* fall through */
		case SYS_CMD_INFO_LINUX:
		default:
send_label:
			if (sg2002_rtos_mailbox_send(&rtos_cmdq, send_to_cpu))
				printf("No valid mailbox is available, ip=%d cmd=%d\n",
				       rtos_cmdq.ip_id, rtos_cmdq.cmd_id);
			break;
		}
	}
}

static QueueHandle_t prvQueueForIp(unsigned int ip_id)
{
	switch (ip_id) {
	case IP_ISP:
		return gTaskCtx[E_QUEUE_ISP].queHandle;
	case IP_VCODEC:
		return gTaskCtx[E_QUEUE_VCODEC].queHandle;
	case IP_VI:
		return gTaskCtx[E_QUEUE_VI].queHandle;
	case IP_RGN:
		return gTaskCtx[E_QUEUE_RGN].queHandle;
	case IP_AUDIO:
		return gTaskCtx[E_QUEUE_AUDIO].queHandle;
	case IP_SYSTEM:
		return gTaskCtx[E_QUEUE_CMDQU].queHandle;
	case IP_CAMERA:
		return gTaskCtx[E_QUEUE_CAMERA].queHandle;
	default:
		return NULL;
	}
}

static BaseType_t prvQueueCommandFromTask(cmdqu_t *rtos_cmdq)
{
	QueueHandle_t queue = prvQueueForIp(rtos_cmdq->ip_id);

	if (queue == NULL) {
		printf("unknown ip_id=%d cmd_id=%d\n", rtos_cmdq->ip_id,
		       rtos_cmdq->cmd_id);
		return pdFAIL;
	}
	return xQueueSend(queue, rtos_cmdq, 0U);
}

static uint32_t prvPackMailboxDiagnostics(
	const struct sg2002_rtos_mailbox_diagnostics *diagnostics)
{
	return ((uint32_t)diagnostics->enabled) |
	       ((uint32_t)diagnostics->raw << 8) |
	       ((uint32_t)diagnostics->mask << 16) |
	       ((uint32_t)diagnostics->pending << 24);
}

static uint32_t prvPlicDiagnostics(void)
{
	uint32_t bit = 1U << (MBOX_INT_C906_2ND % 32U);
	uint32_t offset = (MBOX_INT_C906_2ND / 32U) * 4U;
	uint32_t pending = mmio_read_32(PLIC_PENDING1 + offset);
	uint32_t enabled = mmio_read_32(PLIC_ENABLE1 + offset);
	uint32_t priority = mmio_read_32(
		PLIC_PRIORITY0 + MBOX_INT_C906_2ND * 4U);
	uint32_t threshold = mmio_read_32(PLIC_THRESHOLD);

	return ((pending & bit) ? 1U : 0U) |
	       ((enabled & bit) ? 2U : 0U) |
	       ((priority & 0xffU) << 8) |
	       ((threshold & 0xffU) << 16);
}

static uint32_t prvCsrDiagnostics(void)
{
	uintptr_t mstatus = read_csr(mstatus);
	uintptr_t mie = read_csr(mie);
	uintptr_t mip = read_csr(mip);

	return (((mstatus >> 3) & 1U) << 0) |
	       (((mie >> 11) & 1U) << 1) |
	       (((mip >> 11) & 1U) << 2);
}

static void prvTraceMailboxDiagnostics(
	uint32_t reason,
	const struct sg2002_rtos_mailbox_diagnostics *diagnostics,
	uint32_t plic_state, uint32_t csr_state)
{
	cvitek_boot_trace_irq_diagnostics(
		reason, prvPackMailboxDiagnostics(&initial_mailbox_diagnostics),
		prvPackMailboxDiagnostics(diagnostics), plic_state, csr_state,
		mailbox_irq_count, mailbox_recovery_count);
}

static BaseType_t prvQueueCommandFromISR(
	cmdqu_t *rtos_cmdq, BaseType_t *higher_priority_task_woken)
{
	QueueHandle_t queue = prvQueueForIp(rtos_cmdq->ip_id);

	if (queue == NULL)
		return pdFAIL;
	return xQueueSendFromISR(queue, rtos_cmdq,
				 higher_priority_task_woken);
}

static int prvMailboxISR(int irqn, void *priv)
{
	struct sg2002_rtos_mailbox_diagnostics diagnostics;
	cmdqu_t pending[SG2002_RTOS_MAILBOX_SLOT_COUNT];
	BaseType_t higher_priority_task_woken = pdFALSE;
	uint32_t plic_state;
	uint32_t csr_state;
	unsigned int count;
	unsigned int i;

	(void)irqn;
	(void)priv;
	sg2002_rtos_mailbox_get_diagnostics(&diagnostics);
	plic_state = prvPlicDiagnostics();
	csr_state = prvCsrDiagnostics();
	count = sg2002_rtos_mailbox_receive_from_isr(
		pending, SG2002_RTOS_MAILBOX_SLOT_COUNT);
	mailbox_irq_count++;
	if (mailbox_irq_count == 1U) {
		cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_ISR_ENTRY,
					prvPackMailboxDiagnostics(&diagnostics));
	}
	if (mailbox_irq_count <= 4U ||
	    (mailbox_irq_count & 0xffU) == 0U) {
		prvTraceMailboxDiagnostics(CVITEK_BOOT_TRACE_DIAG_REASON_ISR,
					   &diagnostics, plic_state,
					   csr_state);
	}

	for (i = 0; i < count; i++)
		(void)prvQueueCommandFromISR(&pending[i],
					     &higher_priority_task_woken);
	portYIELD_FROM_ISR(higher_priority_task_woken);
	return 0;
}

static void prvRecoverMailbox(void)
{
	struct sg2002_rtos_mailbox_diagnostics diagnostics;
	cmdqu_t pending[SG2002_RTOS_MAILBOX_SLOT_COUNT];
	uint32_t plic_state;
	uint32_t csr_state;
	unsigned int count;
	unsigned int i;

	sg2002_rtos_mailbox_get_diagnostics(&diagnostics);
	plic_state = prvPlicDiagnostics();
	csr_state = prvCsrDiagnostics();
	count = sg2002_rtos_mailbox_receive(
		pending, SG2002_RTOS_MAILBOX_SLOT_COUNT);
	if (count > 0U) {
		mailbox_recovery_count++;
		prvTraceMailboxDiagnostics(
			CVITEK_BOOT_TRACE_DIAG_REASON_RECOVERY,
			&diagnostics, plic_state, csr_state);
	}
	for (i = 0; i < count; i++) {
		BaseType_t result = prvQueueCommandFromTask(&pending[i]);

#ifdef SG2002_RTOS_TRACE_MAILBOX
		cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_QUEUE_SENT,
					(uint32_t)result);
#else
		(void)result;
#endif
	}
}
