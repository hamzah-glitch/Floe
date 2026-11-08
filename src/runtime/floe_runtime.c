#include "floe_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* source_name = "<program>";

/* ===================================================================== */
/* Errors                                                                */
/* ===================================================================== */

void floe_set_source_name(const char* name) { source_name = name; }

void floe_runtime_error(int64_t line, int64_t column, const char* message) {
    fflush(stdout);
    fprintf(stderr, "%s:%" PRId64 ":%" PRId64 ": runtime error: %s\n", source_name, line, column, message);
    exit(1);
}

void floe_division_by_zero(int64_t line, int64_t column) {
    floe_runtime_error(line, column, "division by zero");
}

static void* checked_alloc(size_t bytes) {
    void* memory = calloc(1, bytes == 0 ? 1 : bytes);
    if (memory == NULL) {
        fflush(stdout);
        fprintf(stderr, "%s: runtime error: out of memory\n", source_name);
        exit(1);
    }
    return memory;
}

/* ===================================================================== */
/* Arrays                                                                */
/* ===================================================================== */

FloeArray* floe_array_new(int64_t length) {
    FloeArray* array = checked_alloc(sizeof(FloeArray));
    array->length = length;
    array->capacity = length < 4 ? 4 : length;
    array->data = checked_alloc((size_t)array->capacity * sizeof(int64_t));
    return array;
}

int64_t floe_array_len(const FloeArray* array) { return array->length; }

static void check_index(const FloeArray* array, int64_t index, int64_t line, int64_t column) {
    if (index < 0 || index >= array->length) {
        char message[128];
        snprintf(message, sizeof message, "index %" PRId64 " is out of bounds for an array of length %" PRId64,
                 index, array->length);
        floe_runtime_error(line, column, message);
    }
}

int64_t floe_array_get(const FloeArray* array, int64_t index, int64_t line, int64_t column) {
    check_index(array, index, line, column);
    return array->data[index];
}

void floe_array_set(FloeArray* array, int64_t index, int64_t value, int64_t line, int64_t column) {
    check_index(array, index, line, column);
    array->data[index] = value;
}

void floe_array_push(FloeArray* array, int64_t value) {
    if (array->length == array->capacity) {
        array->capacity *= 2;
        array->data = realloc(array->data, (size_t)array->capacity * sizeof(int64_t));
        if (array->data == NULL) {
            floe_runtime_error(0, 0, "out of memory");
        }
    }
    array->data[array->length++] = value;
}

static FloeArray* copy_range(const FloeArray* array, int64_t start, int64_t end) {
    FloeArray* copy = floe_array_new(end - start);
    if (end > start) {
        memcpy(copy->data, array->data + start, (size_t)(end - start) * sizeof(int64_t));
    }
    return copy;
}

/* append() returns a new array; the original is unchanged. */
FloeArray* floe_array_append(const FloeArray* array, int64_t value) {
    FloeArray* copy = copy_range(array, 0, array->length);
    floe_array_push(copy, value);
    return copy;
}

FloeArray* floe_array_slice(const FloeArray* array, int64_t start, int64_t end, int64_t line, int64_t column) {
    if (start < 0 || end < start || end > array->length) {
        char message[160];
        snprintf(message, sizeof message,
                 "slice [%" PRId64 "..%" PRId64 "] is out of bounds for an array of length %" PRId64,
                 start, end, array->length);
        floe_runtime_error(line, column, message);
    }
    return copy_range(array, start, end);
}

FloeArray* floe_array_reverse(const FloeArray* array) {
    FloeArray* copy = floe_array_new(array->length);
    for (int64_t i = 0; i < array->length; ++i) {
        copy->data[i] = array->data[array->length - 1 - i];
    }
    return copy;
}

static double as_float(int64_t slot) {
    double value;
    memcpy(&value, &slot, sizeof value);
    return value;
}

static int64_t from_float(double value) {
    int64_t slot;
    memcpy(&slot, &value, sizeof slot);
    return slot;
}

static int compare_ints(const void* a, const void* b) {
    const int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

static int compare_floats(const void* a, const void* b) {
    const double x = as_float(*(const int64_t*)a), y = as_float(*(const int64_t*)b);
    return (x > y) - (x < y);
}

static int compare_strings(const void* a, const void* b) {
    return strcmp((const char*)(intptr_t) * (const int64_t*)a, (const char*)(intptr_t) * (const int64_t*)b);
}

FloeArray* floe_array_sort(const FloeArray* array, int32_t kind) {
    FloeArray* copy = copy_range(array, 0, array->length);
    int (*compare)(const void*, const void*) =
        kind == FLOE_FLOAT ? compare_floats : kind == FLOE_STRING ? compare_strings : compare_ints;
    qsort(copy->data, (size_t)copy->length, sizeof(int64_t), compare);
    return copy;
}

int64_t floe_array_sum(const FloeArray* array, int32_t kind) {
    if (kind == FLOE_FLOAT) {
        double total = 0.0;
        for (int64_t i = 0; i < array->length; ++i) total += as_float(array->data[i]);
        return from_float(total);
    }
    uint64_t total = 0; /* wraps around on overflow, like + */
    for (int64_t i = 0; i < array->length; ++i) total += (uint64_t)array->data[i];
    return (int64_t)total;
}

static int64_t extreme(const FloeArray* array, int32_t kind, int want_max, int64_t line, int64_t column) {
    if (array->length == 0) {
        floe_runtime_error(line, column, want_max ? "max() of an empty array" : "min() of an empty array");
    }
    int64_t best = array->data[0];
    for (int64_t i = 1; i < array->length; ++i) {
        const int64_t v = array->data[i];
        const int better = kind == FLOE_FLOAT ? (want_max ? as_float(v) > as_float(best) : as_float(v) < as_float(best))
                                              : (want_max ? v > best : v < best);
        if (better) best = v;
    }
    return best;
}

int64_t floe_array_min(const FloeArray* array, int32_t kind, int64_t line, int64_t column) {
    return extreme(array, kind, 0, line, column);
}

int64_t floe_array_max(const FloeArray* array, int32_t kind, int64_t line, int64_t column) {
    return extreme(array, kind, 1, line, column);
}

FloeArray* floe_range(int64_t start, int64_t end) {
    FloeArray* array = floe_array_new(end > start ? end - start : 0);
    for (int64_t i = 0; i < array->length; ++i) array->data[i] = start + i;
    return array;
}

/* ===================================================================== */
/* Strings                                                               */
/* ===================================================================== */

char* floe_string_concat(const char* a, const char* b) {
    const size_t la = strlen(a), lb = strlen(b);
    char* result = checked_alloc(la + lb + 1);
    memcpy(result, a, la);
    memcpy(result + la, b, lb + 1);
    return result;
}

int64_t floe_string_compare(const char* a, const char* b) { return strcmp(a, b); }

int64_t floe_string_len(const char* s) { return (int64_t)strlen(s); }

static char* duplicate(const char* text) {
    char* copy = checked_alloc(strlen(text) + 1);
    strcpy(copy, text);
    return copy;
}

char* floe_int_to_string(int64_t value) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%" PRId64, value);
    return duplicate(buffer);
}

void floe_format_float(double value, char* buffer, size_t size) {
    snprintf(buffer, size, "%.15g", value);
    /* "2" -> "2.0" so floats never look like ints; leave inf, nan, 1e+20 alone. */
    if (strpbrk(buffer, ".eEni") == NULL) {
        strncat(buffer, ".0", size - strlen(buffer) - 1);
    }
}

char* floe_float_to_string(double value) {
    char buffer[64];
    floe_format_float(value, buffer, sizeof buffer);
    return duplicate(buffer);
}

char* floe_bool_to_string(int32_t value) { return duplicate(value ? "true" : "false"); }

/* ===================================================================== */
/* Printing                                                              */
/* ===================================================================== */

void floe_print_int(int64_t value) { printf("%" PRId64, value); }

void floe_print_float(double value) {
    char buffer[64];
    floe_format_float(value, buffer, sizeof buffer);
    fputs(buffer, stdout);
}

void floe_print_bool(int32_t value) { fputs(value ? "true" : "false", stdout); }

void floe_print_string(const char* value) { fputs(value, stdout); }

void floe_print_quoted(const char* value) { printf("\"%s\"", value); }

static void print_slot(int64_t slot, int32_t kind, int32_t depth) {
    if (depth > 0) {
        floe_print_array((const FloeArray*)(intptr_t)slot, kind, depth);
        return;
    }
    switch (kind) {
        case FLOE_INT: floe_print_int(slot); break;
        case FLOE_FLOAT: floe_print_float(as_float(slot)); break;
        case FLOE_BOOL: floe_print_bool(slot != 0); break;
        default: floe_print_quoted((const char*)(intptr_t)slot); break;
    }
}

/* [1, 2, 3]; `depth` is the array's own depth (int[] -> 1). */
void floe_print_array(const FloeArray* array, int32_t kind, int32_t depth) {
    putchar('[');
    for (int64_t i = 0; i < array->length; ++i) {
        if (i > 0) fputs(", ", stdout);
        print_slot(array->data[i], kind, depth - 1);
    }
    putchar(']');
}

void floe_print_space(void) { putchar(' '); }

void floe_print_newline(void) { putchar('\n'); }
