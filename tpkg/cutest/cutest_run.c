/*
	run the unit tests
	log
	31 jan 06 (WW): created file.
	21 feb 06 (MG): reworked for cutest
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tpkg/cutest/CuTest.h"

CuSuite * reg_cutest_rbtree(void);
CuSuite * reg_cutest_util(void);

/* dummy functions to link */
struct nsd;
int writepid(struct nsd * ATTR_UNUSED(nsd))
{
	return 0;
}
void bind8_stats(struct nsd * ATTR_UNUSED(nsd))
{
}

void runalltests(void)
{
	CuSuite *suite = CuSuiteNew();
	CuString *output = CuStringNew();

	CuSuiteAddSuite(suite, reg_cutest_rbtree());
	CuSuiteAddSuite(suite, reg_cutest_util());

	CuSuiteRun(suite);

        CuSuiteSummary(suite, output);
        CuSuiteDetails(suite, output);
        printf("%s\n", output->buffer);
}

int main(void)
{
	runalltests();
	return 1;
}
