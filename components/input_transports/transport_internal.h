#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void serial_input_publish_status(const uint8_t *status, size_t len);
