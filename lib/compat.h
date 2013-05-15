/* Copyright 2011 NORDUnet A/S. All rights reserved.
   See LICENSE for licensing information. */

#ifdef _WIN32
#define INLINE __inline
#else
#define INLINE inline
#endif

ssize_t compat_send (int sockfd, const void *buf, size_t len, int flags);
ssize_t compat_recv (int sockfd, void *buf, size_t len, int flags);
