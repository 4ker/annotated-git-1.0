#include "cache.h"
#include "pkt-line.h"
#include "quote.h"
#include "refs.h"
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static char *server_capabilities = NULL;

/*
 * Read all the refs from the other end
 */
struct ref **get_remote_heads(int in, struct ref **list,
			      int nr_match, char **match, int ignore_funny)
{
	*list = NULL;
	for (;;) {
		struct ref *ref;
		unsigned char old_sha1[20];
		static char buffer[1000];
		char *name;
		int len, name_len;

		len = packet_read_line(in, buffer, sizeof(buffer));
		if (!len)
			break;
		if (buffer[len-1] == '\n')
			buffer[--len] = 0;

		if (len < 42 || get_sha1_hex(buffer, old_sha1) || buffer[40] != ' ')
			die("protocol error: expected sha/ref, got '%s'", buffer);
		name = buffer + 41;

		if (ignore_funny && 45 < len && !memcmp(name, "refs/", 5) &&
		    check_ref_format(name + 5))
			continue;

		name_len = strlen(name);
		if (len != name_len + 41) {
			if (server_capabilities)
				free(server_capabilities);
			server_capabilities = strdup(name + name_len + 1);
		}

		if (nr_match && !path_match(name, nr_match, match))
			continue;
		ref = xcalloc(1, sizeof(*ref) + len - 40);
		memcpy(ref->old_sha1, old_sha1, 20);
		memcpy(ref->name, buffer + 41, len - 40);
		*list = ref;
		list = &ref->next;
	}
	return list;
}

int server_supports(const char *feature)
{
	return server_capabilities &&
		strstr(server_capabilities, feature) != NULL;
}

int get_ack(int fd, unsigned char *result_sha1)
{
	static char line[1000];
	int len = packet_read_line(fd, line, sizeof(line));

	if (!len)
		die("git-fetch-pack: expected ACK/NAK, got EOF");
	if (line[len-1] == '\n')
		line[--len] = 0;
	if (!strcmp(line, "NAK"))
		return 0;
	if (!strncmp(line, "ACK ", 3)) {
		if (!get_sha1_hex(line+4, result_sha1)) {
			if (strstr(line+45, "continue"))
				return 2;
			return 1;
		}
	}
	die("git-fetch_pack: expected ACK/NAK, got '%s'", line);
}

int path_match(const char *path, int nr, char **match)
{
	int i;
	int pathlen = strlen(path);

	for (i = 0; i < nr; i++) {
		char *s = match[i];
		int len = strlen(s);

		if (!len || len > pathlen)
			continue;
		if (memcmp(path + pathlen - len, s, len))
			continue;
		if (pathlen > len && path[pathlen - len - 1] != '/')
			continue;
		*s = 0;
		return 1;
	}
	return 0;
}

struct refspec {
	char *src;
	char *dst;
	char force;
};

/*
 * A:B means fast forward remote B with local A.
 * +A:B means overwrite remote B with local A.
 * +A is a shorthand for +A:A.
 * A is a shorthand for A:A.
 */
static struct refspec *parse_ref_spec(int nr_refspec, char **refspec)
{
	int i;
	struct refspec *rs = xcalloc(sizeof(*rs), (nr_refspec + 1));
	for (i = 0; i < nr_refspec; i++) {
		char *sp, *dp, *ep;
		sp = refspec[i];
		if (*sp == '+') {
			rs[i].force = 1;
			sp++;
		}
		ep = strchr(sp, ':');
		if (ep) {
			dp = ep + 1;
			*ep = 0;
		}
		else
			dp = sp;
		rs[i].src = sp;
		rs[i].dst = dp;
	}
	rs[nr_refspec].src = rs[nr_refspec].dst = NULL;
	return rs;
}

static int count_refspec_match(const char *pattern,
			       struct ref *refs,
			       struct ref **matched_ref)
{
	int match;
	int patlen = strlen(pattern);

	for (match = 0; refs; refs = refs->next) {
		char *name = refs->name;
		int namelen = strlen(name);
		if (namelen < patlen ||
		    memcmp(name + namelen - patlen, pattern, patlen))
			continue;
		if (namelen != patlen && name[namelen - patlen - 1] != '/')
			continue;
		match++;
		*matched_ref = refs;
	}
	return match;
}

static void link_dst_tail(struct ref *ref, struct ref ***tail)
{
	**tail = ref;
	*tail = &ref->next;
	**tail = NULL;
}

static struct ref *try_explicit_object_name(const char *name)
{
	unsigned char sha1[20];
	struct ref *ref;
	int len;
	if (get_sha1(name, sha1))
		return NULL;
	len = strlen(name) + 1;
	ref = xcalloc(1, sizeof(*ref) + len);
	memcpy(ref->name, name, len);
	memcpy(ref->new_sha1, sha1, 20);
	return ref;
}

static int match_explicit_refs(struct ref *src, struct ref *dst,
			       struct ref ***dst_tail, struct refspec *rs)
{
	int i, errs;
	for (i = errs = 0; rs[i].src; i++) {
		struct ref *matched_src, *matched_dst;

		matched_src = matched_dst = NULL;
		switch (count_refspec_match(rs[i].src, src, &matched_src)) {
		case 1:
			break;
		case 0:
			/* The source could be in the get_sha1() format
			 * not a reference name.
			 */
			matched_src = try_explicit_object_name(rs[i].src);
			if (matched_src)
				break;
			errs = 1;
			error("src refspec %s does not match any.",
			      rs[i].src);
			break;
		default:
			errs = 1;
			error("src refspec %s matches more than one.",
			      rs[i].src);
			break;
		}
		switch (count_refspec_match(rs[i].dst, dst, &matched_dst)) {
		case 1:
			break;
		case 0:
			if (!memcmp(rs[i].dst, "refs/", 5)) {
				int len = strlen(rs[i].dst) + 1;
				matched_dst = xcalloc(1, sizeof(*dst) + len);
				memcpy(matched_dst->name, rs[i].dst, len);
				link_dst_tail(matched_dst, dst_tail);
			}
			else if (!strcmp(rs[i].src, rs[i].dst) &&
				 matched_src) {
				/* pushing "master:master" when
				 * remote does not have master yet.
				 */
				int len = strlen(matched_src->name) + 1;
				matched_dst = xcalloc(1, sizeof(*dst) + len);
				memcpy(matched_dst->name, matched_src->name,
				       len);
				link_dst_tail(matched_dst, dst_tail);
			}
			else {
				errs = 1;
				error("dst refspec %s does not match any "
				      "existing ref on the remote and does "
				      "not start with refs/.", rs[i].dst);
			}
			break;
		default:
			errs = 1;
			error("dst refspec %s matches more than one.",
			      rs[i].dst);
			break;
		}
		if (errs)
			continue;
		if (matched_dst->peer_ref) {
			errs = 1;
			error("dst ref %s receives from more than one src.",
			      matched_dst->name);
		}
		else {
			matched_dst->peer_ref = matched_src;
			matched_dst->force = rs[i].force;
		}
	}
	return -errs;
}

static struct ref *find_ref_by_name(struct ref *list, const char *name)
{
	for ( ; list; list = list->next)
		if (!strcmp(list->name, name))
			return list;
	return NULL;
}

int match_refs(struct ref *src, struct ref *dst, struct ref ***dst_tail,
	       int nr_refspec, char **refspec, int all)
{
	struct refspec *rs = parse_ref_spec(nr_refspec, refspec);

	if (nr_refspec)
		return match_explicit_refs(src, dst, dst_tail, rs);

	/* pick the remainder */
	for ( ; src; src = src->next) {
		struct ref *dst_peer;
		if (src->peer_ref)
			continue;
		dst_peer = find_ref_by_name(dst, src->name);
		if ((dst_peer && dst_peer->peer_ref) || (!dst_peer && !all))
			continue;
		if (!dst_peer) {
			/* Create a new one and link it */
			int len = strlen(src->name) + 1;
			dst_peer = xcalloc(1, sizeof(*dst_peer) + len);
			memcpy(dst_peer->name, src->name, len);
			memcpy(dst_peer->new_sha1, src->new_sha1, 20);
			link_dst_tail(dst_peer, dst_tail);
		}
		dst_peer->peer_ref = src;
	}
	return 0;
}

enum protocol {
	PROTO_LOCAL = 1,
	PROTO_SSH,
	PROTO_GIT,
};

static enum protocol get_protocol(const char *name)
{
	if (!strcmp(name, "ssh"))
		return PROTO_SSH;
	if (!strcmp(name, "git"))
		return PROTO_GIT;
	if (!strcmp(name, "git+ssh"))
		return PROTO_SSH;
	if (!strcmp(name, "ssh+git"))
		return PROTO_SSH;
	die("I don't handle protocol '%s'", name);
}

#define STR_(s)	# s
#define STR(s)	STR_(s)

#ifndef NO_IPV6

static int git_tcp_connect(int fd[2], const char *prog, char *host, char *path)
{
	int sockfd = -1;
	char *colon, *end;
	char *port = STR(DEFAULT_GIT_PORT);
	struct addrinfo hints, *ai0, *ai;
	int gai;

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		port = colon + 1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	gai = getaddrinfo(host, port, &hints, &ai);
	if (gai)
		die("Unable to look up %s (%s)", host, gai_strerror(gai));

	for (ai0 = ai; ai; ai = ai->ai_next) {
		sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sockfd < 0)
			continue;
		if (connect(sockfd, ai->ai_addr, ai->ai_addrlen) < 0) {
			close(sockfd);
			sockfd = -1;
			continue;
		}
		break;
	}

	freeaddrinfo(ai0);

	if (sockfd < 0)
		die("unable to connect a socket (%s)", strerror(errno));

	fd[0] = sockfd;
	fd[1] = sockfd;
	packet_write(sockfd, "%s %s\n", prog, path);
	return 0;
}

#else /* NO_IPV6 */

static int git_tcp_connect(int fd[2], const char *prog, char *host, char *path)
{
	int sockfd = -1;
	char *colon, *end;
	char *port = STR(DEFAULT_GIT_PORT), *ep;
	struct hostent *he;
	struct sockaddr_in sa;
	char **ap;
	unsigned int nport;

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		port = colon + 1;
	}


	he = gethostbyname(host);
	if (!he)
		die("Unable to look up %s (%s)", host, hstrerror(h_errno));
	nport = strtoul(port, &ep, 10);
	if ( ep == port || *ep ) {
		/* Not numeric */
		struct servent *se = getservbyname(port,"tcp");
		if ( !se )
			die("Unknown port %s\n", port);
		nport = se->s_port;
	}

	for (ap = he->h_addr_list; *ap; ap++) {
		sockfd = socket(he->h_addrtype, SOCK_STREAM, 0);
		if (sockfd < 0)
			continue;

		memset(&sa, 0, sizeof sa);
		sa.sin_family = he->h_addrtype;
		sa.sin_port = htons(nport);
		memcpy(&sa.sin_addr, *ap, he->h_length);

		if (connect(sockfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
			close(sockfd);
			sockfd = -1;
			continue;
		}
		break;
	}

	if (sockfd < 0)
		die("unable to connect a socket (%s)", strerror(errno));

	fd[0] = sockfd;
	fd[1] = sockfd;
	packet_write(sockfd, "%s %s\n", prog, path);
	return 0;
}

#endif /* NO_IPV6 */

static char *git_proxy_command = NULL;
static const char *rhost_name = NULL;
static int rhost_len;

static int git_proxy_command_options(const char *var, const char *value)
{
	if (!strcmp(var, "core.gitproxy")) {
		const char *for_pos;
		int matchlen = -1;
		int hostlen;

		if (git_proxy_command)
			return 0;
		/* [core]
		 * ;# matches www.kernel.org as well
		 * gitproxy = netcatter-1 for kernel.org
		 * gitproxy = netcatter-2 for sample.xz
		 * gitproxy = netcatter-default
		 */
		for_pos = strstr(value, " for ");
		if (!for_pos)
			/* matches everybody */
			matchlen = strlen(value);
		else {
			hostlen = strlen(for_pos + 5);
			if (rhost_len < hostlen)
				matchlen = -1;
			else if (!strncmp(for_pos + 5,
					  rhost_name + rhost_len - hostlen,
					  hostlen) &&
				 ((rhost_len == hostlen) ||
				  rhost_name[rhost_len - hostlen -1] == '.'))
				matchlen = for_pos - value;
			else
				matchlen = -1;
		}
		if (0 <= matchlen) {
			/* core.gitproxy = none for kernel.org */
			if (matchlen == 4 && 
			    !memcmp(value, "none", 4))
				matchlen = 0;
			git_proxy_command = xmalloc(matchlen + 1);
			memcpy(git_proxy_command, value, matchlen);
			git_proxy_command[matchlen] = 0;
		}
		return 0;
	}

	return git_default_config(var, value);
}

static int git_use_proxy(const char *host)
{
	rhost_name = host;
	rhost_len = strlen(host);
	git_proxy_command = getenv("GIT_PROXY_COMMAND");
	git_config(git_proxy_command_options);
	rhost_name = NULL;
	return (git_proxy_command && *git_proxy_command);
}

static int git_proxy_connect(int fd[2], const char *prog, char *host, char *path)
{
	char *port = STR(DEFAULT_GIT_PORT);
	char *colon, *end;
	int pipefd[2][2];
	pid_t pid;

	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			host++;
		} else
			end = host;
	} else
		end = host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		port = colon + 1;
	}

	if (pipe(pipefd[0]) < 0 || pipe(pipefd[1]) < 0)
		die("unable to create pipe pair for communication");
	pid = fork();
	if (!pid) {
		dup2(pipefd[1][0], 0);
		dup2(pipefd[0][1], 1);
		close(pipefd[0][0]);
		close(pipefd[0][1]);
		close(pipefd[1][0]);
		close(pipefd[1][1]);
		execlp(git_proxy_command, git_proxy_command, host, port, NULL);
		die("exec failed");
	}
	fd[0] = pipefd[0][0];
	fd[1] = pipefd[1][1];
	close(pipefd[0][1]);
	close(pipefd[1][0]);
	packet_write(fd[1], "%s %s\n", prog, path);
	return pid;
}

/*
 * Yeah, yeah, fixme. Need to pass in the heads etc.
 */
int git_connect(int fd[2], char *url, const char *prog)
{
	char command[1024];
	char *host, *path = url;
	char *colon = NULL;
	int pipefd[2][2];
	pid_t pid;
	enum protocol protocol = PROTO_LOCAL;

	host = strstr(url, "://");
	if(host) {
		*host = '\0';
		protocol = get_protocol(url);
		host += 3;
		path = strchr(host, '/');
	}
	else {
		host = url;
		if ((colon = strchr(host, ':'))) {
			protocol = PROTO_SSH;
			*colon = '\0';
			path = colon + 1;
		}
	}

	if (!path || !*path)
		die("No path specified. See 'man git-pull' for valid url syntax");

	/*
	 * null-terminate hostname and point path to ~ for URL's like this:
	 *    ssh://host.xz/~user/repo
	 */
	if (protocol != PROTO_LOCAL && host != url) {
		char *ptr = path;
		if (path[1] == '~')
			path++;
		else
			path = strdup(ptr);

		*ptr = '\0';
	}

	if (protocol == PROTO_GIT) {
		if (git_use_proxy(host))
			return git_proxy_connect(fd, prog, host, path);
		return git_tcp_connect(fd, prog, host, path);
	}

	if (pipe(pipefd[0]) < 0 || pipe(pipefd[1]) < 0)
		die("unable to create pipe pair for communication");
	pid = fork();
	if (!pid) {
		snprintf(command, sizeof(command), "%s %s", prog,
			 sq_quote(path));
		dup2(pipefd[1][0], 0);
		dup2(pipefd[0][1], 1);
		close(pipefd[0][0]);
		close(pipefd[0][1]);
		close(pipefd[1][0]);
		close(pipefd[1][1]);
		if (protocol == PROTO_SSH) {
			const char *ssh, *ssh_basename;
			ssh = getenv("GIT_SSH");
			if (!ssh) ssh = "ssh";
			ssh_basename = strrchr(ssh, '/');
			if (!ssh_basename)
				ssh_basename = ssh;
			else
				ssh_basename++;
			execlp(ssh, ssh_basename, host, command, NULL);
		}
		else
			execlp("sh", "sh", "-c", command, NULL);
		die("exec failed");
	}		
	fd[0] = pipefd[0][0];
	fd[1] = pipefd[1][1];
	close(pipefd[0][1]);
	close(pipefd[1][0]);
	return pid;
}

int finish_connect(pid_t pid)
{
	int ret;

	for (;;) {
		ret = waitpid(pid, NULL, 0);
		if (!ret)
			break;
		if (errno != EINTR)
			break;
	}
	return ret;
}
