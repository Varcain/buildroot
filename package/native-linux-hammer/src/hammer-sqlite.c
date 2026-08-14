// SPDX-License-Identifier: MIT
/* Small SQLite shell subset for the memory-constrained NOMMU hammer. */

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_PREFIX "__HAMMER_SQLITE_STATUS__:"
#define SQLITE_HEAP_BYTES (240 * 1024)

/*
 * Linux/NOMMU anonymous mappings require physically contiguous pages.  Keep
 * all SQLite allocations in one segment acquired when this helper is exec'd,
 * before the other hammer workers can fragment the remaining page allocator.
 */
static unsigned char sqlite_heap[SQLITE_HEAP_BYTES]
	__attribute__((aligned(8)));

static int print_row(void *context, int columns, char **values, char **names)
{
	FILE *output = context;
	int column;

	(void)names;
	for (column = 0; column < columns; ++column)
		fprintf(output, "%s%s", column == 0 ? "" : "|",
			values[column] == NULL ? "" : values[column]);
	fputc('\n', output);
	return 0;
}

static int execute(sqlite3 *database, const char *sql, FILE *output)
{
	char *error = NULL;
	sqlite3_int64 memory_current;
	sqlite3_int64 memory_highwater;
	int status;

	status = sqlite3_exec(database, sql, print_row, output, &error);
	if (status != SQLITE_OK) {
		fprintf(stderr, "hammer-sqlite: exec: %s\n",
			error == NULL ? sqlite3_errmsg(database) : error);
		if (sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &memory_current,
				     &memory_highwater, 0) == SQLITE_OK)
			fprintf(stderr,
				"hammer-sqlite: memory current=%lld highwater=%lld heap=%u\n",
				(long long)memory_current,
				(long long)memory_highwater, SQLITE_HEAP_BYTES);
	}
	sqlite3_free(error);
	return status;
}

int main(int argc, char **argv)
{
	sqlite3 *database = NULL;
	FILE *input = stdin;
	FILE *output = stdout;
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	int batch;
	int status;

	if (argc != 3 && argc != 5) {
		fprintf(stderr,
			"usage: hammer-sqlite DATABASE SQL|--batch [INPUT OUTPUT]\n");
		return EXIT_FAILURE;
	}
	batch = argc == 5 && strcmp(argv[2], "--batch") == 0;
	if (argc == 5 && !batch)
		return EXIT_FAILURE;
	status = sqlite3_config(SQLITE_CONFIG_HEAP, sqlite_heap,
			       sizeof(sqlite_heap), 64);
	if (status != SQLITE_OK) {
		fprintf(stderr, "hammer-sqlite: configure fixed heap: %s\n",
			sqlite3_errstr(status));
		return status;
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
	if (!batch) {
		status = execute(database, argv[2], output);
	} else {
		/* Open FIFOs after exec so NOMMU vfork cannot suspend their peer. */
		input = fopen(argv[3], "r");
		if (input != NULL)
			output = fopen(argv[4], "w");
		if (input == NULL || output == NULL) {
			perror("hammer-sqlite: open batch FIFO");
			if (input != NULL)
				fclose(input);
			sqlite3_close(database);
			return EXIT_FAILURE;
		}
		status = SQLITE_OK;
		while ((length = getline(&line, &capacity, input)) >= 0) {
			while (length > 0 &&
			       (line[length - 1] == '\n' || line[length - 1] == '\r'))
				line[--length] = '\0';
			status = execute(database, line, output);
			fprintf(output, STATUS_PREFIX "%d\n", status);
			fflush(output);
		}
		free(line);
		if (ferror(input) && status == SQLITE_OK)
			status = SQLITE_IOERR_READ;
		fclose(input);
		fclose(output);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == SQLITE_OK)
		status = SQLITE_BUSY;
	return status == SQLITE_OK ? EXIT_SUCCESS : status;
}
