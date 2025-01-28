#include <stdio.h>
#include <syslog.h>

int main(int argc, char* argv[])
{
	openlog(NULL, 0, LOG_USER);

	if (argc != 3) 
	{
		syslog(LOG_ERR, "script requires 2 args, has: %d args", argc-1);
		return 1;
	}

	FILE *fptr;
	fptr = fopen(argv[1], "w");

	if (fptr == NULL) 
	{
		syslog(LOG_ERR, "file could not be created");
		return 1;
	}

	syslog(LOG_DEBUG, "Writing %s to %s", argv[2], argv[1]);
	
	if (fprintf(fptr, "%s", argv[2]) < 0 )
	{
		syslog(LOG_ERR, "file could not be written to");
		fclose(fptr);
		return 1;
	}
	
	fclose(fptr);

	return 0;
}
