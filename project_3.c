/*
 * project_3 -- a simple proxy server
 *
 * The third project of the 2016 Fall Semester course
 * CSI4106-01: Computer Networks at Yonsei University.
 *
 * Author: Vincent Au (2016840200)
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdarg.h>
#include <pthread.h>

#define BACKLOG 10 //how many pending connections the queue will hold
#define MAX_BUF 1024 //the max size of messages
#define MAX_PATH_LEN 4096 //max size of the file path
#define COOKIE_EXP 604800 //cookie expiry time in seconds

struct request {
	char method[7];
	char url[2048];
	char http_v[10];
	char host[2048];
	char path[2048];
	char useragent[256];
	char body[2048];
	int has_body;
};

struct response {
	char http_v[10];
	int status_no;
	char status[256];
	char c_type[256];
	char c_length[256];
};

char *
get_mime(char *path);

int
is_alphastring(char *string);

void
write_response(int sockfd, int statusno, const char *status,
		const char * restrict format, ...);

void
write_error(int errno);

void
write_file(char *path, size_t fsize);

void
set_cookie_and_redirect(char *cookie, char *site);


void
write_request(int sockfd, const char * restrict format, ...);

void
unset_cookie();

int
parse_request(char *request, struct request *r_ptr);

void
handle_request(char *request);

void
handle_redirect(char *site);

void
*get_in_addr(struct sockaddr *sa);

void
setup_server(int *listener, char *port);

int connfd;
char *SECRET = "2016840200"; //secret key for /secret
char *PASSWORD = "id=yonsei&pw=network"; //password needed in POST
char *PORT;
struct request req; //information about the last request

int
is_method(char *method)
{
	if (strcmp(req.method, method) == 0)
		return 1;
	return 0;
}



int connect_host(char *hostname)
{
	struct hostent *he;
	struct in_addr **addr_list;

	struct sockaddr_in server;
	int sockfd;
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("couldn't open socket");
		exit(1);
	}

	if ((he = gethostbyname(hostname)) == NULL) {
		perror("Couldn't call gethostbyname()");
		exit(1);
	}

	addr_list = (struct in_addr **) he->h_addr_list;

	memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
	server.sin_family = AF_INET;
	server.sin_port = htons(80);

	if (connect(sockfd, (struct sockaddr *)&server, sizeof(server))) {
		perror("Couldn't connect socket to client");
		exit(1);
	}

	printf("[SRV connected to %s:80]\n", hostname);
	printf("[CLI --- PRX ==> SRV]\n");

	return sockfd;
}

void *client_thread(void *arg)
{
	
	int asdf = *((int*)arg);
	printf("we got a file number of %d\n", asdf);
	int nbytes; //the number of received bytes
	char buf[MAX_BUF]; //buffer for messages

	if ((nbytes = recv(asdf, buf, MAX_BUF, 0)) > 0) {
		//we received a request!
		handle_request(buf);
	}
	close(asdf);  //parent doesn't need this
	return NULL;
}








/*
 * Returns the correct MIME type depending on file extention.
 * Returns "application/octet-stream" if file extension is unknown.
 */
char *
get_mime(char *path)
{
	//grab the file extension
	char *fext = strrchr(path, '.');
	if (fext != NULL) {
		fext += 1; //ignore the '.'

		//supported file types
		if (strcmp(fext, "html") == 0)
			return "text/html";
		if (strcmp(fext, "css") == 0)
			return "text/css";
		if (strcmp(fext, "js") == 0)
			return "text/javascript";
		if (strcmp(fext, "jpg") == 0)
			return "image/jpeg";
		if (strcmp(fext, "png") == 0)
			return "image/png";
	}

	//arbitrary data
	return "application/octet-stream";
}

/*
 * Returns 1 if <string> is entirely alphabetical and 0 otherwise.
 */
int
is_alphastring(char *string)
{
	for (int i = 0; i < (int)strlen(string); i++) {
		if (!isalpha(string[i]))
			return 0;
	}
	return 1;
}

/*
 * Writes a HTTP response to connfd connection socket, appending any string
 * as additional header/body contents.
 * statusno: the HTTP status number
 * status: the HTTP status
 */
void
write_response(int sockfd, int statusno, const char *status, const char * restrict format, ...)
{
	FILE *connfile = fdopen(sockfd, "w");
	fprintf(connfile, "HTTP/1.1 %d %s\r\n", statusno, status);
	va_list args;
	va_start(args, format);
	vfprintf(connfile, format, args);
	va_end(args);
	fflush(connfile);
	fclose(connfile);
}

void
write_request(int sockfd, const char * restrict format, ...)
{
	FILE *connfile = fdopen(sockfd, "w");
	va_list args;
	va_start(args, format);
	vfprintf(connfile, format, args);
	printf("The following message was sent:\n<");
	printf(format, args); //also print it to stdout
	printf(">\n");
	va_end(args);
	fflush(connfile);
	fclose(connfile);
}

/*
 * Writes predefined error response to connfd depending on <errno>
 */
void
write_error(int errno)
{
	switch(errno){
		case 403:
			write_response(connfd, 403, "Forbidden",
					"Content-Type: text/html\r\n"
					"\r\n"
					"<html><head><title>Access Forbidden</title></head><body><h1>403 Forbidden</h1><p>You don't have permission to access the requested URL %s. There is either no index document or the directory is read-protected.</p></body></html>", req.url);
			break;
		case 404:
			write_response(connfd, 404, "Not Found",
					"Content-Type: text/html\r\n"
					"\r\n"
					"<html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1><p>The requested URL %s was not found on this server.</p></body></html>", req.url);
			break;
		default:
			fprintf(stderr, "Unrecognised error number <%d>\n", errno);
			break;
	}
}

/*
 * Writes the file of <fsize> bytes at <path> to the connfd socket.
 * Warning! This function does not check for file errors but assumes
 * the file already exists. Check for existence before calling write_file()!
 */
void
write_file(char *path, size_t fsize)
{
	FILE *file, *connfile;
	unsigned char bytes_to_send[MAX_BUF];
	size_t bytes_read;

	file = fopen(path, "r");
	connfile = fdopen(connfd, "w");

	printf("file size is %ld bytes\n", fsize);

	fprintf(connfile,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %ld\r\n"
			"\r\n", get_mime(path), fsize);
	fflush(connfile);

	while ((bytes_read = fread(bytes_to_send, 1, MAX_BUF, file)) > 0) {
		fwrite(bytes_to_send, 1, bytes_read, connfile);
	}

	fclose(file);
	fclose(connfile);
}

int
parse_response(char *response, struct response *r_ptr)
{
	printf("\n\nPARSE RESPONSE: <%s>\n\n", response);
	char *token, *string, *tofree;
	tofree = string = strdup(response);

	//scan the method and url into the pointer
	sscanf(response, "%s %d %s\r\n", r_ptr->http_v, &r_ptr->status_no,
			r_ptr->status);

	//loop through the request line by line (saved to token)
	while ((token = strsep(&string, "\r\n")) != NULL) {
		if (strncmp(token, "Content-Type: ", 14) == 0) {
			char *type = token + 14;
			strncpy(r_ptr->c_type, type, strlen(token));
		}
		else if (strncmp(token, "Content-Length: ", 16) == 0) {
			char *len = token + 16;
			strncpy(r_ptr->c_length, len, strlen(token));
		}
		else if (strlen(token) == 0) {
			//we've reached the end of the header, expecting body now
			break;
		}
		//skip over the \n character and break when we reach the end
		if (strlen(string) <= 2)
			break;
		string += 1;
	}
	free(tofree);

	return 0;
}

/*
 * Reads through the request and extracts any useful information
 * into the pointer to a request structure <r_ptr>.
 * Returns 0 if successful.
 */
int
parse_request(char *request, struct request *r_ptr)
{
	int in_body = 0; //are we at the request body yet?
	char *token, *string, *tofree;
	tofree = string = strdup(request);

	//scan the method and url into the pointer
	sscanf(request, "%s %s %s\r\n", r_ptr->method, r_ptr->url, r_ptr->http_v);

	//set false as default
	r_ptr->has_body = 0;

	//loop through the request line by line (saved to token)
	while ((token = strsep(&string, "\r\n")) != NULL) {
		if (strncmp(token, "Host: ", 6) == 0) {
			char *host = token + 6;
			strncpy(r_ptr->host, host, strlen(token));

			char *path_offset = strstr(r_ptr->url, host);
			path_offset+=strlen(host);
			//printf("path is: %s\n", path_offset);
			strncpy(r_ptr->path, path_offset, strlen(path_offset));
		}
		else if (strncmp(token, "User-Agent: ", 12) == 0) {
			char *userag = token + 12;
			strncpy(r_ptr->useragent, userag, strlen(token));
		}
		else if (strlen(token) == 0) {
			//we've reached the end of the header, expecting body now
			in_body = 1;
		}
		else if (in_body == 1) {
			strncpy(r_ptr->body, token, strlen(token));
			r_ptr->has_body = 1;
			break; //there should be nothing after body
		}
		//skip over the \n character and break when we reach the end
		if (strlen(string) <= 2)
			break;
		string += 1;
	}
	free(tofree);

	return 0;
}

/*
 * Reads the request and executes the appropriate action depending on
 * information retrieved from parse_request().
 */
void
handle_request(char *request)
{
	parse_request(request, &req);

	//don't worry about non-GET requests
	if (!is_method("GET")) {
		return;
	}

	printf("[CLI ==> PRX --- SRV]\n");
	printf("  > GET %s%s\n", req.host, req.path);
	printf("  > %s\n", req.useragent);

	int servconn;
	servconn = connect_host(req.host);
		/*

> GET yscec.yonsei.ac.kr/a.js
> Mozilla/5.0 (Linux; Android 4.4.2; Nexus 4)
[CLI --- PRX <== SRV]
> 200 OK
> application/javascript 3858bytes
[CLI <== PRX --- SRV]
> 404 Not Found
[CLI disconnected]
[SRV disconnected]


		 * */




	//printf("REQUEST:\n<\n%s\n>\n", request);

	char buffer[MAX_BUF];
	/*
	write_request(servconn, "%s", request);

	int nbytes;
	if ((nbytes = recv(servconn, buffer, MAX_BUF, 0)) > 0) {
		//we received a request!
		write_request(connfd, "%s", buffer);
	}
	*/


	int n = send(servconn,request,strlen(request), 0);

	if (n < 0)
		perror("Error writing to socket");
	else {
		do {
			bzero((char*)buffer,MAX_BUF);
			n = recv(servconn,buffer,MAX_BUF,0);

			if (!(n<=0))
				send(connfd,buffer,n,0);
		} while (n>0);
	}


	/*
	write(servconn, "GET /\r\n", strlen("GET /\r\n")); // write(servconn, char[]*, len);  
	bzero(buffer, MAX_BUF);
	
	printf("!!! Server response: \n");
	while(read(servconn, buffer, MAX_BUF - 1) != 0){
		printf("%s", buffer);
		write_error(404);
		//write_request(connfd, "%s", buffer);
		//fprintf(stderr, "%s", buffer);
		bzero(buffer, MAX_BUF);
	}
	printf("!!! End server response\n");
	*/
	return;

}

/*
 * Get socket address irrespective of IPv4 or IPv6
 * Shamelessly taken from:
 * http://www.beej.us/guide/bgnet/output/html/singlepage/bgnet.html
 * Credits to Brian "Beej Jorgensen" Hall
 */
void
*get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/*
 * Set up the server on socket <listener> using port <port>
 */
void
setup_server(int *listener, char *port)
{
	//set up the structs we need
	int status;
	struct addrinfo hints, *p;
	struct addrinfo *servinfo; //will point to the results

	memset(&hints, 0, sizeof(hints)); //make sure the struct is empty
	hints.ai_family = AF_UNSPEC; //IPv4 or IPv6 is OK (protocol agnostic)
	hints.ai_socktype = SOCK_STREAM; //TCP stream sockets
	hints.ai_flags = AI_PASSIVE; //fill in my IP for me

	if ((status = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
		exit(1);
	}

	//servinfo now points to a linked list of struct addrinfos
	//each of which contains a struct sockaddr of some kind
	int yes = 1;
	for (p = servinfo; p != NULL; p = p->ai_next) {
		*listener = socket(servinfo->ai_family, servinfo->ai_socktype,
				servinfo->ai_protocol);
		if (*listener == -1) {
			perror("ERROR: socket() failed");
			//keep going to see if we can connect to something else
			continue; 
		}

		//allow port reuse
		if (setsockopt(*listener, SOL_SOCKET, SO_REUSEADDR, &yes,
					sizeof(yes)) == -1) {
			perror("ERROR: setsockopt() failed");
			exit(1);
		}

		if (bind(*listener, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
			perror("ERROR: bind() failed");
			//keep going to see if we can connect to something else
			continue;
		}

		//we have something, let's move on
		break;
	}

	//free the linked list since we don't need it anymore
	freeaddrinfo(servinfo);

	if (p == NULL) {
		fprintf(stderr, "ERROR: Failed to bind to anything!\n");
		exit(1);
	}

	//listen time
	if (listen(*listener, BACKLOG) == -1) {
		perror("ERROR: listen() failed");
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	//make sure we have the right number of arguments
	if (argc != 2) {
		//remember: the name of the program is the first argument
		fprintf(stderr, "ERROR: Missing required arguments!\n");
		printf("Usage: %s <port>\n", argv[0]);
		printf("e.g. %s 9001\n", argv[0]);
		exit(1);
	}

	PORT = argv[1]; //port we're listening on

	//int connfd; //file descriptor of connection socket
	int listener; //file descriptor of listening socket
	struct sockaddr_storage their_addr; //connector's address info
	socklen_t sin_size;
	char s[INET6_ADDRSTRLEN]; //the connector's readable IP address
	char buf[MAX_BUF]; //buffer for messages
	int nbytes; //the number of received bytes
	static int count = 1;


	//set up the server on the specified port
	setup_server(&listener, PORT);

	printf("Starting proxy server on port %s\n", PORT);

	while(1) {
		sin_size = sizeof(their_addr);
		//accept()
		connfd = accept(listener, (struct sockaddr *) &their_addr,
				&sin_size);
		if (connfd == -1) {
			perror("ERROR: accept() failed");
			continue;
		}

		inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);

		printf("-----------------------------------------------\n");
		printf("%d [X] Redirection [X] Mobile [X] Falsification\n", count++);


		printf("[CLI connected to %s:%s]\n", s, PORT);

		if (!fork()) { //this is the child process
			close(listener); //child doesn't need the listener

			if ((nbytes = recv(connfd, buf, MAX_BUF, 0)) > 0) {
				//we received a request!
				handle_request(buf);
			}

			close(connfd);
			exit(0);
		}
		close(connfd);  //parent doesn't need this

		/*
		printf("number: %d\n", connfd);
		pthread_t pth; //thread identifier
		int *arg = malloc(sizeof(*arg));
		if (arg == NULL) {
			fprintf(stderr, "Couldn't allocate memory for thread arg.\n");
			exit(EXIT_FAILURE);
		}
		*arg = connfd;
		pthread_create(&pth, NULL, client_thread, arg);
		*/
	}

	close(listener);
	return 0;
}

