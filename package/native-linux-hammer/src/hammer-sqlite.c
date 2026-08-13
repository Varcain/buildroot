// SPDX-License-Identifier: MIT
/* Small SQLite shell subset for the memory-constrained NOMMU hammer. */

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>

static int print_row(void *unused, int columns, char **values, char **names)
{
	int column;

	(void)unused;
	(void)names;
	for (column = 0; column < columns; ++column)
		printf("%s%s", column == 0 ? "" : "|",
		       values[column] == NULL ? "" : values[column]);
	putchar('\n');
	return 0;
}

int main(int argc, char **argv)
{
	sqlite3 *database = NULL;
	char *error = NULL;
	int status;

	if (argc != 3) {
		fprintf(stderr, "usage: hammer-sqlite DATABASE SQL\n");
		return EXIT_FAILURE;
	}
	status = sqlite3_open_v2(argv[1], &database,
				 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (status != SQLITE_OK) {
		fprintf(stderr, "hammer-sqlite: open: %s\n",
			database == NULL ? sqlite3_errstr(status) :
			sqlite3_errmsg(database));
		if (database != NULL)
			sqlite3_close(database);
		return status == 0 ? EXIT_FAILURE : status;
	}
	status = sqlite3_exec(database, argv[2], print_row, NULL, &error);
	if (status != SQLITE_OK)
		fprintf(stderr, "hammer-sqlite: exec: %s\n",
			error == NULL ? sqlite3_errmsg(database) : error);
	sqlite3_free(error);
	if (sqlite3_close(database) != SQLITE_OK && status == SQLITE_OK)
		status = SQLITE_BUSY;
	return status == SQLITE_OK ? EXIT_SUCCESS : status;
}
