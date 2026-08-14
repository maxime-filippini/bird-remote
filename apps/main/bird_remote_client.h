#pragma once

#include <stdbool.h>

typedef void (*bird_remote_result_callback_t)(bool success, int status_code);

/* Starts one POST in a worker task. Returns false if a request is already in
 * flight or the task could not be created. The callback runs in that worker. */
bool bird_remote_next_async(bird_remote_result_callback_t callback);
bool bird_remote_request_in_flight(void);
