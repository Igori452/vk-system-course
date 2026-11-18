#include "thread_pool.h"
#include <pthread.h>

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

struct thread_task {
	thread_task_f function;
	void *arg;
	pthread_mutex_t mutex;
	pthread_cond_t cond_finished;
	
	/* Doubly linked list. */
	struct thread_task* next;
	struct thread_task* prev;
	/* The task has been added to the pool. */
	bool is_pushed;
	/* Task in progress. */
	bool is_running;
	/* Thread completion flag. */
	bool is_finish;
	/* Thread joined. */
	bool is_join;
	/* Task result. */
	void* result;
};

struct thread_pool {
	pthread_t *threads;
	/* Also used for changing next/prev for tasks. */
	pthread_mutex_t mutex;
	pthread_cond_t cond_has_tasks;
	
	/* Pointers to unsolved problems. */
	struct thread_task* tasks_tail;
	struct thread_task* tasks_head; 
	/* Maximum number of threads. */
	size_t max_thread_count;
	/* Current number of threads. */
	size_t tasks_count;
	/* Number of threads. */
	size_t threads_count;
	/* Number of available workers */
	size_t count_free_worker;
	/* Deleting a pool. */
	bool is_deleting;
};

int
thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
	if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS)
	{
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	*pool = malloc(sizeof(struct thread_pool));
	
	(*pool)->max_thread_count = max_thread_count;
	(*pool)->tasks_count = 0;
	(*pool)->threads_count = 0;
	(*pool)->count_free_worker = 0;
	(*pool)->is_deleting = false;
	(*pool)->tasks_head = NULL;
	(*pool)->tasks_tail = NULL;
	(*pool)->threads = NULL;
	
	pthread_mutex_init(&((*pool)->mutex), NULL);
	pthread_cond_init(&((*pool)->cond_has_tasks), NULL);

	return 0;
}

int
thread_pool_thread_count(const struct thread_pool *pool)
{
	pthread_mutex_lock((pthread_mutex_t*)&(pool->mutex));
	size_t th_cnt = pool->threads_count;
	pthread_mutex_unlock((pthread_mutex_t*)&(pool->mutex));

	return th_cnt;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	pthread_mutex_lock(&(pool->mutex));
	if (pool->tasks_count > 0)
	{
		pthread_mutex_unlock(&(pool->mutex));
		return TPOOL_ERR_HAS_TASKS;
	}

	pool->is_deleting = true;
	pthread_cond_broadcast(&(pool->cond_has_tasks));

	size_t threads_cnt = pool->threads_count;
	pthread_t* threads = pool->threads;
	pool->threads = NULL;
	
	pthread_mutex_unlock(&(pool->mutex));

	for (size_t i = 0; i < threads_cnt; ++i) 
	{
		pthread_join(threads[i], NULL);
	}

	free(threads);
	pthread_mutex_destroy(&(pool->mutex));
	pthread_cond_destroy(&(pool->cond_has_tasks));
	free(pool);

	return 0;
}

static void*
worker(void* arg) 
{
	struct thread_pool* th_pool = (struct thread_pool*)arg;

	while(true) 
	{
		pthread_mutex_lock(&(th_pool->mutex));
		++(th_pool->count_free_worker);
		while (th_pool->tasks_head == NULL && !th_pool->is_deleting) 
		{
			pthread_cond_wait(&(th_pool->cond_has_tasks), &(th_pool->mutex));
		}

		if (th_pool->is_deleting)
		{
			pthread_mutex_unlock(&(th_pool->mutex));
			break;
		}

		struct thread_task* th_task = th_pool->tasks_head;
		if (th_pool->tasks_head == th_pool->tasks_tail)
		{
			th_pool->tasks_tail = NULL;
		}
		th_pool->tasks_head = th_pool->tasks_head->next;

		--(th_pool->count_free_worker);

		th_task->next = NULL;
		th_task->prev = NULL;
		pthread_mutex_unlock(&(th_pool->mutex));

		pthread_mutex_lock(&(th_task->mutex));
		th_task->is_running = true;
		void* arg_f = th_task->arg; 
		pthread_mutex_unlock(&(th_task->mutex));

		void* result_f = th_task->function(arg_f);

		pthread_mutex_lock(&(th_task->mutex));
		th_task->result = result_f;
		th_task->is_finish = true;
		th_task->is_running = false;
		pthread_cond_signal(&(th_task->cond_finished));

		pthread_mutex_unlock(&(th_task->mutex));

		pthread_mutex_lock(&(th_pool->mutex));
		--(th_pool->tasks_count);
		pthread_mutex_unlock(&(th_pool->mutex));
	}

	return NULL;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	pthread_mutex_lock(&pool->mutex);
	if (pool->tasks_count == TPOOL_MAX_TASKS)
	{
		pthread_mutex_unlock(&(pool->mutex));
		return TPOOL_ERR_TOO_MANY_TASKS;
	}
	
	pthread_mutex_lock(&(task->mutex));
	task->is_pushed = true;
	task->is_finish = false;
	task->is_join = false;
	pthread_mutex_unlock(&(task->mutex));

	if (pool->tasks_head == NULL)
	{
		pool->tasks_head = task;
		pool->tasks_tail = task;
	}
	else
	{
		pool->tasks_tail->next = task;
		task->prev = pool->tasks_tail;
		pool->tasks_tail = task;
	}
	++(pool->tasks_count);
	pthread_cond_signal(&(pool->cond_has_tasks));
	pthread_mutex_unlock(&(pool->mutex));

	pthread_mutex_lock(&(pool->mutex));
	if (pool->count_free_worker == 0 && pool->max_thread_count > pool->threads_count)
	{
		pthread_t tid;
		pthread_create(&tid, NULL, worker, pool);

		pool->threads = realloc(pool->threads, ++(pool->threads_count) * sizeof(pthread_t));
		pool->threads[pool->threads_count - 1] = tid;
	}
	else 
	{
		pthread_cond_broadcast(&(pool->cond_has_tasks));
	}

	pthread_mutex_unlock(&(pool->mutex));

	return 0;
}

int
thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
	*task = malloc(sizeof(struct thread_task));
	(*task)->arg = arg;
	(*task)->function = function;
	(*task)->next = NULL;
	(*task)->prev = NULL;
	(*task)->result = NULL;
	(*task)->is_finish = false;
	(*task)->is_running = false;
	(*task)->is_pushed = false;
	(*task)->is_join = false;

	pthread_mutex_init(&((*task)->mutex), NULL);
	pthread_cond_init(&(*task)->cond_finished, NULL);

	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	pthread_mutex_lock((pthread_mutex_t*)&(task->mutex));
	bool result = task->is_finish;
	pthread_mutex_unlock((pthread_mutex_t*)&(task->mutex));

	return result;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	pthread_mutex_lock((pthread_mutex_t*)&(task->mutex));
	bool result = task->is_running;
	pthread_mutex_unlock((pthread_mutex_t*)&(task->mutex));

	return result;
}

int
thread_task_join(struct thread_task *task, void **result)
{
	pthread_mutex_lock(&(task->mutex));
	if (!task->is_pushed)
	{
		pthread_mutex_unlock(&(task->mutex));
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	while (!task->is_finish)
	{
		pthread_cond_wait(&(task->cond_finished), &(task->mutex));	
	}

	*result = task->result;
	task->is_join = true;
	pthread_mutex_unlock(&(task->mutex));

	return 0;	

}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	(void)timeout;
	(void)result;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	pthread_mutex_lock(&(task->mutex));
	if (!task->is_join && task->is_pushed)
	{
		pthread_mutex_unlock(&(task->mutex));
		return TPOOL_ERR_TASK_IN_POOL;
	}
	pthread_mutex_unlock(&(task->mutex));
	
	pthread_mutex_destroy(&(task->mutex));
	pthread_cond_destroy(&(task->cond_finished));
	free(task);

	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif
