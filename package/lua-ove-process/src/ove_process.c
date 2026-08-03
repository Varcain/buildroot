#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>

extern char **environ;

static int push_errno(lua_State *L, int error)
{
	lua_pushnil(L);
	lua_pushstring(L, strerror(error));
	lua_pushinteger(L, error);
	return 3;
}

static int add_open(lua_State *L, posix_spawn_file_actions_t *actions, int options_index, const char *name, int fd, int flags)
{
	int rc;
	lua_getfield(L, options_index, name);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	const char *path = luaL_checkstring(L, -1);
	rc = posix_spawn_file_actions_addopen(actions, fd, path, flags, 0666);
	lua_pop(L, 1);
	return rc;
}

static int process_spawn(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	int options_index = lua_isnoneornil(L, 3) ? 0 : 3;
	if (options_index)
		luaL_checktype(L, options_index, LUA_TTABLE);

	size_t nargs = lua_rawlen(L, 2);
	if (nargs > 63)
		return luaL_error(L, "too many spawn arguments");
	char **argv = calloc(nargs + 2, sizeof(*argv));
	if (!argv)
		return push_errno(L, ENOMEM);
	argv[0] = (char *)path;
	for (size_t i = 0; i < nargs; ++i) {
		lua_rawgeti(L, 2, (lua_Integer)i + 1);
		argv[i + 1] = (char *)luaL_checkstring(L, -1);
		lua_pop(L, 1);
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_t *actions_ptr = NULL;
	int rc = 0;
	if (options_index) {
		rc = posix_spawn_file_actions_init(&actions);
		if (rc == 0) {
			actions_ptr = &actions;
			rc = add_open(L, &actions, options_index, "stdin", STDIN_FILENO, O_RDONLY);
		}
		if (rc == 0)
			rc = add_open(L, &actions, options_index, "stdout", STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC);
		if (rc == 0)
			rc = add_open(L, &actions, options_index, "stderr", STDERR_FILENO, O_WRONLY | O_CREAT | O_TRUNC);
	}

	pid_t pid = -1;
	if (rc == 0)
		rc = posix_spawnp(&pid, path, actions_ptr, NULL, argv, environ);
	if (actions_ptr)
		posix_spawn_file_actions_destroy(actions_ptr);
	free(argv);
	if (rc != 0)
		return push_errno(L, rc);
	lua_pushinteger(L, pid);
	return 1;
}

static int process_wait(lua_State *L)
{
	pid_t pid = (pid_t)luaL_checkinteger(L, 1);
	int options = lua_toboolean(L, 2) ? WNOHANG : 0;
	int status = 0;
	pid_t result = waitpid(pid, &status, options);
	if (result < 0)
		return push_errno(L, errno);
	lua_pushinteger(L, result);
	if (result == 0) {
		lua_pushnil(L);
		lua_pushliteral(L, "running");
		return 3;
	}
	if (WIFEXITED(status)) {
		lua_pushinteger(L, WEXITSTATUS(status));
		lua_pushliteral(L, "exited");
	} else if (WIFSIGNALED(status)) {
		lua_pushinteger(L, WTERMSIG(status));
		lua_pushliteral(L, "signaled");
	} else {
		lua_pushinteger(L, status);
		lua_pushliteral(L, "changed");
	}
	return 3;
}

static int process_kill(lua_State *L)
{
	pid_t pid = (pid_t)luaL_checkinteger(L, 1);
	int sig = (int)luaL_optinteger(L, 2, SIGTERM);
	if (kill(pid, sig) != 0)
		return push_errno(L, errno);
	lua_pushboolean(L, 1);
	return 1;
}

static int process_setpriority(lua_State *L)
{
	id_t pid = (id_t)luaL_checkinteger(L, 1);
	int nice = (int)luaL_checkinteger(L, 2);
	if (setpriority(PRIO_PROCESS, pid, nice) != 0)
		return push_errno(L, errno);
	lua_pushboolean(L, 1);
	return 1;
}

static int process_getpriority(lua_State *L)
{
	id_t pid = (id_t)luaL_checkinteger(L, 1);
	errno = 0;
	int nice = getpriority(PRIO_PROCESS, pid);
	if (nice == -1 && errno != 0)
		return push_errno(L, errno);
	lua_pushinteger(L, nice);
	return 1;
}

static int process_getpid(lua_State *L)
{
	lua_pushinteger(L, getpid());
	return 1;
}

static const luaL_Reg process_functions[] = {
	{ "spawn", process_spawn },
	{ "wait", process_wait },
	{ "kill", process_kill },
	{ "setpriority", process_setpriority },
	{ "getpriority", process_getpriority },
	{ "getpid", process_getpid },
	{ NULL, NULL },
};

int luaopen_ove_process(lua_State *L)
{
	luaL_newlib(L, process_functions);
	lua_pushinteger(L, SIGTERM);
	lua_setfield(L, -2, "SIGTERM");
	lua_pushinteger(L, SIGKILL);
	lua_setfield(L, -2, "SIGKILL");
	lua_pushinteger(L, SIGINT);
	lua_setfield(L, -2, "SIGINT");
	return 1;
}
