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
#define MAX_BUF 65536 //the max size of messages
#define MAX_PATH_LEN 4096 //max size of the file path
#define COOKIE_EXP 604800 //cookie expiry time in seconds

struct request {
	char method[7];
	char url[2048];
	char http_v[10];
	char host[2048];
	char path[2048];
	char useragent[256];
	char connection[256];
	char body[2048];
	int has_body;
};

struct response {
	char http_v[10];
	int status_no;
	char status[256];
	char c_type[256];
	char c_length[256];
	int has_type;
	int has_length;
};

struct modes {
	int is_mobile;
	int is_falsify;
	int is_redirect;
};

void
handle_request(struct request req, struct modes m);

int
parse_request(char *request, struct request *r_ptr);

void
*get_in_addr(struct sockaddr *sa);

void
setup_server(int *listener, char *port);

int connfd;
char *PORT;
char s[INET6_ADDRSTRLEN]; //the connector's readable IP address
static int count = 1;
//char *mobile_ua = "Mozilla/5.0 (iPad; U; CPU OS 3_2_1 like Mac OS X; en-us) AppleWebKit/531.21.10 (KHTML, like Gecko) Mobile/7B405";
char *mobile_ua = "Mozilla/5.0 (Linux; Android 7.0; LG-H910 Build/NRD90C) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/51.0.2704.90 Mobile Safari/537.36";



void
check_modes(char *path, struct modes *m)
{
	char *ptr;
	ptr = strrchr(path, '?');
	if (ptr != NULL) {
		ptr += 1;
		if (strcmp(ptr, "start_mobile") == 0)
			m->is_mobile = 1;
		else if (strcmp(ptr, "stop_mobile") == 0)
			m->is_mobile = 0;
	}
}


int
connect_host(char *hostname)
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

	return sockfd;
}

void *client_thread(void *arg)
{
	
	int asdf = *((int*)arg);
	printf("we got a file number of %d\n", asdf);
	int nbytes; //the number of received bytes
	char buf[MAX_BUF]; //buffer for messages
	struct request req;
	struct modes m;
	m.is_mobile = 0;
	m.is_redirect = 0;
	m.is_falsify = 0;

	while ((nbytes = recv(asdf, buf, MAX_BUF, 0)) > 0) {
		//we received a request!
		printf("%s", buf);
		parse_request(buf, &req);
		if (strcmp(req.method, "GET") == 0) {
			handle_request(req, m);
		}
		memset(&buf, 0, sizeof(buf));
	}
	close(asdf);  //parent doesn't need this
	return NULL;
}


/*
 * Returns the length of the header
 */
int
parse_response(char *response, struct response *res)
{
	//printf("\n\nPARSE RESPONSE: <%s>\n\n", response);
	char *token, *string, *tofree;
	tofree = string = strdup(response);
	int worked;
	int found_end = 0;
	res->has_length = 0;
	res->has_type = 0;
	long header_length = 0;

	//scan the method and url into the pointer
	worked = sscanf(response, "%s %d %s\r\n", res->http_v, &res->status_no, res->status);
	
	if (worked < 3)
		return 0;

	printf("[CLI --- PRX <== SRV]\n");
	printf("> %d %s\n", res->status_no, res->status);

	//loop through the request line by line (saved to token)
	while ((token = strsep(&string, "\r\n")) != NULL) {
		if (strncmp(token, "Content-Type: ", 14) == 0) {
			char *type = token + 14;
			strncpy(res->c_type, type, strlen(token));
			res->has_type = 1;
		}
		else if (strncmp(token, "Content-Length: ", 16) == 0) {
			char *len = token + 16;
			strncpy(res->c_length, len, strlen(token));
			res->has_length = 1;
		}
		else if (strlen(token) == 0) {
			//we've reached the end of the header, expecting body now
			found_end = 1;
			break;
		}
		//skip over the \n character and break when we reach the end
		if (strlen(string) <= 2) {
			found_end = 1;
			break;
		}
		string += 1;
	}
	header_length = strlen(response) - strlen(string) + 1; //plus one for the \n i believe
	free(tofree);

	if (res->has_type && !res->has_length) {
		printf("> %s\n", res->c_type);
	}
	else if (res->has_type && res->has_length) {
		printf("> %s %sbytes\n", res->c_type, res->c_length);
	}

	return header_length;
}

/*
 * Reads through the request and extracts any useful information
 * into the pointer to a request structure <r_ptr>.
 * Returns 0 if successful.
 */
int
parse_request(char *request, struct request *r_ptr)
{
	//printf("\n\nPARSE REQUEST: <%s>\n\n", request);
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
		else if (strncmp(token, "Proxy-Connection: ", 18) == 0) {
			char *conn = token + 18;
			strncpy(r_ptr->connection, conn, strlen(token));
		}
		else if (strncmp(token, "Connection: ", 12) == 0) {
			char *conn = token + 12;
			strncpy(r_ptr->connection, conn, strlen(token));
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
 * Generates a custom request and sends it to the socket at servconn.
 */
ssize_t
send_request(int servconn, struct request req, struct modes m)
{
	char connstring[256];
	char request[2048];
	char *poop;

	if (strlen(req.connection) > 0) {
		sprintf(connstring, "Connection: %s\r\n", req.connection);
		poop = connstring;
	} else {
		poop = "";
	}

	char *ua = (m.is_mobile) ? mobile_ua : req.useragent;

	sprintf(request, "GET %s %s\r\n"
			"Host: %s\r\n"
			"User-Agent: %s\r\n"
			"%s"
			"\r\n", req.path, req.http_v, req.host, ua, poop);

	printf("[CLI --- PRX ==> SRV]\n");
	printf("> GET %s%s\n", req.host, req.path);
	printf("> %s\n", ua);
	printf("%s\n", request);

	return send(servconn, request, strlen(request), 0);
}




/*
 * Reads the request and executes the appropriate action depending on
 * information retrieved from parse_request().
 */
void
handle_request(struct request req, struct modes m)
{
	printf("-----------------------------------------------\n");
	printf("%d [%s] Redirection [%s] Mobile [%s] Falsification\n", count,
			m.is_redirect ? "O" : "X",
			m.is_mobile ? "O" : "X",
			m.is_falsify ? "O" : "X"
			);
	printf("[CLI connected to %s:%s]\n", s, PORT);
	printf("[CLI ==> PRX --- SRV]\n");
	printf("> GET %s%s\n", req.host, req.path);
	printf("> %s\n", req.useragent);

	int servconn;
	servconn = connect_host(req.host);
	//printf("REQUEST:\n<\n%s\n>\n", request);

	//if (send(servconn, request, strlen(request), 0) == -1) {
	if (send_request(servconn, req, m) == -1) {
		perror("Error writing to socket");
	}

	char buf[MAX_BUF]; //buffer for messages
	int nbytes; //the number of received bytes
	struct response res;
	long bytes_left;
	long header_length;

	nbytes = recv(servconn, buf, MAX_BUF,0);
	//printf("response <%s>\n", buf);
	send(connfd, buf, nbytes, 0);

	if (nbytes > 0) {
		header_length = parse_response(buf, &res);

		if (res.has_length) {
			//we know exactly how many bytes we're expecting

			bytes_left = atoll(res.c_length);
			//printf("expecting %ld bytes\n", bytes_left);
			bytes_left -= (nbytes - header_length);

			printf("[CLI <== PRX --- SRV]\n");
			printf("> %d %s\n", res.status_no, res.status);
			if (res.has_type && !res.has_length) {
				printf("> %s\n", res.c_type);
			}
			else if (res.has_type && res.has_length) {
				printf("> %s %sbytes\n", res.c_type, res.c_length);
			}

			while (bytes_left > 0) {
				memset(&buf, 0, sizeof(buf));
				nbytes = recv(servconn, buf, MAX_BUF,0);
				//printf("response <%s>\n", buf);
				send(connfd, buf, nbytes, 0);
				bytes_left -= nbytes;
			}
		}

		else {
			//we have no idea how many bytes to expect... uh oh
			memset(&buf, 0, sizeof(buf));
			while ((nbytes = recv(servconn, buf, MAX_BUF,0)) > 0) {
				send(connfd, buf, nbytes, 0);
				//printf("response <%s>\n", buf);

				int len = strlen(buf);
				const char *last_five = &buf[len-5];
				if (strcmp(last_five, "0\r\n\r\n") == 0) {
					printf("the end\n");
					break;
				}
				memset(&buf, 0, sizeof(buf));
			}
		}

	}

	printf("[CLI disconnected]\n");
	printf("[SRV disconnected]\n");

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

		//avoid sigpipe error
		if (setsockopt(*listener, SOL_SOCKET, SO_NOSIGPIPE, &yes,
					sizeof(yes)) == -1) {
			perror("ERROR: setsockopt() failed");
			exit(1);
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
	char buf[MAX_BUF]; //buffer for messages
	int nbytes; //the number of received bytes
	struct request req;
	struct modes m;
	m.is_mobile = 0;
	m.is_redirect = 0;
	m.is_falsify = 0;

	//set up the server on the specified port
	setup_server(&listener, PORT);

	printf("Starting proxy server on port %s\n", PORT);

	while(1) {
		memset(&req, 0, sizeof(req)); //make sure the struct is empty
		memset(&buf, 0, sizeof(buf));

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

		//printf("about to call recv WILL BLOCK!!\n");
		if ((nbytes = recv(connfd, buf, MAX_BUF, 0)) > 0) {
			//printf("recv resolved\n");
			//we received a request!
			parse_request(buf, &req);

			//only process GET requests
			if (strcmp(req.method, "GET") == 0) {
				check_modes(req.path, &m);

//				if (!fork()) { //this is the child process
//					close(listener); //child doesn't need the listener

					handle_request(req, m);
					//close(connfd);
					//exit(0);
				//}
				count++;
			}
			close(connfd);  //parent doesn't need this
		}

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

