/* Enough of Linux's GPIO character device (v2) for base_flash.c to build on a Mac for its tests (test/run.sh): the
 * tests log the lines (GPIO_LOG) and never call the ioctl. The radio's build uses the kernel's own header.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef STUB_LINUX_GPIO_H
#define STUB_LINUX_GPIO_H
#include <stdint.h>
#include <sys/ioctl.h>

#define GPIO_MAX_NAME_SIZE        32
#define GPIO_V2_LINES_MAX         64
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10
#define GPIO_V2_LINE_FLAG_OUTPUT  (1ULL << 3)
#define GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES 2

struct gpio_v2_line_attribute {
    uint32_t id;
    uint32_t padding;
    union {
        uint64_t flags;
        uint64_t values;
        uint32_t debounce_period_us;
    };
};

struct gpio_v2_line_config_attribute {
    struct gpio_v2_line_attribute attr;
    uint64_t                      mask;
};

struct gpio_v2_line_config {
    uint64_t                             flags;
    uint32_t                             num_attrs;
    uint32_t                             padding[5];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
};

struct gpio_v2_line_request {
    uint32_t                   offsets[GPIO_V2_LINES_MAX];
    char                       consumer[GPIO_MAX_NAME_SIZE];
    struct gpio_v2_line_config config;
    uint32_t                   num_lines;
    uint32_t                   event_buffer_size;
    uint32_t                   padding[5];
    int32_t                    fd;
};

#define GPIO_V2_GET_LINE_IOCTL _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#endif
