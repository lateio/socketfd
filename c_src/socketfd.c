/*
 * ------------------------------------------------------------------------------
 *
 * Copyright © 2019, Lauri Moisio <l@arv.io>
 *
 * The ISC License
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED “AS IS” AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * ------------------------------------------------------------------------------
 */

 /*
 	When modifying this file, please be extremely thorough when
	it comes to error handling. We don't want to kneecap BEAM if we
	can avoid it.
  */
#include <erl_nif.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define SOCKETFD_CLOSED (1<<0)
#define SOCKETFD_ANY    (1<<1) // Whether the socket is set up to capture both IPv4 and IPv6 (IPV6_V6ONLY)
#define INET      4
#define INET6     8

//#define DEBUG_TRACE // Add some logging to demonstrate how NIFs and
// and the data types/objects they create, work

#define ARRAY_SIZE(x) ((sizeof(x))/(sizeof(x[0])))

union inet_addr {
	struct sockaddr_in inet;
	struct sockaddr_in6 inet6;
};

struct socketfd {
	 // We have to know if we have already closed the enclosed socket,
	 // as the fd value will be reused, and we might unintendedly
	 // close something else and mangle the program
	 //
	 // Also just setting fd to -1 would have worked.
	int flags;
	int fd;
	union inet_addr addr;
};

static int populate_sockaddr(ErlNifEnv *env, struct sockaddr_storage *addr, ERL_NIF_TERM term, ERL_NIF_TERM *err_ret)
{
	if (!enif_compare(term, enif_make_atom(env, "inet"))) {
		addr->ss_family = AF_INET;
		return 0;
	} else if (!enif_compare(term, enif_make_atom(env, "inet6"))) {
		addr->ss_family = AF_INET6;
		return 0;
	} else if (!enif_compare(term, enif_make_atom(env, "any"))) {
		addr->ss_family = AF_INET6;
		return 0;
	}

	int address_arity;
	const ERL_NIF_TERM *address;
	if (!enif_get_tuple(env, term, &address_arity, &address)) {
		enif_make_badarg(env);
		return -1;
	}

	unsigned int element;
	switch (address_arity) {
	case INET:
		addr->ss_family = AF_INET;
		unsigned int ipv4addr = 0;
		for (int i = 0; i < address_arity; ++i) {
			if (!enif_get_uint(env, address[i], &element) || element > 255) {
				*err_ret = enif_make_tuple2(env, enif_make_atom(env, "bad_address"), term);
				return -1;
			}
			ipv4addr = ipv4addr << 8 | (element & 0xFF);
		}
		((struct sockaddr_in *)addr)->sin_addr.s_addr = htonl(ipv4addr);
		break;
	case INET6:
		addr->ss_family = AF_INET6;
		for (int i = 0; i < address_arity; ++i) {
			if (!enif_get_uint(env, address[i], &element) || element > 0xFFFF) {
				*err_ret = enif_make_tuple2(env, enif_make_atom(env, "bad_address"), term);
				return -1;
			}
			((struct sockaddr_in6 *)addr)->sin6_addr.s6_addr[(i*2)] = (element >> 8) & 0xFF;
			((struct sockaddr_in6 *)addr)->sin6_addr.s6_addr[(i*2) + 1] = element & 0xFF;
		}
		break;
	default:
		*err_ret = enif_make_tuple2(env, enif_make_atom(env, "bad_address"), term);
		return -1;
	}

	return 0;
}

static ERL_NIF_TERM error_return(ErlNifEnv *env, ERL_NIF_TERM reason)
{
	return enif_make_tuple2(env, enif_make_atom(env, "error"), reason);
}

static const struct error_atom {
	int value;
	const char *atom;
} errors[] = {
	{EACCES,          "eacces"         },
	{EADDRINUSE,	  "eaddrinuse"	   },
	{EADDRNOTAVAIL,	  "eaddrnotavail"  },
	{EAFNOSUPPORT,    "eafnosupport"   },
	{EBADF,		  "ebadf"	   },
	{EDESTADDRREQ,	  "edestaddrreq"   },
	{EDOM,		  "edom"	   },
	{EFAULT,	  "efault"	   },
	{EINTR,		  "eintr"	   },
	{EINVAL,	  "einval"	   },
	{EIO,		  "eio"		   },
	{EISCONN,	  "eisconn"	   },
	{EMFILE,          "emfile"         },
	{ENFILE,          "enfile"         },
	{ENOBUFS,         "enobufs"        },
	{ENOMEM,          "enomem"         },
	{ENOPROTOOPT,	  "enoprotoopt"	   },
	{ENOTSOCK,	  "enotsock"	   },
	{EPROTONOSUPPORT, "eprotonosupport"},
	{EPROTOTYPE,      "eprototype"     },
	{EOPNOTSUPP,	  "eopnotsupp"	   }
};

static ERL_NIF_TERM errno_error_return(ErlNifEnv *env, const char *reason_prefix)
{
	ERL_NIF_TERM reason_atom = enif_make_atom(env, "error_placeholder");
	for (int i = 0; i < ARRAY_SIZE(errors); ++i) {
		if (errno != errors[i].value)
			continue;

		reason_atom = enif_make_atom(env, errors[i].atom);
		break;
	}

	ERL_NIF_TERM reason;
	if (!reason_prefix)
		reason = reason_atom;
	else
		reason = enif_make_tuple2(env, enif_make_atom(env, reason_prefix), reason_atom);

	return error_return(env, reason);
}

static int close_socket_fd(int fd)
{
	int rc;

	while ((rc = close(fd)) && errno == EINTR);

	if (!rc)
		fd = -1;

	return !(fd == -1);
}

static ERL_NIF_TERM open_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	if (!enif_compare(argv[2], enif_make_atom(env, "any"))) {
#ifdef __APPLE__
		return error_return(env, enif_make_atom(env, "address_family_any_disabled"));
#endif
#ifdef __linux__
		return error_return(env, enif_make_atom(env, "address_family_any_disabled"));
#endif
	}

	unsigned int port;
	if (!enif_get_uint(env, argv[1], &port))
		return enif_make_badarg(env);

	if (port > 0xFFFF)
		return error_return(env, enif_make_atom(env, "invalid_port"));

	int socket_type;
	if (!enif_compare(argv[0], enif_make_atom(env, "stream"))) {
		socket_type = SOCK_STREAM;
	} else if (!enif_compare(argv[0], enif_make_atom(env, "tcp"))) {
		socket_type = SOCK_STREAM;
	} else if (!enif_compare(argv[0], enif_make_atom(env, "datagram"))) {
		socket_type = SOCK_DGRAM;
	} else if (!enif_compare(argv[0], enif_make_atom(env, "udp"))) {
		socket_type = SOCK_DGRAM;;
	} else {
		return error_return(env, enif_make_atom(env, "invalid_socket_type"));
	}

	// Populate sockaddr from the list
	struct sockaddr_storage addr = { .ss_family = AF_INET };
	ERL_NIF_TERM err_ret = argv[2];
	if (populate_sockaddr(env, &addr, argv[2], &err_ret)) {
		if (err_ret != argv[2])
			return error_return(env, err_ret);
		enif_has_pending_exception(env, &err_ret);
		return err_ret;
	}

	socklen_t addrlen = 0;
	switch (addr.ss_family) {
	case AF_INET:
		((struct sockaddr_in *)&addr)->sin_port = htons(((unsigned short)port));
		addrlen = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		((struct sockaddr_in6 *)&addr)->sin6_port = htons(((unsigned short)port));
		addrlen = sizeof(struct sockaddr_in6);
		break;
	default:
		return error_return(env, enif_make_atom(env, "invalid_address_family"));
	}

	int fd = socket(addr.ss_family, socket_type, 0);
	if (fd < 0)
		return errno_error_return(env, "socket");

	if (addr.ss_family == AF_INET6) {
		int curflags, newflags;
		socklen_t optlen = sizeof(curflags);
		if (getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &curflags, &optlen)) {
			ERL_NIF_TERM error_tuple = errno_error_return(env, "getsockopt");

			if (close_socket_fd(fd))
				enif_raise_exception(env, enif_make_atom(env, "close_socket_error"));

			return error_tuple;
		}

		switch (enif_compare(argv[2], enif_make_atom(env, "any"))) {
		case 0:
			// Make ipv6 wildcard into a catchall
			newflags = 0;
			break;
		default:
			newflags = 1;
			break;
		}

		optlen = sizeof(newflags);
		if (newflags != curflags && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &newflags, optlen) != 0) {
			ERL_NIF_TERM error_tuple = errno_error_return(env, "setsockopt");

			if (close_socket_fd(fd))
				enif_raise_exception(env, enif_make_atom(env, "close_socket_error"));

			return error_tuple;
		}
	}

	if (bind(fd, (struct sockaddr *)&addr, addrlen)) {
		ERL_NIF_TERM error_tuple = errno_error_return(env, "bind");

		if (close_socket_fd(fd))
			enif_raise_exception(env, enif_make_atom(env, "close_socket_error"));

		return error_tuple;
	}

	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd_obj = enif_alloc_resource(*type, sizeof(struct socketfd));
	if (!fd_obj) {
		if (close_socket_fd(fd))
			enif_raise_exception(env, enif_make_atom(env, "close_socket_error"));
		return error_return(env, enif_make_atom(env, "alloc_resource"));
	}

	fd_obj->fd = fd;
	fd_obj->flags = 0;
	memcpy(&fd_obj->addr, &addr, addrlen);

	if (!enif_compare(argv[2], enif_make_atom(env, "any")))
		fd_obj->flags |= SOCKETFD_ANY;

	ERL_NIF_TERM obj = enif_make_resource(env, fd_obj);
	enif_release_resource(fd_obj);

	return enif_make_tuple2(env, enif_make_atom(env, "ok"), obj);
}

static ERL_NIF_TERM dup_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd;
	if (!enif_get_resource(env, argv[0], *type, (void **)&fd))
		return enif_make_badarg(env);

	if (fd->flags & SOCKETFD_CLOSED)
		return error_return(env, enif_make_atom(env, "closed"));

	int newfd;
	do {
		newfd = dup(fd->fd);
	} while (newfd < 0 && errno == EINTR);

	if (newfd < 0)
		return errno_error_return(env, "dup");

	struct socketfd *newfd_obj = enif_alloc_resource(*type, sizeof(struct socketfd));
	if (!newfd_obj) {
		if (close_socket_fd(newfd))
			enif_raise_exception(env, enif_make_atom(env, "close_socket_error"));
		return error_return(env, enif_make_atom(env, "alloc_resource"));
	}

	newfd_obj->fd = newfd;
	newfd_obj->flags = 0;
	memcpy(&newfd_obj->addr, &fd->addr, sizeof(fd->addr));
	ERL_NIF_TERM obj = enif_make_resource(env, newfd_obj);
	enif_release_resource(newfd_obj);

	return enif_make_tuple2(env, enif_make_atom(env, "ok"), obj);
}

static ERL_NIF_TERM get_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd;
	if (!enif_get_resource(env, argv[0], *type, (void **)&fd))
		return enif_make_badarg(env);

	if (fd->flags & SOCKETFD_CLOSED)
		return error_return(env, enif_make_atom(env, "closed"));

	return enif_make_int(env, fd->fd);
}

static ERL_NIF_TERM close_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd;
	if (!enif_get_resource(env, argv[0], *type, (void **)&fd))
		return enif_make_badarg(env);

	if (fd->flags & SOCKETFD_CLOSED) {
		return enif_make_atom(env, "ok");
	} else if (!close_socket_fd(fd->fd)) {
		fd->flags |= SOCKETFD_CLOSED;
		memset(&fd->addr, 0x00, sizeof(fd->addr));
		return enif_make_atom(env, "ok");
	}

	return errno_error_return(env, "close");
}

static int sockaddr_to_tuple(const union inet_addr *addr, ErlNifEnv *env, ERL_NIF_TERM *tuple)
{
	unsigned char buf[16]; // max(IPv4 = 4 bytes, IPv6 = 16 bytes)
	switch (addr->inet.sin_family) {
	case AF_INET:
		memcpy(buf, (void *)&addr->inet.sin_addr.s_addr, sizeof(addr->inet.sin_addr.s_addr));
		*tuple = enif_make_tuple4(
			env,
			enif_make_uint(env, buf[0]),
			enif_make_uint(env, buf[1]),
			enif_make_uint(env, buf[2]),
			enif_make_uint(env, buf[3])
		);
		break;
	case AF_INET6:
		memcpy(buf, (void *)addr->inet6.sin6_addr.s6_addr, sizeof(addr->inet6.sin6_addr.s6_addr));
		*tuple = enif_make_tuple8(
			env,
			enif_make_uint(env, buf[ 0] << 8 | buf[ 1]),
			enif_make_uint(env, buf[ 2] << 8 | buf[ 3]),
			enif_make_uint(env, buf[ 4] << 8 | buf[ 5]),
			enif_make_uint(env, buf[ 6] << 8 | buf[ 7]),
			enif_make_uint(env, buf[ 8] << 8 | buf[ 9]),
			enif_make_uint(env, buf[10] << 8 | buf[11]),
			enif_make_uint(env, buf[12] << 8 | buf[13]),
			enif_make_uint(env, buf[14] << 8 | buf[15])
		);
		break;
	default:
		return -1;
	}

	return 0;
}

static ERL_NIF_TERM address_family_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd;
	if (!enif_get_resource(env, argv[0], *type, (void **)&fd))
		return enif_make_badarg(env);

	if (fd->flags & SOCKETFD_CLOSED)
		return error_return(env, enif_make_atom(env, "closed"));

	if (fd->flags & SOCKETFD_ANY)
		return enif_make_atom(env, "any");

	ERL_NIF_TERM address_family;
	switch (fd->addr.inet.sin_family) {
	case AF_INET:
		address_family = enif_make_atom(env, "inet");
		break;
	case AF_INET6:
		address_family = enif_make_atom(env, "inet6");
		break;
	default:
		return error_return(
			env,
			enif_make_tuple2(
				env,
				enif_make_atom(env, "unknown_address_family"),
				enif_make_uint(env, fd->addr.inet.sin_family)
			)
		);
	}

	return address_family;
}

static ERL_NIF_TERM socktype_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd;
	if (!enif_get_resource(env, argv[0], *type, (void **)&fd))
		return enif_make_badarg(env);

	if (fd->flags & SOCKETFD_CLOSED)
		return error_return(env, enif_make_atom(env, "closed"));

	int type_int;
	socklen_t typelen = sizeof(type_int);
	if (getsockopt(fd->fd, SOL_SOCKET, SO_TYPE, &type_int, &typelen))
		return errno_error_return(env, "getsockopt");

	ERL_NIF_TERM type_atom;
	switch (type_int) {
	case SOCK_STREAM:
		type_atom = enif_make_atom(env, "tpc");
		break;
	case SOCK_DGRAM:
		type_atom = enif_make_atom(env, "udp");
		break;
	default:
		return error_return(
			env,
			enif_make_tuple2(
				env,
				enif_make_atom(env, "unknown_socket_type"),
				enif_make_int(env, type_int)
			)
		);
	}

	return type_atom;
}

static ERL_NIF_TERM address_port_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifResourceType **type = enif_priv_data(env);

	struct socketfd *fd;
	if (!enif_get_resource(env, argv[0], *type, (void **)&fd))
		return enif_make_badarg(env);

	if (fd->flags & SOCKETFD_CLOSED)
		return error_return(env, enif_make_atom(env, "closed"));

	ERL_NIF_TERM address_tuple;
	if (sockaddr_to_tuple(&fd->addr, env, &address_tuple))
		return error_return(
			env,
			enif_make_tuple2(
				env,
				enif_make_atom(env, "unknown_address_family"),
				enif_make_uint(env, fd->addr.inet.sin_family)
			)
		);

	unsigned short port;
	switch (fd->addr.inet.sin_family) {
	case AF_INET:
		port = ntohs(fd->addr.inet.sin_port);
		break;
	case AF_INET6:
		port = ntohs(fd->addr.inet6.sin6_port);
		break;
	default:
		return error_return(
			env,
			enif_make_tuple2(
				env,
				enif_make_atom(env, "unknown_address_family"),
				enif_make_uint(env, fd->addr.inet.sin_family)
			)
		);
	}

	return enif_make_tuple2(env, address_tuple, enif_make_uint(env, port));
}

static void destructor(ErlNifEnv *env, struct socketfd *ptr)
{
	if (!(ptr->flags & SOCKETFD_CLOSED))
		close_socket_fd(ptr->fd);
}

static int load(ErlNifEnv *env, ErlNifResourceType ***priv_data, ERL_NIF_TERM load_info)
{
	*priv_data = enif_alloc(sizeof(ErlNifResourceType *));
	if (!*priv_data)
		return -1;

	ErlNifResourceFlags tried;

	**priv_data = enif_open_resource_type(
		env,
		"socketfd",
		"socketfd",
		(void (*)(ErlNifEnv *, void *))&destructor,
        	ERL_NIF_RT_CREATE,
        	&tried
	);
	if (!**priv_data) {
		enif_free(*priv_data);
		return -1;
	}

	return 0;
}

static ErlNifFunc export[] = {
        {"open",           3, open_socket           },
	{"dup",            1, dup_socket            },
	{"get",            1, get_socket            },
	{"close",          1, close_socket          },
	{"address_family", 1, address_family_socket },
	{"socket_type",    1, socktype_socket       },
	{"address_port",   1, address_port_socket   }
};

ERL_NIF_INIT(
        socketfd,
        export,
        (int (*)(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info))&load,
        NULL,
        NULL,
        NULL
)
