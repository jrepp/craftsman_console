#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>

static struct addrinfo hints;
static int sfd;
static char storage[_SS_SIZE];
static uint32_t usedStorage;
static bool connected;
static bool connecting;
static bool disconnected;

void
sys_networkNoNagle()
{
  int flag = 1;
  setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

void
sys_networkNonBlocking()
{
  int flags = 0;
  if (sfd <= STDERR_FILENO)
    return;

  flags = fcntl(sfd, F_GETFL, 0);
  if (flags == -1)
    return;

  printf("flags %u\n", flags);

  fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

  flags = fcntl(sfd, F_GETFL, 0);
  printf("new flags %u\n", flags);
}

bool
sys_networkConfigure(const char *host, const char *service_or_port)
{
  struct addrinfo *result = NULL;

  if (sfd > STDERR_FILENO)
    close(sfd);
  sfd = 0;
  connecting = connected = disconnected = false;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = IPPROTO_TCP;

  getaddrinfo(host, service_or_port, &hints, &result);
  if (!result)
    return false;

  sfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  sys_networkNoNagle();
  sys_networkNonBlocking();

  if (sfd == -1)
    return false;

  memcpy(storage, result->ai_addr, result->ai_addrlen);
  usedStorage = result->ai_addrlen;

  freeaddrinfo(result);

  return true;
}

bool
sys_networkErrInterrupt()
{
  return (errno == EINTR || errno == EINPROGRESS);
}

bool
sys_networkConnect()
{
  if (sfd <= STDERR_FILENO)
    return false;

  int result = connect(sfd, (struct sockaddr *) storage, usedStorage);
  if (result == -1 && !sys_networkErrInterrupt()) {
    return false;
  }

  connecting = true;

  return true;
}

ssize_t
sys_networkRead(ssize_t n, char buffer[n])
{
  return read(sfd, buffer, n);
}

ssize_t
sys_networkWrite(ssize_t n, const char buffer[n])
{
  return write(sfd, buffer, n);
}

int32_t
sys_networkPoll()
{
  struct pollfd fds = { .fd = sfd, .events = POLLIN | POLLOUT | POLLERR };
  int poll_num = poll(&fds, 1, 0);

  if (poll_num == -1) {
    if (errno == EINTR)
      return 0;

    return errno;
  }

  if (fds.revents & (POLLIN | POLLOUT))
    connected = connecting;
  if (fds.revents & POLLERR)
    disconnected = true;

  // printf("%d poll_num %d revents\n", poll_num, fds.revents);

  return fds.revents;
}

bool
sys_networkIsReady()
{
  sys_networkPoll();

  return connected && !disconnected;
}