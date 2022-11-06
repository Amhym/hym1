#include <iostream>

#include "httpd.h"

using namespace std;

//功能测试宏:
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>//信号用作进程间通信
#include <dirent.h>
#include <sys/wait.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/epoll.h>
#include <map>
#include "threadpool.h"

// #define PORT          8080     /* Server port */
#define HEADER_SIZE   10240L /* Maximum size of a request header line */
#define CGI_POST      10240L /* Buffer size for reading POST data to CGI */
#define CGI_BUFFER    10240L /* Buffer size for reading CGI output */
#define FLAT_BUFFER   10240L /* Buffer size for reading flat files */

/*
 * Standard extensions
 */
#define ENABLE_EXTENSIONS
#ifdef  ENABLE_EXTENSIONS
#define ENABLE_CGI      1    /* Whether or not to enable CGI (also POST and HEAD) */
#define ENABLE_DEFAULTS 1    /* Whether or not to enable default index files (.php, .pl, .html) */
#else
#define ENABLE_CGI      0
#define ENABLE_DEFAULTS 0
#endif

/*
 * Default indexes and execution restrictions.
 */
#define INDEX_DEFAULTS  {(char*)"index.php", (char*)"index.pl", (char*)"index.py", (char*)"index.htm", (char*)"index.html", 0}
#define INDEX_EXECUTES  {          1,          1,          1,           0,            0, (unsigned int)-1}

/*
 * Directory to serve out of.
 */
// #define PAGES_DIRECTORY "htdocs"
#define VERSION_STRING  "klange/0.5"

// epoll max_num
#define OPEN_MAX 1024

//epoll tree root
int efd;

/*
 * Incoming request socket data
 */
struct socket_request {
	int                fd;       /* Socket itself */
	socklen_t          addr_len; /* Length of the address type */
	struct sockaddr_in address;  /* Remote address */
	pthread_t          thread;   /* Handler thread */
};
map<int,socket_request*> mp;

/*
 * CGI process data
 */
struct cgi_wait {
	int                fd;       /* Read */
	int                fd2;      /* Write */
	int                pid;      /* Process ID */
};

/*
 * Server socket.
 */
int serversock;

/*
 * Port
 */
int port;
// char* docs;
string docs;
//是否使用线程池
bool has_pool=false;

/*
 * Last unaccepted socket pointer
 * so we can free it.
 */
void * _last_unaccepted;

/*
 * Better safe than sorry,
 * shutdown the socket and exit.
 */
void handleShutdown(int sig) {
	printf("\n[info] Shutting down.\n");

	/*
	 * Shutdown the socket.
	 */
	shutdown(serversock, SHUT_RDWR);
	close(serversock);
	// cout<<serversock<<"关闭"<<endl;

	/*
	 * Free the thread data block
	 * for the next expected connection.
	 */
	free(_last_unaccepted);

	/*
	 * Exit.
	 */
	exit(sig);
}

/*
 * Resizeable vector
 */
typedef struct {
	void ** buffer;
	unsigned int size;
	unsigned int alloc_size;
} vector_t;

#define INIT_VEC_SIZE 1024

vector_t * alloc_vector(void) {
	vector_t* v = (vector_t *) malloc(sizeof(vector_t));
	v->buffer = (void **) malloc(INIT_VEC_SIZE * sizeof(void *));
	v->size = 0;
	v->alloc_size = INIT_VEC_SIZE;

	return v;
}

void free_vector(vector_t* v) {
	free(v->buffer);
	free(v);
}

void vector_append(vector_t * v, void * item) {
	//如果向量内存不够了，就拓展为原来的两倍
	if(v->size == v->alloc_size) {
		v->alloc_size = v->alloc_size * 2;
		v->buffer = (void **) realloc(v->buffer, v->alloc_size * sizeof(void *));
	}

	v->buffer[v->size] = item;
	v->size++;
}

void * vector_at(vector_t * v, unsigned int idx) {
	return idx >= v->size ? NULL : v->buffer[idx];
}

/*
 * Delete a vector
 * Free its contents and then it.
 */
void delete_vector(vector_t * vector) {
	unsigned int i = 0;
	for (i = 0; i < vector->size; ++i) {
		free(vector_at(vector, i));
	}
	free_vector(vector);
}

/*
 * Convert a character from two hex digits
 * to the raw character. (URL decode)
 */
char from_hex(char ch) {
	return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*
 * Generic text-only response with a particular status.
 * Used for bad requests mostly.
 */
void generic_response(FILE * socket_stream, char * status, char * message) {
	fprintf(socket_stream,
			"HTTP/1.1 %s\r\n"
			"Server: " VERSION_STRING "\r\n"
			"Content-Type: text/plain\r\n"
			"Content-Length: %zu\r\n"
			// "Connection: Keep-Alive\r\n"
			// "Keep-Alive: max=5, timeout=120\r\n"
			"\r\n"
			"%s\r\n", status, strlen(message), message);
}

/*
 * Wait for a CGI thread to finish and
 * close its pipe.
 */
void *wait_pid(void * onwhat) {
	struct cgi_wait * cgi_w = (struct cgi_wait*)onwhat;
	int status;

	/*
	 * Wait for the process to finish
	 */
	waitpid(cgi_w->pid, &status, 0);

	/*
	 * Close the respective pipe
	 */
	close(cgi_w->fd);
	close(cgi_w->fd2);

	/*
	 * Free the data we were sent.
	 */
	free(onwhat);
	return NULL;
}

/*
 * Handle an incoming connection request.
 */
void *handleRequest(void *socket) {
	struct socket_request * request = (struct socket_request *)socket;

	/*
	 * Convert the socket into a standard file descriptor
	 */
	FILE *socket_stream = NULL;
	socket_stream = fdopen(request->fd, "r+");
	if (!socket_stream) {
		// fprintf(stderr,"Ran out of a file descriptors, can not respond to request.\n");
		goto _disconnect;
	};

	/*
	 * Read requests until the client disconnects.
	 */
	while (1) {
		vector_t * queue = alloc_vector();
		char buf[HEADER_SIZE];
		//feof()测试给定流 socket_stream 的文件结束标识符,当设置了与流关联的文件结束标识符时，该函数返回一个非零值，否则返回零。判断连接是否断开
		while (!feof(socket_stream)) {
			/*
			 * While the client has not yet disconnected,
			 * read request headers into the queue.
			 */
			//这个字符数组的长度是HEADER_SIZE - 1，最后一位已经默认是'\0',加上换行符就-2
			char * in = fgets( buf, HEADER_SIZE - 2, socket_stream );

			if (!in) {
				/*
				 * EOF
				 */
				break;
			}

			if (!strcmp(in, "\r\n") || !strcmp(in,"\n")) {
				/*
				 * Reached end of headers.
				 */
				break;
			}

			if (!strstr(in, "\n")) {
				/*
				 * Oversized request line.
				 */
				generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: Request line was too long.");
				delete_vector(queue);
				goto _disconnect;
			}
			/*
			 * Store the request line in the queue for this request.
			 */
			char * request_line = (char*)malloc((strlen(buf)+1) * sizeof(char));
			strcpy(request_line, buf);
			vector_append(queue, (void*)request_line);
		}

		if (feof(socket_stream)) {
			/*
			 * End of stream -> Client closed connection.
			 */

			// cout<<"Client closed connection,queue deleted."<<endl;
			delete_vector(queue);
			break;
		}

		/*
		 * Request variables
		 */
		char * filename          = NULL; /* Filename as received (ie, /index.php) */
		char * querystring       = NULL; /* Query string, URL encoded */
		int request_type         = 0;    /* Request type, 0=GET, 1=POST, 2=HEAD ... */
		char * _filename         = NULL; /* Filename relative to server (ie, pages/index.php) */
		char * ext               = NULL; /* Extension for requested file */
		char * host              = NULL; /* Hostname for request, if supplied. */
		char * http_version      = NULL; /* HTTP version used in request */
		unsigned long c_length   = 0L;   /* Content-Length, usually for POST */
		char * c_type            = NULL; /* Content-Type, usually for POST */
		char * c_cookie          = NULL; /* HTTP_COOKIE */
		char * c_uagent          = NULL; /* User-Agent, for CGI */
		char * c_referer         = NULL; /* Referer, for CGI */

		/*
		 * Process headers
		 */
		unsigned int i = 0;
		for (i = 0; i < queue->size; ++i) {
			char * str = (char*)(vector_at(queue,i));

			/*
			 * Find the colon for a header
			 */
			//cout<<"str是vector_at(queue,"<<i<<")------"<<str<<endl;
			char * colon = strstr(str,": ");
			
			if (!colon) {
				if (i > 0) {
					/*
					 * Request string outside of first entry.
					 */
					generic_response(socket_stream,(char *)"400 Bad Request", (char *)"Bad request: A header line was missing colon.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * Request type
				 */
				int r_type_width = 0;
				switch (str[0]) {
					case 'G':
						if (strstr(str, "GET ") == str) {
							/*
							 * GET: Retreive file
							 */
							r_type_width = 4;
							request_type = 1;
						} else {
							goto _unsupported;
						}
						break;
#if ENABLE_CGI
					case 'P':
						if (strstr(str, "POST ") == str) {
							/*
							 * POST: Send data to CGI
							 */
							r_type_width = 5;
							request_type = 2;
						} else {
							goto _unsupported;
						}
						break;
					case 'H':
						if (strstr(str, "HEAD ") == str) {
							/*
							 * HEAD: Retreive headers only
							 */
							r_type_width = 5;
							request_type = 3;
						} else {
							goto _unsupported;
						}
						break;
#endif
					default:
						/*
						 * Unsupported method.
						 */
						goto _unsupported;
						break;
				}

				filename = str + r_type_width;
				if (filename[0] == ' ' || filename[0] == '\r' || filename[0] == '\n') {
					/*
					 * Request was missing a filename or was in a form we don't want to handle.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No filename.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * Get the HTTP version.
				 */
				http_version = strstr(filename, "HTTP/");
				if (!http_version) {
					/*
					 * No HTTP version was present in the request.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No HTTP version supplied.");
					delete_vector(queue);
					goto _disconnect;
				}
				http_version[-1] = '\0';
				char * tmp_newline;
				tmp_newline = strstr(http_version, "\r\n");
				if (tmp_newline) {
					tmp_newline[0] = '\0';
				}
				tmp_newline = strstr(http_version, "\n");
				if (tmp_newline) {
					tmp_newline[0] = '\0';
				}
				/*
				 * Get the query string.
				 */
				querystring = strstr(filename, "?");
				if (querystring) {
					querystring++;
					querystring[-1] = '\0';
				}
			} else {

				if (i == 0) {
					/*
					 * Non-request line on first line.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: First line was not a request.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * Split up the header.
				 */
				colon[0] = '\0';
				colon += 2;
				char * eol = strstr(colon,"\r");
				if (eol) {
					eol[0] = '\0';
					eol[1] = '\0';
				} else {
					eol = strstr(colon,"\n");
					if (eol) {
						eol[0] = '\0';
					}
				}

				/*
				 * Process the header
				 * str: colon
				 */
				if (!strcmp(str, "Host")) {
					/*
					 * Host: The hostname of the (virtual) host the request was for.
					 */
					host = colon;
				} else if (!strcmp(str, "Content-Length")) {
					/*
					 * Content-Length: Length of message (after these headers) in bytes.
					 */
					c_length = atol(colon);
				} else if (!strcmp(str, "Content-Type")) {
					/*
					 * Content-Type: MIME-type of the message.
					 */
					c_type = colon;
				} else if (!strcmp(str, "Cookie")) {
					/*
					 * Cookie: CGI cookies
					 */
					c_cookie = colon;
				} else if (!strcmp(str, "User-Agent")) {
					/*
					 * Client user-agent string
					 */
					c_uagent = colon;
				} else if (!strcmp(str, "Referer")) {
					/*
					 * Referer page
					 */
					c_referer = colon;
				}
			}
		}

		/*
		 * All Headers have been read
		 */
		if (!request_type) {
_unsupported:
			/*
			 * We did not understand the request
			 */
			generic_response(socket_stream, (char *)"501 Not Implemented", (char *)"Not implemented: The request type sent is not understood by the server.");
			delete_vector(queue);
			goto _disconnect;
		}

		if (!filename || strstr(filename, "'") || strstr(filename," ") ||
			(querystring && strstr(querystring," "))) {
			/*
			 * If a filename wasn't specified, we received
			 * an invalid or malformed request and we should
			 * probably dump it.
			 */
			generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No filename provided.");
			delete_vector(queue);
			goto _disconnect;
		}

		/*
		 * Get some important information on the requested file
		 * _filename: the local file name, relative to `.`
		 */
		_filename = (char*)calloc(sizeof(char) * (strlen(docs.c_str()) + strlen(filename) + 2), 1);
		strcat(_filename, docs.c_str());
		strcat(_filename, filename);
		cout<<_filename<<"-----"<<filename<<endl;//htdocs/big.jpg-----/big.jpg
		if (strstr(_filename, "%")) {
			/*
			 * Convert from URL encoded string.
			 */
			char * buf = (char*)malloc(strlen(_filename) + 1);
			char * pstr = _filename;
			char * pbuf = buf;
			while (*pstr) {
				if (*pstr == '%') {
					if (pstr[1] && pstr[2]) {
						*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
						pstr += 2;
					}
				} else if (*pstr == '+') { 
					*pbuf++ = ' ';
				} else {
					*pbuf++ = *pstr;
				}
				pstr++;
			}
			*pbuf = '\0';
			free(_filename);
			_filename = buf;
		}

		/*
		 * Reject paths that attempt to make relative jumps.
		 * HTTP clients are not supposed to request files like this, so this should be valid.
		 * Because we already put PAGES_DIRECTORY at the front, we should be able to guarantee
		 * that there is a / before any user-supplied directory, so contains("/../") or endswith("/..")
		 * should be an appropriate restriction on user-supplied paths.
		 */
		if (strstr(_filename, "/../") || (strstr(_filename, "/..") == _filename + strlen(_filename) - 3)) {
			generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request");
			free(_filename);
			delete_vector(queue);
			goto _disconnect;
		}

		/*
		 * ext: the file extension, or NULL if it lacks one
		 */
		ext = filename + 1;
		while (strstr(ext+1,".")) {
			ext = strstr(ext+1,".");
		}
		if (ext == filename + 1) {
			/*
			 * Either we didn't find a dot,
			 * or that dot is at the front.
			 * If the dot is at the front, it is not an extension,
			 * but rather an extension-less hidden file.
			 */
			ext = NULL;
		}

		/*
		 * Check if it's a directory (reliably)
		 */
		struct stat stats;
		if (stat(_filename, &stats) == 0 && S_ISDIR(stats.st_mode)) {
			if (_filename[strlen(_filename)-1] != '/') {
				/*
				 * Request for a directory without a trailing /.
				 * Throw a 'moved permanently' and redirect the client
				 * to the directory /with/ the /.
				 */
				fprintf(socket_stream, "HTTP/1.1 301 Moved Permanently\r\n");
				fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
				fprintf(socket_stream, "Location: %s/\r\n", filename);
				fprintf(socket_stream, "Content-Length: 0\r\n\r\n");
			} else {

#if ENABLE_DEFAULTS
				/*
				 * Check for default indexes.
				 */
				struct stat extra_stats;//获取文件信息，成功：0，失败：1；
				char* index_php=NULL;
				index_php=(char*)malloc(strlen(_filename) + 30);

				/*
				 * The types and exection properties of index files
				 * are describe in a #define at the top of this file.
				 */
				char *       index_defaults[] = INDEX_DEFAULTS;
				unsigned int index_executes[] = INDEX_EXECUTES;
				unsigned int index = 0;

				while (index_defaults[index] != (char *)0) {
					index_php[0] = '\0';
					strcat(index_php, _filename);
					strcat(index_php, index_defaults[index]);
					//cout<<index_php<<"====="<<index_defaults[index]<<"===="<<index_executes[index]<<"===="<<extra_stats.st_mode<<"===="<<S_IXOTH<<endl;
					if ((stat(index_php, &extra_stats) == 0) && ((extra_stats.st_mode & S_IXOTH) == index_executes[index])) {
						//S_IXOTH:其他用户具可执行权限,st_mode成员，该成员描述了文件的类型和权限两个属性
						/*
						 * This index exists, use it instead of the directory listing.
						 */
						//cout<<"index:================="<<index<<endl;
						_filename = (char*)realloc(_filename, strlen(index_php)+1);
						stats = extra_stats;
						memcpy(_filename, index_php, strlen(index_php)+1);
						ext = _filename;
						while (strstr(ext+1,".")) {
							ext = strstr(ext+1,".");
						}
						goto _use_file;
					}
					++index;
				}
#endif

				/*
				 * This is a directory, and we were requested properly.
				 * A default file was not found, so display a listing.
				 */
				struct dirent **files = {0};
				int filecount = -1;
				filecount = scandir(_filename, &files, 0, alphasort);

				/*
				 * Prepare to print directory listing.
				 */
				fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
				fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
				fprintf(socket_stream, "Content-Type: text/html\r\n");

				/*
				 * Allocate some memory for the HTML
				 */
				char * listing = (char*)malloc(1024);
				listing[0] = '\0';
				strcat(listing, "<!doctype html><html><head><title>Directory Listing</title></head><body>");
				int i = 0;
				for (i = 0; i < filecount; ++i) {
					/*
					 * Get the full name relative the server so we can stat
					 * this entry to see if it's a directory.
					 */
					char * _fullname=(char*)malloc(strlen(_filename) + 1 + strlen(files[i]->d_name) + 1);
					sprintf(_fullname, "%s/%s", _filename, files[i]->d_name);
					if (stat(_fullname, &stats) == 0 && S_ISDIR(stats.st_mode)) {
						/*
						 * Ignore directories.
						 */
						free(files[i]);
						continue;
					}

					/*
					 * Append a link to the file.
					 */
					char *_file=(char*)malloc(2 * strlen(files[i]->d_name) + 64);
					sprintf(_file, "<a href=\"%s\">%s</a><br>\n", files[i]->d_name, files[i]->d_name);
					listing = (char*)realloc(listing, strlen(listing) + strlen(_file) + 1);
					strcat(listing, _file);
					free(files[i]);
				}
				free(files);

				/*
				 * Close up our HTML
				 */
				listing = (char*)realloc(listing, strlen(listing) + 64);
				strcat(listing,"</body></html>");

				/*
				 * Send out the listing.
				 */
				fprintf(socket_stream, "Content-Length: %zu\r\n", (sizeof(char) * strlen(listing)));
				fprintf(socket_stream, "\r\n");
				fprintf(socket_stream, "%s", listing);
				free(listing);
			}
		} else {
_use_file:
			;
			/*
			 * Open the requested file.
			 */
			FILE * content = fopen(_filename, "rb");
			if (!content) {
				/*
				 * Could not open file - 404. (Perhaps 403)
				 */
				string pa = docs+"/404.htm";
				content = fopen(pa.c_str(), "rb");

				if (!content) {
					/*
					 * If the expected default 404 page was not found
					 * return the generic one and move to the next response.
					 */
					generic_response(socket_stream, (char *)"404 File Not Found", (char *)"The requested file could not be found.");
					goto _next;
				}

				/*
				 * Replace the internal filenames with the 404 page
				 * and continue to load it.
				 */
				fprintf(socket_stream, "HTTP/1.1 404 File Not Found\r\n");
				_filename = (char*)realloc(_filename, strlen((docs+ "/404.htm").c_str()) + 1);
				_filename[0] = '\0';
				strcat(_filename, (docs+"/404.htm").c_str());
				ext = strstr(_filename, ".");
			} else {
				/*
				 * We're good to go.
				 */
#if ENABLE_CGI
				if (stats.st_mode & S_IXOTH) {
					/*
					 * CGI Executable
					 * Close the file
					 */
					fclose(content);

					/*
					 * Prepare pipes.
					 */
					int cgi_pipe_r[2];
					int cgi_pipe_w[2];
					if (pipe(cgi_pipe_r) < 0) {
						fprintf(stderr, "Failed to create read pipe!\n");
					}
					if (pipe(cgi_pipe_w) < 0) {
						fprintf(stderr, "Failed to create write pipe!\n");
					}

					/*
					 * Fork.
					 */
					pid_t _pid = 0;
					_pid = fork();
					if (_pid == 0) {
						/*
						 * Set pipes
						 */
						dup2(cgi_pipe_r[0],STDIN_FILENO);//重定向
						dup2(cgi_pipe_w[1],STDOUT_FILENO);
						/*
						 * This is actually cheating on my pipe.
						 */
						fprintf(stdout, "Expires: -1\r\n");

						/*
						 * Operate in the correct directory.
						 */
						char * dir = _filename;
						while (strstr(_filename,"/")) {
							_filename = strstr(_filename,"/") + 1;
						}
						_filename[-1] = '\0';
						char docroot[1024];
						getcwd(docroot, 1023);
						strcat(docroot, ("/"+docs).c_str());
						chdir(dir);

						/*
						 * Set CGI environment variables.
						 * CONTENT_LENGTH    : POST message length
						 * CONTENT_TYPE      : POST encoding type
						 * DOCUMENT_ROOT     : the root directory
						 * GATEWAY_INTERFACE : The CGI version (CGI/1.1)
						 * HTTP_COOKIE       : Cookies provided by client
						 * HTTP_HOST         : Same as above
						 * HTTP_REFERER      : Referer page.
						 * HTTP_USER_AGENT   : Browser user agent
						 * PATH_TRANSLATED   : On-disk file path
						 * QUERY_STRING      : /file.ext?this_stuff&here
						 * REDIRECT_STATUS   : HTTP status of CGI redirection (PHP)
						 * REMOTE_ADDR       : IP of remote user
						 * REMOTE_HOST       : Hostname of remote user (reverse DNS)
						 * REQUEST_METHOD    : GET, POST, HEAD, etc.
						 * SCRIPT_FILENAME   : Same as PATH_TRANSLATED (PHP, primarily)
						 * SCRIPT_NAME       : Request file path
						 * SERVER_NAME       : Our hostname or Host: header
						 * SERVER_PORT       : TCP host port
						 * SERVER_PROTOCOL   : The HTTP version of the request
						 * SERVER_SOFTWARE   : Our application name and version
						 */
						setenv("SERVER_SOFTWARE", VERSION_STRING, 1);//setenv()增加或者修改环境变量
						if (!host) {
							char hostname[1024];
							hostname[1023]='\0';
							gethostname(hostname, 1023);
							setenv("SERVER_NAME", hostname, 1);
							setenv("HTTP_HOST",   hostname, 1);
						} else {
							setenv("SERVER_NAME", host, 1);
							setenv("HTTP_HOST",   host, 1);
						}
						setenv("DOCUMENT_ROOT", docroot, 1);
						setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
						setenv("SERVER_PROTOCOL", http_version, 1);
						char port_string[20];
						sprintf(port_string, "%d", port);
						setenv("SERVER_PORT", port_string, 1);
						if (request_type == 1) {
							setenv("REQUEST_METHOD", "GET", 1);
						} else if (request_type == 2) {
							setenv("REQUEST_METHOD", "POST", 1); 
						} else if (request_type == 3) {
							setenv("REQUEST_METHOD", "HEAD", 1);
						}//三种请求方式
						if (querystring) {
							if (strlen(querystring)) {
								setenv("QUERY_STRING", querystring, 1);
							} else {
								setenv("QUERY_STRING", "", 1);
							}
						}
						char *fullpath=(char*)malloc(1024 + strlen(_filename));
						getcwd(fullpath, 1023);
						strcat(fullpath, "/");
						strcat(fullpath, _filename);
						setenv("PATH_TRANSLATED", fullpath, 1);
						setenv("SCRIPT_NAME", filename, 1);
						setenv("SCRIPT_FILENAME", fullpath, 1);
						setenv("REDIRECT_STATUS", "200", 1);
						char c_lengths[100];
						c_lengths[0] = '\0';
						sprintf(c_lengths, "%lu", c_length);
						setenv("CONTENT_LENGTH", c_lengths, 1);
						if (c_type) {
							setenv("CONTENT_TYPE", c_type, 1);
						}
						struct hostent * client;
						client = gethostbyaddr((const char *)&request->address.sin_addr.s_addr,
								sizeof(request->address.sin_addr.s_addr), AF_INET);
						if (client != NULL) {
							setenv("REMOTE_HOST", client->h_name, 1);
						}
						setenv("REMOTE_ADDR", inet_ntoa(request->address.sin_addr), 1);
						if (c_cookie) {
							setenv("HTTP_COOKIE", c_cookie, 1);
						}
						if (c_uagent) {
							setenv("HTTP_USER_AGENT", c_uagent, 1);
						}
						if (c_referer) {
							setenv("HTTP_REFERER", c_referer, 1);
						}

						/*
						 * Execute.
						 */
						char executable[1024];
						executable[0] = '\0';
						sprintf(executable, "./%s", _filename);
						execlp(executable, executable, (char *)NULL);

						/*
						 * The CGI application failed to execute. ;_;
						 * This is a bad thing.
						 */
						fprintf(stderr,"[warn] Failed to execute CGI script: %s?%s.\n", fullpath, querystring);

						/*
						 * Clean the crap from the original process.
						 */
						delete_vector(queue);
						free(dir);
						free(_last_unaccepted);
						if(has_pool){

						}else{
							pthread_detach(request->thread);
						}
						// pthread_detach(request->thread);
						free(request);

						/*
						 * Our thread back in the main process should be fine.
						 */
						return NULL;
					}

					/*
					 * We are the server thread.
					 * Open a thread to close the other end of the pipe
					 * when the CGI application finishes executing.
					 */
					struct cgi_wait * cgi_w = (cgi_wait *)malloc(sizeof(struct cgi_wait));
					cgi_w->pid = _pid;
					cgi_w->fd  = cgi_pipe_w[1];
					cgi_w->fd2 = cgi_pipe_r[0];
					pthread_t _waitthread;
					if(has_pool){
						threadpool_add_task(NULL, wait_pid, (void *)cgi_w);
					}else{
						pthread_create(&_waitthread, NULL, wait_pid, (void *)(cgi_w));
					}
					// pthread_create(&_waitthread, NULL, wait_pid, (void *)(cgi_w));

					/*
					 * Open our end of the pipes.
					 * We map cgi_pipe for reading the output from the CGI application.
					 * cgi_pipe_post is mapped to the stdin for the CGI application
					 * and we pipe our POST data (if there is any) here.
					 */
					FILE * cgi_pipe = fdopen(cgi_pipe_w[0], "r");
					FILE * cgi_pipe_post = fdopen(cgi_pipe_r[1], "w");

					if (c_length > 0) {
						/*
						 * Write the POST data to the application.
						 */
						size_t total_read = 0;
						char buf[CGI_POST];
						while ((total_read < c_length) && (!feof(socket_stream))) {
							size_t diff = c_length - total_read;
							if (diff > CGI_POST) {
								/*
								 * If there's more than our buffer left,
								 * obviously, only read enough for the buffer.
								 */
								diff = CGI_POST;
							}
							size_t read;
							read = fread(buf, 1, diff, socket_stream);
							total_read += read;
							/*
							 * Write to the CGI pipe
							 */
							fwrite(buf, 1, read, cgi_pipe_post);
						}
					}
					if (cgi_pipe_post) {
						/*
						 * If we need to, close the pipe.
						 */
						fclose(cgi_pipe_post);
					}

					/*
					 * Read the headers from the CGI application.
					 */
					char buf[CGI_BUFFER];
					if (!cgi_pipe) {
						generic_response(socket_stream, (char *)"500 Internal Server Error", (char *)"Failed to execute CGI script.");
						if(has_pool){

						}else{
							pthread_detach(_waitthread);
						}
						// pthread_detach(_waitthread);
						goto _next;
					}
					fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
					fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
					unsigned int j = 0;
					while (!feof(cgi_pipe)) {
						/*
						 * Read until we are out of headers.
						 */
						char * in = fgets(buf, CGI_BUFFER - 2, cgi_pipe);
						if (!in) {
							fprintf(stderr,"[warn] Read nothing [%d on %p %d %d]\n", ferror(cgi_pipe), (void *)cgi_pipe, cgi_pipe_w[1], _pid);
							perror("[warn] Specifically");
							buf[0] = '\0';
							break;
						}
						if (!strcmp(in, "\r\n") || !strcmp(in, "\n")) {
							/*
							 * Done reading headers.
							 */
							buf[0] = '\0';
							break;
						}
						if (!strstr(in, ": ") && !strstr(in, "\r\n")) {
							/*
							 * Line was too long or is garbage?
							 */
							fprintf(stderr, "[warn] Garbage trying to read header line from CGI [%zu]\n", strlen(buf));
							break;
						}
						fwrite(in, 1, strlen(in), socket_stream);
						++j;
					}
					if (j < 1) {
						fprintf(stderr,"[warn] CGI script did not give us headers.\n");
					}
					if (feof(cgi_pipe)) {
						fprintf(stderr,"[warn] Sadness: Pipe closed during headers.\n");
					}

					if (request_type == 3) {
						/*
						 * On a HEAD request, we're done here.
						 */
						fprintf(socket_stream, "\r\n");
						if(has_pool){

						}else{
							pthread_detach(_waitthread);
						}
						// pthread_detach(_waitthread);
						goto _next;
					}

					int enc_mode = 0;
					if (!strcmp(http_version, "HTTP/1.1")) {
						/*
						 * Set Transfer-Encoding to chunked so we can send
						 * pieces as soon as we get them and not have
						 * to read all of the output at once.
						 */
						fprintf(socket_stream, "Transfer-Encoding: chunked\r\n");
					} else {
						/*
						 * Not HTTP/1.1
						 * Use Connection: Close
						 */
						fprintf(socket_stream, "Connection: close\r\n\r\n");
						enc_mode = 1;
					}

					/*
					 * Sometimes, shit gets borked.
					 */
					if (strlen(buf) > 0) {
						fprintf(stderr, "[warn] Trying to dump remaining content.\n");
						fprintf(socket_stream, "\r\n%zX\r\n", strlen(buf));
						fwrite(buf, 1, strlen(buf), socket_stream);
					}

					/*
					 * Read output from CGI script and send as chunks.
					 */
					while (!feof(cgi_pipe)) {
						size_t read = -1;
						read = fread(buf, 1, CGI_BUFFER - 1, cgi_pipe);
						if (read < 1) {
							/*
							 * Read nothing, we are done (or something broke)
							 */
							fprintf(stderr, "[warn] Read nothing on content without eof.\n");
							perror("[warn] Error on read");
							break;
						}
						if (enc_mode == 0) {
							/*
							 * Length of this chunk.
							 */
							fprintf(socket_stream, "\r\n%zX\r\n", read);
						}
						fwrite(buf, 1, read, socket_stream);
					}
					if (enc_mode == 0) {
						/*
						 * We end `chunked` encoding with a 0-length block
						 */
						fprintf(socket_stream, "\r\n0\r\n\r\n");
					}

					/*
					 * Release memory for the waiting thread.
					 */
					if(has_pool){
						
					}else{
						pthread_detach(_waitthread);
					}
					// pthread_detach(_waitthread);
					if (cgi_pipe) {
						/*
						 * If we need to, close this pipe as well.
						 */
						fclose(cgi_pipe);
					}

					/*
					 * Done executing CGI, move to next request or close
					 */
					if (enc_mode == 0) {
						/*
						 * HTTP/1.1
						 * Chunked encoding.
						 */
						goto _next;
					} else {
						/*
						 * HTTP/1.0
						 * Non-chunked, break the connection.
						 */
						delete_vector(queue);
						goto _disconnect;
					}
				}
#endif

				/*
				 * Flat file: Status OK.
				 */
				fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
			}

			/*
			 * Server software header
			 */
			fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");

			/*
			 * Determine the MIME type for the file.
			 */
			if (ext) {
				if (!strcmp(ext,".htm") || !strcmp(ext,".html")) {
					fprintf(socket_stream, "Content-Type: text/html\r\n");
				} else if (!strcmp(ext,".css")) {
					fprintf(socket_stream, "Content-Type: text/css\r\n");
				} else if (!strcmp(ext,".png")) {
					fprintf(socket_stream, "Content-Type: image/png\r\n");
				} else if (!strcmp(ext,".jpg")) {
					fprintf(socket_stream, "Content-Type: image/jpeg\r\n");
				} else if (!strcmp(ext,".gif")) {
					fprintf(socket_stream, "Content-Type: image/gif\r\n");
				} else if (!strcmp(ext,".pdf")) {
					fprintf(socket_stream, "Content-Type: application/pdf\r\n");
				} else if (!strcmp(ext,".manifest")) {
					fprintf(socket_stream, "Content-Type: text/cache-manifest\r\n");
				} else {
					fprintf(socket_stream, "Content-Type: text/unknown\r\n");
				}
			} else {
				fprintf(socket_stream, "Content-Type: text/unknown\r\n");
			}

			if (request_type == 3) {
				/*
				 * On a HEAD request, stop here,
				 * we only needed the headers.
				 */
				fprintf(socket_stream, "\r\n");
				fclose(content);
				goto _next;
			}

			/*
			 * Determine the length of the response.
			 */
			fseek(content, 0L, SEEK_END);
			long size = ftell(content);
			fseek(content, 0L, SEEK_SET);

			/*
			 * Send that length.
			 */
			fprintf(socket_stream, "Content-Length: %lu\r\n", size);
			fprintf(socket_stream, "\r\n");

			/*
			 * Read the file.
			 */
			char buffer[FLAT_BUFFER];
			fflush(stdout);
			while (!feof(content)) {
				/*
				 * Write out the file as a stream until
				 * we hit the end of it.
				 */
				size_t read = fread(buffer, 1, FLAT_BUFFER-1, content);
				fwrite(buffer, 1, read, socket_stream);
			}

			fprintf(socket_stream, "\r\n");

			/*
			 * Close the file.
			 */
			fclose(content);
		}

_next:
		/*
		 * Clean up.
		 */
		fflush(socket_stream);
		free(_filename);
		delete_vector(queue);
	}

_disconnect:
	/*
	 * Disconnect.
	 */
	int res=epoll_ctl(efd, EPOLL_CTL_DEL, request->fd, NULL);
	if (res == 0)
		cout<<"EPOLL_CTL_DEL request->fd "<<request->fd<<"成功！"<<endl;
	if (socket_stream) {
		fclose(socket_stream);
	}
	shutdown(request->fd, 2);//关闭request->fd的读写功能
	// cout<<"ret:"<<ret<<endl;
	/*
	 * Clean up the thread
	 */
	if (request->thread) {
		if(has_pool){

		}else{
			pthread_detach(request->thread);
		}
		// pthread_detach(request->thread);
	}
	// free(request);

	/*
	 * pthread_exit is implicit when we return...
	 */
	return NULL;
}

void start_httpd(unsigned short port, string doc_root,int poolnum)
{
	cerr << "Starting server (port: " << port <<
		", doc_root: " << doc_root << ")" << endl;
	docs=doc_root.c_str();
	//创建线程池，池里最小poolnum个线程，最大100，队列最大100
	if(poolnum!=0){
		threadpool_create(poolnum,100,100);
		has_pool=true;
	}

	/*
	 * Initialize the TCP socket
	 */
	struct sockaddr_in sin;
	int serversock          = socket(AF_INET, SOCK_STREAM, 0);
	sin.sin_family      = AF_INET;
	sin.sin_port        = htons(port);
	sin.sin_addr.s_addr = INADDR_ANY;

	/*
	 * Set reuse for the socket.
	 */
	int _true = 1;
	if (setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR, &_true, sizeof(int)) < 0) {
		close(serversock);
		return;
	}

	/*
	 * Bind the socket.
	 */
	if (bind(serversock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		fprintf(stderr,"Failed to bind socket to port %d!\n", port);
		return;
	}

	/*
	 * Start listening for requests from browsers.
	 */
	listen(serversock, 50);
	printf("[info] Listening on port %d.\n", port);
	printf("[info] Serving out of '%s '.\n",docs.c_str());
	printf("[info] Server version string is " VERSION_STRING ".\n");

	/*
	 * Extensions
	 */
#if ENABLE_CGI
	printf("[extn] CGI support is enabled.\n");
#endif
#if ENABLE_DEFAULTS
	printf("[extn] Default indexes are enabled.\n");
#endif

	/*
	 * Use our shutdown handler.
	 */
	signal(SIGINT, handleShutdown);//(Signal Interrupt) 中断信号，如 ctrl-C

	/*
	 * Ignore SIGPIPEs so we can do unsafe writes
	 */
	signal(SIGPIPE, SIG_IGN);//当服务器close一个连接时，若client端接着发数据。根据TCP协议的规定，会收到一个RST响应，client再往这个服务器发送数据时，系统会发出一个SIGPIPE信号给进程，告诉进程这个连接已经断开了，不要再写了。
	//根据信号的默认处理规则SIGPIPE信号的默认执行动作是terminate(终止、退出),所以client会退出。若不想客户端退出可以把SIGPIPE设为SIG_IGN 这时SIGPIPE交给了系统处理。
	/*
	 * Start accepting connections
	 */

	// Extension 4:epoll
	int nready, res,i, connfd,sockfd;
	
	struct epoll_event tep, ep[OPEN_MAX];
	efd = epoll_create(OPEN_MAX);  //edf:监听红黑树的树根
	if (efd == -1)
		perror("epoll_create");
	//把serversock添加到红黑树上
	tep.events = EPOLLIN; 
	tep.data.fd = serversock;
	res = epoll_ctl(efd, EPOLL_CTL_ADD, serversock, &tep);
	if (res == -1)
		perror("epoll_ctl");
	
	while (1) {
		/*
		 * Accept an incoming connection and pass it on to a new thread.
		 */
		nready = epoll_wait(efd, ep, OPEN_MAX, -1); /* 阻塞监听 */
		// cout<<"nready:"<<nready<<endl;
		if (nready == -1)
			perror("epoll_wait");
		for(i=0;i<nready;i++){
			if(!(ep[i].events & EPOLLIN))
				continue;//如果不是读事件就继续下一个
			if(ep[i].data.fd == serversock){
				// serversock满足读事件，有新的客户端发起请求
				unsigned int c_len;
				struct socket_request * incoming = (socket_request *)calloc(sizeof(struct socket_request),1);
				c_len = sizeof(incoming->address);
				_last_unaccepted = (void *)incoming;
				incoming->fd = accept(serversock, (struct sockaddr *) &(incoming->address), &c_len);

				connfd = incoming->fd;//备份
				tep.events = EPOLLIN; 
				tep.data.fd = connfd;
				res = epoll_ctl(efd, EPOLL_CTL_ADD, connfd, &tep);
				mp[connfd] = incoming;
				if (res == -1)
					perror("epoll_ctl");
				if (res == 0)
					cout<<"EPOLL_CTL_ADD connfd "<<connfd<<"成功！"<<endl;
			}else{
				//clientfd们满足读事件
				sockfd = ep[i].data.fd;
				struct socket_request* incoming = mp[sockfd];
				_last_unaccepted = NULL; 
				if(has_pool){
					threadpool_add_task(NULL, handleRequest, (void *)incoming);
				}else{
					pthread_create(&(incoming->thread), NULL, handleRequest, (void *)(incoming));
				}
			
			}
		}

		// unsigned int c_len;
		// struct socket_request * incoming = (socket_request *)calloc(sizeof(struct socket_request),1);
		// c_len = sizeof(incoming->address);
		// _last_unaccepted = (void *)incoming;
		// incoming->fd = accept(serversock, (struct sockaddr *) &(incoming->address), &c_len);
		// _last_unaccepted = NULL;
		// pthread_create(&(incoming->thread), NULL, handleRequest, (void *)(incoming));
	}

	/*
	 * We will clean up when we receive a SIGINT.
	 */
 
}