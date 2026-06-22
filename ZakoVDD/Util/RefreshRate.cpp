#include "RefreshRate.h"

#include <cmath>

namespace
{
int gcd(int a, int b)
{
	while (b != 0)
	{
		int temp = b;
		b = a % b;
		a = temp;
	}
	return a;
}
}

void float_to_vsync(float refresh_rate, int& num, int& den)
{
	den = 10000;
	num = static_cast<int>(round(refresh_rate * den));

	int divisor = gcd(num, den);
	num /= divisor;
	den /= divisor;
}
