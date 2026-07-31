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
#include "intr_conf.h"
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
static BaseType_t prvQueueCommandFromTask(cmdqu_t *rtos_cmdq);
static void prvPollMailbox(void);

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
	int irq_ret = 0;

	printf("create cvi task\n");

	sg2002_rtos_mailbox_init();
	/* IRQ61 is not delivered on SG2002 C906L; the CMDQU task owns RX polling. */
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
	printf("prvCmdQuRunTask run; c906l_mailbox_layered_v1\n");

	for (;;) {
		if (xQueueReceive(gTaskCtx[E_QUEUE_CMDQU].queHandle,
				  &rtos_cmdq, 0U) != pdPASS) {
			prvPollMailbox();
			udelay(SG2002_RTOS_MAILBOX_POLL_INTERVAL_US);
			taskYIELD();
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
			rtos_cmdq.cmd_id = SYS_CMD_INFO_DUMP_MSG;
			rtos_cmdq.param_ptr = (unsigned int)dump_uart_msg();
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
		printf("unknown ip_id=%d cmd_id=%d\n", rtos_cmdq->ip_id,
		       rtos_cmdq->cmd_id);
		return pdFAIL;
	}
}

static void prvPollMailbox(void)
{
	cmdqu_t pending[SG2002_RTOS_MAILBOX_SLOT_COUNT];
	unsigned int count;
	unsigned int i;

	count = sg2002_rtos_mailbox_receive(
		pending, SG2002_RTOS_MAILBOX_SLOT_COUNT);
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
