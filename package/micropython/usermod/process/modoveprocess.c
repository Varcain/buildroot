// SPDX-License-Identifier: MIT

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "py/runtime.h"

extern char **environ;

#define PROCESS_MAX_ARGUMENTS 31

static int add_open(posix_spawn_file_actions_t *actions, mp_obj_t path_in,
        int fd, int flags) {
    if (path_in == mp_const_none) {
        return 0;
    }
    const char *path = mp_obj_str_get_str(path_in);
    return posix_spawn_file_actions_addopen(actions, fd, path, flags, 0666);
}

static mp_obj_t process_spawn(size_t n_args, const mp_obj_t *args) {
    const char *path = mp_obj_str_get_str(args[0]);
    size_t argument_count;
    mp_obj_t *argument_objects;
    mp_obj_get_array(args[1], &argument_count, &argument_objects);
    if (argument_count > PROCESS_MAX_ARGUMENTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many spawn arguments"));
    }

    char *arguments[PROCESS_MAX_ARGUMENTS + 2];
    arguments[0] = (char *)path;
    for (size_t index = 0; index < argument_count; ++index) {
        arguments[index + 1] = (char *)mp_obj_str_get_str(argument_objects[index]);
    }
    arguments[argument_count + 1] = NULL;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t *actions_pointer = NULL;
    int result = 0;
    if (n_args > 2) {
        result = posix_spawn_file_actions_init(&actions);
        if (result == 0) {
            actions_pointer = &actions;
            result = add_open(&actions, args[2], STDIN_FILENO, O_RDONLY);
        }
        if (result == 0 && n_args > 3) {
            result = add_open(&actions, args[3], STDOUT_FILENO,
                O_WRONLY | O_CREAT | O_TRUNC);
        }
        if (result == 0 && n_args > 4) {
            result = add_open(&actions, args[4], STDERR_FILENO,
                O_WRONLY | O_CREAT | O_TRUNC);
        }
    }

    pid_t pid = -1;
    if (result == 0) {
        result = posix_spawnp(&pid, path, actions_pointer, NULL, arguments, environ);
    }
    if (actions_pointer != NULL) {
        posix_spawn_file_actions_destroy(actions_pointer);
    }
    if (result != 0) {
        mp_raise_OSError(result);
    }
    return mp_obj_new_int(pid);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(process_spawn_obj, 2, 5, process_spawn);

static mp_obj_t process_wait(size_t n_args, const mp_obj_t *args) {
    pid_t pid = (pid_t)mp_obj_get_int(args[0]);
    int options = n_args < 2 || mp_obj_is_true(args[1]) ? WNOHANG : 0;
    int status = 0;
    pid_t result = waitpid(pid, &status, options);
    if (result < 0) {
        mp_raise_OSError(errno);
    }

    mp_obj_t values[3] = {
        mp_obj_new_int(result),
        mp_const_none,
        MP_OBJ_NEW_QSTR(MP_QSTR_running),
    };
    if (result != 0 && WIFEXITED(status)) {
        values[1] = mp_obj_new_int(WEXITSTATUS(status));
        values[2] = MP_OBJ_NEW_QSTR(MP_QSTR_exited);
    } else if (result != 0 && WIFSIGNALED(status)) {
        values[1] = mp_obj_new_int(WTERMSIG(status));
        values[2] = MP_OBJ_NEW_QSTR(MP_QSTR_signaled);
    } else if (result != 0) {
        values[1] = mp_obj_new_int(status);
        values[2] = MP_OBJ_NEW_QSTR(MP_QSTR_changed);
    }
    return mp_obj_new_tuple(3, values);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(process_wait_obj, 1, 2, process_wait);

static mp_obj_t process_kill(size_t n_args, const mp_obj_t *args) {
    pid_t pid = (pid_t)mp_obj_get_int(args[0]);
    int signal_number = n_args < 2 ? SIGTERM : mp_obj_get_int(args[1]);
    if (kill(pid, signal_number) != 0) {
        mp_raise_OSError(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(process_kill_obj, 1, 2, process_kill);

static mp_obj_t process_setpriority(mp_obj_t pid_in, mp_obj_t priority_in) {
    id_t pid = (id_t)mp_obj_get_int(pid_in);
    int priority = mp_obj_get_int(priority_in);
    if (setpriority(PRIO_PROCESS, pid, priority) != 0) {
        mp_raise_OSError(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(process_setpriority_obj, process_setpriority);

static mp_obj_t process_getpriority(mp_obj_t pid_in) {
    id_t pid = (id_t)mp_obj_get_int(pid_in);
    errno = 0;
    int priority = getpriority(PRIO_PROCESS, pid);
    if (priority == -1 && errno != 0) {
        mp_raise_OSError(errno);
    }
    return mp_obj_new_int(priority);
}
static MP_DEFINE_CONST_FUN_OBJ_1(process_getpriority_obj, process_getpriority);

static mp_obj_t process_getpid(void) {
    return mp_obj_new_int(getpid());
}
static MP_DEFINE_CONST_FUN_OBJ_0(process_getpid_obj, process_getpid);

static const mp_rom_map_elem_t process_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ove_process) },
    { MP_ROM_QSTR(MP_QSTR_spawn), MP_ROM_PTR(&process_spawn_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait), MP_ROM_PTR(&process_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_kill), MP_ROM_PTR(&process_kill_obj) },
    { MP_ROM_QSTR(MP_QSTR_setpriority), MP_ROM_PTR(&process_setpriority_obj) },
    { MP_ROM_QSTR(MP_QSTR_getpriority), MP_ROM_PTR(&process_getpriority_obj) },
    { MP_ROM_QSTR(MP_QSTR_getpid), MP_ROM_PTR(&process_getpid_obj) },
    { MP_ROM_QSTR(MP_QSTR_SIGTERM), MP_ROM_INT(SIGTERM) },
    { MP_ROM_QSTR(MP_QSTR_SIGKILL), MP_ROM_INT(SIGKILL) },
    { MP_ROM_QSTR(MP_QSTR_SIGINT), MP_ROM_INT(SIGINT) },
};
static MP_DEFINE_CONST_DICT(process_module_globals, process_module_globals_table);

const mp_obj_module_t process_user_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&process_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ove_process, process_user_module);
