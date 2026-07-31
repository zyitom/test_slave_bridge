/*
 * printf replacement for the EtherCAT core1 probe image: format into a stack
 * buffer, push into the SHARE_RAM diagnostic ring, let core0 print it.
 *
 * WHY A HAND-ROLLED FORMATTER INSTEAD OF vsnprintf
 *
 * vsnprintf is the same newlib _vfprintf_r core that printf uses, so linking it
 * drags in exactly the two blocks this migration is trying to shed: newlib's
 * float formatting (~15.7 KB) and the libgcc soft-double it calls (~9.2 KB),
 * measured on this project's own map file (see ../../CORE_SWAP_MIGRATION.md
 * section 3.3). That is ~25 KB of core1 ILM to format integers. The newlib core
 * can also reach for the heap for its internal buffers, which makes it a poor
 * fit for a function that must stay callable from the PDI ISR.
 *
 * The alternative costs a few hundred bytes and covers every call site that
 * exists: the 12 printfs in the SDK EtherCAT port layer are plain strings with
 * no conversions at all, and this project's own log lines need integers and
 * strings. So the formatter below implements %s %d %u %x %X %c %% with an
 * optional minimum field width -- and nothing else. An unsupported conversion is
 * emitted verbatim rather than silently swallowed, so a future caller that
 * reaches past this subset shows up in the log instead of disappearing.
 */

#include "ecat_diag.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* One log line. Sized against the longest port-layer string (the 108-byte
 * "EtherCAT communication is possible even if the EEPROM is blank..." line)
 * with headroom, and small enough to sit on the PDI ISR stack. Output past this
 * is truncated, never written out of bounds. */
#define ECAT_DIAG_LINE_MAX (192U)

/* Cursor over the caller's line buffer. Overflow is recorded by letting `used`
 * saturate at `capacity`, so every emit path is a single bounds check. */
typedef struct {
    char* buffer;
    size_t capacity;
    size_t used;
} ecat_diag_sink_t;

static void diag_put(ecat_diag_sink_t* sink, char c) {
    if (sink->used < sink->capacity) {
        sink->buffer[sink->used] = c;
    }
    /* Counting past capacity would let a long line wrap the cursor; clamp. */
    if (sink->used < sink->capacity) {
        sink->used++;
    }
}

static void diag_put_string(ecat_diag_sink_t* sink, const char* text) {
    if (text == NULL) {
        text = "(null)";
    }
    while (*text != '\0') {
        diag_put(sink, *text++);
    }
}

/* Emit `value` in `base` (10 or 16), zero-extended to at least `width`
 * characters using `pad`. Digits are generated least-significant first into a
 * local buffer, so the maximum is 32 bits of binary -- 32 digits covers every
 * base >= 2 without a size assumption. */
static void diag_put_unsigned(
    ecat_diag_sink_t* sink, uint32_t value, uint32_t base, bool upper, uint32_t width,
    char pad) {
    static const char kLower[] = "0123456789abcdef";
    static const char kUpper[] = "0123456789ABCDEF";
    const char* digits = upper ? kUpper : kLower;

    char scratch[32];
    uint32_t length = 0;
    do {
        scratch[length++] = digits[value % base];
        value /= base;
    } while (value != 0U);

    for (uint32_t i = length; i < width; ++i) {
        diag_put(sink, pad);
    }
    while (length > 0U) {
        diag_put(sink, scratch[--length]);
    }
}

static void diag_put_signed(
    ecat_diag_sink_t* sink, int32_t value, uint32_t width, char pad) {
    /* Negate in unsigned space: -(int32_t)INT32_MIN is undefined behaviour. */
    if (value < 0) {
        const uint32_t magnitude = (uint32_t)0 - (uint32_t)value;
        /* The sign consumes one of the padded columns, matching printf. */
        diag_put(sink, '-');
        diag_put_unsigned(sink, magnitude, 10U, false, width > 0U ? width - 1U : 0U, pad);
        return;
    }
    diag_put_unsigned(sink, (uint32_t)value, 10U, false, width, pad);
}

/* Returns the formatted length clamped to the buffer size (i.e. the number of
 * bytes actually in `buffer`), not the untruncated length. Callers here only
 * forward it to the ring, so the snprintf-style "would have been" count would
 * buy nothing and could over-read. */
static size_t diag_format(char* buffer, size_t capacity, const char* format, va_list args) {
    ecat_diag_sink_t sink = {buffer, capacity, 0U};

    for (const char* p = format; *p != '\0'; ++p) {
        if (*p != '%') {
            diag_put(&sink, *p);
            continue;
        }

        ++p;
        if (*p == '\0') {
            /* Trailing '%': emit it rather than reading past the string. */
            diag_put(&sink, '%');
            break;
        }

        char pad = ' ';
        if (*p == '0') {
            pad = '0';
            ++p;
        }
        uint32_t width = 0U;
        while (*p >= '0' && *p <= '9') {
            width = (width * 10U) + (uint32_t)(*p - '0');
            ++p;
        }

        switch (*p) {
        case 's': diag_put_string(&sink, va_arg(args, const char*)); break;
        case 'd':
        case 'i': diag_put_signed(&sink, va_arg(args, int32_t), width, pad); break;
        case 'u': diag_put_unsigned(&sink, va_arg(args, uint32_t), 10U, false, width, pad); break;
        case 'x': diag_put_unsigned(&sink, va_arg(args, uint32_t), 16U, false, width, pad); break;
        case 'X': diag_put_unsigned(&sink, va_arg(args, uint32_t), 16U, true, width, pad); break;
        case 'c': diag_put(&sink, (char)va_arg(args, int)); break;
        case '%': diag_put(&sink, '%'); break;
        default:
            /* Unsupported conversion: reproduce it literally so the gap is
             * visible in the log instead of consuming an argument blindly (which
             * would desynchronise every remaining one). */
            diag_put(&sink, '%');
            diag_put(&sink, *p);
            break;
        }
    }

    return sink.used;
}

int ecat_diag_printf(const char* format, ...) {
    char line[ECAT_DIAG_LINE_MAX];

    va_list args;
    va_start(args, format);
    const size_t length = diag_format(line, sizeof(line), format, args);
    va_end(args);

    ecat_diag_write(line, length);
    return (int)length;
}

/* Strong override of the weak stub in the SDK's hpm_debug_console.c (that stub
 * is the CONFIG_NDEBUG_CONSOLE=1 build of the file, which is why a strong
 * definition here links instead of clashing -- see ../CMakeLists.txt). It backs
 * anything that still reaches newlib stdio despite the printf macro, so no code
 * path can end up in uart_send_byte(NULL, ...). */
int _write(int file, char* data, int size) {
    (void)file;
    if (size <= 0) {
        return 0;
    }
    ecat_diag_write(data, (size_t)size);
    return size;
}
