#include "camera_process_task.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stddef.h>

static QueueHandle_t s_camera_process_request_queue;
static QueueHandle_t s_camera_process_result_queue;
static volatile uint32_t s_camera_process_request_outstanding;
static uint32_t s_camera_process_request_id;
static volatile uint32_t s_camera_process_stack_min_free_bytes;

static TickType_t Camera_ProcessRemainingTicks(TickType_t start_tick,
                                               TickType_t timeout_ticks)
{
    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;

    return (elapsed_ticks < timeout_ticks) ?
        (timeout_ticks - elapsed_ticks) : 0U;
}

static void Camera_ProcessUpdateStackStats(void)
{
    s_camera_process_stack_min_free_bytes =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL) *
        (uint32_t)sizeof(StackType_t);
}

static CameraProcessResult_t Camera_ProcessApplyRequest(
    const CameraProcessRequest_t *request)
{
    CameraImageProcessStatus_t process_result;

    if (request == NULL)
    {
        return CAMERA_PROCESS_RESULT_ERROR_NULL;
    }

    if ((request->mode < CAMERA_PROCESS_MODE_BYPASS) ||
        (request->mode > CAMERA_PROCESS_MODE_BINARY))
    {
        process_result = CAMERA_PROCESS_ERROR_MODE;
    }
    else
    {
        process_result = Camera_ImageProcess_ApplyToFrameBuffer(
            request->mode,
            request->threshold);
    }

    if (process_result != CAMERA_PROCESS_OK)
    {
        process_result = Camera_ImageProcess_ApplyToFrameBuffer(
            CAMERA_PROCESS_MODE_BYPASS,
            request->threshold);
    }

    return (CameraProcessResult_t)process_result;
}

bool Camera_ProcessTaskInit(void)
{
    s_camera_process_request_queue = xQueueCreate(
        CAMERA_PROCESS_REQUEST_QUEUE_DEPTH,
        sizeof(CameraProcessRequest_t));
    s_camera_process_result_queue = xQueueCreate(
        CAMERA_PROCESS_RESULT_QUEUE_DEPTH,
        sizeof(CameraProcessResponse_t));
    s_camera_process_request_outstanding = 0U;
    s_camera_process_request_id = 0U;
    s_camera_process_stack_min_free_bytes = 0U;

    return ((s_camera_process_request_queue != NULL) &&
            (s_camera_process_result_queue != NULL));
}

CameraProcessResult_t Camera_ProcessRequestFrame(CameraProcessMode_t mode,
                                                 uint8_t threshold,
                                                 uint32_t timeout_ms)
{
    CameraProcessRequest_t request;
    CameraProcessResponse_t response;
    TickType_t start_tick;
    TickType_t timeout_ticks;
    TickType_t remaining_ticks;

    if ((timeout_ms == 0U) ||
        (s_camera_process_request_queue == NULL) ||
        (s_camera_process_result_queue == NULL) ||
        (s_camera_process_request_outstanding != 0U))
    {
        return CAMERA_PROCESS_RESULT_NOT_READY;
    }

    while (xQueueReceive(
               s_camera_process_result_queue,
               &response,
               0U) == pdPASS)
    {
        /* Discard a completion left by a request whose caller timed out. */
    }

    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0U)
    {
        timeout_ticks = 1U;
    }
    start_tick = xTaskGetTickCount();

    s_camera_process_request_id++;
    request.request_id = s_camera_process_request_id;
    request.mode = mode;
    request.threshold = threshold;
    s_camera_process_request_outstanding = 1U;

    if (xQueueSend(
            s_camera_process_request_queue,
            &request,
            timeout_ticks) != pdPASS)
    {
        s_camera_process_request_outstanding = 0U;
        return CAMERA_PROCESS_RESULT_REQUEST_FAILED;
    }

    for (;;)
    {
        remaining_ticks = Camera_ProcessRemainingTicks(
            start_tick,
            timeout_ticks);
        if (remaining_ticks == 0U)
        {
            return CAMERA_PROCESS_RESULT_TIMEOUT;
        }

        if (xQueueReceive(
                s_camera_process_result_queue,
                &response,
                remaining_ticks) != pdPASS)
        {
            return CAMERA_PROCESS_RESULT_TIMEOUT;
        }

        if (response.request_id == request.request_id)
        {
            s_camera_process_request_outstanding = 0U;
            return response.result;
        }

        /* A stale response cannot complete the current correlated request. */
    }
}

void Camera_ProcessTask(void *argument)
{
    CameraProcessRequest_t request;
    CameraProcessResponse_t response;

    (void)argument;
    Camera_ProcessUpdateStackStats();

    for (;;)
    {
        if (xQueueReceive(
                s_camera_process_request_queue,
                &request,
                portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        response.request_id = request.request_id;
        response.result = Camera_ProcessApplyRequest(&request);
        Camera_ProcessUpdateStackStats();

        (void)xQueueSend(
            s_camera_process_result_queue,
            &response,
            0U);
        s_camera_process_request_outstanding = 0U;
    }
}

bool Camera_ProcessTaskIsIdle(void)
{
    return ((s_camera_process_request_queue != NULL) &&
            (s_camera_process_result_queue != NULL) &&
            (s_camera_process_request_outstanding == 0U));
}

uint32_t Camera_ProcessTaskGetStackMinFreeBytes(void)
{
    return s_camera_process_stack_min_free_bytes;
}
