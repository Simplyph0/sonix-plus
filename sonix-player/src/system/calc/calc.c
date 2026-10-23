#include "calc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// The expression
// ---------------------------------------------------------------------------
//
//     expression := term { ('+' | '-') term }
//     term       := unary { ('*' | '/') unary }
//     unary      := '-' unary | '+' unary | power
//     power      := primary [ ('^' | 'r') unary ]
//     primary    := number | '(' expression ')'
//
// An exponent takes a unary, so 2^-1 is 0.5 and 2^3^2 is 2^(3^2); a unary takes
// a power, so -2^2 is -(2^2).

typedef struct {
	const char *p;
	bool ok;
} parser_t;

static double parse_expression(parser_t *ps);

static void skip_space(parser_t *ps) {
	while (*ps->p == ' ') {
		ps->p++;
	}
}

static double parse_primary(parser_t *ps) {
	skip_space(ps);
	if (*ps->p == '(') {
		ps->p++;
		double v = parse_expression(ps);
		skip_space(ps);
		if (*ps->p != ')') {
			ps->ok = false;
			return 0;
		}
		ps->p++;
		return v;
	}
	// Only what a number starts with goes to strtod, which would otherwise read
	// "inf", "nan" and hexadecimal too.
	if (!isdigit((unsigned char)*ps->p) && *ps->p != '.') {
		ps->ok = false;
		return 0;
	}
	char *end;
	double v = strtod(ps->p, &end);
	if (end == ps->p) {
		ps->ok = false;
		return 0;
	}
	ps->p = end;
	return v;
}

static double parse_unary(parser_t *ps);

static double parse_power(parser_t *ps) {
	double base = parse_primary(ps);
	skip_space(ps);
	if (*ps->p == '^') {
		ps->p++;
		return pow(base, parse_unary(ps));
	}
	if (*ps->p == 'r') {
		ps->p++;
		double degree = parse_unary(ps);
		if (degree == 0) {
			ps->ok = false;
			return 0;
		}
		// The odd root of a negative number is real.
		if (base < 0 && fmod(degree, 2) != 0 && degree == floor(degree)) {
			return -pow(-base, 1.0 / degree);
		}
		return pow(base, 1.0 / degree);
	}
	return base;
}

static double parse_unary(parser_t *ps) {
	skip_space(ps);
	if (*ps->p == '-') {
		ps->p++;
		return -parse_unary(ps);
	}
	if (*ps->p == '+') {
		ps->p++;
		return parse_unary(ps);
	}
	return parse_power(ps);
}

static double parse_term(parser_t *ps) {
	double v = parse_unary(ps);
	for (;;) {
		skip_space(ps);
		if (*ps->p == '*') {
			ps->p++;
			v *= parse_unary(ps);
		} else if (*ps->p == '/') {
			ps->p++;
			double d = parse_unary(ps);
			if (d == 0) {
				ps->ok = false;
				return 0;
			}
			v /= d;
		} else {
			return v;
		}
	}
}

static double parse_expression(parser_t *ps) {
	double v = parse_term(ps);
	for (;;) {
		skip_space(ps);
		if (*ps->p == '+') {
			ps->p++;
			v += parse_term(ps);
		} else if (*ps->p == '-') {
			ps->p++;
			v -= parse_term(ps);
		} else {
			return v;
		}
	}
}

bool calc_eval(const char *expr, double *out) {
	parser_t ps = {expr ? expr : "", true};
	double v = parse_expression(&ps);
	skip_space(&ps);
	if (!ps.ok || *ps.p != '\0' || !isfinite(v)) {
		return false;
	}
	*out = v;
	return true;
}

// ---------------------------------------------------------------------------
// The keys of the scientific page
// ---------------------------------------------------------------------------

static double to_radians(double x, bool degrees) { return degrees ? x * M_PI / 180.0 : x; }
static double from_radians(double x, bool degrees) { return degrees ? x * 180.0 / M_PI : x; }

bool calc_apply(calc_fn_t fn, double x, bool degrees, double *out) {
	double r;
	switch (fn) {
	case CALC_SIN:
		r = sin(to_radians(x, degrees));
		break;
	case CALC_COS:
		r = cos(to_radians(x, degrees));
		break;
	case CALC_TAN:
		// A right angle has no tangent; the rounding of pi/2 would otherwise
		// give a huge number instead of saying so.
		if (degrees && fmod(fabs(x) - 90.0, 180.0) == 0) {
			return false;
		}
		r = tan(to_radians(x, degrees));
		break;
	case CALC_ASIN:
		r = from_radians(asin(x), degrees);
		break;
	case CALC_ACOS:
		r = from_radians(acos(x), degrees);
		break;
	case CALC_ATAN:
		r = from_radians(atan(x), degrees);
		break;
	case CALC_SINH:
		r = sinh(x);
		break;
	case CALC_COSH:
		r = cosh(x);
		break;
	case CALC_TANH:
		r = tanh(x);
		break;
	case CALC_ASINH:
		r = asinh(x);
		break;
	case CALC_ACOSH:
		r = acosh(x);
		break;
	case CALC_ATANH:
		r = atanh(x);
		break;
	case CALC_LN:
		r = log(x);
		break;
	case CALC_LOG10:
		r = log10(x);
		break;
	case CALC_LOG2:
		r = log2(x);
		break;
	case CALC_EXP:
		r = exp(x);
		break;
	case CALC_EXP10:
		r = pow(10.0, x);
		break;
	case CALC_EXP2:
		r = pow(2.0, x);
		break;
	case CALC_SQUARE:
		r = x * x;
		break;
	case CALC_CUBE:
		r = x * x * x;
		break;
	case CALC_RECIPROCAL:
		r = 1.0 / x;
		break;
	case CALC_SQRT:
		r = sqrt(x);
		break;
	case CALC_CBRT:
		r = cbrt(x);
		break;
	case CALC_FACTORIAL:
		if (x < 0 || x > 170) {
			return false;
		}
		r = x == floor(x) ? round(tgamma(x + 1.0)) : tgamma(x + 1.0);
		break;
	default:
		return false;
	}

	// sin(180 degrees) is a few parts in 10^16 rather than 0, which would show;
	// anything that small next to 1 is the arithmetic, not the answer.
	if (fabs(r) < 1e-15 && (fn == CALC_SIN || fn == CALC_COS || fn == CALC_TAN)) {
		r = 0;
	}
	if (!isfinite(r)) {
		return false;
	}
	*out = r;
	return true;
}

// ---------------------------------------------------------------------------
// Showing a number
// ---------------------------------------------------------------------------

// Puts commas into the run of digits at the start of `digits` (an integer part,
// with an optional minus before it), writing to `out`. Returns what was written.
static size_t group_integer(const char *digits, size_t length, char *out, size_t size) {
	size_t n = 0;
	size_t i = 0;
	if (length > 0 && digits[0] == '-') {
		if (n + 1 < size) {
			out[n++] = '-';
		}
		i = 1;
	}
	size_t count = length - i;
	for (; i < length; i++, count--) {
		if (n + 1 < size) {
			out[n++] = digits[i];
		}
		if (count > 1 && (count - 1) % 3 == 0 && n + 1 < size) {
			out[n++] = ',';
		}
	}
	return n;
}

// The same grouping for a whole number as text: its integer part, then the rest
// (a point and decimals, an exponent) untouched.
static void group_text(const char *text, char *out, size_t size) {
	size_t int_len = 0;
	size_t start = text[0] == '-' ? 1 : 0;
	while (isdigit((unsigned char)text[start + int_len])) {
		int_len++;
	}
	size_t n = group_integer(text, start + int_len, out, size);
	snprintf(out + n, size > n ? size - n : 0, "%s", text + start + int_len);
}

void calc_format(double value, char *out, size_t size) {
	if (value == 0) {
		value = 0; // not -0
	}
	char raw[48];
	snprintf(raw, sizeof(raw), "%.10g", value);

	// 1e+20 and 1e-05 read as 1e20 and 1e-5.
	char *e = strchr(raw, 'e');
	if (e) {
		char mantissa[40];
		snprintf(mantissa, sizeof(mantissa), "%.*s", (int)(e - raw), raw);
		const char *exp = e + 1;
		bool negative = *exp == '-';
		if (*exp == '+' || *exp == '-') {
			exp++;
		}
		while (*exp == '0' && exp[1]) {
			exp++;
		}
		snprintf(raw, sizeof(raw), "%se%s%s", mantissa, negative ? "-" : "", exp);
	}
	group_text(raw, out, size);
}

void calc_group_entry(const char *entry, char *out, size_t size) {
	group_text(entry, out, size);
}
