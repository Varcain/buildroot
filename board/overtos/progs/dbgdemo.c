/* GDB source-level-debug demo for an FDPIC userspace program. */
#include <unistd.h>

static int add(int a, int b)
{
	return a + b; /* breakpoint target; backtrace: add <- compute <- main */
}

static int compute(int n)
{
	int s = 0;
	for (int i = 0; i < n; i++)
		s = add(s, i);
	return s;
}

int main(void)
{
	write(1, "dbgdemo start\n", 14);
	int r = compute(10); /* = 45 */
	char b[8];
	b[0] = 'r';
	b[1] = '=';
	b[2] = (char)('0' + (r / 10) % 10);
	b[3] = (char)('0' + r % 10);
	b[4] = '\n';
	write(1, b, 5);
	return 0;
}
