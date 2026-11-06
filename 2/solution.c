#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h> 
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#include <pwd.h>

#include <string.h>

// python3 checker.py -e ./mybash --with_background WITH_BACKGROUND --tests ./tests.txt --verbose 1

static unsigned 
get_count_command(const struct expr *head) 
{
	unsigned cnt = 0;
	while (head != NULL)
	{
		++cnt;
		head = head->next;
	}

	return cnt;
}

static void
cd_exec_command(const char* args) 
{
	chdir(args);
}

static void
exit_exec_command(const struct command* cmd) 
{
	int code = 0;

	if (cmd->arg_count != 0) 
	{
		code = atoi(cmd->args[0]);
	}

	exit(code);
}

static bool 
has_pipe(const struct expr* e) 
{
	e = e->next;
	while (e != NULL)
	{
		if (e->type == EXPR_TYPE_PIPE)
		{
			return true;
		}
		e = e->next;
	}
	return false;
}

static void
clear_zombie() 
{
	while (waitpid(-1, NULL, WNOHANG) > 0);
}

static int
execute_command_line(const struct command_line *line)
{
	assert(line != NULL);

	int output_stream = STDOUT_FILENO;

	if (!line->is_background) 
	{
		if (line->out_type == OUTPUT_TYPE_FILE_NEW) 
		{
			output_stream = open(line->out_file, O_WRONLY|O_CREAT|O_TRUNC, 0666);
		} 
		else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) 
		{
			output_stream = open(line->out_file, O_WRONLY|O_APPEND, 0666);
		}
	}
	const struct expr *e = line->head;

	unsigned count_command = get_count_command(e);
	int out_fds[count_command * 2];
	pid_t pids[count_command];

	pipe(&out_fds[0]);
	size_t out_it = 0;

	bool pipe_flag = false;
	bool has_pipe_flag = has_pipe(e);

	while (e != NULL) 
	{
		if (e->type == EXPR_TYPE_COMMAND) 
		{
			
			if (!pipe_flag && !has_pipe_flag && strcmp(e->cmd.exe, "cd") == 0)
			{
				cd_exec_command(e->cmd.args[0]);
			} 
			else if (!pipe_flag && !has_pipe_flag && strcmp(e->cmd.exe, "exit") == 0) 
			{
				exit_exec_command(&e->cmd);
			}
			else
			{
				pid_t pid = fork();
				if (pid == 0) 
				{
					if (strcmp(e->cmd.exe, "cd") == 0)
					{
						cd_exec_command(e->cmd.args[0]);
					} 
					else if (strcmp(e->cmd.exe, "exit") == 0) 
					{
						exit_exec_command(&e->cmd);
					}
					else 
					{
						close(out_fds[out_it]);
						if (pipe_flag)
						{
							dup2(out_fds[out_it - 2], STDIN_FILENO);
							close(out_fds[out_it - 2]);
						}
						
						int output_child_stream = STDOUT_FILENO;
						if (line->is_background && !has_pipe_flag) 
						{
							if (line->out_type == OUTPUT_TYPE_FILE_NEW) 
							{
								output_child_stream = open(line->out_file, O_WRONLY|O_CREAT|O_TRUNC, 0666);
							} 
							else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) 
							{
								output_child_stream = open(line->out_file, O_WRONLY|O_APPEND, 0666);
							}
						}

						dup2(out_fds[out_it + 1], output_child_stream);
						close(out_fds[out_it + 1]);

						size_t exec_argv = e->cmd.arg_count;
						char* exec_args[exec_argv + 2];
						exec_args[0] = e->cmd.exe; 
						for (size_t i = 0; i < e->cmd.arg_count; ++i)
						{
							exec_args[i + 1] = e->cmd.args[i];
						}
						exec_args[exec_argv + 1] = NULL;

						execvp(e->cmd.exe, exec_args);
						perror(e->cmd.exe); 
						exit(127);
					}
				}
				else if (pid > 0)
				{
					pids[out_it / 2] = pid;
				}
			}

			if (pipe_flag)
			{
				assert(out_it >= 2);
				close(out_fds[out_it - 2]);
				pipe_flag = false;
			}
			close(out_fds[out_it + 1]);
			out_it += 2;
		} 
		else if (e->type == EXPR_TYPE_PIPE) 
		{
			has_pipe_flag = has_pipe(e);
			pipe_flag = true;
			pipe(&out_fds[out_it]);
		} 
		else if (e->type == EXPR_TYPE_AND) 
		{
			printf("\tAND\n");
		} 
		else if (e->type == EXPR_TYPE_OR) 
		{
			printf("\tOR\n");
		} 
		else 
		{
			assert(false);
		}
		e = e->next;
	}

	if (line->is_background)
	{
		close(out_fds[out_it - 2]);
		return 0;
	}

	char buf [4096];
	ssize_t n = 0;

	while ((n = read(out_fds[out_it - 2], buf, sizeof(buf))) > 0)
	{
		write(output_stream, buf, n);
	}
	
	if (output_stream != STDOUT_FILENO) 
	{
		close(output_stream);
	}

	close(out_fds[out_it - 2]);

	int status;
	int last_exit_code = 0;

	signal(SIGCHLD, SIG_DFL);

	for (size_t i = 0; i < out_it / 2; ++i) 
	{
		if (pids[i] == 0)
		{
			continue;
		}

		waitpid(pids[i], &status, 0);
		if (i == (out_it / 2) - 1)
		{
			if (WIFEXITED(status)) 
			{
				last_exit_code = WEXITSTATUS(status);
			} 
			else if (WIFSIGNALED(status)) 
			{
				last_exit_code = 128 + WTERMSIG(status);
			}	
		}
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = clear_zombie;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	return last_exit_code;
}

int
main(void)
{

	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	int code = 0;
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) 
	{
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) 
		{
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			code = execute_command_line(line);
			command_line_delete(line);
		}
	}
	parser_delete(p);
	return code;
}
