#ifndef HTTPD_H
#define HTTPD_H

#include <string>

using namespace std;


void start_httpd(unsigned short port, string doc_root,int poolnum);

#endif // HTTPD_H
