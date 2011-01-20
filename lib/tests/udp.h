#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>

typedef ssize_t (*data_cb) (const uint8_t *buf, ssize_t len);
struct polldata {
  int s;
  data_cb cb;
  struct timeval *timeout;
};

struct polldata *udp_server (const char *bindto, struct timeval *timeout, data_cb cb);
ssize_t udp_poll (struct polldata *data);
