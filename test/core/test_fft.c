#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "modem/fft.h"

/*
 * The rebuilt FFT must be bit-identical to the inherited FourierTransform it is
 * ported from, because the busy detector's thresholds were tuned against its
 * exact output (including the reduced-M_PI twiddle factors). The original is
 * linked in as the oracle and compared over random inputs at every power-of-two
 * size the code uses.
 */

/* The inherited implementation from src/common/FFT.c. */
extern void FourierTransform(int NumSamples, float *RealIn, float *RealOut,
			     float *ImagOut, int InverseTransform);

static float frand(void)
{
	return (float)((double)rand() / RAND_MAX * 2.0 - 1.0);
}

static void compare_at(int n, bool inverse)
{
	float *in = malloc((size_t)n * sizeof(float));
	float *re = malloc((size_t)n * sizeof(float));
	float *im = malloc((size_t)n * sizeof(float));
	float *ore = malloc((size_t)n * sizeof(float));
	float *oim = malloc((size_t)n * sizeof(float));
	assert_non_null(in);
	assert_non_null(oim);

	for (int trial = 0; trial < 50; trial++) {
		for (int i = 0; i < n; i++)
			in[i] = frand() * 8000.0f;   /* audio-ish magnitudes. */

		ardop_fft(n, in, re, im, inverse);
		/* The oracle writes RealOut/ImagOut and reads RealIn; give it a
		 * private copy of the input so aliasing rules match ours. */
		float *in2 = malloc((size_t)n * sizeof(float));
		memcpy(in2, in, (size_t)n * sizeof(float));
		FourierTransform(n, in2, ore, oim, inverse ? 1 : 0);
		free(in2);

		for (int i = 0; i < n; i++) {
			assert_memory_equal(&re[i], &ore[i], sizeof(float));
			assert_memory_equal(&im[i], &oim[i], sizeof(float));
		}
	}

	free(in); free(re); free(im); free(ore); free(oim);
}

static void test_fft_forward_matches_oracle(void **state)
{
	(void)state;
	srand(1);
	compare_at(1024, false);   /* the busy-detector size. */
	compare_at(256, false);
	compare_at(64, false);
}

static void test_fft_inverse_matches_oracle(void **state)
{
	(void)state;
	srand(2);
	compare_at(1024, true);
	compare_at(128, true);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_fft_forward_matches_oracle),
		cmocka_unit_test(test_fft_inverse_matches_oracle),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
