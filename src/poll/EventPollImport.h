#ifndef D_EVENT_POLL_IMPORT_H
#define D_EVENT_POLL_IMPORT_H

#ifdef HAVE_LIBUV
#  include "libuv/LibuvEventPoll.h"
#endif // HAVE_LIBUV
#ifdef HAVE_EPOLL
#  include "epoll/EpollEventPoll.h"
#endif // HAVE_EPOLL
#ifdef HAVE_PORT_ASSOCIATE
#  include "port/PortEventPoll.h"
#endif // HAVE_PORT_ASSOCIATE
#ifdef HAVE_KQUEUE
#  include "kqueue/KqueueEventPoll.h"
#endif // HAVE_KQUEUE
#ifdef HAVE_POLL
#  include "poll/PollEventPoll.h"
#endif // HAVE_POLL
#include "select/SelectEventPoll.h"

#endif // D_EVENT_POLL_IMPORT_H
