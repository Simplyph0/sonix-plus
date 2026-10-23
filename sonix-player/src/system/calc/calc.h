#ifndef CALC_H
#define CALC_H

#include <stdbool.h>
#include <stddef.h>

// The arithmetic behind the Calculator page, apart from any drawing.

// Evaluates an expression written with + - * / ^ and r, brackets, and numbers
// that may carry an exponent (1.5E3). The usual order applies: ^ and r first,
// then * and /, then + and -; a minus in front of a number is its sign, and
// -2^2 is -4. `A r B` is the B-th root of A. Spaces are ignored.
//
// False for an expression that is not one, and for one whose value is not a
// finite number (a division by zero, the root of a negative).
bool calc_eval(const char *expr, double *out);

// What the keys of the scientific page do to the number on the display.
typedef enum {
	CALC_SIN,
	CALC_COS,
	CALC_TAN,
	CALC_ASIN,
	CALC_ACOS,
	CALC_ATAN,
	CALC_SINH,
	CALC_COSH,
	CALC_TANH,
	CALC_ASINH,
	CALC_ACOSH,
	CALC_ATANH,
	CALC_LN,
	CALC_LOG10,
	CALC_LOG2,
	CALC_EXP,
	CALC_EXP10,
	CALC_EXP2,
	CALC_SQUARE,
	CALC_CUBE,
	CALC_RECIPROCAL,
	CALC_SQRT,
	CALC_CBRT,
	CALC_FACTORIAL,
} calc_fn_t;

// Applies one of them to `x`. The trigonometric ones take degrees, or give them,
// when `degrees` is set, and radians otherwise. False when the result is not a
// finite number.
bool calc_apply(calc_fn_t fn, double x, bool degrees, double *out);

// A number as the display shows it: up to ten significant digits, the integer
// part in groups of three, an exponent as "e20".
void calc_format(double value, char *out, size_t size);

// A number being typed ("1234.5", "-0.", "1.5E-3") with its integer part grouped
// the same way, and what the user has typed left as it is.
void calc_group_entry(const char *entry, char *out, size_t size);

#endif /* CALC_H */
