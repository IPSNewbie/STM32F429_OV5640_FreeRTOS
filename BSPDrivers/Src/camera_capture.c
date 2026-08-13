#include "camera_capture.h"

#include "camera_dcmi_dma.h"
#include "camera_pc_dump.h"

#include "queue.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t timeout_ms;
    TaskHandle_t requester;
} CameraCaptureRequest_t;

typedef enum
{
    CAMERA_CAPTURE_ISR_EVENT_NONE = 0U,
    CAMERA_CAPTURE_ISR_EVENT_FRAME = 1U,
    CAMERA_CAPTURE_ISR_EVENT_ERROR = 2U
} CameraCaptureIsrEvent_t;

static QueueHandle_t s_camera_capture_request_queue;
static TaskHandle_t s_camera_capture_task;
static volatile CameraCaptureIsrEvent_t s_camera_capture_isr_event;
static CameraCaptureStats_t s_camera_capture_stats;

static void Camera_CaptureUpdateStackStats(void)
{
    s_camera_capture_stats.stack_min_free_bytes =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL) * (uint32_t)sizeof(StackType_t);
}

static CameraCaptureResult_t Camera_CaptureExecute(uint32_t timeout_ms)
{
    TickType_t timeout_ticks;
    uint8_t start_status;

    /* Discard a late notification from an already stopped capture. */
    (void)ulTaskNotifyTake(pdTRUE, 0U);
    s_camera_capture_isr_event = CAMERA_CAPTURE_ISR_EVENT_NONE;
    Camera_DCMI_ClearSnapshotDone();

    start_status = Camera_DCMI_StartSnapshotToBuffer(
        Camera_PC_Dump_GetBufferAddress(),
        Camera_PC_Dump_GetWordCount());
    s_camera_capture_stats.last_start_status = start_status;
    if (start_status != 0U)
    {
        Camera_DCMI_Stop();
        return CAMERA_CAPTURE_START_FAILED;
    }

    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0U)
    {
        timeout_ticks = 1U;
    }

    if (ulTaskNotifyTake(pdTRUE, timeout_ticks) == 0U)
    {
        Camera_DCMI_Stop();
        return CAMERA_CAPTURE_TIMEOUT;
    }

    Camera_DCMI_Stop();
    if (s_camera_capture_isr_event == CAMERA_CAPTURE_ISR_EVENT_FRAME)
    {
        return CAMERA_CAPTURE_OK;
    }

    return CAMERA_CAPTURE_HAL_ERROR;
}

bool Camera_CaptureInit(void)
{
    s_camera_capture_request_queue = xQueueCreate(
        CAMERA_CAPTURE_REQUEST_QUEUE_DEPTH,
        sizeof(CameraCaptureRequest_t));
    s_camera_capture_task = NULL;
    s_camera_capture_isr_event = CAMERA_CAPTURE_ISR_EVENT_NONE;
    s_camera_capture_stats.heartbeat_count = 0U;
    s_camera_capture_stats.stack_min_free_bytes = 0U;
    s_camera_capture_stats.last_start_status = 0U;
    s_camera_capture_stats.last_result = CAMERA_CAPTURE_NOT_READY;

    return (s_camera_capture_request_queue != NULL);
}

CameraCaptureResult_t Camera_CaptureRequestFrame(uint32_t timeout_ms)
{
    CameraCaptureRequest_t request;
    uint32_t notification_value = (uint32_t)CAMERA_CAPTURE_NOT_READY;

    if ((timeout_ms == 0U) ||
        (s_camera_capture_request_queue == NULL) ||
        (s_camera_capture_task == NULL))
    {
        return CAMERA_CAPTURE_NOT_READY;
    }

    request.timeout_ms = timeout_ms;
    request.requester = xTaskGetCurrentTaskHandle();

    /* Clear a stale completion before placing the next one-to-one request. */
    (void)xTaskNotifyWait(0U, UINT32_MAX, &notification_value, 0U);
    if (xQueueSend(s_camera_capture_request_queue, &request, 0U) != pdPASS)
    {
        return CAMERA_CAPTURE_REQUEST_FAILED;
    }

    if (xTaskNotifyWait(0U, UINT32_MAX, &notification_value, portMAX_DELAY) != pdTRUE)
    {
        return CAMERA_CAPTURE_REQUEST_FAILED;
    }

    return (CameraCaptureResult_t)notification_value;
}

void Camera_CaptureTask(void *argument)
{
    CameraCaptureRequest_t request;
    CameraCaptureResult_t result;

    (void)argument;
    s_camera_capture_task = xTaskGetCurrentTaskHandle();
    Camera_CaptureUpdateStackStats();

    for (;;)
    {
        if (xQueueReceive(s_camera_capture_request_queue, &request, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        s_camera_capture_stats.heartbeat_count++;
        result = Camera_CaptureExecute(request.timeout_ms);
        s_camera_capture_stats.last_result = result;
        Camera_CaptureUpdateStackStats();

        (void)xTaskNotify(
            request.requester,
            (uint32_t)result,
            eSetValueWithOverwrite);
        s_camera_capture_stats.heartbeat_count++;
    }
}

BaseType_t Camera_CaptureNotifyFrameCompleteFromISR(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_camera_capture_task != NULL)
    {
        s_camera_capture_isr_event = CAMERA_CAPTURE_ISR_EVENT_FRAME;
        vTaskNotifyGiveFromISR(s_camera_capture_task, &higher_priority_task_woken);
    }

    return higher_priority_task_woken;
}

BaseType_t Camera_CaptureNotifyErrorFromISR(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_camera_capture_task != NULL)
    {
        s_camera_capture_isr_event = CAMERA_CAPTURE_ISR_EVENT_ERROR;
        vTaskNotifyGiveFromISR(s_camera_capture_task, &higher_priority_task_woken);
    }

    return higher_priority_task_woken;
}

const CameraCaptureStats_t *Camera_CaptureGetStats(void)
{
    return &s_camera_capture_stats;
}
