#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h> 
#include <unistd.h>
#include <sys/wait.h>

#include <pwd.h>
#include <limits.h>

#include <string.h>

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
exit_exec_command() 
{
	int code = 0;
	exit(code);
}

static void
execute_command_line(const struct command_line *line)
{
	assert(line != NULL);

	int output_stream = STDOUT_FILENO;

	if (line->out_type == OUTPUT_TYPE_STDOUT) {
		output_stream = STDOUT_FILENO;
	} else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
		output_stream = open(line->out_file, O_WRONLY|O_CREAT|O_TRUNC);
	} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
		output_stream = open(line->out_file, O_WRONLY|O_APPEND);
	} else {
		assert(false);
	}
	const struct expr *e = line->head;

	unsigned count_command = get_count_command(e);
	int* out_fds = malloc(count_command * 2 * sizeof(int));
	pipe(&out_fds[0]);
	size_t out_it = 0;

	while (e != NULL) {
		if (e->type == EXPR_TYPE_COMMAND) {
			if (strcmp(e->cmd.exe, "cd") == 0)
			{
				assert(e->cmd.arg_capacity != 0);
				cd_exec_command(e->cmd.args[0]);
			} else if (strcmp(e->cmd.exe, "exit") == 0) 
			{
				exit_exec_command();
			}
			else
			{
				if (fork() == 0) 
				{
					close(out_fds[out_it]);

					dup2(out_fds[out_it + 1], STDOUT_FILENO);
					close(out_fds[out_it + 1]);

					size_t exec_argv = e->cmd.arg_capacity;
					if (exec_argv == 0)
					{
						++exec_argv;
					}
					char* exec_args[exec_argv + 1];
					exec_args[0] = e->cmd.exe; 
					for (size_t i = 0; i + 1 < e->cmd.arg_capacity; ++i)
					{
						exec_args[i + 1] = e->cmd.args[i];
					}
					exec_args[exec_argv] = NULL;

					execvp(e->cmd.exe, exec_args);
				}
			}
			
			close(out_fds[out_it + 1]);
			out_it += 2;
		} else if (e->type == EXPR_TYPE_PIPE) {
		
			//pipe(&out_fds[out_it]);
			//dup2(out_fds[out_it - 2], STDIN_FILENO); // тут ошибка, подтягивается родительский ввод
			//close(out_fds[out_it - 2]);

		} else if (e->type == EXPR_TYPE_AND) {
			printf("\tAND\n");
		} else if (e->type == EXPR_TYPE_OR) {
			printf("\tOR\n");
		} else {
			assert(false);
		}
		e = e->next;
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

	free(out_fds);
}



int
main(void)
{
	struct passwd *pw = getpwuid(getuid());
    const char *username = pw ? pw->pw_name : "unknown";

    char hostname[HOST_NAME_MAX + 1];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname");
        return 1;
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return 1;
    }

	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	printf("%s@%s:%s$ ", username, hostname, cwd);
	fflush(stdout);
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
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
			execute_command_line(line);
			command_line_delete(line);
		}
		if (getcwd(cwd, sizeof(cwd)) == NULL) {
			perror("getcwd");
			return 1;
		}
		printf("%s@%s:%s$ ", username, hostname, cwd);
		fflush(stdout);
	}
	parser_delete(p);
	return 0;
}
