#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
	/** Is the channel closed? */
	bool is_closed;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus* coro_bus_new_alloc = calloc(1, sizeof(struct coro_bus));
	
	if (coro_bus_new_alloc == NULL) 
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return NULL;
	}
	
	coro_bus_new_alloc->channel_count = -1;
	return coro_bus_new_alloc;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	for (int i = 0; i < bus->channel_count; ++i)
	{
		coro_bus_channel_close(bus, i);
	}
	free(bus->channels);
	free(bus);
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{

	/*
	 * One of the tests will force you to reuse the channel
	 * descriptors. It means, that if your maximal channel
	 * descriptor is N, and you have any free descriptor in
	 * the range 0-N, then you should open the new channel on
	 * that old descriptor.
	 *
	 * A more precise instruction - check if any of the
	 * bus->channels[i] with i = 0 -> bus->channel_count is
	 * free (== NULL). If yes - reuse the slot. Don't grow the
	 * bus->channels array, when have space in it.
	 */
	struct coro_bus_channel* new_ch = calloc(1, sizeof(struct coro_bus_channel));

	if (new_ch == NULL) 
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	new_ch->size_limit = size_limit;
	new_ch->is_closed = false;

	rlist_create(&new_ch->send_queue.coros);
	rlist_create(&new_ch->recv_queue.coros);

	new_ch->data.capacity = size_limit;
	new_ch->data.data = malloc(size_limit * sizeof(unsigned));
	if (new_ch->data.data == NULL) 
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	new_ch->data.size = 0;

	if (bus->channel_count == -1) 
	{
		bus->channels = malloc(sizeof(struct coro_bus_channel));
		if (bus->channels == NULL)
		{
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}
		bus->channel_count = 1;
		bus->channels[0] = new_ch;
		return 0;
	}

	for (int i = 0; i < bus->channel_count; ++i)
	{
		if (bus->channels[i] == NULL)
		{
			bus->channels[i] = new_ch;
			return i;
		}
	}

	struct coro_bus_channel** tmp_chs = realloc(bus->channels, (bus->channel_count + 1) * sizeof(struct coro_bus_channel));
	if (tmp_chs == NULL)
	{
		coro_bus_delete(bus);
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	
	bus->channels = tmp_chs;
	bus->channels[bus->channel_count] = new_ch;

	return bus->channel_count++;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	/*
	 * Be very attentive here. What happens, if the channel is
	 * closed while there are coroutines waiting on it? For
	 * example, the channel was empty, and some coros were
	 * waiting on its recv_queue.
	 *
	 * If you wakeup those coroutines and just delete the
	 * channel right away, then those waiting coroutines might
	 * on wakeup try to reference invalid memory.
	 *
	 * Can happen, for example, if you use an intrusive list
	 * (rlist), delete the list itself (by deleting the
	 * channel), and then the coroutines on wakeup would try
	 * to remove themselves from the already destroyed list.
	 *
	 * Think how you could address that. Remove all the
	 * waiters from the list before freeing it? Yield this
	 * coroutine after waking up the waiters but before
	 * freeing the channel, so the waiters could safely leave?
	 */
	 if (bus->channel_count <= 0)
	 {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
	 }
	 else 
	 {
		struct coro_bus_channel* current_ch = bus->channels[channel];
		if (current_ch == NULL) 
		{
			return;
		}

		current_ch->is_closed = true;

		while (!rlist_empty(&current_ch->send_queue.coros)) 
		{
			struct wakeup_entry* coro_w = 
				rlist_entry(rlist_first(&current_ch->send_queue.coros), struct wakeup_entry, base);
			coro_wakeup(coro_w->coro);
			coro_yield();
		}
		
		while (!rlist_empty(&current_ch->recv_queue.coros))
		{
			struct wakeup_entry* coro_w = 
				rlist_entry(rlist_first(&current_ch->recv_queue.coros), struct wakeup_entry, base);
			coro_wakeup(coro_w->coro);
			coro_yield();
		}

		free(current_ch->data.data);
		
		free(current_ch);
		bus->channels[channel] = NULL;
	 }
}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	/*
	 * Try sending in a loop, until success. If error, then
	 * check which one is that. If 'wouldblock', then suspend
	 * this coroutine and try again when woken up.
	 *
	 * If see the channel has space, then wakeup the first
	 * coro in the send-queue. That is needed so when there is
	 * enough space for many messages, and many coroutines are
	 * waiting, they would then wake each other up one by one
	 * as lone as there is still space.
	 */
	if (bus->channel_count <= 0 || channel >= bus->channel_count)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel* current_ch = bus->channels[channel]; 
	if (current_ch == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	bool in_list = false;
	struct wakeup_entry coro_sus;
	coro_sus.coro = coro_this();
	while (coro_bus_try_send(bus, channel, data) && !current_ch->is_closed)
	{
		if (!in_list) 
		{
			rlist_add_tail(&current_ch->send_queue.coros, &coro_sus.base);
			in_list = !in_list;
		}
		coro_suspend();
	}

	if (in_list)
	{
		rlist_del(&coro_sus.base);
	}

	
	return coro_bus_errno() ? -1 : 0;
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{

	if (bus->channel_count <= 0 || channel >= bus->channel_count)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel* current_ch = bus->channels[channel];

	/*
	 * Append data if has space. Otherwise 'wouldblock' error.
	 * Wakeup the first coro in the recv-queue! To let it know
	 * there is data.
	 */
	if (current_ch == NULL || current_ch->is_closed)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	int current_size = current_ch->data.size;
	if ((int)current_ch->data.capacity == current_size)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	++(current_ch->data.size);
	current_ch->data.data[current_size] = data;

	if (!rlist_empty(&current_ch->recv_queue.coros))
	{
		struct wakeup_entry* wakeup_coro = 
			rlist_entry(rlist_first(&current_ch->recv_queue.coros), struct wakeup_entry, base);
		coro_wakeup(wakeup_coro->coro);
		rlist_move_tail(&current_ch->recv_queue.coros, &wakeup_coro->base);
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (bus->channel_count <= 0 || channel >= bus->channel_count)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel* current_ch = bus->channels[channel]; 
	if (current_ch == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	bool in_list = false;
	struct wakeup_entry coro_sus;
	coro_sus.coro = coro_this();
	while (coro_bus_try_recv(bus, channel, data) && !current_ch->is_closed)
	{
		if (!in_list) 
		{
			rlist_add_tail(&current_ch->recv_queue.coros, &coro_sus.base);
			in_list = !in_list;
		}
		coro_suspend();
	}

	if (in_list)
	{
		rlist_del(&coro_sus.base);
	}

	return coro_bus_errno() ? -1 : 0;
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{

	if (bus->channel_count <= 0 || channel >= bus->channel_count)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel* current_ch = bus->channels[channel];

	if (current_ch == NULL || current_ch->is_closed)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	else if (current_ch->data.size == 0)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	*data = *current_ch->data.data;
	--(current_ch->data.size);
	for (size_t i = 0; i < current_ch->data.size; ++i) 
	{
		current_ch->data.data[i] = current_ch->data.data[i + 1];
	}

	if (!rlist_empty(&current_ch->send_queue.coros))
	{
		struct wakeup_entry* wakeup_coro = 
			rlist_entry(rlist_first(&current_ch->send_queue.coros), struct wakeup_entry, base);
		coro_wakeup(wakeup_coro->coro);
		rlist_move_tail(&current_ch->send_queue.coros, &wakeup_coro->base);
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}


#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	coro_bus_try_broadcast(bus, data);
	enum coro_bus_error_code err = coro_bus_errno();
	while (bus != NULL && err == CORO_BUS_ERR_WOULD_BLOCK) 
	{
		coro_yield();
		coro_bus_try_broadcast(bus, data);
		err = coro_bus_errno();
	}

	if (bus == NULL) 
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	return err;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (bus->channel_count <= 0) 
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	bool empty_bus = true;
	for (int i = 0; i < bus->channel_count; ++i) 
	{
		struct coro_bus_channel* current_ch = bus->channels[i]; 

		if (current_ch == NULL)
		{
			continue;
		}

		empty_bus = false;
		int current_size = current_ch->data.size;
		if ((int)current_ch->data.capacity == current_size)
		{
			coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
			return -1;
		}
	}

	if (empty_bus)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	for (int i = 0; i < bus->channel_count; ++i) 
	{
		struct coro_bus_channel* current_ch = bus->channels[i]; 
		if (current_ch != NULL)
		{
			coro_bus_try_send(bus, i, data);
		}
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	int res = coro_bus_try_send_v(bus, channel, data, count);

	bool in_list = false;
	struct wakeup_entry coro_v;
	coro_v.coro = coro_this();
	while (res <= 0 && coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
	{

		if (!in_list)
		{
			rlist_add_tail(&bus->channels[channel]->send_queue.coros, &coro_v.base);
			in_list = !in_list;
		}
		coro_suspend();
		res = coro_bus_try_send_v(bus, channel, data, count);
	}

	if (in_list)
	{
		rlist_del(&coro_v.base);
	}

	return res < 0 ? -1 : (int)res;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	unsigned i = 0;
	int res = 0;
	for (; i < count && !res; ++i) 
	{
		res = coro_bus_try_send(bus, channel, data[i]);
	}

	if (res)
	{
		--i;
	}

	return i ? (int)i : -1;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	int res = coro_bus_try_recv_v(bus, channel, data, capacity);

	bool in_list = false;
	struct wakeup_entry coro_v;
	coro_v.coro = coro_this();
	while (res <= 0 && coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK)
	{
		if (!in_list)
		{
			rlist_add_tail(&bus->channels[channel]->recv_queue.coros, &coro_v.base);
			in_list = !in_list;
		}
		coro_suspend();
		res = coro_bus_try_recv_v(bus, channel, data, capacity);
	}

	if (in_list)
	{
		rlist_del(&coro_v.base);
	}

	return res < 0 ? -1 : (int)res;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	unsigned i = 0;
	int res = 0;
	for (; i < capacity && !res; ++i) 
	{
		res = coro_bus_try_recv(bus, channel, &data[i]);
	}
	
	if (res)
	{
		--i;
	}

	return i ? (int)i : -1;
}

#endif