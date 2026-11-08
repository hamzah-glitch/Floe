/*
 * Floe runtime library (C).
 *
 * Compiled Floe programs (Phase 2) call these functions from the generated
 * LLVM IR and are linked with floe_runtime.o. The interpreter (Phase 1) uses
 * the printing and formatting functions too, so both phases print exactly the
 * same text.
 *
 * Arrays: every element occupies one 8-byte slot. An int is stored as is, a
 * float by its bit pattern, a bool as 0/1, and a string or nested array as a
 * pointer. Arrays are reference values and are never freed.
 */
#ifndef FLOE_RUNTIME_H
#define FLOE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FloeArray {
    int64_t length;
    int64_t capacity;
    int64_t* data;
} FloeArray;

/* Element kinds, used for printing, sorting and summing. */
enum { FLOE_INT = 0, FLOE_FLOAT = 1, FLOE_BOOL = 2, FLOE_STRING = 3 };

/* ---- errors ---- */
void floe_set_source_name(const char* name);
void floe_runtime_error(int64_t line, int64_t column, const char* message);
void floe_division_by_zero(int64_t line, int64_t column);

/* ---- arrays ---- */
FloeArray* floe_array_new(int64_t length);
int64_t floe_array_len(const FloeArray* array);
int64_t floe_array_get(const FloeArray* array, int64_t index, int64_t line, int64_t column);
void floe_array_set(FloeArray* array, int64_t index, int64_t value, int64_t line, int64_t column);
void floe_array_push(FloeArray* array, int64_t value);
FloeArray* floe_array_append(const FloeArray* array, int64_t value);
FloeArray* floe_array_slice(const FloeArray* array, int64_t start, int64_t end, int64_t line, int64_t column);
FloeArray* floe_array_reverse(const FloeArray* array);
FloeArray* floe_array_sort(const FloeArray* array, int32_t kind);
int64_t floe_array_sum(const FloeArray* array, int32_t kind);
int64_t floe_array_min(const FloeArray* array, int32_t kind, int64_t line, int64_t column);
int64_t floe_array_max(const FloeArray* array, int32_t kind, int64_t line, int64_t column);
FloeArray* floe_range(int64_t start, int64_t end);

/* ---- strings ---- */
char* floe_string_concat(const char* a, const char* b);
int64_t floe_string_compare(const char* a, const char* b);
int64_t floe_string_len(const char* s);
char* floe_int_to_string(int64_t value);
char* floe_float_to_string(double value);
char* floe_bool_to_string(int32_t value);

/* Shortest round-trippable text for a float; always shows a '.', e.g. 2.0. */
void floe_format_float(double value, char* buffer, size_t size);

/* ---- printing ---- */
void floe_print_int(int64_t value);
void floe_print_float(double value);
void floe_print_bool(int32_t value);
void floe_print_string(const char* value);
void floe_print_quoted(const char* value);
void floe_print_array(const FloeArray* array, int32_t kind, int32_t depth);
void floe_print_space(void);
void floe_print_newline(void);

#ifdef __cplusplus
}
#endif

#endif
