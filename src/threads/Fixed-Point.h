
#include <debug.h>
#include <list.h>
#include <stdint.h>
#define PN 17
#define QN 14
#define fraction (1 << (QN)) 

struct real
{
    int value;
};

// Convert x to integer (rounding toward zero): x / f
int convert_fixed_to_integer (struct real real );
// Convert x to integer (rounding to nearest): (x + f / 2) / f if x >= 0,
// (x - f / 2) / f if x <= 0.
int convert_fixed_to_integer_nearest(struct real real);
// Add x and y: x + y
struct real add_two_fixed_points(struct real x , struct real y);
// Subtract y from x: x - y
struct real subtract_two_fixed_points(struct real x , struct real y);
// Add x and n: x + n * f
struct real add_fixed_point_and_integer(struct real x , int n);
// Subtract n from x: x - n * f
struct real subtract_fixed_point_and_integer(struct real x , int n);
struct real subtract_fixed_point_and_integer_right( int n,struct real x);
// Multiply x by y: ((int64_t) x) * y / f
struct real multiply_two_fixed_points(struct real x , struct real y);
// Multiply x by n: x * n
struct real multiply_fixed_point_and_integer(struct real x , int n);
// Divide x by y: ((int64_t) x) * f / y
struct real divide_two_fixed_points(struct real x , struct real y);
// Divide x by n: x / n
struct real divide_fixed_point_and_integer(struct real x , int n);