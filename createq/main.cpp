#include <cstdio>
#include <cstdlib>
#include <sys/types.h>
#include <bits/localefwd.h>
#include "..//include//higplat.h"

void Usage(char* pszProgramName);

int main(int argc, char* argv[])
{
	char*  lpname;
	uint number, size, mode, type;
	lpname = NULL;
	number = 0;
	size = 0;
	mode = NORMAL_MODE;
	type = BINARY_TYPE;

	int i;
	for (i = 1; i < argc; i++)
	{
		if ((*argv[i] == '-') || (*argv[i] == '/'))
		{
			switch (tolower(*(argv[i] + 1)))
			{
			case 'n':
				number = (uint)atoi(argv[++i]);
				break;

			case 'l':
				size = (uint)atoi(argv[++i]);
				break;

			//case 'b':
			//	type = BINARY_TYPE;
			//	break;

			case 's':
				mode = SHIFT_MODE;
				break;
			// Help.
			case 'h':
			case '?':
			default:
				Usage(argv[0]);
				return 0;
			}
		}
		else
		{
			lpname = argv[i];
		}
	}
	if (lpname == NULL || size == 0 || number == 0)
	{
		Usage(argv[0]);
		return 0;
	}

	if (CreateQ(lpname, size, number, type, mode))
	{
		printf("Queue \'%s\' created successfully!\n", lpname);
		printf("size = %d, number = %d\n", size, number);
		return 0;
	}
	printf("Create queue \'%s\' fail with error code %d\n", lpname, GetLastErrorQ());
	return 0;
}


void Usage(char* pszProgramName)
{
	//fprintf(stderr, "Usage:  createq queue-name -n record-number -l record-size [-b] [-s]\n");
	fprintf(stderr, "Usage:  createq queue-name -n record-number -l record-size [-s]\n");
	fprintf(stderr, "Arguments:\n");
	fprintf(stderr,
		"\tqueue-name\tname of the queue(max 39 characters)\n");
	fprintf(stderr,
		"\trecord-number\trecord number\n");
	fprintf(stderr,
		"\trecord-size\trecord size(in bytes)\n");
	//fprintf(stderr,
	//	"\t-b\t\tbinary type with this switch, ascii type without it\n");
	fprintf(stderr,
		"\t-s\t\tshift mode with this switch, normal mode without it\n");
	fprintf(stderr,
		"Example:\tcreateq myqueue -n 100 -l 100\n");
}