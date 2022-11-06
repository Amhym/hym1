#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include "httpd.h"
#include <string.h>

using namespace std;

void usage(char * argv0)
{
	cerr << "Usage: " << argv0 << " listen_port docroot_dir nopool/pool N" << endl;
}

int main(int argc, char *argv[])
{
	if (argc != 4 && argc != 5) {
		usage(argv[0]);
		return 1;
	}

	long int port = strtol(argv[1], NULL, 10);

	if (errno == EINVAL || errno == ERANGE) {
		usage(argv[0]);
		return 2;
	}

	if (port <= 0 || port > USHRT_MAX) {
		cerr << "Invalid port: " << port << endl;
		return 3;
	}

	string doc_root = argv[2];

	string str = "nopool";
	const char * st = str.c_str();
	if(strcmp(argv[3],"st") ){
		start_httpd(port,doc_root,0);
	}else{
		start_httpd(port,doc_root,atoi(argv[4]));
	}

	// start_httpd(port, doc_root);

	return 0;
}
