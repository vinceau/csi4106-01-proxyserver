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

#define BACKLOG 10 //how many pending connections the queue will hold
#define MAX_BUF 1024 //the max size of messages
#define MAX_PATH_LEN 4096 //max size of the file path
#define COOKIE_EXP 604800 //cookie expiry time in seconds

struct request {
	char method[7];
	char url[2048];
	char cookie[256];
	char body[2048];
	int has_cookie; //0 for no, 1 for yes
	int has_body;
	int is_mobile;
};

char *
get_mime(char *path);

int
is_alphastring(char *string);

void
write_response(int statusno, const char *status, const char * restrict format, ...);

void
write_error(int errno);

void
write_file(char *path, size_t fsize);

void
set_cookie_and_redirect(char *cookie, char *site);

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

int connfd; //file descriptor of connection socket
char *ROOT; //root directory for all files
char *SECRET = "2016840200"; //secret key for /secret
char *PASSWORD = "id=yonsei&pw=network"; //password needed in POST
struct request req; //information about the last request

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
write_response(int statusno, const char *status, const char * restrict format, ...)
{
	FILE *connfile = fdopen(connfd, "w");
	fprintf(connfile, "HTTP/1.1 %d %s\r\n", statusno, status);
	va_list args;
	va_start(args, format);
	vfprintf(connfile, format, args);
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
			write_response(403, "Forbidden",
					"Content-Type: text/html\r\n"
					"\r\n"
					"<html><head><title>Access Forbidden</title></head><body><h1>403 Forbidden</h1><p>You don't have permission to access the requested URL %s. There is either no index document or the directory is read-protected.</p></body></html>", req.url);
			break;
		case 404:
			write_response(404, "Not Found",
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

/*
 * Sets a cookie that expires in COOKIE_EXP seconds and redirect to
 * the secret page.
 */
void
set_cookie_and_redirect(char *cookie, char *site)
{
	write_response(302, "Found",
			"Location: %s\r\n"
			"Set-Cookie: cookie=%s; path=/; max-age=%d\r\n",
			site, cookie, COOKIE_EXP);
}

/*
 * Unsets the cookie we set with set_cookie().
 */
void
unset_cookie()
{
	write_response(200, "OK",
			"Set-Cookie: cookie=; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT\r\n");
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
	sscanf(request, "%s %s ", r_ptr->method, r_ptr->url);

	//set false as default
	r_ptr->is_mobile = 0;
	r_ptr->has_cookie = 0;
	r_ptr->has_body = 0;

	//loop through the request line by line (saved to token)
	while ((token = strsep(&string, "\r\n")) != NULL) {
		if (strncmp(token, "User-Agent: ", 12) == 0) {
			//we're looking at the user agent now
			if (strstr(token, "Mobile") != NULL) { //we're on mobile
				r_ptr->is_mobile = 1;
			}
		}
		else if (strncmp(token, "Cookie: ", 8) == 0) {
			//save the cookie to the pointer
			char *cookie = token + 8;
			strncpy(r_ptr->cookie, cookie, strlen(token));
			r_ptr->has_cookie = 1;
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
	printf("%s\n", request);
	parse_request(request, &req);

	printf("method: %s\n", req.method);
	printf("url: %s\n", req.url);
	printf("cookie: %s\n", req.cookie);
	printf("body: %s\n", req.body);
	printf("has_cookie: %d\n", req.has_cookie);
	printf("has_body: %d\n", req.has_body);
	printf("is_mobile: %d\n", req.is_mobile);

	char *url = req.url;

	//the only POST request supported is at /login
	if (strcmp(req.method, "POST") == 0) {
		//handle the login component
		if (strcmp(url, "/login") == 0) {
			//we have the correct password
			if (req.has_body && strstr(req.body, PASSWORD) != NULL)
				return set_cookie_and_redirect(SECRET, "/secret/index.html");
			//the password was wrong so access is still forbidden
			return write_error(403);
		}
		//for all other POST requests return 405 error
		return write_response(405, "Method Not Allowed", "");
	}

	//filter out all other request methods
	if (strcmp(req.method, "GET") != 0)
		return write_response(405, "Method Not Allowed", "");

	//everything else from here on out should be a GET request
	//handle secret
	if (strncmp(url, "/secret/", 8) == 0) {
		if (!req.has_cookie || strstr(req.cookie, SECRET) == NULL)
			return write_error(403);
	}

	/*
	if (strcmp(url, "/remcookie") == 0)
		return unset_cookie();
	*/

	char path[MAX_PATH_LEN];
	//change root directory to mobile if on mobile
	char *mob = (req.is_mobile) ? "/mobile" : "";
	//if we're at a folder, add index.html
	char *fol = (url[strlen(url)-1] == '/') ? "index.html" : "";
	sprintf(path, "%s%s%s%s", ROOT, mob, url, fol);
	printf("file: %s\n", path);

	//let's see if we've got an actual file
	char newrl[2048]; //new url should we need to redirect
	struct stat st;
	stat(path, &st);
	if (S_ISREG(st.st_mode)) {
		//we've a normal file, so write it out to the socket
		return write_file(path, st.st_size);
	} else if (S_ISDIR(st.st_mode)) {
		//we've a directory so append a '/' character and try again
		sprintf(newrl, "%s/", url);
		return handle_redirect(newrl);
	}

	//file not found
	//test for go requests
	if (strncmp(url, "/go/", 4) == 0) {
		char *site = url + 4;
		if (strlen(site) > 0 && is_alphastring(site)) {
			sprintf(newrl, "http://www.%s.com/", site);
			return handle_redirect(newrl);
		}
	}

	//if we've made it down here then just return error
	return write_error(404);
}

/*
 * Sets HTTP header to redirect to <site>
 */
void
handle_redirect(char *site)
{
	printf("Redirecting to: %s\n", site);
	write_response(302, "Found", "Location: %s\r\n", site);
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
	if (argc != 3) {
		//remember: the name of the program is the first argument
		fprintf(stderr, "ERROR: Missing required arguments!\n");
		printf("Usage: %s <port> <folder>\n", argv[0]);
		printf("e.g. %s 8080 website\n", argv[0]);
		exit(1);
	}

	char *port = argv[1]; //port we're listening on
	ROOT = argv[2]; //root directory

	int listener; //file descriptor of listening socket
	struct sockaddr_storage their_addr; //connector's address info
	socklen_t sin_size;
	char s[INET6_ADDRSTRLEN]; //the connector's readable IP address

	char buf[MAX_BUF]; //buffer for messages
	int nbytes; //the number of received bytes

	//set up the server on the specified port
	setup_server(&listener, port);

	printf("SERVER: Listening on port %s for connections...\n", port);

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

		printf("SERVER: received connection from %s\n", s);

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
	}
	return 0;
}

