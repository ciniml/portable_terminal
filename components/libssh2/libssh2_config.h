/* Hand-rolled libssh2 config for ESP-IDF (no autoconf).
 * Targets newlib + lwIP BSD sockets + mbedTLS 3.6 (IDF 6.0).
 * libssh2 picks this up because we compile with -DHAVE_CONFIG_H=1, which
 * makes libssh2_setup.h #include this file.
 */
#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

/* POSIX-ish headers newlib gives us */
#define HAVE_UNISTD_H        1
#define HAVE_INTTYPES_H      1
#define HAVE_STDLIB_H        1
#define HAVE_SYS_TIME_H      1
#define HAVE_SYS_UIO_H       1
#define HAVE_SYS_SELECT_H    1
#define HAVE_SYS_SOCKET_H    1
#define HAVE_SYS_IOCTL_H     1
#define HAVE_ARPA_INET_H     1
#define HAVE_NETINET_IN_H    1
#define HAVE_FCNTL_H         1

/* Functions */
#define HAVE_GETTIMEOFDAY    1
#define HAVE_SELECT          1
#define HAVE_SNPRINTF        1
#define HAVE_STRTOLL         1

/* No zlib — leave LIBSSH2_HAVE_ZLIB undefined (comp.c uses #ifdef). */

/* Use lwIP non-blocking I/O via fcntl O_NONBLOCK */
#define HAVE_O_NONBLOCK      1

#endif  /* LIBSSH2_CONFIG_H */
